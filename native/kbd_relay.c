/*
 * kbd_relay - userspace takeover of the HONOR keyboard.
 *
 * Two reasons to sit between the keyboard and Android:
 *   1. The firmware reports ESC as KEY_BACK, which Android turns into a back
 *      navigation. Remapped to KEY_ESC here so apps get a real Escape.
 *   2. KEY_LEFTMETA reaches the kernel fine, but the OEM framework swallows it
 *      before apps see it. Re-emitting it from a virtual keyboard with a
 *      different vendor id bypasses that keylayout-based interception.
 *
 * Build: aarch64-linux-android21-clang -O2 -o kbd_relay kbd_relay.c
 */
#include <linux/input.h>
#include <linux/uinput.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define LONG_BITS (sizeof(long) * 8)
#define NLONGS(x) (((x) + LONG_BITS - 1) / LONG_BITS)

static int find_keyboard(char *out, size_t out_len) {
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
            matched = (strcmp(name, "HONOR Smart Touch Keyboard") == 0);
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

int main(int argc, char **argv) {
    char dev[128] = {0};
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--", 2) != 0) snprintf(dev, sizeof(dev), "%s", argv[i]);
    }
    if (!dev[0] && find_keyboard(dev, sizeof(dev)) != 0) {
        fprintf(stderr, "keyboard not found\n");
        return 1;
    }

    int kfd = open(dev, O_RDONLY);
    if (kfd < 0) { perror("open keyboard"); return 1; }

    /* Copy the source key capabilities so every key keeps working. */
    unsigned long keybits[NLONGS(KEY_MAX + 1)];
    memset(keybits, 0, sizeof(keybits));
    ioctl(kfd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);

    if (ioctl(kfd, EVIOCGRAB, 1) < 0) {
        perror("EVIOCGRAB");
    }

    int ufd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (ufd < 0) { perror("open /dev/uinput"); ioctl(kfd, EVIOCGRAB, 0); close(kfd); return 1; }

    ioctl(ufd, UI_SET_EVBIT, EV_KEY);
    for (int code = 0; code <= KEY_MAX; code++) {
        if (keybits[code / LONG_BITS] & (1UL << (code % LONG_BITS))) {
            ioctl(ufd, UI_SET_KEYBIT, code);
        }
    }
    ioctl(ufd, UI_SET_KEYBIT, KEY_ESC);
    /* These two carry the remapped Win keys, so they must be declared even
     * though the physical keyboard does not advertise them. */
    ioctl(ufd, UI_SET_KEYBIT, KEY_RED);
    ioctl(ufd, UI_SET_KEYBIT, KEY_GREEN);

    struct uinput_user_dev uidev;
    memset(&uidev, 0, sizeof(uidev));
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "honor-keyboard-relay");
    uidev.id.bustype = BUS_VIRTUAL;
    uidev.id.vendor = 0x1234;
    uidev.id.product = 0x567a;
    uidev.id.version = 1;
    if (write(ufd, &uidev, sizeof(uidev)) < 0) { perror("uidev"); close(ufd); close(kfd); return 1; }
    if (ioctl(ufd, UI_DEV_CREATE) < 0) { perror("UI_DEV_CREATE"); close(ufd); close(kfd); return 1; }

    {
        FILE *pf = fopen("/data/local/tmp/kbd_relay.pid", "w");
        if (pf) { fprintf(pf, "%d\n", getpid()); fclose(pf); }
    }

    fprintf(stderr, "kbd_relay: %s\n", dev);

    struct input_event ev, out;
    while (read(kfd, &ev, sizeof(ev)) == sizeof(ev)) {
        /* EV_KEY carries the keys, EV_SYN delimits the frames. Dropping SYN
         * makes Android coalesce and reorder keys, which shows up as garbage
         * typing. Everything else (MSC scancodes, LED state) is not needed. */
        if (ev.type != EV_KEY && ev.type != EV_SYN) {
            continue;
        }
        out = ev;
        if (ev.type == EV_KEY && ev.code == KEY_BACK) {
            /* The firmware's ESC key. Android would treat it as "back". */
            out.code = KEY_ESC;
        }
        if (ev.type == EV_KEY && ev.code == KEY_LEFTMETA) {
            /* Honor's PC mode watches for a real Meta and fires its own
             * Windows-style shortcuts (Win+Shift+S screenshots the tablet).
             * Sending PROG_RED instead keeps the system blind to it; the app
             * translates PROG_RED back to VK_LWIN for the host. */
            out.code = KEY_RED;
        }
        if (ev.type == EV_KEY && ev.code == KEY_RIGHTMETA) {
            out.code = KEY_GREEN;
        }
        if (write(ufd, &out, sizeof(out)) < 0) { /* ignore */ }
    }

    ioctl(ufd, UI_DEV_DESTROY);
    ioctl(kfd, EVIOCGRAB, 0);
    close(ufd);
    close(kfd);
    return 0;
}
