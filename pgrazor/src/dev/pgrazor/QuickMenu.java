package dev.pgrazor;

import android.app.Activity;
import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.*;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.view.*;
import android.widget.*;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * Compact vertical icon+text quick menu toggled by the R launcher (PGSharp-style).
 * Icons are the shared {@link MenuIcon} glyphs, also used by the settings home screen. Theme and the
 * walking-speed caption follow {@link Control}; map/autowalk/location actions are delegated
 * to the host through {@link Listener} so map handling is never duplicated here.
 */
public final class QuickMenu {
    public interface Listener {
        void onMap();
        /** Cycles the walk speed and returns the new value (km/h); the caption refreshes from it. */
        double onCycleWalkSpeed();
        void onAutoWalk();
        void onGameRoutes();
        void onFeeds();
        void onFavorites();
        void onHotspots();
        void onTeleport();
        void onAccounts();
        void onSettings();
        /** Asks to hide every PGRazor overlay (three fingers on screen show them again). */
        void onHideOverlays();
    }

    static final int MAP = MenuIcon.MAP, WALK = MenuIcon.WALK, AUTOWALK = MenuIcon.AUTOWALK, FEEDS = MenuIcon.FEEDS, FAVORITES = MenuIcon.FAVORITES, TELEPORT = MenuIcon.TELEPORT,
        ACCOUNTS = MenuIcon.ACCOUNTS, SETTINGS = MenuIcon.SETTINGS, EVENTS = MenuIcon.EVENTS, AUTOMATION = MenuIcon.AUTOMATION, HIDE = MenuIcon.HIDE, ROUTES = MenuIcon.ROUTES, RESEARCH = MenuIcon.RESEARCH;

    // Append-only order: persisted visibility bits must not follow icon IDs (some are shared).
    static final String[] CUSTOMIZABLE = {"Map", "Walking speed", "Autowalk", "In-game Routes", "Feeds", "Favorites", "Hotspots", "Teleport", "Switch account", "Events", "Automation", "Research", "Hide overlay"};
    private final List<View> rows = new ArrayList<>();
    private final List<Integer> visibilityBits = new ArrayList<>();
    private int appliedHidden = -1;
    private final Activity activity;
    private final Listener listener;
    private final View anchor;
    private final FrameLayout root;
    private final LinearLayout card;
    private final ScrollView viewport;
    private TextView walkLabel;
    private final List<TextView> rowLabels = new ArrayList<>();
    private final List<MenuIcon> rowIcons = new ArrayList<>();
    private final List<Boolean> rowEnabled = new ArrayList<>();

    public QuickMenu(Activity activity, ViewGroup parent, View anchor, Listener listener) {
        this.activity = activity; this.anchor = anchor; this.listener = listener;
        root = new FrameLayout(activity);
        // A transparent scrim catches outside taps to dismiss without dimming the game.
        View scrim = new View(activity); scrim.setClickable(true); scrim.setOnClickListener(v -> hide());
        root.addView(scrim, new FrameLayout.LayoutParams(-1, -1));

        card = new LinearLayout(activity); card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(0, dp(6), 0, dp(6)); card.setElevation(dp(12));
        card.addView(row(MAP, "Map", true, listener::onMap, 1));
        LinearLayout walkRow = (LinearLayout) row(WALK, "Walk", true, this::onWalk, 2); walkLabel = (TextView) walkRow.getChildAt(1);
        card.addView(walkRow);
        card.addView(row(AUTOWALK, "Autowalk", true, listener::onAutoWalk, 4));
        card.addView(row(ROUTES, "In-game Routes", true, listener::onGameRoutes, 8));
        card.addView(row(FEEDS, "Feeds", true, listener::onFeeds, 16));
        card.addView(row(FAVORITES, "Favorites", true, listener::onFavorites, 32));
        card.addView(row(MAP, "Hotspots", true, listener::onHotspots, 64));
        card.addView(row(TELEPORT, "Teleport", true, listener::onTeleport, 128));
        card.addView(row(ACCOUNTS, "Switch account", true, listener::onAccounts, 256));
        card.addView(row(EVENTS, "Events", true, () -> dev.pgrazor.events.EventPanel.showSelector(activity, Control.darkTheme()), 512));
        card.addView(row(AUTOMATION, "Automation", true, () -> AutomationPanel.show(activity), 1024));
        card.addView(row(RESEARCH, "Research", true, () -> ResearchSettings.show(activity), 2048));
        card.addView(row(HIDE, "Hide overlay", true, listener::onHideOverlays, 4096));
        card.addView(row(SETTINGS, "Settings", true, listener::onSettings, 0));
        viewport=new ScrollView(activity);viewport.setFillViewport(false);
        viewport.addView(card,new ScrollView.LayoutParams(-1,-2));
        root.addView(viewport,new FrameLayout.LayoutParams(dp(196),-2));
        root.addOnLayoutChangeListener((v,l,t,r,b,ol,ot,or,ob)->{if(isShowing()&&(r-l!=or-ol||b-t!=ob-ot))position();});
        root.setOnApplyWindowInsetsListener((v,insets)->{if(isShowing())viewport.post(this::position);return insets;});

        parent.addView(root, new FrameLayout.LayoutParams(-1, -1));
        root.setVisibility(View.GONE);
        applyTheme();
    }

    private View row(int kind, String label, boolean enabled, Runnable action, int visibilityBit) {
        LinearLayout r = new LinearLayout(activity); r.setOrientation(LinearLayout.HORIZONTAL);
        r.setGravity(Gravity.CENTER_VERTICAL); r.setMinimumHeight(dp(48)); r.setPadding(dp(14), dp(6), dp(16), dp(6));
        MenuIcon icon = new MenuIcon(activity, kind); icon.disabled = !enabled;
        r.addView(icon, new LinearLayout.LayoutParams(dp(22), dp(22)));
        TextView t = new TextView(activity); t.setText(label); t.setTextSize(14.5f); t.setPadding(dp(14), 0, 0, 0);
        r.addView(t, new LinearLayout.LayoutParams(0, -2, 1f));
        if (enabled && action != null) {
            r.setClickable(true); r.setFocusable(true);
            r.setBackground(new RippleDrawable(ColorStateList.valueOf(0x3345e1ba), null, rounded(0xffffffff, 10)));
            r.setOnClickListener(v -> {if(kind!=WALK)hide();action.run();});
        }
        rows.add(r); visibilityBits.add(visibilityBit);
        rowIcons.add(icon); rowLabels.add(t); rowEnabled.add(enabled);
        return r;
    }

    private void onWalk() {
        listener.onCycleWalkSpeed();
        updateWalk();
        // Keep the menu open so the user can keep cycling the speed quickly.
    }
    private void updateWalk() {
        if (walkLabel == null) return;
        double speed = Control.engine.speedKmh;
        walkLabel.setText(String.format(Locale.US, "%.1f km/h", speed));
    }

    private void updateVisibility() {
        int hidden=Control.quickMenuHidden();if(hidden==appliedHidden)return;
        appliedHidden=hidden;
        for(int i=0;i<rows.size();i++)rows.get(i).setVisibility((hidden&visibilityBits.get(i))==0?View.VISIBLE:View.GONE);
        if(isShowing())card.post(this::position);
    }
    public void onRefresh() { if (isShowing()) {updateVisibility();updateWalk();} }
    public void applyScale(float scale){viewport.setPivotX(0);viewport.setPivotY(0);viewport.setScaleX(scale);viewport.setScaleY(scale);if(isShowing())card.post(this::position);}
    public boolean isShowing() { return root.getVisibility() == View.VISIBLE; }
    public void toggle() { if (isShowing()) hide(); else show(); }
    public void hide() { root.setVisibility(View.GONE); }
    public void show() {
        updateVisibility();
        updateWalk();
        root.setVisibility(View.VISIBLE); root.bringToFront();
        card.post(this::position);
    }
    private void position() {
        if(root.getWidth()<=0||root.getHeight()<=0)return;
        android.graphics.Rect safe=UiViewport.bounds(root);
        int margin=dp(8);safe.inset(Math.min(margin,safe.width()/4),Math.min(margin,safe.height()/4));
        float scale=Math.max(.01f,viewport.getScaleX());
        int width=UiViewportPolicy.menuExtent(dp(196),safe.width(),scale);
        card.measure(View.MeasureSpec.makeMeasureSpec(width,View.MeasureSpec.EXACTLY),View.MeasureSpec.makeMeasureSpec(0,View.MeasureSpec.UNSPECIFIED));
        int height=UiViewportPolicy.menuExtent(card.getMeasuredHeight(),safe.height(),scale);
        ViewGroup.LayoutParams lp=viewport.getLayoutParams();
        if(lp.width!=width||lp.height!=height){lp.width=width;lp.height=height;viewport.setLayoutParams(lp);}
        int cw=Math.round(width*scale),ch=Math.round(height*scale),gap=dp(8);
        float x=anchor.getX(),y=anchor.getY()+OverlayDrag.visualHeight(anchor)+gap;
        if(y+ch>safe.bottom)y=anchor.getY()-ch-gap;
        viewport.setX(UiViewportPolicy.clamp(x,safe.left,safe.right,cw));
        viewport.setY(UiViewportPolicy.clamp(y,safe.top,safe.bottom,ch));
    }
    public void destroy() {
        hide();
        if (root.getParent() instanceof ViewGroup) ((ViewGroup) root.getParent()).removeView(root);
    }

    public void applyTheme() {
        card.setBackground(rounded(surface(), 14));
        for (int i = 0; i < rowLabels.size(); i++) {
            rowLabels.get(i).setTextColor(rowEnabled.get(i) ? ink() : muted());
            rowIcons.get(i).invalidate();
        }
    }

    private int dp(float x) { return Math.round(x * activity.getResources().getDisplayMetrics().density); }
    private int ink() { return Control.darkTheme() ? Color.rgb(235, 241, 246) : Color.rgb(25, 35, 47); }
    private int muted() { return Control.darkTheme() ? Color.rgb(154, 171, 187) : Color.rgb(88, 105, 121); }
    private int surface() { return Control.darkTheme() ? Color.rgb(20, 27, 37) : Color.rgb(249, 251, 253); }
    private GradientDrawable rounded(int color, int radius) {
        GradientDrawable d = new GradientDrawable(); d.setColor(color); d.setCornerRadius(dp(radius)); return d;
    }
}
