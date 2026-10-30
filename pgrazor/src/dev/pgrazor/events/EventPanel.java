package dev.pgrazor.events;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Application;
import android.content.Context;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.ContextThemeWrapper;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;

/** Event selection stays independent of gameplay policy and native object classification. */
public final class EventPanel {
    private EventPanel() {}

    public static void showSelector(Activity activity, boolean dark) {
        if (activity.isFinishing() || activity.isDestroyed()) return;
        AlertDialog dialog = new AlertDialog.Builder(theme(activity, dark))
            .setTitle("Events")
            .setItems(new String[]{"Halloween 2026"}, (selected, index) -> showHalloween(activity, dark))
            .setNegativeButton("Close", null).create();
        bind(activity, dialog, () -> {});
        dialog.show();
    }

    private static Context theme(Activity activity, boolean dark) {
        return new ContextThemeWrapper(activity, dark ? android.R.style.Theme_Material_Dialog_Alert
            : android.R.style.Theme_Material_Light_Dialog_Alert);
    }

    private static void showHalloween(Activity activity, boolean dark) {
        if (activity.isFinishing() || activity.isDestroyed()) return;
        Context context = theme(activity, dark);
        LinearLayout content = new LinearLayout(context);
        content.setOrientation(LinearLayout.VERTICAL);
        int pad = Math.round(20 * context.getResources().getDisplayMetrics().density);
        content.setPadding(pad, pad / 2, pad, pad / 2);
        TextView description = new TextView(context);
        description.setText("Opens every visible map box and skips the encounter screen for any Pokemon result.");
        description.setTextSize(15);
        content.addView(description);
        TextView status = new TextView(context);
        status.setPadding(0, pad / 2, 0, pad / 2);
        status.setTextSize(13);
        content.addView(status);
        Switch collector = new Switch(context);
        collector.setText("Open boxes and skip encounters");
        collector.setTextSize(15);
        content.addView(collector);

        ScrollView scroller = new ScrollView(context);
        scroller.addView(content);

        AlertDialog dialog = new AlertDialog.Builder(context).setTitle("Halloween 2026")
            .setView(scroller).setNegativeButton("Close", null).create();
        Handler handler = new Handler(Looper.getMainLooper());
        Runnable refresh = new Runnable() {
            public void run() {
                if (!dialog.isShowing()) return;
                boolean available = EventSettings.boxCollectorAvailable();
                collector.setEnabled(available);
                collector.setChecked(EventSettings.boxCollectorEnabled(activity));
                status.setText(available
                    ? "Ready to open visible boxes. Encounter screens are skipped without catching."
                    : "Unavailable: the box collector has not been verified. Collection is off.");
                handler.postDelayed(this, 500);
            }
        };
        collector.setOnCheckedChangeListener((button, checked) -> {
            if (checked == EventSettings.boxCollectorEnabled(activity)) return;
            EventSettings.setBoxCollectorRequested(activity, checked);
            button.setChecked(EventSettings.boxCollectorEnabled(activity));
        });
        bind(activity, dialog, () -> handler.removeCallbacks(refresh));
        dialog.setOnShowListener(ignored -> refresh.run());
        dialog.show();
    }

    private static void bind(Activity owner, AlertDialog dialog, Runnable cleanup) {
        Application application = owner.getApplication();
        Application.ActivityLifecycleCallbacks lifecycle = new Application.ActivityLifecycleCallbacks() {
            public void onActivityPaused(Activity activity) { if (activity == owner) dialog.dismiss(); }
            public void onActivityDestroyed(Activity activity) { if (activity == owner) dialog.dismiss(); }
            public void onActivityCreated(Activity activity, Bundle state) {}
            public void onActivityStarted(Activity activity) {}
            public void onActivityResumed(Activity activity) {}
            public void onActivityStopped(Activity activity) {}
            public void onActivitySaveInstanceState(Activity activity, Bundle state) {}
        };
        application.registerActivityLifecycleCallbacks(lifecycle);
        dialog.setOnDismissListener(ignored -> {
            cleanup.run();
            application.unregisterActivityLifecycleCallbacks(lifecycle);
        });
    }
}
