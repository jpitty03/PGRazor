package dev.pgrazor;

import android.app.Activity;
import android.graphics.Typeface;
import android.os.SystemClock;
import android.text.format.DateFormat;
import android.view.Gravity;
import android.widget.TextView;
import java.util.Locale;

/** Small local clock driven by Pad's existing lifecycle/tick, with no background timer. */
final class LocalTimeOverlay extends TextView {
    private final LocalTimeClock clock = new LocalTimeClock();
    private long lastRefresh;
    private boolean closed;

    LocalTimeOverlay(Activity activity) {
        super(activity);
        setTextSize(13); setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        setGravity(Gravity.CENTER); setSingleLine(true);
        int padding = SettingsUi.dp(activity, 10);
        setPadding(padding, 0, padding, 0);
        setMinWidth(SettingsUi.dp(activity, 100));
        setVisibility(GONE);
        applyTheme();
    }

    void applyTheme() {
        setBackground(SettingsUi.rounded(getContext(), Control.darkTheme() ? 0xcc141b25 : 0xccf1f5f8, 10));
        setTextColor(SettingsUi.ink());
    }

    void refresh(boolean allowed) {
        boolean visible = !closed && allowed && Control.localTimeOverlay() && Control.desiredEnabled();
        boolean showing = getVisibility() == VISIBLE;
        if (visible != showing) setVisibility(visible ? VISIBLE : GONE);
        if (!visible || !isShown() || getWindowVisibility() != VISIBLE) return;
        long elapsed = SystemClock.elapsedRealtime();
        if (showing && elapsed - lastRefresh < 1000) return;
        lastRefresh = elapsed;
        String label = clock.label(Control.engine.latitude, Control.engine.longitude,
                System.currentTimeMillis(), DateFormat.is24HourFormat(getContext()), Locale.getDefault());
        if (!label.contentEquals(getText())) {
            setText(label);
            setContentDescription("Local time at spoofed location: " + label + ". Drag to move.");
        }
    }

    void close() { closed = true; setVisibility(GONE); }
}
