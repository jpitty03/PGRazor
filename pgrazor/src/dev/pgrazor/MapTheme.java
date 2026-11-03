package dev.pgrazor;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.os.Build;
import android.view.ContextThemeWrapper;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowInsets;
import android.widget.Button;
import android.widget.CompoundButton;
import android.widget.TextView;

/**
 * Shared dark/light palette and full-screen helpers so the world map and cooldown history match the
 * PGRazor settings panel ({@link Pad}). Map tiles keep their original raster colours; only the
 * surrounding chrome — text, buttons, checkboxes and nested dialogs — is themed.
 */
final class MapTheme {
    private MapTheme() {}

    static boolean dark() { return Control.darkTheme(); }
    static int ink()     { return dark() ? Color.rgb(235, 241, 246) : Color.rgb(25, 35, 47); }
    static int muted()   { return dark() ? Color.rgb(154, 171, 187) : Color.rgb(88, 105, 121); }
    static int surface() { return dark() ? Color.rgb(20, 27, 37)    : Color.rgb(249, 251, 253); }
    static int accent()  { return dark() ? Color.rgb(69, 225, 186)  : Color.rgb(0, 116, 92); }

    /** A dialog-themed context so nested AlertDialogs follow the chosen dark/light appearance. */
    static Context wrap(Activity activity) {
        return new ContextThemeWrapper(activity, dark()
            ? android.R.style.Theme_Material_Dialog_Alert : android.R.style.Theme_Material_Light_Dialog_Alert);
    }

    static int dp(Context c, float value) { return Math.round(value * c.getResources().getDisplayMetrics().density); }

    private static GradientDrawable rounded(Context c, int color, int radius) {
        GradientDrawable d = new GradientDrawable(); d.setColor(color); d.setCornerRadius(dp(c, radius)); return d;
    }

    /**
     * The app's button look: a raised fill with a thin outline, inset slightly so buttons placed side
     * by side keep a visible gap instead of merging into one block.
     */
    static android.graphics.drawable.Drawable buttonBackground(Context c) {
        GradientDrawable fill = rounded(c, dark() ? 0xff202b39 : 0xffedf2f6, 9);
        fill.setStroke(dp(c, 1), dark() ? 0xff3a4d63 : 0xffc9d5df);
        int inset = dp(c, 2);
        return new RippleDrawable(ColorStateList.valueOf(0x3345e1ba), new android.graphics.drawable.InsetDrawable(fill, inset), null);
    }

    /** Applies {@link #buttonBackground}; an inset background would otherwise replace the label's padding. */
    static void setButtonBackground(View view) {
        int l = view.getPaddingLeft(), t = view.getPaddingTop(), r = view.getPaddingRight(), b = view.getPaddingBottom();
        view.setBackground(buttonBackground(view.getContext()));
        view.setPadding(l, t, r, b);
    }

    /** Recolours a panel: surface background, inked text, and the panel's buttons/checkboxes. */
    static void style(View root) {
        if (root == null) return;
        root.setBackgroundColor(surface());
        styleTree(root);
    }
    private static void styleTree(View view) {
        Context c = view.getContext();
        if (view instanceof TextView) {
            TextView t = (TextView)view;
            t.setTextColor(new ColorStateList(new int[][]{new int[]{-android.R.attr.state_enabled}, new int[]{}}, new int[]{muted(), ink()}));
            t.setLinkTextColor(accent()); // keep the OpenStreetMap attribution link legible and tappable
        }
        if (view instanceof Button && !(view instanceof CompoundButton)) setButtonBackground(view);
        if (view instanceof CompoundButton) {
            ((CompoundButton)view).setButtonTintList(new ColorStateList(new int[][]{new int[]{android.R.attr.state_checked}, new int[]{}}, new int[]{accent(), muted()}));
        }
        if (view instanceof ViewGroup) {
            ViewGroup g = (ViewGroup)view;
            for (int i = 0; i < g.getChildCount(); i++) styleTree(g.getChildAt(i));
        }
    }

    /** Accent the dialog's action buttons so Teleport / Walk here / Done stay obvious. */
    static void styleButtons(AlertDialog dialog) {
        for (int which : new int[]{AlertDialog.BUTTON_POSITIVE, AlertDialog.BUTTON_NEGATIVE, AlertDialog.BUTTON_NEUTRAL}) {
            Button b = dialog.getButton(which);
            if (b != null) { b.setTextColor(accent()); b.setAllCaps(false); }
        }
    }

    /**
     * Makes an AlertDialog fill the screen on a solid surface, while padding its title, content and
     * action buttons clear of the status/navigation bars, display cutout and IME so nothing is clipped.
     */
    static void fullScreen(AlertDialog dialog) {
        Window w = dialog.getWindow();
        if (w == null) return;
        w.setBackgroundDrawable(new ColorDrawable(surface()));
        w.addFlags(android.view.WindowManager.LayoutParams.FLAG_DRAWS_SYSTEM_BAR_BACKGROUNDS);
        w.setStatusBarColor(surface()); w.setNavigationBarColor(surface());
        if (Build.VERSION.SDK_INT >= 30) w.setDecorFitsSystemWindows(false);
        w.addFlags(android.view.WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN);
        // AlertDialog normally wraps its internal panel even when the window fills the display.
        // Stretch that panel and its custom-content frame so weighted history/map content fills it.
        for (String name : new String[]{"parentPanel", "customPanel", "custom"}) {
            int id = dialog.getContext().getResources().getIdentifier(name, "id", "android");
            View panel = id == 0 ? null : w.findViewById(id);
            if (panel != null && panel.getLayoutParams() != null) {
                ViewGroup.LayoutParams lp = panel.getLayoutParams();
                lp.height = ViewGroup.LayoutParams.MATCH_PARENT; panel.setLayoutParams(lp);
            }
        }
        w.setLayout(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
        final View decor = w.getDecorView();
        decor.setOnApplyWindowInsetsListener((v, insets) -> { applyInsets(v, insets); return insets; });
        decor.requestApplyInsets();
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
}
