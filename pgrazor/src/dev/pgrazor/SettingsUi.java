package dev.pgrazor;

import android.content.Context;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.view.*;
import android.widget.*;

/**
 * Shared PGRazor settings styling. Single home for the theme palette and the recursive
 * {@code styleTree} implementation so {@link Pad} (overlays/launcher/joystick) and
 * {@link SettingsPanel} (the settings body) apply identical colors without duplicating it.
 */
final class SettingsUi {
    private SettingsUi() {}

    static void enabled(View view,boolean enabled){
        view.setAlpha(enabled?1f:0.45f); enableChildren(view,enabled);
    }
    private static void enableChildren(View view,boolean enabled){
        view.setEnabled(enabled);
        if(view instanceof ViewGroup){ViewGroup group=(ViewGroup)view;for(int i=0;i<group.getChildCount();i++)enableChildren(group.getChildAt(i),enabled);}
    }

    static int dp(Context context, float x) { return Math.round(x * context.getResources().getDisplayMetrics().density); }
    static int ink() { return Control.darkTheme()?Color.rgb(235,241,246):Color.rgb(25,35,47); }
    static int muted() { return Control.darkTheme()?Color.rgb(154,171,187):Color.rgb(88,105,121); }
    static int surface() { return Control.darkTheme()?Color.rgb(20,27,37):Color.rgb(249,251,253); }
    static int accent() { return Control.darkTheme()?Color.rgb(69,225,186):Color.rgb(0,116,92); }
    static GradientDrawable rounded(Context context,int color,int radius){GradientDrawable d=new GradientDrawable();d.setColor(color);d.setCornerRadius(dp(context,radius));return d;}

    static Context dialogContext(Context context){return new ContextThemeWrapper(context,Control.darkTheme()?android.R.style.Theme_Material_Dialog_Alert:android.R.style.Theme_Material_Light_Dialog_Alert);}

    static TextView text(Context context,String label,int size,int color){
        TextView v = new TextView(context); v.setText(label); v.setTextSize(size); v.setTextColor(color); return v;
    }
    static Button button(Context context,String label){
        Button v = new Button(context); v.setText(label); v.setAllCaps(false); v.setTextSize(13); return v;
    }

    static void styleTree(Context context,View view){
        if(view instanceof TextView){TextView t=(TextView)view;
            t.setTextColor(new android.content.res.ColorStateList(new int[][]{new int[]{-android.R.attr.state_enabled},new int[]{}},new int[]{muted(),ink()}));
            if(!(view instanceof Button))t.setPadding(dp(context,4),dp(context,3),dp(context,4),dp(context,3));
        }
        if(view instanceof Button){TextView t=(TextView)view;t.setTextSize(14);t.setMinHeight(dp(context,48));t.setPadding(dp(context,10),dp(context,6),dp(context,10),dp(context,6));t.setGravity(Gravity.CENTER_VERTICAL|Gravity.START);
            if(!(view instanceof CompoundButton))view.setBackground(new android.graphics.drawable.RippleDrawable(android.content.res.ColorStateList.valueOf(0x2845e1ba),rounded(context,Control.darkTheme()?0xff202b39:0xffedf2f6,9),null));
        }
        if(view instanceof CompoundButton)((CompoundButton)view).setButtonTintList(new android.content.res.ColorStateList(new int[][]{new int[]{-android.R.attr.state_enabled},new int[]{android.R.attr.state_checked},new int[]{}},new int[]{muted(),accent(),muted()}));
        if(view instanceof Switch){Switch s=(Switch)view;int off=Control.darkTheme()?0xff4a5a6a:0xffb4c2ce;s.setText("");s.setMinHeight(0);s.setPadding(dp(context,8),0,0,0);
            s.setThumbTintList(new android.content.res.ColorStateList(new int[][]{new int[]{-android.R.attr.state_enabled},new int[]{android.R.attr.state_checked},new int[]{}},new int[]{0xff6b7883,accent(),off}));
            s.setTrackTintList(new android.content.res.ColorStateList(new int[][]{new int[]{-android.R.attr.state_enabled},new int[]{android.R.attr.state_checked},new int[]{}},new int[]{0x33808080,(accent()&0x00ffffff)|0x66000000,(off&0x00ffffff)|0x55000000}));
        }
        if(view instanceof ViewGroup){ViewGroup group=(ViewGroup)view;for(int i=0;i<group.getChildCount();i++)styleTree(context,group.getChildAt(i));}
    }
}
