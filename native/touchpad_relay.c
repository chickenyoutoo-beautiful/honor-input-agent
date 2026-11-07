/*
 * touchpad_relay - userspace takeover of the HONOR touchpad.
 *
 * The touchpad reports INPUT_PROP_DIRECT, so Android treats it as a touchscreen:
 * pointer capture fails, two-finger scroll never reaches apps, and the pointer
 * jumps to absolute positions. This daemon grabs the device and re-emits a
 * proper relative mouse, with high-resolution wheel events for smooth scrolling.
 *
 * Tunables (all optional):
 *   --sens <float>    pointer speed multiplier      (default 1.8)
 *   --scroll <int>    wheel HiRes units per pixel   (default 5, higher = faster)
 *   --invert <0|1>    flip scroll direction         (default 0)
 *   --tap-ms <int>    max tap duration in ms        (default 220)
 *
 * Build: aarch64-linux-android21-clang -O2 -o touchpad_relay touchpad_relay.c
 */
#include <linux/input.h>
#include <linux/uinput.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <time.h>

#define MAX_SLOTS 10

static float g_sens = 1.8f;
static int g_hi_res = 5;
static int g_invert = 0;
static int g_tap_ms = 220;

#define HI_RES_PER_NOTCH 120
#define TAP_MAX_TRAVEL 14
#define MOVE_MAX 24

static int slot_active[MAX_SLOTS];
static int slot_x[MAX_SLOTS];
static int slot_y[MAX_SLOTS];
static int cur_slot;

static int prev_avg_x = -1, prev_avg_y = -1;
static int hi_res_accum;

static int single_prev_x = -1, single_prev_y = -1;
static long down_time_ms;
static int travelled;
static int physical_click;
static int saw_two_fingers;
static long two_down_ms;
static int two_travel;

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void emit(int fd, int type, int code, int value) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type; ev.code = code; ev.value = value;
    if (write(fd, &ev, sizeof(ev)) < 0) { /* ignore */ }
}

static void sync_report(int fd) {
    emit(fd, EV_SYN, SYN_REPORT, 0);
}

static int find_touchpad(char *out, size_t out_len) {
    FILE *f = fopen("/proc/bus/input/devices", "r");
    if (!f) return -1;
    char line[512], name[256] = {0}, handlers[512] = {0};
    int matched = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "N: Name=", 8) == 0) {
            char *p = strchr(line, '"');
            if (p) {
                char *q = strrchr(p + 1, '"');
                if (q) { *q = 0; snprintf(name, sizeof(name), "%s", p + 1); }
            }
            matched = strstr(name, "HONOR Smart Touch") != NULL
                    && strstr(name, "Keyboard") == NULL
                    && strstr(name, "Consumer") == NULL;
            handlers[0] = 0;
        } else if (strncmp(line, "H: Handlers=", 12) == 0 && matched) {
            snprintf(handlers, sizeof(handlers), "%s", line + 12);
            char *ev = strstr(handlers, "event");
            if (ev) {
                char dev[64]; int n = 0;
                while (ev[n] && ev[n] != ' ' && ev[n] != '\n' && n < 32) { dev[n] = ev[n]; n++; }
                dev[n] = 0;
                snprintf(out, out_len, "/dev/input/%s", dev);
                fclose(f);
                return 0;
            }
        }
    }
    fclose(f);
    return -1;
}

static int create_uinput(void) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { perror("open /dev/uinput"); return -1; }

    ioctl(fd, UI_SET_EVBIT, EV_REL);
    ioctl(fd, UI_SET_RELBIT, REL_X);
    ioctl(fd, UI_SET_RELBIT, REL_Y);
    ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
    ioctl(fd, UI_SET_RELBIT, REL_HWHEEL);
#ifdef REL_WHEEL_HI_RES
    ioctl(fd, UI_SET_RELBIT, REL_WHEEL_HI_RES);
    ioctl(fd, UI_SET_RELBIT, REL_HWHEEL_HI_RES);
#endif
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);

    struct uinput_user_dev uidev;
    memset(&uidev, 0, sizeof(uidev));
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "honor-touchpad-relay");
    uidev.id.bustype = BUS_VIRTUAL;
    uidev.id.vendor = 0x1234;
    uidev.id.product = 0x5679;
    uidev.id.version = 1;

    if (write(fd, &uidev, sizeof(uidev)) < 0) { perror("uidev"); close(fd); return -1; }
    if (ioctl(fd, UI_DEV_CREATE) < 0) { perror("UI_DEV_CREATE"); close(fd); return -1; }
    return fd;
}

static int count_active(void) {
    int n = 0;
    for (int i = 0; i < MAX_SLOTS; i++) if (slot_active[i]) n++;
    return n;
}

static int avg_of(const int *arr) {
    int sum = 0, n = 0;
    for (int i = 0; i < MAX_SLOTS; i++) if (slot_active[i]) { sum += arr[i]; n++; }
    return n ? sum / n : -1;
}

static void emit_hi_res(int fd, int dy_px, int dx_px) {
#ifdef REL_WHEEL_HI_RES
    if (dy_px != 0) {
        int v = dy_px * g_hi_res;
        if (g_invert) v = -v;
        emit(fd, EV_REL, REL_WHEEL_HI_RES, v);
        hi_res_accum += v;
        while (hi_res_accum >= HI_RES_PER_NOTCH) {
            emit(fd, EV_REL, REL_WHEEL, 1);
            hi_res_accum -= HI_RES_PER_NOTCH;
        }
        while (hi_res_accum <= -HI_RES_PER_NOTCH) {
            emit(fd, EV_REL, REL_WHEEL, -1);
            hi_res_accum += HI_RES_PER_NOTCH;
        }
        sync_report(fd);
    }
    if (dx_px != 0) {
        emit(fd, EV_REL, REL_HWHEEL_HI_RES, dx_px * g_hi_res);
        sync_report(fd);
    }
#else
    (void)fd; (void)dy_px; (void)dx_px;
#endif
}

static void reset_gesture(void) {
    prev_avg_x = prev_avg_y = -1;
    hi_res_accum = 0;
    single_prev_x = single_prev_y = -1;
    travelled = 0;
}

int main(int argc, char **argv) {
    char dev[128] = {0};
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sens") && i + 1 < argc) {
            g_sens = (float) atof(argv[++i]);
        } else if (!strcmp(argv[i], "--scroll") && i + 1 < argc) {
            g_hi_res = atoi(argv[++i]);
            if (g_hi_res < 1) g_hi_res = 1;
        } else if (!strcmp(argv[i], "--invert") && i + 1 < argc) {
            g_invert = atoi(argv[++i]) ? 1 : 0;
        } else if (!strcmp(argv[i], "--tap-ms") && i + 1 < argc) {
            g_tap_ms = atoi(argv[++i]);
        } else if (strncmp(argv[i], "--", 2) != 0) {
            snprintf(dev, sizeof(dev), "%s", argv[i]);
        }
    }
    if (!dev[0] && find_touchpad(dev, sizeof(dev)) != 0) {
        fprintf(stderr, "touchpad not found\n");
        return 1;
    }

    int tfd = open(dev, O_RDONLY);
    if (tfd < 0) { perror("open touchpad"); return 1; }

    if (ioctl(tfd, EVIOCGRAB, 1) < 0) {
        perror("EVIOCGRAB");
    }

    int ufd = create_uinput();
    if (ufd < 0) { ioctl(tfd, EVIOCGRAB, 0); close(tfd); return 1; }

    /* Record the pid so the controller can kill this exact process later; the
     * su wrapper is the parent, so killing it would leave this daemon alive. */
    {
        FILE *pf = fopen("/data/local/tmp/touchpad_relay.pid", "w");
        if (pf) { fprintf(pf, "%d\n", getpid()); fclose(pf); }
    }

    fprintf(stderr, "touchpad_relay: %s sens=%.2f scroll=%d invert=%d tap=%dms\n",
            dev, g_sens, g_hi_res, g_invert, g_tap_ms);

    struct input_event ev;
    while (read(tfd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (ev.type == EV_ABS) {
            switch (ev.code) {
                case ABS_MT_SLOT:
                    if (ev.value >= 0 && ev.value < MAX_SLOTS) cur_slot = ev.value;
                    break;
                case ABS_MT_TRACKING_ID:
                    if (cur_slot < MAX_SLOTS) {
                        int was = slot_active[cur_slot];
                        slot_active[cur_slot] = (ev.value != -1);
                        if (!slot_active[cur_slot]) { slot_x[cur_slot] = slot_y[cur_slot] = 0; }
                        if (!was && slot_active[cur_slot] && count_active() == 1) {
                            down_time_ms = now_ms();
                            travelled = 0;
                            single_prev_x = single_prev_y = -1;
                        }
                    }
                    break;
                case ABS_MT_POSITION_X:
                    if (cur_slot < MAX_SLOTS) slot_x[cur_slot] = ev.value;
                    break;
                case ABS_MT_POSITION_Y:
                    if (cur_slot < MAX_SLOTS) slot_y[cur_slot] = ev.value;
                    break;
                default:
                    break;
            }
        } else if (ev.type == EV_KEY &&
                   (ev.code == BTN_LEFT || ev.code == BTN_RIGHT || ev.code == BTN_MIDDLE)) {
            physical_click = 1;
            emit(ufd, EV_KEY, ev.code, ev.value);
            sync_report(ufd);
        } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            int n = count_active();

            if (n >= 2) {
                if (!saw_two_fingers) { saw_two_fingers = 1; two_down_ms = now_ms(); two_travel = 0; }
                int ax = avg_of(slot_x), ay = avg_of(slot_y);
                if (ax >= 0 && ay >= 0) {
                    if (prev_avg_x >= 0 && prev_avg_y >= 0) {
                        int dx = ax - prev_avg_x;
                        int dy = ay - prev_avg_y;
                        emit_hi_res(ufd, dy, dx);
                        travelled += abs(dx) + abs(dy);
                        two_travel += abs(dx) + abs(dy);
                    }
                    prev_avg_x = ax;
                    prev_avg_y = ay;
                }
                single_prev_x = single_prev_y = -1;
            } else if (n == 1) {
                int ax = avg_of(slot_x), ay = avg_of(slot_y);
                if (ax >= 0 && ay >= 0) {
                    if (single_prev_x >= 0 && single_prev_y >= 0) {
                        int dx = ax - single_prev_x;
                        int dy = ay - single_prev_y;
                        if (dx > MOVE_MAX) dx = MOVE_MAX;
                        if (dx < -MOVE_MAX) dx = -MOVE_MAX;
                        if (dy > MOVE_MAX) dy = MOVE_MAX;
                        if (dy < -MOVE_MAX) dy = -MOVE_MAX;
                        travelled += abs(dx) + abs(dy);
                        int rx = (int) (dx * g_sens);
                        int ry = (int) (dy * g_sens);
                        if (rx || ry) {
                            if (rx) emit(ufd, EV_REL, REL_X, rx);
                            if (ry) emit(ufd, EV_REL, REL_Y, ry);
                            sync_report(ufd);
                        }
                    }
                    single_prev_x = ax;
                    single_prev_y = ay;
                    prev_avg_x = prev_avg_y = -1;
                }
            } else {
                if (saw_two_fingers) {
                    long dt2 = now_ms() - two_down_ms;
                    if (!physical_click && dt2 <= g_tap_ms && two_travel <= TAP_MAX_TRAVEL) {
                        emit(ufd, EV_KEY, BTN_RIGHT, 1);
                        sync_report(ufd);
                        emit(ufd, EV_KEY, BTN_RIGHT, 0);
                        sync_report(ufd);
                    }
                    saw_two_fingers = 0;
                    two_down_ms = 0;
                    two_travel = 0;
                    down_time_ms = 0;
                } else if (down_time_ms > 0) {
                    long dt = now_ms() - down_time_ms;
                    if (!physical_click && dt <= g_tap_ms && travelled <= TAP_MAX_TRAVEL) {
                        emit(ufd, EV_KEY, BTN_LEFT, 1);
                        sync_report(ufd);
                        emit(ufd, EV_KEY, BTN_LEFT, 0);
                        sync_report(ufd);
                    }
                    down_time_ms = 0;
                }
                physical_click = 0;
                reset_gesture();
            }
        }
    }

    ioctl(ufd, UI_DEV_DESTROY);
    ioctl(tfd, EVIOCGRAB, 0);
    close(ufd);
    close(tfd);
    return 0;
}
