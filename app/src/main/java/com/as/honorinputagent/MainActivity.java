package com.as.honorinputagent;

import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.app.AppCompatActivity;

import com.google.android.material.materialswitch.MaterialSwitch;
import com.google.android.material.slider.Slider;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Locale;

/**
 * Front-end for the two input relays, each switched on its own.
 *
 * The touchpad is reported as a DIRECT touchscreen and the Win key is swallowed
 * by the OEM framework, so pointer capture fails and Windows shortcuts never
 * reach the host. The relays grab the devices and re-emit them as ordinary ones;
 * stopping a relay releases its device and restores stock behaviour at once.
 */
public class MainActivity extends AppCompatActivity {

    public static final String PREF = "relay";
    public static final String KEY_SENS = "sens";
    public static final String KEY_SCROLL = "scroll";
    public static final String KEY_INVERT = "invert";

    private TextView sensValue;
    private TextView scrollValue;
    private TextView valueTouch;
    private TextView valueKbd;
    private View dotTouch;
    private View dotKbd;
    private MaterialSwitch switchTouch;
    private MaterialSwitch switchKbd;
    private MaterialSwitch invertSwitch;
    private Slider sensSlider;
    private Slider scrollSlider;
    private SharedPreferences prefs;
    private boolean binding;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable poller = new Runnable() {
        @Override
        public void run() {
            refreshStatus();
            handler.postDelayed(this, 1500);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        prefs = getSharedPreferences(PREF, MODE_PRIVATE);
        sensValue = findViewById(R.id.sens_value);
        scrollValue = findViewById(R.id.scroll_value);
        valueTouch = findViewById(R.id.value_touch);
        valueKbd = findViewById(R.id.value_kbd);
        dotTouch = findViewById(R.id.dot_touch);
        dotKbd = findViewById(R.id.dot_kbd);
        switchTouch = findViewById(R.id.switch_touch);
        switchKbd = findViewById(R.id.switch_kbd);
        invertSwitch = findViewById(R.id.invert);
        sensSlider = findViewById(R.id.sens);
        scrollSlider = findViewById(R.id.scroll);

        binding = true;
        sensSlider.setValue(getSens());
        scrollSlider.setValue(getScroll());
        invertSwitch.setChecked(prefs.getBoolean(KEY_INVERT, false));
        binding = false;
        updateLabels();

        sensSlider.addOnChangeListener((slider, value, fromUser) -> {
            if (binding) return;
            prefs.edit().putFloat(KEY_SENS, value).apply();
            updateLabels();
            restartIfRunning();
        });
        scrollSlider.addOnChangeListener((slider, value, fromUser) -> {
            if (binding) return;
            prefs.edit().putInt(KEY_SCROLL, Math.round(value)).apply();
            updateLabels();
            restartIfRunning();
        });
        invertSwitch.setOnCheckedChangeListener((b, checked) -> {
            if (binding) return;
            prefs.edit().putBoolean(KEY_INVERT, checked).apply();
            restartIfRunning();
        });

        switchTouch.setOnCheckedChangeListener((b, checked) -> {
            if (binding) return;
            send(checked, RelayService.TARGET_TOUCH);
        });
        switchKbd.setOnCheckedChangeListener((b, checked) -> {
            if (binding) return;
            send(checked, RelayService.TARGET_KBD);
        });
    }

    private void send(boolean start, String target) {
        Intent i = new Intent(this, RelayService.class);
        i.setAction(start ? "start" : "stop");
        i.putExtra(RelayService.EXTRA_TARGET, target);
        i.putExtra(RelayService.EXTRA_SENS, getSens());
        i.putExtra(RelayService.EXTRA_SCROLL, getScroll());
        i.putExtra(RelayService.EXTRA_INVERT, prefs.getBoolean(KEY_INVERT, false));
        startService(i);
        handler.postDelayed(this::refreshStatus, 900);
    }

    /** Restarts the touchpad relay so a new sensitivity takes effect. */
    private void restartIfRunning() {
        if (switchTouch.isChecked()) {
            send(false, RelayService.TARGET_TOUCH);
            handler.postDelayed(() -> send(true, RelayService.TARGET_TOUCH), 500);
        }
    }

    private float getSens() {
        return prefs.getFloat(KEY_SENS, 1.8f);
    }

    private int getScroll() {
        return prefs.getInt(KEY_SCROLL, 5);
    }

    private void updateLabels() {
        sensValue.setText(String.format(Locale.US, "%.1f×", getSens()));
        scrollValue.setText(String.format(Locale.US, "%d×", getScroll()));
    }

    @Override
    protected void onResume() {
        super.onResume();
        installAssets();
        handler.post(poller);
    }

    @Override
    protected void onPause() {
        super.onPause();
        handler.removeCallbacks(poller);
    }

    private void installAssets() {
        try {
            File dir = getFilesDir();
            File tp = new File(dir, "touchpad_relay");
            File kbd = new File(dir, "kbd_relay");
            copyAsset("touchpad_relay", tp);
            copyAsset("kbd_relay", kbd);
            su("cp -f '" + tp.getAbsolutePath() + "' /data/local/tmp/touchpad_relay; "
                    + "cp -f '" + kbd.getAbsolutePath() + "' /data/local/tmp/kbd_relay; "
                    + "chmod 755 /data/local/tmp/touchpad_relay /data/local/tmp/kbd_relay");
        } catch (Exception e) {
            toast("安装失败: " + e.getMessage());
        }
    }

    private void copyAsset(String name, File dst) throws Exception {
        try (InputStream in = getAssets().open(name);
             OutputStream out = new FileOutputStream(dst)) {
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        }
        dst.setExecutable(true, false);
    }

    private void refreshStatus() {
        new Thread(() -> {
            boolean touch = alive("/data/local/tmp/touchpad_relay.pid");
            boolean kbd = alive("/data/local/tmp/kbd_relay.pid");
            handler.post(() -> {
                binding = true;
                switchTouch.setChecked(touch);
                switchKbd.setChecked(kbd);
                binding = false;
                valueTouch.setText(touch ? R.string.running : R.string.stopped);
                valueKbd.setText(kbd ? R.string.running : R.string.stopped);
                valueTouch.setTextColor(getColor(touch ? R.color.ok : R.color.text_secondary));
                valueKbd.setTextColor(getColor(kbd ? R.color.ok : R.color.text_secondary));
                dotTouch.setBackgroundResource(touch ? R.drawable.dot_on : R.drawable.dot_off);
                dotKbd.setBackgroundResource(kbd ? R.drawable.dot_on : R.drawable.dot_off);
            });
        }).start();
    }

    private boolean alive(String pidFile) {
        return su("[ -f " + pidFile + " ] && cat " + pidFile
                + " | while read p; do [ -d /proc/$p ] && echo alive; done; true")
                .contains("alive");
    }

    private String su(String cmd) {
        StringBuilder sb = new StringBuilder();
        try {
            Process p = new ProcessBuilder("su", "-c", cmd)
                    .redirectErrorStream(true).start();
            try (InputStream in = p.getInputStream()) {
                byte[] buf = new byte[4096];
                int n;
                while ((n = in.read(buf)) > 0) sb.append(new String(buf, 0, n));
            }
            p.waitFor();
        } catch (Exception e) {
            sb.append("error: ").append(e.getMessage());
        }
        return sb.toString();
    }

    private void toast(String msg) {
        handler.post(() -> Toast.makeText(this, msg, Toast.LENGTH_SHORT).show());
    }
}
