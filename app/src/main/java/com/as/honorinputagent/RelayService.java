package com.as.honorinputagent;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.os.IBinder;

import java.io.InputStream;
import java.util.ArrayList;
import java.util.List;

/**
 * Runs the two input relays independently.
 *
 * KernelSU's su kills every descendant when its session ends, so detached
 * launches do not survive; the daemons are kept as children of this service and
 * additionally publish their own pid, because the tracked Process is the su
 * wrapper and killing it alone would orphan the daemon.
 */
public class RelayService extends Service {

    public static final String EXTRA_TARGET = "target";
    public static final String EXTRA_SENS = "sens";
    public static final String EXTRA_SCROLL = "scroll";
    public static final String EXTRA_INVERT = "invert";
    public static final String TARGET_TOUCH = "touch";
    public static final String TARGET_KBD = "kbd";

    private static final String CHANNEL_ID = "relay";
    private static final int NOTIFICATION_ID = 1;
    private static final String TP = "/data/local/tmp/touchpad_relay";
    private static final String KBD = "/data/local/tmp/kbd_relay";
    private static final String TP_PID = "/data/local/tmp/touchpad_relay.pid";
    private static final String KBD_PID = "/data/local/tmp/kbd_relay.pid";

    private final List<Process> processes = new ArrayList<>();
    private static volatile boolean running;

    public static boolean isRunning() {
        return running;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        try {
            startForeground(NOTIFICATION_ID, buildNotification());
        } catch (Throwable t) {
            android.util.Log.w("HonorInputAgent", "startForeground failed: " + t);
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent == null) {
            return START_NOT_STICKY;
        }
        String target = intent.getStringExtra(EXTRA_TARGET);
        if (target == null) target = TARGET_TOUCH;
        boolean stop = "stop".equals(intent.getAction());

        if (stop) {
            stopTarget(target);
        } else {
            startTarget(target, intent);
        }
        running = alive(TP, TP_PID) || alive(KBD, KBD_PID);
        if (!running) {
            stopSelf();
        }
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
    }

    private void startTarget(String target, Intent intent) {
        float sens = intent.getFloatExtra(EXTRA_SENS, 1.8f);
        int scroll = intent.getIntExtra(EXTRA_SCROLL, 5);
        boolean invert = intent.getBooleanExtra(EXTRA_INVERT, false);

        if (TARGET_TOUCH.equals(target) && !alive(TP, TP_PID)) {
            String args = String.format(java.util.Locale.US,
                    " --sens %.2f --scroll %d --invert %d", sens, scroll, invert ? 1 : 0);
            spawn(TP + args, "touchpad");
        } else if (TARGET_KBD.equals(target) && !alive(KBD, KBD_PID)) {
            spawn(KBD, "keyboard");
        }
    }

    private void stopTarget(String target) {
        if (TARGET_TOUCH.equals(target)) {
            kill(TP, TP_PID, "touchpad_relay");
        } else if (TARGET_KBD.equals(target)) {
            kill(KBD, KBD_PID, "kbd_relay");
        }
    }

    private void spawn(String path, String tag) {
        try {
            String cmd = path + " > /data/local/tmp/" + tag + ".log 2>&1";
            Process p = new ProcessBuilder("su", "-c", cmd).start();
            processes.add(p);
            new Thread(() -> {
                try (InputStream in = p.getInputStream()) {
                    byte[] buf = new byte[4096];
                    while (in.read(buf) > 0) { /* drain */ }
                } catch (Exception ignored) {
                }
            }, "drain-" + tag).start();
        } catch (Exception e) {
            android.util.Log.e("HonorInputAgent", "spawn " + tag + ": " + e);
        }
    }

    private void kill(String path, String pidFile, String name) {
        su("[ -f " + pidFile + " ] && kill -9 $(cat " + pidFile + ") 2>/dev/null; "
                + "rm -f " + pidFile + "; "
                + "pkill -9 -f " + path + "; pkill -9 -x " + name + "; true");
    }

    /** True when the pid recorded by the daemon is still a live process. */
    private boolean alive(String path, String pidFile) {
        String out = su("[ -f " + pidFile + " ] && cat " + pidFile
                + " | while read p; do [ -d /proc/$p ] && echo alive; done; true");
        return out.contains("alive");
    }

    private String su(String cmd) {
        StringBuilder sb = new StringBuilder();
        try {
            Process p = new ProcessBuilder("su", "-c", cmd).redirectErrorStream(true).start();
            try (InputStream in = p.getInputStream()) {
                byte[] buf = new byte[4096];
                int n;
                while ((n = in.read(buf)) > 0) sb.append(new String(buf, 0, n));
            }
            p.waitFor();
        } catch (Exception ignored) {
        }
        return sb.toString();
    }

    private Notification buildNotification() {
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.O) {
            NotificationChannel ch = new NotificationChannel(
                    CHANNEL_ID, "输入代理", NotificationManager.IMPORTANCE_LOW);
            NotificationManager nm = getSystemService(NotificationManager.class);
            if (nm != null) nm.createNotificationChannel(ch);
        }
        Notification.Builder b = (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.O)
                ? new Notification.Builder(this, CHANNEL_ID)
                : new Notification.Builder(this);
        return b.setContentTitle("输入代理运行中")
                .setContentText("触控板与键盘已接管，停止后恢复原生")
                .setSmallIcon(android.R.drawable.ic_menu_manage)
                .setOngoing(true)
                .build();
    }
}
