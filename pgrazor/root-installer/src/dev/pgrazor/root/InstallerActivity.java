package dev.pgrazor.root;

import android.app.Activity;
import android.app.AlertDialog;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * The only screen of the PGRazor Root installer. It shows the device's root/Zygisk/module/game
 * status honestly, installs the bundled Zygisk module through the detected root manager on an
 * explicit tap, offers a launch-time on/off switch and an always-available reboot button. Detection
 * is read-only; install, switching and reboot require an explicit action. All root work runs off
 * the main thread.
 */
public final class InstallerActivity extends Activity {
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final SimpleDateFormat clock = new SimpleDateFormat("HH:mm:ss", Locale.US);

    private TextView statusView;
    private TextView logView;
    private ScrollView logScroll;
    private Button checkButton;
    private Button installButton;
    private Button rebootButton;
    private Button toggleButton;
    private boolean pendingLogged;

    private volatile boolean busy;
    private RootStatus status;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        LinearLayout screen = new LinearLayout(this);
        screen.setOrientation(LinearLayout.VERTICAL);
        screen.setPadding(32, 56, 32, 32);
        screen.setBackgroundColor(Color.rgb(9, 17, 29));

        TextView heading = new TextView(this);
        heading.setText("PGRazor Root installer");
        heading.setTextSize(24);
        heading.setTextColor(Color.WHITE);
        screen.addView(heading);

        TextView subtitle = new TextView(this);
        subtitle.setText("Installs PGRazor into the official Pokémon GO " + InstallerConfig.GAME_VERSION_NAME
            + " as a Zygisk module. The game stays unmodified; changes require your confirmation.");
        subtitle.setTextSize(14);
        subtitle.setTextColor(Color.LTGRAY);
        subtitle.setPadding(0, 8, 0, 16);
        screen.addView(subtitle);

        statusView = new TextView(this);
        statusView.setTextSize(15);
        statusView.setTextColor(Color.WHITE);
        statusView.setText("Checking device…");
        screen.addView(statusView);

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.setPadding(0, 16, 0, 8);
        checkButton = addButton(buttons, "Re-check", v -> refresh());
        installButton = addButton(buttons, "Install module", v -> confirmInstall());
        installButton.setEnabled(false);
        screen.addView(buttons);
        LinearLayout actions = new LinearLayout(this);
        actions.setOrientation(LinearLayout.HORIZONTAL);
        toggleButton = addButton(actions, "PGRazor: On / Off", v -> confirmToggle());
        rebootButton = addButton(actions, "Reboot", v -> confirmReboot());
        toggleButton.setEnabled(false);
        rebootButton.setEnabled(false);
        screen.addView(actions);

        TextView logHeading = new TextView(this);
        logHeading.setText("Log");
        logHeading.setTextSize(16);
        logHeading.setTextColor(Color.WHITE);
        logHeading.setPadding(0, 16, 0, 4);
        screen.addView(logHeading);

        logScroll = new ScrollView(this);
        logView = new TextView(this);
        logView.setTextSize(12);
        logView.setTextColor(Color.rgb(200, 220, 200));
        logView.setTextIsSelectable(true);
        logScroll.addView(logView);
        screen.addView(logScroll, new LinearLayout.LayoutParams(-1, 0, 1));

        setContentView(screen);
        refresh();
    }

    private Button addButton(LinearLayout row, String label, View.OnClickListener onClick) {
        Button button = new Button(this);
        button.setText(label);
        button.setOnClickListener(onClick);
        row.addView(button, new LinearLayout.LayoutParams(0, -2, 1));
        return button;
    }

    /** Read-only status refresh, run off the main thread (the root probe blocks). */
    private void refresh() {
        if (busy) return;
        setBusy(true);
        statusView.setText("Checking device…");
        log("Checking root, Zygisk, module and game status…");
        new Thread(() -> {
            RootStatus detected = RootStatus.detect(this);
            handler.post(() -> {
                status = detected;
                statusView.setText(detected.describe());
                if (!detected.rawProbe.isEmpty()) log("Probe:\n" + detected.rawProbe);
                toggleButton.setText(detected.toggleSupported ? (detected.injectionOff ? "PGRazor: Off" : "PGRazor: On") : "PGRazor: On / Off");
                rebootButton.setText(detected.rebootNeeded ? "Reboot to activate" : "Reboot");
                if (detected.rebootNeeded && !pendingLogged) log("A module change is pending. Reboot to activate it.");
                pendingLogged = detected.rebootNeeded;
                if (!detected.rooted) {
                    log("No root access. Grant root to this app in your root manager, then Re-check.");
                } else if (!detected.managerSupported) {
                    log("No supported root manager (needs Magisk 27+, KernelSU or APatch).");
                } else {
                    if (!"Enabled".equals(detected.zygiskLabel) && !detected.zygiskLabel.startsWith("Enabled (") && !detected.zygiskLabel.startsWith("Configured (")) {
                        log("Zygisk is required. PGRazor will not load until Zygisk is active: " + detected.zygiskLabel);
                    }
                    if (detected.gameInstalled && !detected.gameCertTrusted) {
                        log("The installed Pokémon GO is not Niantic-signed. Use the official app; PGRazor stays off otherwise.");
                    } else if (detected.gameInstalled && !detected.gameVersionMatch) {
                        log("Installed game is not " + InstallerConfig.GAME_VERSION_NAME
                            + ". The module installs, but PGRazor stays off until the versions match.");
                    } else if (!detected.gameInstalled) {
                        log("Pokémon GO is not installed. The module installs, but there is nothing for it to load into yet.");
                    }
                }
                setBusy(false);
            });
        }, "pgrazor-root-detect").start();
    }

    private void confirmInstall() {
        if (busy || status == null || !status.canInstall()) return;
        new AlertDialog.Builder(this)
            .setTitle("Install PGRazor Root module?")
            .setMessage("This installs the Zygisk module with " + status.managerLabel
                + ". The module only loads into the official Pokémon GO and takes effect after a reboot.")
            .setPositiveButton("Install", (dialog, which) -> install())
            .setNegativeButton("Cancel", null)
            .show();
    }

    private void install() {
        if (busy || status == null || !status.canInstall()) return;
        setBusy(true);
        log("Installing module with " + status.managerLabel + "…");
        final RootStatus target = status;
        new Thread(() -> {
            RootShell.Result result;
            File zip;
            try {
                zip = extractModule();
            } catch (IOException copyFailed) {
                handler.post(() -> {
                    log("Could not stage the module: " + copyFailed.getMessage());
                    setBusy(false);
                });
                return;
            }
            result = target.install(zip);
            final RootShell.Result outcome = result;
            handler.post(() -> {
                if (!outcome.output.isEmpty()) log(outcome.output);
                if (outcome.ok()) {
                    log("Module installed. Tap Reboot to activate it.");
                } else if (outcome.timedOut) {
                    log("Install result is unknown. The root manager may still be working; check it before attempting another install.");
                } else if (!outcome.started) {
                    log("Install could not run: " + outcome.output);
                } else {
                    log("Install failed (exit " + outcome.code + "). See the output above.");
                }
                setBusy(false);
                refresh();
            });
        }, "pgrazor-root-install").start();
    }

    private void confirmToggle() {
        if (busy || status == null || !status.canToggle()) return;
        final boolean on = status.injectionOff;
        new AlertDialog.Builder(this)
            .setTitle(on ? "Turn PGRazor on?" : "Turn PGRazor off?")
            .setMessage("Pok\u00e9mon GO will be closed. PGRazor will be " + (on ? "on" : "off")
                + " the next time you open it. No reboot needed.")
            .setPositiveButton(on ? "Turn on" : "Turn off", (dialog, which) -> setInjection(on))
            .setNegativeButton("Cancel", null).show();
    }

    private void setInjection(boolean on) {
        if (busy || status == null || !status.canToggle()) return;
        final RootStatus target = status;
        setBusy(true);
        new Thread(() -> {
            RootShell.Result result = target.setInjection(on);
            handler.post(() -> {
                if (!result.output.isEmpty()) log(result.output);
                if (result.ok()) log("PGRazor is " + (on ? "on" : "off (module stays installed)")
                    + ". Open Pok\u00e9mon GO when ready.");
                else log(result.timedOut ? "Switch result is unknown; re-checking the module state."
                    : "Could not confirm the change. See the output above.");
                setBusy(false);
                refresh();
            });
        }, "pgrazor-root-toggle").start();
    }

    private void confirmReboot() {
        if (busy || status == null || !status.rooted) return;
        new AlertDialog.Builder(this)
            .setTitle("Reboot now?")
            .setMessage(status.rebootNeeded ? "Zygisk reads its module list only at boot. Reboot to activate the pending PGRazor Root change." : "Reboot this device now? Pok\u00e9mon GO and other apps will close.")
            .setPositiveButton("Reboot", (dialog, which) -> {
                if (busy || status == null || !status.rooted) return;
                setBusy(true);
                log("Rebooting…");
                new Thread(() -> {
                    RootShell.Result result = RootStatus.reboot();
                    handler.post(() -> {
                        if (!result.ok() && !result.output.isEmpty()) log(result.output);
                        setBusy(false);
                    });
                }, "pgrazor-root-reboot").start();
            })
            .setNegativeButton("Later", null)
            .show();
    }

    /** Copies the bundled module zip into app storage so the root process can read it by path. */
    private File extractModule() throws IOException {
        File out = new File(getFilesDir(), InstallerConfig.MODULE_ASSET);
        File temp = new File(out.getPath() + ".tmp");
        try (InputStream in = getAssets().open(InstallerConfig.MODULE_ASSET);
             FileOutputStream os = new FileOutputStream(temp)) {
            byte[] buffer = new byte[65536];
            int n;
            while ((n = in.read(buffer)) != -1) os.write(buffer, 0, n);
            os.getFD().sync();
        }
        if (out.exists() && !out.delete()) throw new IOException("Cannot replace " + out);
        if (!temp.renameTo(out)) throw new IOException("Cannot stage " + out);
        return out;
    }

    private void setBusy(boolean value) {
        busy = value;
        checkButton.setEnabled(!value);
        installButton.setEnabled(!value && status != null && status.canInstall());
        toggleButton.setEnabled(!value && status != null && status.canToggle());
        rebootButton.setEnabled(!value && status != null && status.rooted);
    }

    private void log(String message) {
        String line = "[" + clock.format(new Date()) + "] " + message + "\n";
        logView.append(line);
        logScroll.post(() -> logScroll.fullScroll(View.FOCUS_DOWN));
    }
}
