package dev.pgrazor;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Typeface;
import android.text.TextUtils;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * Shundo Sniper's on-map card: what it is doing now, how many teleports and checks it has made, and a
 * Start/Stop button. Pad owns its OverlayDrag, visibility and lifecycle and calls {@link #refresh} on each
 * heartbeat. It appears once the sniper has been started; the close button stops the sniper and hides it.
 */
public final class ShundoOverlay extends LinearLayout {
    private final Activity activity;
    private final TextView badge, step, stats, close;
    private final Button toggle;
    private Boolean lastRunning;  // null until first painted while shown
    private String lastStep = "", lastStats = "";

    public ShundoOverlay(Activity activity) {
        super(activity);
        this.activity = activity;
        setOrientation(VERTICAL);
        int pad = SettingsUi.dp(activity, 10);
        setPadding(pad, SettingsUi.dp(activity, 8), pad, SettingsUi.dp(activity, 8));
        setElevation(SettingsUi.dp(activity, 6));

        LinearLayout top = new LinearLayout(activity);
        top.setOrientation(HORIZONTAL);
        top.setGravity(Gravity.CENTER_VERTICAL);
        TextView title = new TextView(activity);
        title.setText("Shundo Sniper");
        title.setTextSize(13);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        top.addView(title, new LayoutParams(0, LayoutParams.WRAP_CONTENT, 1f));
        badge = new TextView(activity);
        badge.setTextSize(10);
        badge.setTypeface(Typeface.DEFAULT_BOLD);
        badge.setPadding(SettingsUi.dp(activity, 7), SettingsUi.dp(activity, 2), SettingsUi.dp(activity, 7), SettingsUi.dp(activity, 2));
        LayoutParams badgeLayout = new LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT);
        badgeLayout.rightMargin = SettingsUi.dp(activity, 6);
        top.addView(badge, badgeLayout);
        toggle = new Button(activity);
        toggle.setAllCaps(false);
        toggle.setTextSize(12);
        toggle.setMinWidth(0); toggle.setMinimumWidth(0); toggle.setMinHeight(0); toggle.setMinimumHeight(0);
        toggle.setPadding(SettingsUi.dp(activity, 12), SettingsUi.dp(activity, 4), SettingsUi.dp(activity, 12), SettingsUi.dp(activity, 4));
        MapTheme.setButtonBackground(toggle);
        toggle.setOnClickListener(v -> { ShundoSniper.toggle(activity); refresh(true); });
        top.addView(toggle, new LayoutParams(LayoutParams.WRAP_CONTENT, SettingsUi.dp(activity, 30)));
        close = new TextView(activity);
        close.setText("×");
        close.setTextSize(18);
        close.setGravity(Gravity.CENTER);
        close.setContentDescription("Stop Shundo Sniper and hide this card");
        close.setOnClickListener(v -> { ShundoSniper.closeOverlay(activity); refresh(true); });
        LayoutParams closeLayout = new LayoutParams(SettingsUi.dp(activity, 28), SettingsUi.dp(activity, 30));
        closeLayout.leftMargin = SettingsUi.dp(activity, 2);
        top.addView(close, closeLayout);
        addView(top);

        step = new TextView(activity);
        step.setTextSize(12);
        step.setMaxLines(2);
        step.setEllipsize(TextUtils.TruncateAt.END);
        LayoutParams stepLayout = new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT);
        stepLayout.topMargin = SettingsUi.dp(activity, 5);
        addView(step, stepLayout);
        stats = new TextView(activity);
        stats.setTextSize(11);
        stats.setMaxLines(1);
        stats.setEllipsize(TextUtils.TruncateAt.END);
        LayoutParams statsLayout = new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT);
        statsLayout.topMargin = SettingsUi.dp(activity, 3);
        addView(stats, statsLayout);
        refresh(true);
    }

    /** Pad calls this on each heartbeat; hidden beneath the settings panel and the quick menu. */
    public void refresh(boolean visible) {
        setVisibility(visible && ShundoSniper.overlayShown() ? VISIBLE : GONE);
        if (getVisibility() != VISIBLE) return;
        boolean running = ShundoSniper.active();
        String nowStep = ShundoSniper.overlayStep(), nowStats = ShundoSniper.overlayStats();
        setBackground(SettingsUi.rounded(activity, MapTheme.surface(), SettingsUi.dp(activity, 14)));
        if (lastRunning == null || running != lastRunning) {
            lastRunning = running;
            badge.setText(running ? "RUNNING" : "STOPPED");
            badge.setTextColor(running ? Color.WHITE : MapTheme.muted());
            badge.setBackground(SettingsUi.rounded(activity, running ? Color.rgb(56, 181, 112) : 0x22808080, SettingsUi.dp(activity, 9)));
            toggle.setText(running ? "Stop" : "Start");
            toggle.setTextColor(running ? MapTheme.ink() : MapTheme.accent());
            setContentDescription("Shundo Sniper " + (running ? "running" : "stopped") + ". Drag to reposition.");
        }
        for (TextView t : new TextView[]{(TextView) ((LinearLayout) getChildAt(0)).getChildAt(0), step}) t.setTextColor(MapTheme.ink());
        stats.setTextColor(MapTheme.muted());
        close.setTextColor(MapTheme.muted());
        if (!nowStep.equals(lastStep)) { lastStep = nowStep; step.setText(nowStep); }
        if (!nowStats.equals(lastStats)) { lastStats = nowStats; stats.setText(nowStats); }
    }

    public void close() { toggle.setOnClickListener(null); close.setOnClickListener(null); setVisibility(GONE); }
}
