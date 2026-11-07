package dev.pgrazor;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.graphics.Typeface;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;

/**
 * PGRazor Root run log, shown as an in-game dialog on the game's own activity. The patched APK uses
 * {@link LogActivity}, a private activity it declares in its manifest; the untouched game APK has no
 * such entry, so the same run log is surfaced here with {@link MapTheme} styling instead.
 *
 * <p>Viewing mirrors {@link LogActivity}: it shows the last 64&nbsp;KiB of {@link Control#currentLog()}
 * and offers a Refresh. Export cannot use the Storage Access Framework (there is no activity-result
 * callback for a dialog hosted by the game), so a verified full copy is written to the game's
 * app-visible external files directory through {@link LogExport#copyToExternal}, with the saved path
 * reported back; this needs no new storage permission.
 */
final class RootedLogDialog {
    private static final long MAX_TAIL = 65536;

    private RootedLogDialog() {}

    static void show(Activity activity) {
        if (activity == null || activity.isFinishing() || activity.isDestroyed()) return;
        Context ui = MapTheme.wrap(activity);
        int pad = MapTheme.dp(activity, 16);

        LinearLayout body = new LinearLayout(ui);
        body.setOrientation(LinearLayout.VERTICAL);
        body.setPadding(pad, pad, pad, pad);

        TextView hint = new TextView(ui);
        hint.setTextSize(13);
        body.addView(hint);

        ScrollView scroll = new ScrollView(ui);
        TextView text = new TextView(ui);
        text.setTextIsSelectable(true);
        text.setTypeface(Typeface.MONOSPACE);
        text.setTextSize(12);
        text.setPadding(0, pad / 2, 0, pad / 2);
        scroll.addView(text);
        body.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1));

        LinearLayout actions = new LinearLayout(ui);
        actions.setOrientation(LinearLayout.HORIZONTAL);
        body.addView(actions);
        Button refresh = new Button(ui);
        refresh.setText("Refresh");
        actions.addView(refresh, new LinearLayout.LayoutParams(0, -2, 1));
        Button save = new Button(ui);
        save.setText("Save copy");
        actions.addView(save, new LinearLayout.LayoutParams(0, -2, 1));

        Runnable reload = () -> load(hint, text);
        refresh.setOnClickListener(v -> reload.run());

        AlertDialog dialog = new AlertDialog.Builder(ui)
                .setTitle("PGRazor · current run")
                .setView(body)
                .setPositiveButton("Close", null)
                .create();

        // Export off the main thread; the copy re-reads and digests the whole file.
        Context app = activity.getApplicationContext();
        Handler handler = new Handler(Looper.getMainLooper());
        save.setOnClickListener(v -> {
            File file = Control.currentLog();
            if (file == null) { Toast.makeText(activity, "No log file to save", Toast.LENGTH_SHORT).show(); return; }
            save.setEnabled(false);
            new Thread(() -> {
                try {
                    File saved = LogExport.copyToExternal(app, file);
                    handler.post(() -> {
                        if (dialog.isShowing()) save.setEnabled(true);
                        Toast.makeText(app, "Saved to " + saved.getAbsolutePath(), Toast.LENGTH_LONG).show();
                    });
                } catch (IOException error) {
                    handler.post(() -> {
                        if (dialog.isShowing()) save.setEnabled(true);
                        Toast.makeText(app, "Save failed: " + error.getMessage(), Toast.LENGTH_LONG).show();
                    });
                }
            }, "pgrazor-log-export").start();
        });

        dialog.setOnShowListener(d -> {
            MapTheme.fullScreen(dialog);
            MapTheme.style(body);
            MapTheme.styleButtons(dialog);
            reload.run();
        });
        DialogLifecycle.bind(activity, dialog, null);
        dialog.show();
    }

    private static void load(TextView hint, TextView text) {
        File file = Control.currentLog();
        if (file == null) {
            hint.setText("");
            text.setText("No log file is available yet. Android logcat may contain startup errors.");
            return;
        }
        try (RandomAccessFile input = new RandomAccessFile(file, "r")) {
            long start = Math.max(0, input.length() - MAX_TAIL);
            input.seek(start);
            byte[] bytes = new byte[(int) (input.length() - start)];
            input.readFully(bytes);
            hint.setText(start > 0
                    ? "Showing the latest 64 KiB. Save copy writes the complete run to this device."
                    : "Save copy writes the complete run to this device.");
            text.setText(new String(bytes, StandardCharsets.UTF_8));
        } catch (IOException error) {
            hint.setText("");
            text.setText("Cannot read log: " + error.getMessage());
        }
    }
}
