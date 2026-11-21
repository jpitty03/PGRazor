package dev.pgrazor;

import android.animation.Animator;
import android.animation.AnimatorListenerAdapter;
import android.animation.ValueAnimator;
import android.app.Activity;
import android.os.SystemClock;
import android.text.format.DateFormat;
import android.view.GestureDetector;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.animation.DecelerateInterpolator;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.util.Date;
import java.util.Locale;

/**
 * The Session stats card under the coordinates on the Settings home page. Collapsed it shows the last
 * hour's XP and catches; tapped open it shows a card for the last hour and one for the last 24 hours,
 * switched by a swipe or a tap with a sliding transition.
 */
final class SessionStatsCard {
    private static final long[] WINDOWS = {SessionStatsHistory.HOUR_MS, SessionStatsHistory.DAY_MS};
    private static final String[] TITLES = {"Last hour", "Last 24 hours"};
    private static final int[] TILES = {SessionStatsHistory.XP, SessionStatsHistory.DUST, SessionStatsHistory.CAUGHT,
        SessionStatsHistory.SEEN, SessionStatsHistory.STOPS, SessionStatsHistory.KM, SessionStatsHistory.HATCHED};
    private static final String[] LABELS = {"XP", "stardust gained", "caught", "seen", "PokéStops", "walked", "hatched"};
    // Kept while the game runs, so the card opens the way it was left.
    private static boolean expanded;
    private static int page;

    private final Activity activity;
    private final LinearLayout root, body;
    private final TextView title, summary;
    private final View chevron;
    private final FrameLayout pager;
    private final Page[] pages = new Page[2];
    private final View[] dots = new View[2];
    private long refreshedAt;
    private boolean animating;

    private final class Page {
        final LinearLayout view;
        final TextView heading, note;
        final TextView[] values = new TextView[TILES.length], labels = new TextView[TILES.length];
        Page(String name) {
            view = new LinearLayout(activity); view.setOrientation(LinearLayout.VERTICAL);
            LinearLayout top = new LinearLayout(activity); top.setGravity(Gravity.CENTER_VERTICAL);
            heading = text(name.toUpperCase(Locale.US), 11, SettingsUi.accent()); heading.setTypeface(SettingsUi.medium());
            heading.setLetterSpacing(0.08f);
            top.addView(heading, new LinearLayout.LayoutParams(0, -2, 1f));
            note = text("", 11, SettingsUi.faint()); top.addView(note);
            view.addView(top);
            for (int row = 0; row < (TILES.length + 1) / 2; row++) {
                LinearLayout line = new LinearLayout(activity);
                LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2); lp.topMargin = dp(row == 0 ? 10 : 12);
                for (int col = 0; col < 2; col++) {
                    int i = row * 2 + col;
                    if (i >= TILES.length) { line.addView(new View(activity), new LinearLayout.LayoutParams(0, 0, 1f)); continue; }
                    LinearLayout tile = new LinearLayout(activity); tile.setOrientation(LinearLayout.VERTICAL);
                    values[i] = text("–", 20, SettingsUi.ink()); values[i].setTypeface(SettingsUi.medium()); values[i].setSingleLine(true);
                    labels[i] = text(LABELS[i], 12, SettingsUi.muted()); labels[i].setSingleLine(true);
                    tile.addView(values[i]); tile.addView(labels[i]);
                    line.addView(tile, new LinearLayout.LayoutParams(0, -2, 1f));
                }
                view.addView(line, lp);
            }
        }
    }

    SessionStatsCard(Activity activity) {
        this.activity = activity;
        root = new LinearLayout(activity); root.setOrientation(LinearLayout.VERTICAL);
        root.setBackground(SettingsUi.cardBackground(activity)); root.setTag(SettingsUi.TAG_CARD, Boolean.TRUE);
        root.setClipChildren(true);

        LinearLayout header = new LinearLayout(activity); header.setGravity(Gravity.CENTER_VERTICAL);
        header.setPadding(dp(14), dp(12), dp(12), dp(12));
        header.setBackground(SettingsUi.pressable(activity)); header.setClickable(true);
        title = text("Session stats", 14, SettingsUi.ink()); title.setTypeface(SettingsUi.medium());
        header.addView(title);
        summary = text("", 12, SettingsUi.muted()); summary.setSingleLine(true); summary.setGravity(Gravity.END);
        summary.setEllipsize(android.text.TextUtils.TruncateAt.END);
        LinearLayout.LayoutParams sp = new LinearLayout.LayoutParams(0, -2, 1f); sp.leftMargin = dp(10); sp.rightMargin = dp(8);
        header.addView(summary, sp);
        SettingsUi.Chevron down = new SettingsUi.Chevron(activity); down.open = true; chevron = down;
        header.addView(chevron, new LinearLayout.LayoutParams(dp(20), dp(20)));
        header.setOnClickListener(v -> toggle());
        root.addView(header);

        body = new LinearLayout(activity); body.setOrientation(LinearLayout.VERTICAL);
        body.setPadding(dp(14), 0, dp(14), dp(12));
        pager = new FrameLayout(activity); pager.setClipChildren(true);
        for (int i = 0; i < 2; i++) {
            pages[i] = new Page(TITLES[i]);
            pager.addView(pages[i].view, new FrameLayout.LayoutParams(-1, -2));
            pages[i].view.setVisibility(i == page ? View.VISIBLE : View.INVISIBLE);
        }
        body.addView(pager);
        LinearLayout dotRow = new LinearLayout(activity); dotRow.setGravity(Gravity.CENTER);
        for (int i = 0; i < 2; i++) {
            final int target = i;
            dots[i] = new View(activity);
            LinearLayout.LayoutParams dp = new LinearLayout.LayoutParams(dp(6), dp(6)); dp.leftMargin = dp(4); dp.rightMargin = dp(4);
            dotRow.addView(dots[i], dp);
            dots[i].setOnClickListener(v -> show(target));
        }
        LinearLayout.LayoutParams dr = new LinearLayout.LayoutParams(-1, dp(22)); dr.topMargin = dp(8);
        body.addView(dotRow, dr);
        GestureDetector swipe = new GestureDetector(activity, new GestureDetector.SimpleOnGestureListener() {
            @Override public boolean onDown(MotionEvent e) { return true; }
            @Override public boolean onSingleTapUp(MotionEvent e) { show(1 - page); return true; }
            @Override public boolean onFling(MotionEvent a, MotionEvent b, float vx, float vy) {
                if (Math.abs(vx) < Math.abs(vy) || Math.abs(vx) < dp(300)) return false;
                show(vx < 0 ? Math.min(1, page + 1) : Math.max(0, page - 1));
                return true;
            }
        });
        pager.setOnTouchListener((v, e) -> {
            if (e.getActionMasked() == MotionEvent.ACTION_DOWN) v.getParent().requestDisallowInterceptTouchEvent(true);
            if (e.getActionMasked() == MotionEvent.ACTION_UP || e.getActionMasked() == MotionEvent.ACTION_CANCEL) v.getParent().requestDisallowInterceptTouchEvent(false);
            return swipe.onTouchEvent(e);
        });
        root.addView(body);
        body.setVisibility(expanded ? View.VISIBLE : View.GONE);
        chevron.setRotation(expanded ? 180 : 0);
        applyTheme();
        refresh(true);
    }

    View view() { return root; }
    private int dp(float x) { return SettingsUi.dp(activity, x); }
    private TextView text(String s, int size, int color) { return SettingsUi.text(activity, s, size, color); }

    private void toggle() {
        if (animating) return;
        expanded = !expanded;
        chevron.animate().rotation(expanded ? 180 : 0).setDuration(220).setInterpolator(new DecelerateInterpolator()).start();
        int width = root.getWidth() > 0 ? root.getWidth() : activity.getResources().getDisplayMetrics().widthPixels;
        body.measure(View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY), View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED));
        int full = body.getMeasuredHeight();
        if (expanded) { refresh(true); body.setAlpha(0f); setHeight(0); body.setVisibility(View.VISIBLE); }
        ValueAnimator height = ValueAnimator.ofInt(expanded ? 0 : full, expanded ? full : 0);
        height.setDuration(260); height.setInterpolator(new DecelerateInterpolator(1.5f));
        height.addUpdateListener(a -> { setHeight((int) a.getAnimatedValue()); body.setAlpha(expanded ? a.getAnimatedFraction() : 1 - a.getAnimatedFraction()); });
        height.addListener(new AnimatorListenerAdapter() {
            @Override public void onAnimationEnd(Animator a) {
                animating = false;
                setHeight(ViewGroup.LayoutParams.WRAP_CONTENT); body.setAlpha(1f);
                body.setVisibility(expanded ? View.VISIBLE : View.GONE);
            }
        });
        animating = true; height.start();
    }
    private void setHeight(int h) {
        ViewGroup.LayoutParams lp = body.getLayoutParams(); lp.height = h; body.setLayoutParams(lp);
    }
    // Slides the current card out and the other one in from the side it comes from.
    private void show(int target) {
        if (target == page || animating) return;
        View out = pages[page].view, in = pages[target].view;
        float shift = pager.getWidth() > 0 ? pager.getWidth() : dp(300);
        int dir = target > page ? 1 : -1;
        page = target; refresh(true); updateDots();
        animating = true;
        in.setTranslationX(dir * shift); in.setAlpha(0f); in.setVisibility(View.VISIBLE);
        out.animate().translationX(-dir * shift * 0.35f).alpha(0f).setDuration(220).setInterpolator(new DecelerateInterpolator())
            .withEndAction(() -> { out.setVisibility(View.INVISIBLE); out.setTranslationX(0); out.setAlpha(1f); }).start();
        in.animate().translationX(0).alpha(1f).setDuration(280).setInterpolator(new DecelerateInterpolator(1.8f))
            .withEndAction(() -> animating = false).start();
    }
    private void updateDots() {
        for (int i = 0; i < 2; i++) {
            dots[i].setBackground(SettingsUi.rounded(activity, i == page ? SettingsUi.accent() : SettingsUi.border(), 3));
        }
    }

    /** Updates the numbers; called from the Settings refresh, at most once a second. */
    void refresh(boolean now) {
        long elapsed = SystemClock.elapsedRealtime();
        if (!now && elapsed - refreshedAt < 1000) return;
        refreshedAt = elapsed;
        SessionStatsHistory.Change hour = SessionStats.change(SessionStatsHistory.HOUR_MS);
        if (hour == null) {
            summary.setText("Collecting…");
        } else {
            summary.setText(String.format(Locale.US, "+%,d XP · +%,d dust · last hour",
                (long) hour.get(SessionStatsHistory.XP), (long) hour.get(SessionStatsHistory.DUST)));
        }
        if (!expanded && !now) return;
        for (int p = 0; p < 2; p++) fill(pages[p], p == 0 ? hour : SessionStats.change(WINDOWS[p]), WINDOWS[p]);
    }
    private void fill(Page p, SessionStatsHistory.Change c, long window) {
        if (c == null) {
            for (TextView v : p.values) v.setText("–");
            p.note.setText("Waiting for the game");
            return;
        }
        double xp = c.get(SessionStatsHistory.XP), caught = c.get(SessionStatsHistory.CAUGHT), seen = c.get(SessionStatsHistory.SEEN);
        p.values[0].setText(String.format(Locale.US, "+%,d", (long) xp));
        p.values[1].setText(String.format(Locale.US, "+%,d", (long) c.get(SessionStatsHistory.DUST)));
        p.values[2].setText(String.format(Locale.US, "%,d", (long) caught));
        p.values[3].setText(String.format(Locale.US, "%,d", (long) seen));
        p.labels[3].setText(seen >= 1 ? String.format(Locale.US, "seen · %d%% caught", Math.round(Math.min(1, caught / seen) * 100)) : "seen");
        p.values[4].setText(String.format(Locale.US, "%,d", (long) c.get(SessionStatsHistory.STOPS)));
        p.values[5].setText(String.format(Locale.US, "%.1f km", c.get(SessionStatsHistory.KM)));
        p.values[6].setText(String.format(Locale.US, "%,d", (long) c.get(SessionStatsHistory.HATCHED)));
        // A history shorter than the window says since when it counts.
        if (c.coveredMs + 120_000 < window) {
            Date since = new Date(c.updatedAt - c.coveredMs);
            p.note.setText("since " + DateFormat.getTimeFormat(activity).format(since));
        } else if (window == SessionStatsHistory.DAY_MS && xp > 0) {
            p.note.setText(String.format(Locale.US, "%,d XP / h", Math.round(xp / 24)));
        } else {
            p.note.setText("rolling");
        }
    }

    /** Re-applies the palette after the Settings tree was restyled for a theme switch. */
    void applyTheme() {
        root.setBackground(SettingsUi.cardBackground(activity));
        title.setTextColor(SettingsUi.ink()); summary.setTextColor(SettingsUi.muted()); chevron.invalidate();
        for (Page p : pages) {
            p.heading.setTextColor(SettingsUi.accent()); p.note.setTextColor(SettingsUi.faint());
            for (TextView v : p.values) v.setTextColor(SettingsUi.ink());
            for (TextView l : p.labels) l.setTextColor(SettingsUi.muted());
            p.values[0].setTextColor(SettingsUi.accent()); p.values[1].setTextColor(SettingsUi.accent());
        }
        updateDots();
    }
}
