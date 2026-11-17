package dev.pgrazor;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.graphics.drawable.ColorDrawable;
import android.os.Build;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowInsets;
import android.widget.Button;
import android.widget.TextView;

/**
 * Shared dark/light palette and full-screen helpers so the world map and cooldown history match the
 * PGRazor settings panel ({@link Pad}). Every colour delegates to {@link SettingsUi}, the single token
 * source. Map tiles keep their original raster colours; only the surrounding chrome — text, buttons,
 * checkboxes and nested dialogs — is themed.
 */
final class MapTheme {
    private MapTheme() {}

    static boolean dark() { return Control.darkTheme(); }
    static int ink()     { return SettingsUi.ink(); }
    static int muted()   { return SettingsUi.muted(); }
    static int surface() { return SettingsUi.surface(); }
    static int accent()  { return SettingsUi.accent(); }

    /** A dialog-themed context so nested AlertDialogs follow the chosen dark/light appearance. */
    static Context wrap(Activity activity) {
        return SettingsUi.dialogContext(activity);
    }

    static int dp(Context c, float value) { return SettingsUi.dp(c, value); }

    /** The app's button look: a raised fill with a hairline outline and the shared accent ripple. */
    static android.graphics.drawable.Drawable buttonBackground(Context c) {
        return SettingsUi.ripple(SettingsUi.shape(c, SettingsUi.raised(), 9, SettingsUi.border()));
    }

    /** Applies {@link #buttonBackground}; the shared shape carries no inset, so label padding is untouched. */
    static void setButtonBackground(View view) {
        view.setBackground(buttonBackground(view.getContext()));
    }

    /** Recolours a panel: page background, themed controls, and legible link text. */
    static void style(View root) {
        if (root == null) return;
        root.setBackgroundColor(SettingsUi.bg());
        SettingsUi.styleTree(root.getContext(), root);
        links(root);
    }
    private static void links(View view) {
        if (view instanceof TextView) ((TextView) view).setLinkTextColor(accent());
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); i++) links(group.getChildAt(i));
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
