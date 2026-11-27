package dev.pgrazor;

import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.text.SpannableStringBuilder;
import android.text.TextUtils;
import android.text.style.ForegroundColorSpan;
import android.text.style.TypefaceSpan;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.CompoundButton;
import android.widget.EditText;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;

/**
 * Shared PGRazor design language. One home for the palette, shape, type and spacing tokens plus the
 * recursive {@code styleTree} so {@link Pad} (overlays/launcher/joystick), {@link SettingsPanel} (the
 * settings body), {@link QuickMenu} and the dialogs that opt in all draw from the same system.
 *
 * <p>Tokens are deliberately plain ints and small drawables: no resource images, no custom fonts,
 * no animation. Every surface is a single {@link GradientDrawable} (or a ripple around one), so a
 * theme switch allocates a handful of shapes once instead of per frame.
 */
final class SettingsUi {
    private SettingsUi() {}

    // ---------------------------------------------------------------- spacing and shape scale
    static final int SPACE_1 = 4, SPACE_2 = 8, SPACE_3 = 12, SPACE_4 = 16, SPACE_5 = 20, SPACE_6 = 24;
    static final int RADIUS_CONTROL = 10, RADIUS_FIELD = 12, RADIUS_CARD = 14, RADIUS_SHEET = 20;
    /** The widest the settings column grows before it centres on tablets. */
    static final int CONTENT_MAX_DP = 620;

    // ---------------------------------------------------------------- type scale (sp)
    static final float TYPE_TITLE = 20, TYPE_SECTION = 14, TYPE_BODY = 14, TYPE_META = 12, TYPE_MICRO = 11;

    // ---------------------------------------------------------------- palette
    static boolean dark() { return Control.darkTheme(); }
    /** Full-screen backdrop behind cards. */
    static int bg()      { return dark() ? 0xff0d131a : 0xfff3f6f9; }
    /** Cards, dialogs, menus, floating pills. */
    static int surface() { return dark() ? 0xff121a23 : 0xffffffff; }
    /** Fields, action rows and the second surface step. */
    static int raised()  { return dark() ? 0xff19222d : 0xfff7fafc; }
    /** Hairline borders on cards, fields and pills. */
    static int border()  { return dark() ? 0xff25303c : 0xffdce4ec; }
    static int ink()     { return dark() ? 0xffe9eff5 : 0xff16212c; }
    static int muted()   { return dark() ? 0xff9aabbc : 0xff5c6d7c; }
    static int faint()   { return dark() ? 0xff71828f : 0xff7e8e9c; }
    static int accent()  { return dark() ? 0xff45e1ba : 0xff00745c; }
    /** A solid accent fill dark enough for white text in both themes (primary buttons, close discs). */
    static int accentFill() { return dark() ? 0xff0b6e58 : 0xff00745c; }
    /** Text drawn on an accent fill. */
    static int accentInk() { return dark() ? 0xff06231d : 0xffffffff; }
    static int accentSoft() { return (accent() & 0x00ffffff) | 0x1f000000; }
    static int success() { return dark() ? 0xff4ed8a0 : 0xff0f7b53; }
    static int warn()    { return dark() ? 0xfff2b75e : 0xff9a5b00; }
    static int danger()  { return dark() ? 0xfff07a72 : 0xffb3261e; }
    static int rippleColor() { return (accent() & 0x00ffffff) | 0x33000000; }

    static int dp(Context context, float x) { return Math.round(x * context.getResources().getDisplayMetrics().density); }

    /**
     * Flips the status/navigation bar glyphs with the palette. The panel draws inside the game's
     * window, so its bars keep white glyphs unless we ask for dark ones; without this the clock and
     * battery wash out on the light backdrop.
     */
    static void systemBars(android.app.Activity activity, boolean darkIcons) {
        android.view.Window window = activity.getWindow();
        if (window == null) return;
        android.view.View decor = window.getDecorView();
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            // SystemUI reads WindowInsetsController appearance; the legacy flags alone are not
            // reflected in it on this platform, which leaves the clock white on a light bar.
            android.view.WindowInsetsController controller = window.getInsetsController();
            if (controller != null && decor.isAttachedToWindow()) {
                int mask = android.view.WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS
                    | android.view.WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS;
                controller.setSystemBarsAppearance(darkIcons ? mask : 0, mask);
            }
        }
        int flags = decor.getSystemUiVisibility();
        int mask = android.view.View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR | android.view.View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR;
        int next = darkIcons ? flags | mask : flags & ~mask;
        if (next != flags) decor.setSystemUiVisibility(next);
    }

    // ---------------------------------------------------------------- type
    static Typeface medium() { return Typeface.create("sans-serif-medium", Typeface.NORMAL); }
    static Typeface mono() { return Typeface.MONOSPACE; }

    static TextView text(Context context, String label, int size, int color) {
        TextView v = new TextView(context);
        v.setText(label); v.setTextSize(size); v.setTextColor(color); v.setLineSpacing(0, 1.15f);
        return v;
    }
    static TextView title(Context context, String label) {
        TextView v = text(context, label, (int) TYPE_TITLE, ink()); v.setTypeface(medium()); return v;
    }
    static TextView sectionLabel(Context context, String label) {
        TextView v = text(context, label, (int) TYPE_SECTION, muted()); v.setTypeface(medium()); return v;
    }
    static TextView caption(Context context, String label) {
        TextView v = text(context, label, (int) TYPE_META, muted()); return v;
    }

    // ---------------------------------------------------------------- shape
    static GradientDrawable rounded(Context context, int color, int radius) {
        GradientDrawable d = new GradientDrawable(); d.setColor(color); d.setCornerRadius(dp(context, radius)); return d;
    }
    static GradientDrawable shape(Context context, int color, int radius, int stroke) {
        GradientDrawable d = rounded(context, color, radius); d.setStroke(Math.max(1, dp(context, 1)), stroke); return d;
    }
    static Drawable cardBackground(Context context) { return shape(context, surface(), RADIUS_CARD, border()); }
    static Drawable fieldBackground(Context context) { return shape(context, raised(), RADIUS_FIELD, border()); }
    static Drawable rowBackground(Context context) { return ripple(shape(context, raised(), RADIUS_CONTROL, border())); }
    static Drawable pillBackground(Context context) { return shape(context, surface(), RADIUS_FIELD, border()); }
    static Drawable pressable(Context context) { return new RippleDrawable(ColorStateList.valueOf(rippleColor()), null, null); }
    static Drawable accentPill(Context context) { return rounded(context, accentSoft(), 8); }
    static Drawable ripple(Drawable content) { return new RippleDrawable(ColorStateList.valueOf(rippleColor()), content, null); }

    /** A 1 dp hairline for {@code LinearLayout.setDividerDrawable}, with no extra view per row. */
    static Drawable hairline(Context context) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(border());
        d.setSize(1, Math.max(1, dp(context, 1)));
        return d;
    }

    /** Option lists: a hairline before each control or row, none between an option and the description under it. */
    static boolean dividerBefore(ViewGroup list, int index) {
        if (index <= 0 || index >= list.getChildCount()) return false;
        View child = list.getChildAt(index);
        if (child.getVisibility() == View.GONE || isNote(child)) return false;
        for (int i = index - 1; i >= 0; i--) if (list.getChildAt(i).getVisibility() != View.GONE) return true;
        return false;
    }
    private static boolean isNote(View v) {
        return v instanceof TextView && !(v instanceof Button) && !(v instanceof android.widget.EditText) && !v.isClickable();
    }
    private static final Paint DIVIDER = new Paint();
    /** Draws the dividerBefore hairlines over a vertical list, in the gap above each qualifying child. */
    static void drawDividers(ViewGroup list, Canvas canvas, int inset) {
        DIVIDER.setColor(border());
        int h = Math.max(1, dp(list.getContext(), 1));
        int previous = -1;
        for (int i = 0; i < list.getChildCount(); i++) {
            View child = list.getChildAt(i);
            if (child.getVisibility() == View.GONE) continue;
            if (previous >= 0 && dividerBefore(list, i)) {
                int top = (list.getChildAt(previous).getBottom() + child.getTop() - h) / 2;  // middle of the gap
                canvas.drawRect(list.getPaddingLeft() + inset, top, list.getWidth() - list.getPaddingRight() - inset, top + h, DIVIDER);
            }
            previous = i;
        }
    }
    /**
     * A vertical list that separates option + description pairs with hairlines (see dividerBefore), spaced
     * like the Settings rows: room around each option, descriptions smaller and quieter.
     */
    static final class OptionList extends android.widget.LinearLayout {
        OptionList(Context context) { super(context); setOrientation(VERTICAL); }
        @Override public void addView(View child, int index, ViewGroup.LayoutParams params) {
            MarginLayoutParams m = params instanceof MarginLayoutParams ? (MarginLayoutParams) params : new LayoutParams(params);
            Context c = getContext();
            if (isNote(child)) {
                TextView t = (TextView) child;
                t.setTextSize(12); t.setLineSpacing(0, 1.25f); t.setAlpha(0.75f);
                t.setPadding(dp(c, 6), 0, dp(c, 6), 0);
                m.topMargin = dp(c, 2); m.bottomMargin = dp(c, 10);
            } else {
                m.topMargin = dp(c, 10); m.bottomMargin = dp(c, 2);
            }
            super.addView(child, index, m);
        }
        @Override protected void dispatchDraw(Canvas canvas) { super.dispatchDraw(canvas); drawDividers(this, canvas, 0); }
    }

    /** A transparent 1 x dp spacer usable as a LinearLayout middle divider. */
    static Drawable gap(Context context, int heightDp) {
        GradientDrawable d = new GradientDrawable(); d.setColor(Color.TRANSPARENT); d.setSize(1, dp(context, heightDp)); return d;
    }

    static View divider(Context context) {
        View v = new View(context); v.setBackgroundColor(border()); return v;
    }

    // ---------------------------------------------------------------- text colours / controls
    static ColorStateList textColors() {
        return new ColorStateList(new int[][]{new int[]{-android.R.attr.state_enabled}, new int[]{}}, new int[]{faint(), ink()});
    }
    static ColorStateList switchThumb() {
        int off = dark() ? 0xff4a5a6a : 0xffb4c2ce;
        return new ColorStateList(new int[][]{new int[]{-android.R.attr.state_enabled}, new int[]{android.R.attr.state_checked}, new int[]{}},
            new int[]{dark() ? 0xff3a4652 : 0xffc7d2dc, accent(), off});
    }
    static ColorStateList switchTrack() {
        int off = dark() ? 0xff2c3742 : 0xffd7e0e7;
        return new ColorStateList(new int[][]{new int[]{-android.R.attr.state_enabled}, new int[]{android.R.attr.state_checked}, new int[]{}},
            new int[]{dark() ? 0xff232c35 : 0xffeef2f5, (accent() & 0x00ffffff) | 0x66000000, off});
    }
    static ColorStateList choiceTint() {
        return new ColorStateList(new int[][]{new int[]{-android.R.attr.state_enabled}, new int[]{android.R.attr.state_checked}, new int[]{}},
            new int[]{faint(), accent(), muted()});
    }

    static void enabled(View view, boolean enabled) {
        float alpha = enabled ? 1f : 0.45f;
        if (view.getAlpha() != alpha) view.setAlpha(alpha);
        enableChildren(view, enabled);
    }
    private static void enableChildren(View view, boolean enabled) {
        if (view.isEnabled() != enabled) view.setEnabled(enabled);
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); i++) enableChildren(group.getChildAt(i), enabled);
        }
    }

    static Context dialogContext(Context context) {
        return new android.view.ContextThemeWrapper(context,
            Control.darkTheme() ? android.R.style.Theme_Material_Dialog_Alert : android.R.style.Theme_Material_Light_Dialog_Alert);
    }

    static Button button(Context context, String label) { return new RowButton(context, label); }

    /**
     * A flat settings action row. Text of the form {@code "Label: value"} renders the value in the
     * accent colour so scanning a page reads as labels with their current state, without a second
     * view per row. Re-setting the same text is a no-op at the caller, so refresh ticks stay cheap.
     */
    static final class RowButton extends Button {
        /** Inside a group card the row is flat: the card paints the surface and the divider separates rows. */
        private boolean flat;
        RowButton(Context context, String label) {
            super(context);
            setAllCaps(false);
            setTextSize(TYPE_BODY);
            setGravity(Gravity.CENTER_VERTICAL | Gravity.START);
            setMinHeight(dp(context, 52)); setMinimumHeight(dp(context, 52));
            setPadding(dp(context, 14), dp(context, 8), dp(context, 14), dp(context, 8));
            setTextColor(textColors());
            setBackground(rowBackground(context));
            setMaxLines(2);
            setEllipsize(TextUtils.TruncateAt.END);
            if (label != null && !label.isEmpty()) setText(label);
            applyTheme();
        }
        void setFlat(boolean value) {
            if (flat == value) return;
            flat = value; applyTheme();
        }
        @Override public void setText(CharSequence text, BufferType type) { super.setText(format(text), BufferType.NORMAL); }
        void applyTheme() {
            setBackground(flat ? pressable(getContext()) : rowBackground(getContext()));
            setTextColor(textColors());
            CharSequence text = getText();
            if (text != null && text.length() > 0) super.setText(format(text.toString()), BufferType.NORMAL);
        }
        private SpannableStringBuilder format(CharSequence source) {
            String text = source == null ? "" : source.toString();
            SpannableStringBuilder out = new SpannableStringBuilder(text);
            int split = text.indexOf(": ");
            if (split <= 0 || split + 2 >= text.length()) return out;
            int valueStart = split + 2;
            out.setSpan(new ForegroundColorSpan(accent()), valueStart, out.length(), SpannableStringBuilder.SPAN_EXCLUSIVE_EXCLUSIVE);
            out.setSpan(new TypefaceSpan(medium()), valueStart, out.length(), SpannableStringBuilder.SPAN_EXCLUSIVE_EXCLUSIVE);
            return out;
        }
    }

    // ---------------------------------------------------------------- recursive theming
    /**
     * Applies the palette to an already-built tree. Called on a dark/light switch and when a dialog
     * opens; never per frame. Sizes and paddings are chosen at construction so this stays cheap.
     */
    static void styleTree(Context context, View view) {
        if (view.getTag(TAG_CARD) != null) {
            view.setBackground(cardBackground(context));
        } else if (view.getTag(TAG_CARD_ROW) != null) {
            view.setBackground(ripple(cardBackground(context)));
        }
        if (view instanceof EditText && view.getTag(TAG_PLAIN_FIELD) == null) {
            EditText field = (EditText) view;
            field.setTextColor(ink()); field.setHintTextColor(faint());
            field.setBackground(fieldBackground(context));
            int pad = dp(context, 12);
            field.setPadding(pad, dp(context, 8), pad, dp(context, 8));
        } else if (view instanceof RowButton) {
            ((RowButton) view).applyTheme();
        } else if (view instanceof TextView) {
            ((TextView) view).setTextColor(textColors());
        }
        if (view instanceof Button && !(view instanceof CompoundButton) && !(view instanceof RowButton)) {
            Button button = (Button) view;
            button.setAllCaps(false); button.setTextSize(TYPE_BODY);
            button.setMinHeight(dp(context, 48));
            button.setPadding(dp(context, 12), dp(context, 8), dp(context, 12), dp(context, 8));
            button.setBackground(rowBackground(context));
        }
        if (view instanceof CompoundButton) {
            CompoundButton choice = (CompoundButton) view;
            choice.setButtonTintList(choiceTint());
            choice.setTextColor(textColors());
        }
        if (view instanceof SeekBar) {
            SeekBar bar = (SeekBar) view;
            bar.setProgressTintList(ColorStateList.valueOf(accent()));
            bar.setThumbTintList(ColorStateList.valueOf(accent()));
            bar.setProgressBackgroundTintList(ColorStateList.valueOf(border()));
        }
        if (view instanceof Switch) {
            Switch s = (Switch) view;
            s.setText(""); s.setMinHeight(0);
            s.setPadding(dp(context, 8), 0, 0, 0);
            s.setThumbTintList(switchThumb()); s.setTrackTintList(switchTrack());
        }
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); i++) styleTree(context, group.getChildAt(i));
        }
    }

    /** Marks a field whose parent already paints its own surface (the search field), so theming leaves it alone. */
    static final int TAG_PLAIN_FIELD = 0x0f0f0001;
    /** Marks the content column of a settings group with its display label for search indexing. */
    static final int TAG_GROUP = 0x0f0f0002;
    /** Marks a collapsible section's content with its header view, so search can open it before scrolling. */
    static final int TAG_SECTION_HEAD = 0x0f0f0003;
    /** Marks a card surface so a theme switch repaints it instead of leaving a dark card in light mode. */
    static final int TAG_CARD = 0x0f0f0004;
    /** Marks a tappable card row: same surface as a card, with the standard ripple. */
    static final int TAG_CARD_ROW = 0x0f0f0005;
    /** Marks a row that is not a switch, checkbox or button (a slider row) with its search name. */
    static final int TAG_SEARCH_ROW = 0x0f0f0006;

    // ---------------------------------------------------------------- small drawn glyphs
    /** A quiet row chevron: points right, or down when {@link #open} is set. */
    static final class Chevron extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        boolean open;
        Chevron(Context context) {
            super(context);
            setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
        }
        @Override protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            float cx = getWidth() / 2f, cy = getHeight() / 2f, r = dp(getContext(), 4);
            paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(dp(getContext(), 1.7f));
            paint.setStrokeCap(Paint.Cap.ROUND); paint.setStrokeJoin(Paint.Join.ROUND); paint.setColor(faint());
            android.graphics.Path path = new android.graphics.Path();
            if (open) {
                path.moveTo(cx - r, cy - r / 2f); path.lineTo(cx, cy + r / 2f); path.lineTo(cx + r, cy - r / 2f);
            } else {
                path.moveTo(cx - r / 2f, cy - r); path.lineTo(cx + r / 2f, cy); path.lineTo(cx - r / 2f, cy + r);
            }
            canvas.drawPath(path, paint);
        }
    }

    /** A magnifier drawn at 18dp with the same stroke as the other chrome glyphs. */
    static final class SearchGlyph extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        SearchGlyph(Context context) { super(context); setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO); }
        @Override protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            float cx = getWidth() * 0.44f, cy = getHeight() * 0.42f, r = Math.min(getWidth(), getHeight()) * 0.26f;
            paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(dp(getContext(), 1.7f));
            paint.setStrokeCap(Paint.Cap.ROUND); paint.setColor(faint());
            canvas.drawCircle(cx, cy, r, paint);
            canvas.drawLine(cx + r * 0.72f, cy + r * 0.72f, cx + r * 1.55f, cy + r * 1.55f, paint);
        }
    }

    /** An X drawn inside a 40dp hitbox, used to clear the settings search. */
    static final class ClearGlyph extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        ClearGlyph(Context context) {
            super(context);
            setFocusable(true); setClickable(true);
            setContentDescription("Clear search");
            setBackground(new RippleDrawable(ColorStateList.valueOf(rippleColor()), null, rounded(getContext(), 0xffffffff, 20)));
        }
        @Override protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            float cx = getWidth() / 2f, cy = getHeight() / 2f, r = dp(getContext(), 4.5f);
            paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(dp(getContext(), 1.7f));
            paint.setStrokeCap(Paint.Cap.ROUND); paint.setColor(muted());
            canvas.drawLine(cx - r, cy - r, cx + r, cy + r, paint);
            canvas.drawLine(cx - r, cy + r, cx + r, cy - r, paint);
        }
    }

    /** A filled status dot used by the home telemetry strip; its colour can change without reallocating the view. */
    static final class Dot extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private int color;
        Dot(Context context, int color) { super(context); this.color = color; setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO); }
        void setColor(int value) {
            if (color == value) return;
            color = value; invalidate();
        }
        @Override protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            paint.setStyle(Paint.Style.FILL); paint.setColor(color);
            canvas.drawCircle(getWidth() / 2f, getHeight() / 2f, Math.min(getWidth(), getHeight()) / 2f, paint);
        }
    }
}
