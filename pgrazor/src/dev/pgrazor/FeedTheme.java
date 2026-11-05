package dev.pgrazor;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.os.Build;
import android.view.ContextThemeWrapper;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowInsets;
import android.widget.Button;
import android.widget.CompoundButton;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;

/**
 * The single design system for every PGRazor surface (settings panel, quick menu, overlays and
 * every dialog). The light theme follows the game's own settings screen: a lime-to-mint gradient
 * page, white rounded cards with hairline dividers, slate-teal bold text, grey chevrons, teal
 * values and toggles, gradient pill buttons and a round teal close button. The dark theme is a
 * night version of the same layout. Map tiles keep their original raster colours.
 *
 * <p>All sizes live in {@link Size} so they can be tuned in one place.
 */
final class FeedTheme {
    private FeedTheme() {}

    // ---------------------------------------------------------------------------------------
    // Sizes (dp / sp). Tune these to resize the whole UI; every module reads from here.
    // ---------------------------------------------------------------------------------------
    static final class Size {
        private Size() {}
        /** Screen titles such as "PGRAZOR" and dialog titles. */
        static final float TITLE_SP = 24f;
        /** Row labels in cards, the quick menu and switches. */
        static final float ROW_TEXT_SP = 16f;
        /** Helper / status lines under rows. */
        static final float CAPTION_SP = 12.5f;
        /** Minimum height of a list row inside a card. */
        static final int ROW_HEIGHT_DP = 54;
        /** Corner radius of the white cards. */
        static final int CARD_RADIUS_DP = 10;
        /** Vertical gap between cards. */
        static final int CARD_GAP_DP = 16;
        /** Inner left/right padding of cards (text and divider inset). */
        static final int CARD_PADDING_DP = 16;
        /** Diameter of the round teal close button. */
        static final int CLOSE_BUTTON_DP = 58;
        /** Diameter of the round white header button (theme switch). */
        static final int HEADER_BUTTON_DP = 48;
        /** Width and height of the on-map joystick. */
        static final int JOYSTICK_DP = 72;
        /** Width of the quick menu card. */
        static final int QUICK_MENU_WIDTH_DP = 145;
        /** Diameter of the round icon in each quick menu row. */
        static final int QUICK_ICON_DP = 29;
        /** Height of a quick menu row. */
        static final int QUICK_ROW_DP = 48;
        /** Quick menu label size. */
        static final float QUICK_TEXT_SP = 11f;
        /** Corner radius of the quick menu card. */
        static final int QUICK_RADIUS_DP = 14;
        /** Gap between the R launcher and the quick menu. */
        static final int QUICK_OFFSET_DP = 8;
        /**
         * Quick menu entries, top to bottom. Remove a name to hide it. Valid names: MAP, WALK, AUTOWALK,
         * FEEDS, FAVORITES, TELEPORT, SETTINGS. SETTINGS is always kept so the panel stays reachable.
         */
        static final String QUICK_MENU_ITEMS = "MAP,WALK,AUTOWALK,ROUTES,FEEDS,FAVORITES,HOTSPOTS,TELEPORT,ACCOUNTS,SETTINGS";
        /** Diameter of the floating R launcher. */
        static final int LAUNCHER_DP = 50;
        /** Height of pill buttons (dialog actions). */
        static final int PILL_HEIGHT_DP = 44;
        /** Corner radius of non-full-screen dialogs. */
        static final int DIALOG_RADIUS_DP = 18;
        /** Cooldown timer pill (top right): width. Fits its longest label, "☰  Waiting for GPS", at 14 sp. */
        static final int COOLDOWN_WIDTH_DP = 90;
        /** Cooldown timer pill: height. */
        static final int COOLDOWN_HEIGHT_DP = 24;
        /** Cooldown timer pill: text size. */
        static final float COOLDOWN_TEXT_SP = 12f;
        /** Catching-berry picker pill (right edge): width. */
        static final int BERRY_WIDTH_DP = 130;
        /** Catching-berry picker pill: height. */
        static final int BERRY_HEIGHT_DP = 36;
        /** Catching-berry picker pill: text size. */
        static final float BERRY_TEXT_SP = 12f;
    }

    // ---------------------------------------------------------------------------------------
    // Colour tokens.
    // ---------------------------------------------------------------------------------------
    static boolean dark() { return Control.darkTheme(); }
    /** Page gradient, top-left (lime) and bottom-right (mint). */
    static int pageTop()    { return dark() ? Color.rgb(28, 58, 52)   : Color.rgb(214, 245, 166); }
    static int pageBottom() { return dark() ? Color.rgb(15, 42, 44)   : Color.rgb(160, 226, 205); }
    /** Primary text: the game's slate teal. */
    static int ink()        { return dark() ? Color.rgb(228, 240, 236) : Color.rgb(66, 94, 94); }
    /** Secondary text, captions, disabled. */
    static int muted()      { return dark() ? Color.rgb(147, 170, 166) : Color.rgb(124, 146, 146); }
    /** Screen titles ("SETTINGS" style). */
    static int title()      { return dark() ? Color.rgb(214, 232, 226) : Color.rgb(78, 104, 104); }
    /** Mid page colour, used where a single solid background is needed. */
    static int surface()    { return dark() ? Color.rgb(22, 50, 48)   : Color.rgb(188, 236, 186); }
    /** White card. */
    static int card()       { return dark() ? Color.rgb(32, 48, 47)   : Color.rgb(251, 252, 252); }
    /** Subtle fill inside a card (joystick base, pressed rows). */
    static int control()    { return dark() ? Color.rgb(42, 60, 58)   : Color.rgb(238, 243, 242); }
    /** Row dividers and hairlines. */
    static int outline()    { return dark() ? Color.rgb(50, 70, 68)   : Color.rgb(228, 233, 233); }
    /** Grey list chevron. */
    static int chevron()    { return dark() ? Color.rgb(120, 142, 138) : Color.rgb(160, 170, 170); }
    /** Toggles, rings, icons. */
    static int accent()     { return dark() ? Color.rgb(94, 214, 184) : Color.rgb(54, 184, 158); }
    /** Second stop of the accent gradient (lime-green side). */
    static int accentLime() { return dark() ? Color.rgb(150, 220, 140) : Color.rgb(156, 216, 136); }
    /** Values shown on the right of a row ("0.377.1-G-64" style). */
    static int value()      { return dark() ? Color.rgb(108, 195, 214) : Color.rgb(58, 143, 163); }
    /** Deep teal of the round close button. */
    static int closeFill()  { return dark() ? Color.rgb(32, 150, 146) : Color.rgb(30, 140, 140); }
    /** Accent at low alpha: ripples and selected tints. */
    static int accentSoft() { return (accent() & 0x00ffffff) | (dark() ? 0x2e000000 : 0x26000000); }
    /** Destructive actions ("Sign Out" red). */
    static int danger()     { return dark() ? Color.rgb(240, 110, 116) : Color.rgb(210, 65, 75); }
    /** Amber used for "cooldown running" and other warnings. */
    static int warn()       { return dark() ? Color.rgb(255, 190, 100) : Color.rgb(196, 118, 10); }

    // Dialog palette sampled from the game's own cards and buttons (Social / Party Play screens).
    /** Dialog card: plain white like the game's cards. */
    static int dlgCard()     { return dark() ? Color.rgb(32, 48, 47)    : Color.WHITE; }
    /** Dialog titles and body text: slate #3E5659. */
    static int dlgInk()      { return dark() ? Color.rgb(228, 240, 236) : Color.rgb(62, 86, 89); }
    /** Dialog helper text: #7A8E8F. */
    static int dlgMuted()    { return dark() ? Color.rgb(160, 180, 176) : Color.rgb(122, 142, 143); }
    /** Disabled field text: #AAB4B5. */
    static int dlgDisabled() { return dark() ? Color.rgb(110, 128, 124) : Color.rgb(170, 180, 181); }
    /** Teal end of the game's button gradient: #65C39E. */
    static int dlgTeal()     { return Color.rgb(101, 195, 158); }
    /** Launcher halves: lighter teal on top, deeper teal below. */
    static int dlgTealLight() { return dlgTeal(); }
    static int dlgTealDark()  { return dark() ? Color.rgb(54, 140, 120) : Color.rgb(44, 110, 98); }
    /** Lime start of the game's button gradient: #96D294. */
    static int dlgLime()     { return Color.rgb(150, 210, 148); }
    /** Outline of the game's secondary pill and of input boxes: #7EC9B4. */
    static int dlgOutline()  { return dark() ? Color.rgb(84, 160, 138) : Color.rgb(126, 201, 180); }
    /** Mint block behind grouped content (rewards box): #E6F2DE. */
    static int dlgMint()     { return dark() ? Color.rgb(40, 62, 56)    : Color.rgb(230, 242, 222); }
    /** Hairlines inside the mint block. */
    static int dlgMintLine() { return dark() ? Color.rgb(56, 82, 74)    : Color.rgb(212, 230, 203); }
    /** Neutral hairline under titles: #E3E3E3. */
    static int dlgHair()     { return dark() ? Color.rgb(58, 76, 74)    : Color.rgb(227, 227, 227); }
    /** Disabled input fill. */
    static int dlgFieldOff() { return dark() ? Color.rgb(44, 60, 58)    : Color.rgb(243, 246, 245); }

    static Typeface medium() { return Typeface.create("sans-serif-medium", Typeface.NORMAL); }
    private static Typeface brandFont;
    /**
     * Sharp italic brand face used ONLY for the "PGRAZOR" title: Racing Sans One (SIL OFL), bundled at
     * assets pgrazor/fonts/title.ttf. Falls back to the heaviest system italic (Roboto Black) if the file is missing.
     */
    static Typeface brandFont(Context c) {
        if (brandFont == null) {
            try { brandFont = Typeface.createFromAsset(c.getAssets(), "pgrazor/fonts/title.ttf"); }
            catch (RuntimeException missing) { brandFont = Build.VERSION.SDK_INT >= 28 ? Typeface.create(Typeface.create("sans-serif-black", Typeface.NORMAL), 900, true) : Typeface.create("sans-serif-black", Typeface.BOLD_ITALIC); }
        }
        return brandFont;
    }
    static Typeface bold()   { return Typeface.create("sans-serif", Typeface.BOLD); }

    static int dp(Context c, float value) { return Math.round(value * c.getResources().getDisplayMetrics().density); }

    // ---------------------------------------------------------------------------------------
    // Drawables.
    // ---------------------------------------------------------------------------------------
    /** Rounded fill with an optional 1dp outline (pass 0 for none). */
    static GradientDrawable shape(Context c, int fill, int stroke, float radius) {
        GradientDrawable d = new GradientDrawable(); d.setColor(fill); d.setCornerRadius(dp(c, radius));
        if (stroke != 0) d.setStroke(Math.max(1, dp(c, 1)), stroke);
        return d;
    }
    /** The lime-to-mint page gradient. */
    static GradientDrawable pageBackground(Context c) {
        return new GradientDrawable(GradientDrawable.Orientation.TL_BR, new int[]{pageTop(), pageBottom()});
    }
    /** Floating dialogs: a plain white rounded card, like the game's cards. */
    static GradientDrawable dialogBackground(Context c) {
        return shape(c, dlgCard(), 0, Size.DIALOG_RADIUS_DP);
    }
    /** Mint rounded block that groups lists inside a dialog (the game's rewards box). */
    static GradientDrawable mintBlock(Context c) {
        return shape(c, dlgMint(), 0, 12);
    }
    /** Game primary button: lime-to-teal gradient pill. */
    static Drawable dialogPrimaryPill(Context c) {
        GradientDrawable d = new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT, new int[]{dlgLime(), dlgTeal()});
        d.setCornerRadius(dp(c, 100));
        return new RippleDrawable(ColorStateList.valueOf(0x33ffffff), d, null);
    }
    /** Game secondary button: white pill with a 2dp teal outline. */
    static Drawable dialogSecondaryPill(Context c) {
        GradientDrawable d = shape(c, dlgCard(), 0, 100); d.setStroke(dp(c, 2), dlgOutline());
        return new RippleDrawable(ColorStateList.valueOf((dlgTeal() & 0x00ffffff) | 0x30000000), d, null);
    }
    /**
     * Rounded input box (pill) with the game's outline; numbers are centred. Disabled boxes turn grey,
     * the focused one gets a stronger teal outline.
     */
    static void styleField(android.widget.EditText e) {
        Context c = e.getContext();
        GradientDrawable on = shape(c, dlgCard(), 0, 100); on.setStroke(Math.max(1, dp(c, 1.5f)), dlgOutline());
        GradientDrawable focus = shape(c, dlgCard(), 0, 100); focus.setStroke(dp(c, 2), dlgTeal());
        GradientDrawable off = shape(c, dlgFieldOff(), 0, 100); off.setStroke(Math.max(1, dp(c, 1.5f)), dlgHair());
        android.graphics.drawable.StateListDrawable states = new android.graphics.drawable.StateListDrawable();
        states.addState(new int[]{-android.R.attr.state_enabled}, off);
        states.addState(new int[]{android.R.attr.state_focused}, focus);
        states.addState(new int[]{}, on);
        e.setBackgroundTintList(null); e.setBackground(states);
        e.setTextColor(new ColorStateList(new int[][]{new int[]{-android.R.attr.state_enabled}, new int[]{}}, new int[]{dlgDisabled(), dlgInk()}));
        e.setHintTextColor(dlgMuted()); e.setTypeface(medium());
        e.setMinHeight(dp(c, 40)); e.setMinimumHeight(dp(c, 40));
        e.setPadding(dp(c, 14), dp(c, 6), dp(c, 14), dp(c, 6));
        int type = e.getInputType(), cls = type & android.text.InputType.TYPE_MASK_CLASS;
        boolean multi = (type & android.text.InputType.TYPE_TEXT_FLAG_MULTI_LINE) != 0;
        String sample = e.getText().length() > 0 ? e.getText().toString() : (e.getHint() == null ? "" : e.getHint().toString());
        boolean numeric = cls == android.text.InputType.TYPE_CLASS_NUMBER || cls == android.text.InputType.TYPE_CLASS_PHONE
            || (!sample.isEmpty() && sample.matches("[-+0-9.,\\s]+"));
        e.setGravity(!multi && numeric ? Gravity.CENTER : (Gravity.CENTER_VERTICAL | Gravity.START));
        ViewGroup.LayoutParams lp = e.getLayoutParams();
        if (lp instanceof ViewGroup.MarginLayoutParams) { ViewGroup.MarginLayoutParams m = (ViewGroup.MarginLayoutParams)lp; m.topMargin = Math.max(m.topMargin, dp(c, 4)); m.bottomMargin = Math.max(m.bottomMargin, dp(c, 4)); e.setLayoutParams(m); }
    }
    /** Checkboxes and radios inside dialogs: teal when checked, grey outline otherwise. */
    private static void styleDialogCompound(CompoundButton b) {
        b.setButtonTintList(new ColorStateList(new int[][]{new int[]{-android.R.attr.state_enabled}, new int[]{android.R.attr.state_checked}, new int[]{}},
            new int[]{dlgHair(), dlgTeal(), dlgDisabled()}));
        b.setTextColor(new ColorStateList(new int[][]{new int[]{-android.R.attr.state_enabled}, new int[]{}}, new int[]{dlgDisabled(), dlgInk()}));
    }
    /** Fields and checkboxes anywhere in a dialog, whichever styler built the content. */
    private static void styleDialogTree(View v) {
        if (v instanceof android.widget.EditText) styleField((android.widget.EditText)v);
        else if (v instanceof CompoundButton && !(v instanceof Switch)) styleDialogCompound((CompoundButton)v);
        if (v instanceof ViewGroup) { ViewGroup g = (ViewGroup)v; for (int i = 0; i < g.getChildCount(); i++) styleDialogTree(g.getChildAt(i)); }
    }
    /** White card. Pair with {@link #elevate} for the game's soft shadow. */
    static GradientDrawable cardBackground(Context c) {
        return shape(c, card(), 0, Size.CARD_RADIUS_DP);
    }
    /** Soft drop shadow, as on the game's cards and round buttons. */
    static void elevate(View v, float dpValue) {
        v.setElevation(dp(v.getContext(), dpValue));
        v.setOutlineProvider(android.view.ViewOutlineProvider.BACKGROUND);
    }
    /** Lime-to-teal gradient pill: the game's primary button. */
    static Drawable primaryPill(Context c) {
        GradientDrawable d = new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT, new int[]{accentLime(), accent()});
        d.setCornerRadius(dp(c, 100));
        return new RippleDrawable(ColorStateList.valueOf(0x33ffffff), d, null);
    }
    /** White pill with teal text: the game's secondary button. */
    static Drawable secondaryPill(Context c) {
        return new RippleDrawable(ColorStateList.valueOf(accentSoft()), shape(c, card(), 0, 100), null);
    }
    /** Transparent rippling row, used for list rows inside cards. */
    static Drawable rowRipple(Context c) {
        return new RippleDrawable(ColorStateList.valueOf(accentSoft()), null, shape(c, 0xffffffff, 0, 6));
    }
    /** Kept for older call sites: now the secondary pill. */
    static Drawable controlBackground(Context c) { return secondaryPill(c); }

    /** Toggle colours: white thumb on a teal track when on, grey track when off. */
    /**
     * The game's toggle: a slim pill track (light grey off, teal-to-lime gradient on) and a larger white
     * knob with a grey ring and a small inner ring, overlapping the track.
     */
    static void styleSwitch(Switch s) {
        Context c = s.getContext();
        int knob = dp(c, 28), trackH = dp(c, 22), inset = (knob - trackH) / 2;
        int offTrack = dark() ? 0xff3e5350 : 0xffe4e9e8, ring = dark() ? 0xff6f8380 : 0xffcdd4d3;
        GradientDrawable on = new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT, new int[]{dlgTeal(), dlgLime()});
        on.setCornerRadius(dp(c, 100));
        GradientDrawable off = shape(c, offTrack, 0, 100);
        GradientDrawable disabled = shape(c, (offTrack & 0x00ffffff) | 0x77000000, 0, 100);
        android.graphics.drawable.StateListDrawable track = new android.graphics.drawable.StateListDrawable();
        track.addState(new int[]{-android.R.attr.state_enabled}, new android.graphics.drawable.InsetDrawable(disabled, 0, inset, 0, inset));
        track.addState(new int[]{android.R.attr.state_checked}, new android.graphics.drawable.InsetDrawable(on, 0, inset, 0, inset));
        track.addState(new int[]{}, new android.graphics.drawable.InsetDrawable(off, 0, inset, 0, inset));
        GradientDrawable outer = new GradientDrawable(); outer.setShape(GradientDrawable.OVAL);
        outer.setColor(dark() ? 0xffe9eeed : Color.WHITE); outer.setStroke(dp(c, 1.6f), ring); outer.setSize(knob, knob);
        GradientDrawable inner = new GradientDrawable(); inner.setShape(GradientDrawable.OVAL);
        inner.setColor(Color.TRANSPARENT); inner.setStroke(dp(c, 1.4f), ring);
        android.graphics.drawable.LayerDrawable thumb = new android.graphics.drawable.LayerDrawable(new Drawable[]{outer, inner});
        thumb.setLayerGravity(1, Gravity.CENTER); thumb.setLayerSize(1, dp(c, 11), dp(c, 11));
        s.setThumbTintList(null); s.setTrackTintList(null);
        s.setThumbDrawable(thumb); s.setTrackDrawable(track);
        s.setSwitchMinWidth(dp(c, 54));
    }
    /** Checkbox / radio tint. */
    static void styleCompound(CompoundButton b) {
        b.setButtonTintList(new ColorStateList(new int[][]{new int[]{-android.R.attr.state_enabled}, new int[]{android.R.attr.state_checked}, new int[]{}},
            new int[]{(muted() & 0x00ffffff) | 0x80000000, accent(), chevron()}));
    }
    /** Upper-case, spaced, centred title in the game's plain style (dialogs, log and backup screens). */
    static void styleTitle(TextView t) {
        t.setAllCaps(true); t.setLetterSpacing(0.05f); t.setTypeface(Typeface.create("sans-serif", Typeface.NORMAL));
        t.setTextSize(Size.TITLE_SP); t.setTextColor(title()); t.setGravity(Gravity.CENTER);
    }
    /** The "PGRAZOR" title only: same placement, sharp italic brand face. */
    static void styleBrandTitle(TextView t) {
        styleTitle(t); t.setTypeface(brandFont(t.getContext())); t.setLetterSpacing(0.04f);
    }

    // ---------------------------------------------------------------------------------------
    // Applying the theme.
    // ---------------------------------------------------------------------------------------
    /** A dialog-themed context so nested AlertDialogs follow the chosen dark/light appearance. */
    static Context wrap(Activity activity) {
        return new FirstFrameGate(activity, dark()
            ? android.R.style.Theme_Material_Dialog_Alert : android.R.style.Theme_Material_Light_Dialog_Alert);
    }
    /**
     * PGRazorX: a dialog is drawn once before its OnShowListener restyles it, so for a frame the stock Material
     * dialog showed through. Every dialog built on this context (its layout is inflated through it) holds back that
     * first frame until the show listener has run; at most a few frames are held, so a dialog is never left blank.
     */
    private static final class FirstFrameGate extends ContextThemeWrapper {
        private static final java.util.Map<View, Boolean> SEEN = new java.util.WeakHashMap<>();
        private android.view.LayoutInflater inflater;
        FirstFrameGate(Context base, int theme) { super(base, theme); }
        @Override public Object getSystemService(String name) {
            if (!LAYOUT_INFLATER_SERVICE.equals(name)) return super.getSystemService(name);
            if (inflater == null) {
                android.view.LayoutInflater li = android.view.LayoutInflater.from(getBaseContext()).cloneInContext(this);
                try {
                    li.setFactory2(new android.view.LayoutInflater.Factory2() {
                        public View onCreateView(View parent, String tag, Context c, android.util.AttributeSet attrs) { if (parent != null) watch(parent); return null; }
                        public View onCreateView(String tag, Context c, android.util.AttributeSet attrs) { return null; }
                    });
                } catch (RuntimeException alreadySet) { /* another factory owns this inflater: no gate */ }
                inflater = li;
            }
            return inflater;
        }
        private static void watch(View parent) {
            if (SEEN.put(parent, Boolean.TRUE) != null) return;
            parent.addOnAttachStateChangeListener(new View.OnAttachStateChangeListener() {
                public void onViewAttachedToWindow(View v) { v.removeOnAttachStateChangeListener(this); hold(v.getRootView()); }
                public void onViewDetachedFromWindow(View v) {}
            });
        }
        private static void hold(View root) {
            if (SEEN.put(root, Boolean.TRUE) != null) return;
            final boolean[] ready = {false}; final int[] held = {0};
            root.getViewTreeObserver().addOnPreDrawListener(new android.view.ViewTreeObserver.OnPreDrawListener() {
                public boolean onPreDraw() {
                    if (!ready[0] && held[0]++ == 0) root.post(() -> { ready[0] = true; root.invalidate(); });
                    if (ready[0] || held[0] > 6) { root.getViewTreeObserver().removeOnPreDrawListener(this); return true; }
                    return false;
                }
            });
        }
    }

    /** Recolours a content tree: transparent root over the page gradient, inked text, themed controls. */
    static void style(View root) {
        if (root == null) return;
        root.setBackgroundColor(Color.TRANSPARENT);
        styleTree(root);
    }
    private static void styleTree(View view) {
        Context c = view.getContext();
        if (view instanceof TextView) {
            TextView t = (TextView)view;
            t.setTextColor(new ColorStateList(new int[][]{new int[]{-android.R.attr.state_enabled}, new int[]{}}, new int[]{muted(), ink()}));
            t.setLinkTextColor(value()); // keep the OpenStreetMap attribution link legible and tappable
        }
        if (view instanceof Button && !(view instanceof CompoundButton)) {
            Button b = (Button)view;
            b.setBackground(secondaryPill(c)); b.setAllCaps(false); b.setTypeface(bold());
            b.setTextColor(new ColorStateList(new int[][]{new int[]{-android.R.attr.state_enabled}, new int[]{}}, new int[]{muted(), value()}));
            b.setStateListAnimator(null); elevate(b, 1.5f);
            b.setMinHeight(dp(c, Size.PILL_HEIGHT_DP)); b.setMinimumHeight(dp(c, Size.PILL_HEIGHT_DP));
            ViewGroup.LayoutParams lp = b.getLayoutParams();
            if (lp instanceof ViewGroup.MarginLayoutParams) { ViewGroup.MarginLayoutParams m = (ViewGroup.MarginLayoutParams)lp; m.topMargin = Math.max(m.topMargin, dp(c, 4)); m.bottomMargin = Math.max(m.bottomMargin, dp(c, 4)); }
        }
        if (view instanceof CompoundButton) styleCompound((CompoundButton)view);
        if (view instanceof Switch) styleSwitch((Switch)view);
        if (view instanceof android.widget.EditText) styleField((android.widget.EditText)view);
        if (view instanceof ViewGroup) {
            ViewGroup g = (ViewGroup)view;
            for (int i = 0; i < g.getChildCount(); i++) styleTree(g.getChildAt(i));
        }
    }

    /** Gives a whole Activity the page gradient and matching system bars. */
    static void page(Activity activity) {
        Window w = activity.getWindow();
        w.setBackgroundDrawable(pageBackground(activity));
        systemBars(w);
    }
    private static void systemBars(Window w) {
        w.addFlags(android.view.WindowManager.LayoutParams.FLAG_DRAWS_SYSTEM_BAR_BACKGROUNDS);
        w.setStatusBarColor(pageTop()); w.setNavigationBarColor(pageBottom());
        if (Build.VERSION.SDK_INT >= 23) {
            View decor = w.getDecorView(); int flags = decor.getSystemUiVisibility();
            int light = View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR | (Build.VERSION.SDK_INT >= 26 ? View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR : 0);
            decor.setSystemUiVisibility(dark() ? flags & ~light : flags | light);
        }
    }

    /**
     * Dialog chrome and actions in the game's style: white rounded card (unless full screen), centred
     * upper-case slate title with a short hairline, content directly on the card, rounded input boxes,
     * and stacked pill buttons (gradient primary on top, outlined secondary below).
     */
    static void styleButtons(AlertDialog dialog) {
        Window w = dialog.getWindow();
        Context c = dialog.getContext();
        if (w != null) {
            boolean floating = FLOATING.containsKey(w);
            boolean full = w.getAttributes().width == ViewGroup.LayoutParams.MATCH_PARENT && !floating;
            if (!full && !floating) w.setBackgroundDrawable(dialogBackground(c));
            int titleId = c.getResources().getIdentifier("alertTitle", "id", "android");
            View title = titleId == 0 ? null : w.findViewById(titleId);
            if (title instanceof TextView) {
                TextView t = (TextView)title;
                t.setAllCaps(true); t.setLetterSpacing(0.12f); t.setTypeface(medium()); t.setTextSize(17f);
                t.setTextColor(dlgInk()); t.setGravity(Gravity.CENTER);
                t.setSingleLine(false); t.setMaxLines(2); t.setEllipsize(null);
                ViewGroup.LayoutParams lp = t.getLayoutParams(); if (lp != null) { lp.width = ViewGroup.LayoutParams.MATCH_PARENT; t.setLayoutParams(lp); }
                // Short centred hairline under the title, like "PARTY PLAY".
                android.graphics.drawable.LayerDrawable rule = new android.graphics.drawable.LayerDrawable(new Drawable[]{shape(c, dlgHair(), 0, 1)});
                rule.setLayerGravity(0, Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL); rule.setLayerSize(0, dp(c, 110), dp(c, 2));
                t.setBackground(rule); t.setPadding(dp(c, 8), dp(c, 6), dp(c, 8), dp(c, 14));
            }
            View message = w.findViewById(android.R.id.message);
            if (message instanceof TextView) { TextView m = (TextView)message; m.setTextColor(dlgInk()); m.setGravity(Gravity.CENTER_HORIZONTAL); m.setLineSpacing(dp(c, 2), 1f); }
            // Content sits directly on the white card, inset from its rounded edges.
            if (!full) for (String name : new String[]{"contentPanel", "customPanel"}) {
                int id = c.getResources().getIdentifier(name, "id", "android");
                View panel = id == 0 ? null : w.findViewById(id);
                if (panel == null || panel.getVisibility() != View.VISIBLE) continue;
                panel.setBackground(null); panel.setElevation(0); panel.setClipToOutline(false);
                int side = dp(c, floating ? 4 : 16);
                panel.setPadding(side, dp(c, 2), side, dp(c, 4));
                ViewGroup.LayoutParams lp = panel.getLayoutParams();
                if (lp instanceof ViewGroup.MarginLayoutParams) { ViewGroup.MarginLayoutParams m = (ViewGroup.MarginLayoutParams)lp; m.leftMargin = m.rightMargin = 0; panel.setLayoutParams(m); }
            }
            styleDialogTree(w.getDecorView());
        }
        int pillWidth = Math.round(c.getResources().getDisplayMetrics().widthPixels * 0.62f);
        java.util.List<Button> shown = new java.util.ArrayList<>();
        for (int which : new int[]{AlertDialog.BUTTON_POSITIVE, AlertDialog.BUTTON_NEGATIVE, AlertDialog.BUTTON_NEUTRAL}) {
            Button b = dialog.getButton(which);
            if (b == null) continue;
            b.setAllCaps(true); b.setLetterSpacing(0.12f); b.setTypeface(medium()); b.setTextSize(14); b.setStateListAnimator(null);
            b.setMinHeight(dp(c, 46)); b.setMinimumHeight(dp(c, 46)); b.setPadding(dp(c, 24), 0, dp(c, 24), 0);
            if (which == AlertDialog.BUTTON_POSITIVE) {
                b.setBackground(dialogPrimaryPill(c)); b.setTextColor(Color.WHITE); elevate(b, 2);
            } else {
                b.setBackground(dialogSecondaryPill(c)); b.setTextColor(dlgTeal()); b.setElevation(0);
            }
            // Wide pills make the platform button bar stack them vertically (primary on top), as in the game.
            b.setMinWidth(pillWidth); b.setMinimumWidth(pillWidth);
            ViewGroup.LayoutParams lp = b.getLayoutParams();
            if (lp instanceof LinearLayout.LayoutParams) { LinearLayout.LayoutParams m = (LinearLayout.LayoutParams)lp; m.gravity = Gravity.CENTER_HORIZONTAL; m.topMargin = m.bottomMargin = dp(c, 5); m.leftMargin = m.rightMargin = dp(c, 4); b.setLayoutParams(m); }
            if (b.getVisibility() == View.VISIBLE) shown.add(b);
        }
        if (!shown.isEmpty() && shown.get(0).getParent() instanceof LinearLayout) {
            final LinearLayout bar = (LinearLayout)shown.get(0).getParent();
            if (bar.getParent() instanceof View) { View host = (View)bar.getParent(); host.setPadding(host.getPaddingLeft(), dp(c, 6), host.getPaddingRight(), dp(c, 14)); }
            bar.post(() -> {
                if (bar.getOrientation() == LinearLayout.VERTICAL) { bar.setGravity(Gravity.CENTER_HORIZONTAL); return; }
                // A bar that cannot stack (or a single button): centre the pills side by side instead.
                int spacerId = c.getResources().getIdentifier("spacer", "id", "android");
                View spacer = spacerId == 0 ? null : bar.findViewById(spacerId);
                if (spacer != null) spacer.setVisibility(View.GONE);
                bar.setGravity(Gravity.CENTER_HORIZONTAL);
                int visible = 0; for (Button b : shown) if (b.getVisibility() == View.VISIBLE) visible++;
                for (Button b : shown) {
                    if (visible > 1) { b.setMinWidth(0); b.setMinimumWidth(0); }
                    ViewGroup.LayoutParams lp = b.getLayoutParams();
                    if (visible > 1 && lp instanceof LinearLayout.LayoutParams) { LinearLayout.LayoutParams m = (LinearLayout.LayoutParams)lp; m.width = 0; m.weight = 1; b.setLayoutParams(m); }
                }
            });
        }
    }

    /**
     * Game pill on the page background: white with a soft shadow and teal text; the selected one is the
     * lime-to-teal gradient with white text (mode switches, "History"-style buttons).
     */
    static void gamePill(Button b, boolean selected) {
        Context c = b.getContext();
        if (selected) {
            GradientDrawable g = new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT, new int[]{dlgLime(), dlgTeal()});
            g.setCornerRadius(dp(c, 100));
            b.setBackground(new RippleDrawable(ColorStateList.valueOf(0x33ffffff), g, null));
            b.setTextColor(Color.WHITE);
        } else {
            b.setBackground(new RippleDrawable(ColorStateList.valueOf(0x2236b89e), shape(c, card(), 0, 100), null));
            b.setTextColor(accent());
        }
        b.setTypeface(bold()); b.setAllCaps(false); b.setStateListAnimator(null);
        elevate(b, 2f);
    }

    /** Section header on the page (outside the cards): upper-case, bold, slate, like "QUALITY". */
    static void sectionHeader(TextView t) {
        t.setAllCaps(true); t.setTypeface(bold()); t.setTextSize(15); t.setLetterSpacing(0.04f); t.setTextColor(ink());
    }

    /** The game's round close button: white rim, deep teal disc, soft inner ring and a white X. */
    static final class CloseButton extends View {
        private final android.graphics.Paint paint = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
        CloseButton(Context c) {
            super(c);
            setContentDescription("Close"); setFocusable(true); setClickable(true);
            setOutlineProvider(new android.view.ViewOutlineProvider() { public void getOutline(View v, android.graphics.Outline o) { o.setOval(0, 0, v.getWidth(), v.getHeight()); } });
            setForeground(new RippleDrawable(ColorStateList.valueOf(0x33ffffff), null, new android.graphics.drawable.ShapeDrawable(new android.graphics.drawable.shapes.OvalShape())));
            setElevation(dp(c, 4));
        }
        @Override protected void onDraw(android.graphics.Canvas cv) {
            super.onDraw(cv);
            float cx = getWidth() / 2f, cy = getHeight() / 2f, r = Math.min(cx, cy);
            paint.setStyle(android.graphics.Paint.Style.FILL); paint.setColor(Color.WHITE); cv.drawCircle(cx, cy, r, paint);
            paint.setColor(closeFill()); cv.drawCircle(cx, cy, r * 0.86f, paint);
            paint.setStyle(android.graphics.Paint.Style.STROKE); paint.setStrokeWidth(r * 0.05f); paint.setColor(0x66ffffff); cv.drawCircle(cx, cy, r * 0.72f, paint);
            float x = r * 0.26f; paint.setStrokeCap(android.graphics.Paint.Cap.ROUND); paint.setStrokeWidth(r * 0.09f); paint.setColor(Color.WHITE);
            cv.drawLine(cx - x, cy - x, cx + x, cy + x, paint); cv.drawLine(cx - x, cy + x, cx + x, cy - x, paint);
        }
    }

    /**
     * The game's action bar for dialogs: the secondary action (neutral) on the left, the round close
     * button in the centre (it performs the negative action, e.g. Cancel) and the primary action on the
     * right. The platform buttons are moved into a fixed footer under the dialog's own content, so the
     * content scrolls and the actions never do. {@code host} receives the footer as its last child; when
     * null, the dialog's custom view is wrapped so it shrinks above the footer. Call after
     * {@link #styleButtons}, before the first layout (e.g. in the show listener).
     */
    static void gameFooter(AlertDialog dialog, ViewGroup host) {
        Context c = dialog.getContext();
        Window w = dialog.getWindow();
        if (w == null) return;
        Button positive = dialog.getButton(AlertDialog.BUTTON_POSITIVE), negative = dialog.getButton(AlertDialog.BUTTON_NEGATIVE),
            neutral = dialog.getButton(AlertDialog.BUTTON_NEUTRAL);
        LinearLayout footer = new LinearLayout(c);
        footer.setOrientation(LinearLayout.HORIZONTAL);
        footer.setGravity(Gravity.CENTER_VERTICAL);
        footer.setPadding(dp(c, 6), dp(c, 10), dp(c, 6), dp(c, 6));
        footer.setClipToPadding(false); footer.setClipChildren(false);
        footer.addView(footerSlot(c, neutral, false), new LinearLayout.LayoutParams(0, dp(c, 46), 1));
        CloseButton close = new CloseButton(c);
        close.setOnClickListener(v -> { if (negative != null && negative.getText() != null && negative.getText().length() > 0) negative.performClick(); else dialog.dismiss(); });
        if (negative != null && negative.getText() != null) close.setContentDescription(negative.getText());
        LinearLayout.LayoutParams cl = new LinearLayout.LayoutParams(dp(c, 56), dp(c, 56));
        cl.leftMargin = cl.rightMargin = dp(c, 10);
        footer.addView(close, cl);
        footer.addView(footerSlot(c, positive, true), new LinearLayout.LayoutParams(0, dp(c, 46), 1));
        int barId = c.getResources().getIdentifier("buttonPanel", "id", "android");
        View bar = barId == 0 ? null : w.findViewById(barId);
        if (bar != null) bar.setVisibility(View.GONE);
        if (host != null) { host.addView(footer, new LinearLayout.LayoutParams(-1, -2)); return; }
        int customId = c.getResources().getIdentifier("custom", "id", "android");
        View customView = customId == 0 ? null : w.findViewById(customId);
        if (!(customView instanceof ViewGroup) || ((ViewGroup)customView).getChildCount() == 0) {
            // No custom view (message dialogs): put the footer where the buttons were.
            if (bar instanceof ViewGroup && bar.getParent() instanceof ViewGroup) {
                ViewGroup parent = (ViewGroup)bar.getParent();
                parent.addView(footer, parent.indexOfChild(bar) + 1, new LinearLayout.LayoutParams(-1, -2));
            }
            return;
        }
        ViewGroup custom = (ViewGroup)customView;
        View content = custom.getChildAt(0);
        custom.removeView(content);
        LinearLayout wrapper = new LinearLayout(c);
        wrapper.setOrientation(LinearLayout.VERTICAL);
        wrapper.addView(content, new LinearLayout.LayoutParams(-1, 0, 1));
        // Hairline between the scrolling content and the fixed actions.
        View rule = new View(c); rule.setBackgroundColor(dlgHair());
        wrapper.addView(rule, new LinearLayout.LayoutParams(-1, Math.max(1, dp(c, 1))));
        wrapper.addView(footer, new LinearLayout.LayoutParams(-1, -2));
        custom.addView(wrapper, new android.widget.FrameLayout.LayoutParams(-1, -2));
        // The custom panel may shrink to fit the window, so the content above the footer scrolls.
        for (String name : new String[]{"customPanel", "custom"}) {
            int id = c.getResources().getIdentifier(name, "id", "android");
            View panel = id == 0 ? null : w.findViewById(id);
            if (panel != null) panel.setMinimumHeight(0);
        }
    }

    /** A footer slot: the moved platform button styled as a pill, or an empty spacer keeping the close centred. */
    private static View footerSlot(Context c, Button b, boolean primary) {
        if (b == null || b.getText() == null || b.getText().length() == 0) {
            View spacer = new View(c); spacer.setVisibility(View.INVISIBLE); return spacer;
        }
        if (b.getParent() instanceof ViewGroup) ((ViewGroup)b.getParent()).removeView(b);
        b.setMinWidth(0); b.setMinimumWidth(0); b.setMinHeight(0); b.setMinimumHeight(0);
        b.setPadding(dp(c, 8), 0, dp(c, 8), 0); b.setTextSize(13.5f); b.setLetterSpacing(0.1f); b.setSingleLine(true);
        b.setAllCaps(true); b.setTypeface(medium()); b.setStateListAnimator(null);
        if (primary) { b.setBackground(dialogPrimaryPill(c)); b.setTextColor(new ColorStateList(new int[][]{new int[]{-android.R.attr.state_enabled}, new int[]{}}, new int[]{0xccffffff, Color.WHITE})); elevate(b, 2); }
        else { b.setBackground(dialogSecondaryPill(c)); b.setTextColor(new ColorStateList(new int[][]{new int[]{-android.R.attr.state_enabled}, new int[]{}}, new int[]{dlgDisabled(), dlgTeal()})); b.setElevation(0); }
        return b;
    }

    /** Big bold page title (full-screen game pages such as Feeds and the world map), no hairline. */
    static void pageTitle(AlertDialog dialog) {
        Window w = dialog.getWindow();
        Context c = dialog.getContext();
        int titleId = c.getResources().getIdentifier("alertTitle", "id", "android");
        View title = w == null || titleId == 0 ? null : w.findViewById(titleId);
        if (title instanceof TextView) {
            TextView t = (TextView) title;
            t.setTextSize(22); t.setTypeface(bold()); t.setLetterSpacing(0.04f); t.setBackground(null);
            t.setPadding(dp(c, 8), dp(c, 10), dp(c, 8), dp(c, 2));
        }
    }

    /**
     * Makes an AlertDialog fill the screen on the page gradient, while padding its title, content and
     * action buttons clear of the status/navigation bars, display cutout and IME so nothing is clipped.
     */
    static void fullScreen(AlertDialog dialog) {
        Window w = dialog.getWindow();
        if (w == null) return;
        w.setBackgroundDrawable(pageBackground(dialog.getContext()));
        systemBars(w);
        if (Build.VERSION.SDK_INT >= 30) w.setDecorFitsSystemWindows(false);
        w.addFlags(android.view.WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN);
        // AlertDialog normally wraps its internal panel even when the window fills the display.
        // Stretch that panel and its custom-content frame so weighted history/map content fills it.
        stretchContent(dialog, w);
        w.setLayout(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
        final View decor = w.getDecorView();
        decor.setOnApplyWindowInsetsListener((v, insets) -> { applyInsets(v, insets); return insets; });
        decor.requestApplyInsets();
    }
    /** Lets the custom content take the remaining height while the title and buttons keep their own. */
    private static void stretchContent(AlertDialog dialog, Window w) {
        for (String name : new String[]{"parentPanel", "customPanel", "custom"}) {
            int id = dialog.getContext().getResources().getIdentifier(name, "id", "android");
            View panel = id == 0 ? null : w.findViewById(id);
            if (panel == null || panel.getLayoutParams() == null) continue;
            ViewGroup.LayoutParams lp = panel.getLayoutParams();
            if (name.equals("customPanel") && lp instanceof LinearLayout.LayoutParams) {
                LinearLayout.LayoutParams l = (LinearLayout.LayoutParams)lp; l.height = 0; l.weight = 1; panel.setLayoutParams(l);
            } else { lp.height = ViewGroup.LayoutParams.MATCH_PARENT; panel.setLayoutParams(lp); }
        }
    }
    private static final java.util.Map<Window, Boolean> FLOATING = new java.util.WeakHashMap<>();
    /**
     * A screen-sized dialog drawn as a floating white card (like the regular game dialogs) instead of a
     * full-bleed page: a margin around it, dim behind, and padding clear of the system bars and IME.
     */
    static void floatingFullScreen(AlertDialog dialog) {
        Window w = dialog.getWindow();
        if (w == null) return;
        FLOATING.put(w, true);
        final Context c = dialog.getContext();
        if (Build.VERSION.SDK_INT >= 30) w.setDecorFitsSystemWindows(false);
        w.addFlags(android.view.WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN | android.view.WindowManager.LayoutParams.FLAG_DIM_BEHIND
            | android.view.WindowManager.LayoutParams.FLAG_DRAWS_SYSTEM_BAR_BACKGROUNDS);
        w.setDimAmount(0.45f); w.setStatusBarColor(Color.TRANSPARENT); w.setNavigationBarColor(Color.TRANSPARENT);
        stretchContent(dialog, w);
        w.setLayout(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
        final int m = dp(c, 12);
        final View decor = w.getDecorView();
        decor.setOnApplyWindowInsetsListener((v, insets) -> {
            applyInsets(v, insets);
            int l = v.getPaddingLeft() + m, t = v.getPaddingTop() + m, r = v.getPaddingRight() + m, b = v.getPaddingBottom() + m;
            w.setBackgroundDrawable(new android.graphics.drawable.InsetDrawable(dialogBackground(c), l, t, r, b));
            v.setPadding(l, t + dp(c, 6), r, b + dp(c, 4));
            return insets;
        });
        decor.requestApplyInsets();
    }
    /** Buttons inside dialog content: game secondary pills (white, teal outline, teal text), no shadow. */
    static void styleGameButtons(View v) {
        if (v instanceof Button && !(v instanceof CompoundButton)) {
            Button b = (Button)v; Context c = b.getContext();
            gameSelected(b, false); b.setStateListAnimator(null); b.setElevation(0); b.setTypeface(medium());
            b.setMinHeight(dp(c, 40)); b.setMinimumHeight(dp(c, 40));
            ViewGroup.LayoutParams lp = b.getLayoutParams();
            if (lp instanceof ViewGroup.MarginLayoutParams && b.getParent() instanceof LinearLayout && ((LinearLayout)b.getParent()).getOrientation() == LinearLayout.HORIZONTAL) {
                ViewGroup.MarginLayoutParams mm = (ViewGroup.MarginLayoutParams)lp; mm.leftMargin = mm.rightMargin = dp(c, 3); b.setLayoutParams(mm);
            }
        }
        if (v instanceof ViewGroup) { ViewGroup g = (ViewGroup)v; for (int i = 0; i < g.getChildCount(); i++) styleGameButtons(g.getChildAt(i)); }
    }
    /** Selected option: solid teal pill with white text; otherwise the outlined secondary pill. */
    static void gameSelected(Button b, boolean on) {
        Context c = b.getContext();
        if (on) { b.setBackground(new RippleDrawable(ColorStateList.valueOf(0x33ffffff), shape(c, dlgTeal(), 0, 100), null)); b.setTextColor(Color.WHITE); }
        else { b.setBackground(dialogSecondaryPill(c)); b.setTextColor(new ColorStateList(new int[][]{new int[]{-android.R.attr.state_enabled}, new int[]{}}, new int[]{dlgDisabled(), dlgTeal()})); }
        b.setTypeface(medium());
    }
    private static void applyInsets(View v, WindowInsets insets) {
        if (insets == null) return;
        int top, bottom, left, right;
        if (Build.VERSION.SDK_INT >= 30) {
            android.graphics.Insets bars = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.ime());
            android.graphics.Insets safe = insets.getInsetsIgnoringVisibility(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
            top = Math.max(bars.top, safe.top); bottom = Math.max(bars.bottom, safe.bottom);
            left = Math.max(bars.left, safe.left); right = Math.max(bars.right, safe.right);
        } else {
            top = insets.getSystemWindowInsetTop(); bottom = insets.getSystemWindowInsetBottom();
            left = insets.getSystemWindowInsetLeft(); right = insets.getSystemWindowInsetRight();
        }
        v.setPadding(left, top, right, bottom);
    }

    static AlertDialog showListDialog(Context context, String title, String[] items, final android.content.DialogInterface.OnClickListener listener) {
        LinearLayout layout = new LinearLayout(context);
        layout.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(context, 12);
        layout.setPadding(pad, pad, pad, pad);
        ScrollView scroll = new ScrollView(context);
        LinearLayout list = new LinearLayout(context);
        list.setOrientation(LinearLayout.VERTICAL);
        scroll.addView(list, new ScrollView.LayoutParams(-1, -2));
        layout.addView(scroll, new LinearLayout.LayoutParams(-1, -2));

        AlertDialog dialog = new AlertDialog.Builder(context)
            .setTitle(title)
            .setView(layout)
            .setNegativeButton("Close", null)
            .create();

        list.setBackground(mintBlock(context));
        list.setPadding(dp(context, 12), dp(context, 2), dp(context, 12), dp(context, 2));
        java.util.List<TextView> rows = new java.util.ArrayList<>();
        for (int i = 0; i < items.length; i++) {
            final int index = i;
            if (i > 0) { View line = new View(context); line.setBackgroundColor(dlgMintLine()); list.addView(line, new LinearLayout.LayoutParams(-1, Math.max(1, dp(context, 1)))); }
            TextView row = new TextView(context);
            row.setText(items[i]); row.setTextSize(15); row.setGravity(Gravity.CENTER_VERTICAL | Gravity.START);
            row.setMinHeight(dp(context, 48)); row.setPadding(dp(context, 4), dp(context, 8), dp(context, 4), dp(context, 8));
            row.setClickable(true); row.setFocusable(true); row.setBackground(rowRipple(context));
            row.setOnClickListener(v -> {
                listener.onClick(dialog, index);
                dialog.dismiss();
            });
            list.addView(row, new LinearLayout.LayoutParams(-1, -2)); rows.add(row);
        }

        FeedTheme.style(layout);
        for (TextView row : rows) row.setTextColor(dlgInk());
        Window w = dialog.getWindow();
        if (w != null) {
            w.setBackgroundDrawable(dialogBackground(context));
        }
        dialog.setOnShowListener(d -> FeedTheme.styleButtons(dialog));
        return dialog;
    }
}
