package dev.pgrazor;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.graphics.drawable.ColorDrawable;
import android.os.Handler;
import android.os.Looper;
import android.widget.Button;
import android.widget.Switch;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

/**
 * One-shot "Send &amp; Open Gifts" dialog. Drives the coordinator-owned {@link GiftBatchSettings}
 * runtime: a single pass sends available gifts and then opens received gifts, with the native
 * coordinator handling each eligible friend once. Chrome follows {@link MapTheme}; polling and the
 * dialog stay scoped through {@link DialogLifecycle}, refreshing only the visible dialog once a second.
 */
public final class GiftBatchDialog {
    private GiftBatchDialog() {}

    public static void show(Activity activity) {
        if (activity == null || activity.isFinishing() || activity.isDestroyed()) return;
        Context ui = MapTheme.wrap(activity);
        LinearLayout body = new LinearLayout(ui); body.setOrientation(LinearLayout.VERTICAL);
        int pad = MapTheme.dp(ui, 16); body.setPadding(pad, pad, pad, pad);

        TextView description = new TextView(ui);
        description.setText("Sends your available gifts to eligible friends, then opens the gifts you have received. "
            + "Each friend is handled once. The game's own daily limits for sending and opening still apply, so some "
            + "may be skipped. A single pass runs now — there is no recurring schedule.");
        body.addView(description);

        Switch fullBag = new Switch(ui);
        fullBag.setText("Open even when item bag is full");
        fullBag.setChecked(GiftBatchSettings.fullBagDefault());
        body.addView(fullBag);
        TextView fullBagCaption = new TextView(ui);
        fullBagCaption.setText("Uses the game's full-bag reward setting. Full-bag opening may award fewer rewards.");
        body.addView(fullBagCaption);

        TextView status = new TextView(ui); status.setText(GiftBatchSettings.status()); body.addView(status);
        Button start = SettingsUi.button(ui, "Start"); SettingsUi.styleTree(ui, start); body.addView(start);
        Button stop = SettingsUi.button(ui, "Stop"); SettingsUi.styleTree(ui, stop); body.addView(stop);

        ScrollView scroll = new ScrollView(ui); scroll.addView(body);
        AlertDialog dialog = new AlertDialog.Builder(ui).setTitle("Send & Open Gifts").setView(scroll)
            .setPositiveButton("Close", null).create();
        Handler handler = new Handler(Looper.getMainLooper());
        Runnable update = () -> {
            boolean busy = GiftBatchSettings.running();
            SettingsUi.enabled(fullBag, !busy);
            start.setEnabled(!busy);
            stop.setEnabled(busy && !GiftBatchSettings.stopPending());
            status.setText(GiftBatchSettings.status());
        };
        Runnable refresh = new Runnable() {
            public void run() { if (dialog.isShowing()) { update.run(); handler.postDelayed(this, 1000); } }
        };
        start.setOnClickListener(v -> {
            try { GiftBatchSettings.start(fullBag.isChecked()); }
            catch (IllegalArgumentException | IllegalStateException refused) { toast(activity, refused.getMessage()); }
            update.run();
        });
        stop.setOnClickListener(v -> { GiftBatchSettings.stop(); update.run(); });
        DialogLifecycle.bind(activity, dialog, () -> handler.removeCallbacks(refresh));
        dialog.setOnShowListener(d -> {
            MapTheme.style(body); MapTheme.styleButtons(dialog);
            dialog.getWindow().setBackgroundDrawable(new ColorDrawable(MapTheme.surface()));
            handler.post(refresh);
        });
        dialog.show();
    }

    private static void toast(Activity activity, String message) {
        Toast.makeText(activity, message == null || message.isEmpty() ? "Could not start." : message,
            Toast.LENGTH_SHORT).show();
    }
}
