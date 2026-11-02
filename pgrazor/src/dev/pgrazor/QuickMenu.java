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
 * Icons are drawn as canvas vectors so nothing depends on an emoji font. Theme and the
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
    }

    static final int MAP = 0, WALK = 1, AUTOWALK = 2, FEEDS = 3, FAVORITES = 4, TELEPORT = 5, ACCOUNTS = 6, SETTINGS = 7, EVENTS = 8, AUTOMATION = 9, ROUTES = 11;

    // Append-only order: persisted visibility bits must not follow icon IDs (some are shared).
    static final String[] CUSTOMIZABLE = {"Map", "Walking speed", "Autowalk", "In-game Routes", "Feeds", "Favorites", "Hotspots", "Teleport", "Switch account", "Events", "Automation"};
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
    private View walkIcon;
    private final List<TextView> rowLabels = new ArrayList<>();
    private final List<Icon> rowIcons = new ArrayList<>();
    private final List<Boolean> rowEnabled = new ArrayList<>();

    public QuickMenu(Activity activity, ViewGroup parent, View anchor, Listener listener) {
        this.activity = activity; this.anchor = anchor; this.listener = listener;
        root = new FrameLayout(activity);
        // A transparent scrim catches outside taps to dismiss without dimming the game.
        View scrim = new View(activity); scrim.setClickable(true); scrim.setOnClickListener(v -> hide());
        root.addView(scrim, new FrameLayout.LayoutParams(-1, -1));

        card = new LinearLayout(activity); card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(6),dp(4),dp(6),dp(4)); card.setElevation(dp(8));
        card.addView(row(MAP, "Map", true, listener::onMap, 1));
        LinearLayout walkRow = (LinearLayout) row(WALK, "Walk", true, this::onWalk, 2); walkLabel = (TextView) walkRow.getChildAt(1); walkIcon=walkRow.getChildAt(0);
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
        card.addView(row(SETTINGS, "Settings", true, listener::onSettings, 0));
        viewport=new ScrollView(activity);viewport.setFillViewport(false);
        viewport.setClipToPadding(true);viewport.addView(card,new ScrollView.LayoutParams(-1,-2));
        root.addView(viewport,new FrameLayout.LayoutParams(dp(MapTheme.Size.QUICK_MENU_WIDTH_DP),-2));
        root.addOnLayoutChangeListener((v,l,t,r,b,ol,ot,or,ob)->{if(isShowing()&&(r-l!=or-ol||b-t!=ob-ot))position();});
        root.setOnApplyWindowInsetsListener((v,insets)->{if(isShowing())viewport.post(this::position);return insets;});

        parent.addView(root, new FrameLayout.LayoutParams(-1, -1));
        root.setVisibility(View.GONE);
        applyTheme();
    }

    private View row(int kind, String label, boolean enabled, Runnable action, int visibilityBit) {
        LinearLayout r = new DividedRow(activity); r.setOrientation(LinearLayout.HORIZONTAL);
        r.setGravity(Gravity.CENTER_VERTICAL); r.setMinimumHeight(dp(MapTheme.Size.QUICK_ROW_DP)); r.setPadding(dp(8), dp(4), dp(12), dp(4));
        Icon icon = new Icon(activity, kind); icon.disabled = !enabled;
        r.addView(icon, new LinearLayout.LayoutParams(dp(MapTheme.Size.QUICK_ICON_DP), dp(MapTheme.Size.QUICK_ICON_DP)));
        TextView t = new TextView(activity); t.setText(label); t.setTextSize(MapTheme.Size.QUICK_TEXT_SP); t.setPadding(dp(12), 0, 0, 0);
        t.setSingleLine(false); t.setMaxLines(Integer.MAX_VALUE); // preserve labels at large system font scales
        r.addView(t, new LinearLayout.LayoutParams(0, -2, 1f));
        if (enabled && action != null) {
            r.setClickable(true); r.setFocusable(true);
            r.setBackground(new RippleDrawable(ColorStateList.valueOf(MapTheme.accentSoft()), null, rounded(0xffffffff, 12)));
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
        if (walkIcon != null) walkIcon.invalidate();
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
        int width=UiViewportPolicy.menuExtent(dp(MapTheme.Size.QUICK_MENU_WIDTH_DP),safe.width(),scale);
        card.measure(View.MeasureSpec.makeMeasureSpec(width,View.MeasureSpec.EXACTLY),View.MeasureSpec.makeMeasureSpec(0,View.MeasureSpec.UNSPECIFIED));
        int height=UiViewportPolicy.menuExtent(card.getMeasuredHeight(),safe.height(),scale);
        ViewGroup.LayoutParams lp=viewport.getLayoutParams();
        if(lp.width!=width||lp.height!=height){lp.width=width;lp.height=height;viewport.setLayoutParams(lp);}
        int cw=Math.round(width*scale),ch=Math.round(height*scale),gap=dp(MapTheme.Size.QUICK_OFFSET_DP);
        float x=anchor.getX(),y=anchor.getY()+OverlayDrag.visualHeight(anchor)+gap;
        if(y+ch>safe.bottom)y=anchor.getY()-ch-gap;
        viewport.setX(UiViewportPolicy.clamp(x,safe.left,safe.right,cw));
        viewport.setY(UiViewportPolicy.clamp(y,safe.top,safe.bottom,ch));
    }
    public void destroy() {
        hide();
        if (root.getParent() instanceof ViewGroup) ((ViewGroup) root.getParent()).removeView(root);
    }

    /** Opacity of the menu card (the invisible tap-to-close scrim is unaffected). */
    public void setOpacity(float alpha) { card.setAlpha(alpha); }

    public void applyTheme() {
        // PGRazorX: the VGP ON look: card surface with the accent wash and a 2dp accent ring.
        GradientDrawable wash = rounded(MapTheme.accentSoft(), MapTheme.Size.QUICK_RADIUS_DP); wash.setStroke(dp(2), accent());
        card.setBackground(new android.graphics.drawable.LayerDrawable(new android.graphics.drawable.Drawable[]{
            rounded((MapTheme.card() & 0x00ffffff) | 0xeb000000, MapTheme.Size.QUICK_RADIUS_DP), wash}));
        card.setOutlineProvider(android.view.ViewOutlineProvider.BACKGROUND);
        for (int i = 0; i < card.getChildCount(); i++) card.getChildAt(i).invalidate();
        for (int i = 0; i < rowLabels.size(); i++) {
            TextView label = rowLabels.get(i);
            label.setTextColor(rowEnabled.get(i) ? ink() : muted());
            label.setTypeface(MapTheme.bold());
            rowIcons.get(i).invalidate();
        }
        if (walkLabel != null) walkLabel.setTextColor(MapTheme.value());
    }

    private int dp(float x) { return Math.round(x * activity.getResources().getDisplayMetrics().density); }
    private int ink() { return MapTheme.ink(); }
    private int muted() { return MapTheme.muted(); }
    private int accent() { return MapTheme.accent(); }
    private GradientDrawable rounded(int color, int radius) {
        GradientDrawable d = new GradientDrawable(); d.setColor(color); d.setCornerRadius(dp(radius)); return d;
    }

    /**
     * Menu row that draws a thin, faint separator above itself when another row precedes it: 1dp, almost
     * edge to edge (8dp margins), in the text colour at low alpha so it reads on the white and dark cards.
     */
    private final class DividedRow extends LinearLayout {
        private final Paint line = new Paint();
        DividedRow(Context c) { super(c); setWillNotDraw(false); }
        protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            if (!(getParent() instanceof ViewGroup) || ((ViewGroup) getParent()).indexOfChild(this) == 0) return;
            line.setColor((MapTheme.ink() & 0x00ffffff) | 0x24000000);
            canvas.drawRect(dp(8), 0, getWidth() - dp(8), Math.max(1, dp(1)), line);
        }
    }
    /** Walk icon pose for the current speed preset: standing, walking, jogging, running. */
    static int walkStage() {
        double v = Control.engine.speedKmh;
        return v < 0.5 ? 0 : v < 12 ? 1 : v < 20 ? 2 : 3;
    }
    private static final java.util.Map<String, Path> PATHS = new java.util.HashMap<>();
    /** Minimal SVG path reader (absolute M, L, C, Z) so the glyphs share one geometry with the design preview. */
    private static Path path(String d) {
        Path cached = PATHS.get(d); if (cached != null) return cached;
        Path p = new Path(); java.util.regex.Matcher m = java.util.regex.Pattern.compile("[MLCZ]|-?\\d*\\.?\\d+").matcher(d);
        java.util.List<String> t = new ArrayList<>(); while (m.find()) t.add(m.group());
        char cmd = 'M';
        for (int i = 0; i < t.size();) {
            String tok = t.get(i);
            if (tok.length() == 1 && Character.isLetter(tok.charAt(0))) { cmd = tok.charAt(0); i++; if (cmd == 'Z') p.close(); continue; }
            if (cmd == 'C') { float[] v = new float[6]; for (int k = 0; k < 6; k++) v[k] = Float.parseFloat(t.get(i++)); p.cubicTo(v[0], v[1], v[2], v[3], v[4], v[5]); }
            else { float x = Float.parseFloat(t.get(i++)), y = Float.parseFloat(t.get(i++)); if (cmd == 'M') { p.moveTo(x, y); cmd = 'L'; } else p.lineTo(x, y); }
        }
        PATHS.put(d, p); return p;
    }
    /** Rounded-square game button in the VGP ON colours (lime-to-accent tile, soft edge) with a white glyph. */
    private final class Icon extends View {
        private final int kind;
        boolean disabled;
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private int glyphColor = Color.WHITE;
        Icon(Context c, int kind) { super(c); this.kind = kind; setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO); }
        protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            // PGRazorX: the VGP tile's shape and colours: a rounded square with the lime-to-accent gradient and a soft
            // edge, and a white glyph (a flat grey tile when disabled).
            float size = Math.min(getWidth(), getHeight()), corner = size * 0.3f;
            paint.setPathEffect(null); paint.setShader(null); paint.setStyle(Paint.Style.FILL);
            if (disabled) paint.setColor(MapTheme.dlgHair());
            else paint.setShader(new android.graphics.LinearGradient(0, 0, size, size, MapTheme.accentLime(), accent(), android.graphics.Shader.TileMode.CLAMP));
            canvas.drawRoundRect(0.5f, 0.5f, size - 0.5f, size - 0.5f, corner, corner, paint); paint.setShader(null);
            if (!disabled) { paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(Math.max(1f, size / 36f)); paint.setColor((MapTheme.closeFill() & 0x00ffffff) | 0x59000000);
                canvas.drawRoundRect(0.5f, 0.5f, size - 0.5f, size - 0.5f, corner, corner, paint); paint.setStyle(Paint.Style.FILL); }
            float scale = size / 40f * 0.93f;
            canvas.save(); canvas.translate(size * 0.22f, size * 0.2325f); canvas.scale(scale, scale);
            glyphColor = disabled ? MapTheme.dlgDisabled() : Color.WHITE; glyph(canvas, kind);
            canvas.restore();
        }
        private void prep(Paint.Style style, float width) {
            paint.setShader(null); paint.setPathEffect(null); paint.setColor(glyphColor); paint.setStyle(style); paint.setStrokeWidth(width);
            paint.setStrokeCap(Paint.Cap.ROUND); paint.setStrokeJoin(Paint.Join.ROUND);
        }
        private void stroke(Canvas c, String d, float w) { prep(Paint.Style.STROKE, w); c.drawPath(path(d), paint); }
        private void fill(Canvas c, String d) { prep(Paint.Style.FILL, 0); c.drawPath(path(d), paint); }
        private void fillRound(Canvas c, String d, float w) { prep(Paint.Style.FILL_AND_STROKE, w); c.drawPath(path(d), paint); }
        private void dots(Canvas c, String d, float w) { prep(Paint.Style.STROKE, w); paint.setPathEffect(new DashPathEffect(new float[]{0.01f, 3.4f}, 0)); c.drawPath(path(d), paint); paint.setPathEffect(null); }
        private void circle(Canvas c, float x, float y, float r, float w) { prep(w == 0 ? Paint.Style.FILL : Paint.Style.STROKE, w); c.drawCircle(x, y, r, paint); }
        private void gear(Canvas c) {
            Path g = new Path(); g.moveTo(GEAR[0], GEAR[1]); for (int i = 2; i < GEAR.length; i += 2) g.lineTo(GEAR[i], GEAR[i + 1]); g.close();
            g.addCircle(12, 12, 2.9f, Path.Direction.CW); g.setFillType(Path.FillType.EVEN_ODD);
            prep(Paint.Style.FILL, 0); c.drawPath(g, paint);
            prep(Paint.Style.STROKE, 0.8f); c.drawPath(g, paint);
        }
        /** Glyphs in a 24x24 box (generated from the same geometry as the preview). */
        private void glyph(Canvas c,int kind){
            switch(kind){
                case MAP: stroke(c,"M3,6.6 L8.6,4.6 L15.4,6.6 L21,4.6 L21,17.4 L15.4,19.4 L8.6,17.4 L3,19.4 Z",1.9f); stroke(c,"M8.6,4.6 L8.6,17.4",1.6f); stroke(c,"M15.4,6.6 L15.4,19.4",1.6f); break;
                case AUTOWALK: dots(c,"M4.4,17.8 C4.4,14.4 10.8,15.4 10.8,12 C10.8,8.6 4.4,9.6 4.4,6.2",2.0f); circle(c,4.4f,20.1f,1.9f,1.6f); circle(c,4.4f,3.9f,2.1f,0f); circle(c,17.98f,5.84f,1.8f,0f); stroke(c,"M17.58,8.48 L16.94,13.44",2.16f); stroke(c,"M17.42,9.84 L15.42,12.32",1.89f); stroke(c,"M17.42,9.84 L19.58,12.16",1.89f); stroke(c,"M16.94,13.44 L18.78,15.92 L19.42,18.8",1.89f); stroke(c,"M16.94,13.44 L15.82,16.08 L14.62,18.56",1.89f); break;
                case FEEDS: circle(c,5.6f,18.4f,2.3f,0f); stroke(c,"M5.6,12.2 C9.0,12.2 11.8,15.0 11.8,18.4",2.2f); stroke(c,"M5.6,7.8 C11.5,7.8 16.2,12.5 16.2,18.4",2.2f); stroke(c,"M5.6,3.4 C13.9,3.4 20.6,10.1 20.6,18.4",2.2f); break;
                case FAVORITES: fillRound(c,"M12.0,4.2 L14.17,9.61 L19.99,10.0 L15.52,13.74 L16.94,19.4 L12.0,16.3 L7.06,19.4 L8.48,13.74 L4.01,10.0 L9.83,9.61 Z",1.6f); break;
                case TELEPORT: fillRound(c,"M12,3.2 L19.6,19.8 L12,15.8 L4.4,19.8 Z",1.6f); break;
                case EVENTS: stroke(c,"M4,8 L20,8 L20,20 L4,20 Z M3,5 L21,5 L21,9 L3,9 Z M12,5 L12,20",1.7f); break;
                case AUTOMATION: fillRound(c,"M13,3 L6,13.5 L11,13.5 L10,21 L18,10 L13,10 Z",1.2f); break;
                case SETTINGS: gear(c); break;
                // v0.1.6: In-game Routes (a path between two pins) and Switch account (a person with a switch arrow).
                case ROUTES: dots(c,"M6,17.6 C6,12.6 18,12.4 18,7.6",2.0f); circle(c,6f,19.2f,2.1f,0f); circle(c,18f,5.4f,2.1f,0f); break;
                case ACCOUNTS: circle(c,9f,7.6f,3.1f,0f); fillRound(c,"M3,19.6 C3,13.2 15,13.2 15,19.6 Z",1.2f); stroke(c,"M16.2,6.4 C19.8,6.0 21.6,9.6 19.8,12.6",1.8f); stroke(c,"M15.6,4.4 L16.2,6.4 L18.2,7.0",1.8f); break;
                case WALK: switch(walkStage()){
                    case 0: circle(c,12f,4.3f,2.25f,0f); stroke(c,"M12,7.6 L12,13.8",2.4f); stroke(c,"M12,9.3 L9.3,13.4",2.1f); stroke(c,"M12,9.3 L14.7,13.4",2.1f); stroke(c,"M12,13.8 L10.2,20.4",2.1f); stroke(c,"M12,13.8 L13.8,20.4",2.1f); break;
                    case 1: circle(c,12.6f,4.3f,2.25f,0f); stroke(c,"M12.1,7.6 L11.3,13.8",2.4f); stroke(c,"M11.9,9.3 L9.4,12.4",2.1f); stroke(c,"M11.9,9.3 L14.6,12.2",2.1f); stroke(c,"M11.3,13.8 L13.6,16.9 L14.4,20.5",2.1f); stroke(c,"M11.3,13.8 L9.9,17.1 L8.4,20.2",2.1f); break;
                    case 2: circle(c,13.8f,4.4f,2.25f,0f); stroke(c,"M13,7.7 L11,13.4",2.4f); stroke(c,"M12.7,9.1 L15.3,10.7 L16.6,8.6",2.1f); stroke(c,"M12.7,9.1 L9.9,10.3 L8.9,12.8",2.1f); stroke(c,"M11,13.4 L14.4,15.5 L15.1,19.9",2.1f); stroke(c,"M11,13.4 L9,16.8 L6.4,17.7",2.1f); break;
                    case 3: circle(c,15.6f,4.5f,2.25f,0f); stroke(c,"M14.6,7.9 L11.6,13",2.4f); stroke(c,"M14.3,9.3 L17.4,10.2 L19.1,7.8",2.1f); stroke(c,"M14.3,9.3 L11.1,9.7 L9.7,12.6",2.1f); stroke(c,"M11.6,13 L15.6,14.6 L17.1,18.8",2.1f); stroke(c,"M11.6,13 L9.6,16.9 L5.9,17.6",2.1f); stroke(c,"M1.6,6.6 L5.6,6.6",1.7f); stroke(c,"M2.6,10.2 L6.2,10.2",1.7f); stroke(c,"M1.2,13.8 L4.4,13.8",1.7f); break;
                } break;
            }
        }
        private final float[] GEAR={18.72f,10.45f,21.32f,10.77f,21.32f,13.23f,18.72f,13.55f,17.85f,15.66f,19.46f,17.72f,17.72f,19.46f,15.66f,17.85f,13.55f,18.72f,13.23f,21.32f,10.77f,21.32f,10.45f,18.72f,8.34f,17.85f,6.28f,19.46f,4.54f,17.72f,6.15f,15.66f,5.28f,13.55f,2.68f,13.23f,2.68f,10.77f,5.28f,10.45f,6.15f,8.34f,4.54f,6.28f,6.28f,4.54f,8.34f,6.15f,10.45f,5.28f,10.77f,2.68f,13.23f,2.68f,13.55f,5.28f,15.66f,6.15f,17.72f,4.54f,19.46f,6.28f,17.85f,8.34f};
    }
}
