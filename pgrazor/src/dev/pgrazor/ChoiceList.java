package dev.pgrazor;

import android.app.AlertDialog;
import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.text.SpannableStringBuilder;
import android.text.Spanned;
import android.text.style.AbsoluteSizeSpan;
import android.text.style.ForegroundColorSpan;
import android.view.Gravity;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.util.function.IntConsumer;

/**
 * Dialog options drawn as buttons. A plain AlertDialog item list reads as text, so each option here
 * is a rounded, tappable card in the app's button style: the first line of a label is the option,
 * any further lines a muted detail. Attach after create() and before show(); a tap dismisses the
 * dialog and then reports the option's index.
 */
public final class ChoiceList {
    private ChoiceList() {}

    public static AlertDialog attach(AlertDialog dialog, String[] labels, IntConsumer pick) {
        Context c = dialog.getContext();
        LinearLayout list = new LinearLayout(c); list.setOrientation(LinearLayout.VERTICAL);
        list.setPadding(dp(c, 20), dp(c, 8), dp(c, 20), dp(c, 4));
        for (int i = 0; i < labels.length; i++) {
            final int index = i;
            TextView option = new TextView(c);
            option.setText(label(c, labels[i]));
            option.setGravity(Gravity.CENTER_VERTICAL | Gravity.START);
            option.setMinHeight(dp(c, 52));
            option.setPadding(dp(c, 16), dp(c, 10), dp(c, 16), dp(c, 10));
            GradientDrawable card = new GradientDrawable();
            card.setColor(MapTheme.dark() ? 0xff202b39 : 0xffedf2f6); card.setCornerRadius(dp(c, 10));
            card.setStroke(dp(c, 1), MapTheme.dark() ? 0xff3a4d63 : 0xffc9d5df);
            option.setBackground(new RippleDrawable(ColorStateList.valueOf(0x3345e1ba), card, null));
            option.setClickable(true); option.setFocusable(true);
            option.setOnClickListener(v -> { dialog.dismiss(); pick.accept(index); });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2); lp.bottomMargin = dp(c, 8);
            list.addView(option, lp);
        }
        ScrollView scroll = new ScrollView(c); scroll.addView(list);
        dialog.setView(scroll);
        return dialog;
    }

    /** First line in the ink color, the rest smaller and muted. */
    private static CharSequence label(Context c, String text) {
        int split = text.indexOf('\n');
        SpannableStringBuilder s = new SpannableStringBuilder(text);
        s.setSpan(new ForegroundColorSpan(MapTheme.ink()), 0, split < 0 ? text.length() : split, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        s.setSpan(new AbsoluteSizeSpan(15, true), 0, split < 0 ? text.length() : split, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        if (split >= 0) {
            s.setSpan(new ForegroundColorSpan(MapTheme.muted()), split, text.length(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
            s.setSpan(new AbsoluteSizeSpan(13, true), split, text.length(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        }
        return s;
    }

    private static int dp(Context c, float value) { return MapTheme.dp(c, value); }
}
