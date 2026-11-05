package dev.pgrazor;

import android.content.Context;
import android.graphics.*;
import android.graphics.drawable.GradientDrawable;
import android.view.*;
import android.widget.*;

/**
 * Shared PGRazor settings styling. Single home for the theme palette and the recursive
 * {@code styleTree} implementation so {@link Pad} (overlays/launcher/joystick) and
 * {@link SettingsPanel} (the settings body) apply identical colors without duplicating it.
 * PGRazorX: the palette and sizes come from the {@link FeedTheme} game-style design system.
 */
/** PR #2 settings widgets (RowButton, styleTree), used only by the PoGoCoordinates feed. */
final class FeedUi {
    private FeedUi() {}

    static void enabled(View view,boolean enabled){
        view.setAlpha(enabled?1f:0.45f); enableChildren(view,enabled);
    }
    private static void enableChildren(View view,boolean enabled){
        view.setEnabled(enabled);
        if(view instanceof ViewGroup){ViewGroup group=(ViewGroup)view;for(int i=0;i<group.getChildCount();i++)enableChildren(group.getChildAt(i),enabled);}
    }

    static int dp(Context context, float x) { return Math.round(x * context.getResources().getDisplayMetrics().density); }
    static int ink() { return FeedTheme.ink(); }
    static int muted() { return FeedTheme.muted(); }
    static int surface() { return FeedTheme.surface(); }
    static int accent() { return FeedTheme.accent(); }
    static GradientDrawable rounded(Context context,int color,int radius){GradientDrawable d=new GradientDrawable();d.setColor(color);d.setCornerRadius(dp(context,radius));return d;}

    static Context dialogContext(Context context){return FeedTheme.wrap((android.app.Activity)context);}

    static TextView text(Context context,String label,int size,int color){
        TextView v = new TextView(context); v.setText(label); v.setTextSize(size); v.setTextColor(color); return v;
    }
    /** A list row with a chevron, as used throughout the game's settings. */
    static Button button(Context context,String label){
        Button v = new RowButton(context); v.setText(label); v.setAllCaps(false); v.setTextSize(FeedTheme.Size.ROW_TEXT_SP); return v;
    }
    /** A plain floating button for on-map overlays (berry picker, cooldown timer). */
    static Button overlayButton(Context context,String label){
        Button v = new Button(context); v.setText(label); v.setAllCaps(false); v.setTextSize(13); v.setStateListAnimator(null); return v;
    }
    /** Shrinks a fixed-size overlay's text (down to 8sp) when its label would not fit; maxSp is the configured size. */
    static void fitText(TextView t,float maxSp){
        int max=Math.max(9,Math.round(maxSp));
        t.setAutoSizeTextTypeUniformWithConfiguration(8,max,1,android.util.TypedValue.COMPLEX_UNIT_SP);
    }
    /** Oval or proportional round-rect outline so custom-drawn buttons cast the soft game shadow. */
    static ViewOutlineProvider roundRectOutline(float radiusFraction){
        return new ViewOutlineProvider(){public void getOutline(View v,Outline o){int w=v.getWidth(),h=v.getHeight();if(radiusFraction<0)o.setOval(0,0,w,h);else o.setRoundRect(0,0,w,h,Math.min(w,h)*radiusFraction);}};
    }

    /** "Label .......... Value >" row: the label stays left, the current value is drawn in teal on the right. */
    static void setOption(Button b,String label,String value,boolean enabled){
        b.setEnabled(enabled);
        if(b instanceof RowButton){b.setText(label);((RowButton)b).setValue(value);return;}
        android.text.SpannableString t=new android.text.SpannableString(label+"   "+value);int start=label.length()+3;
        t.setSpan(new android.text.style.ForegroundColorSpan(enabled?FeedTheme.value():muted()),start,t.length(),0);b.setText(t);
    }

    /** True when the nearest visible sibling above is also a list row, so a divider belongs between them. */
    private static boolean rowAbove(View v){
        if(!(v.getParent() instanceof ViewGroup))return false;ViewGroup p=(ViewGroup)v.getParent();
        for(int i=p.indexOfChild(v)-1;i>=0;i--){View s=p.getChildAt(i);if(s.getVisibility()==View.GONE)continue;return s instanceof RowButton||s instanceof RowLayout;}
        return false;
    }
    private static final Paint DIVIDER=new Paint();
    private static void drawRowDivider(View v,Canvas c){if(!rowAbove(v))return;DIVIDER.setColor(FeedTheme.outline());c.drawRect(0,0,v.getWidth(),Math.max(1,v.getResources().getDisplayMetrics().density),DIVIDER);}
    /** A list row container (switch rows, info rows) that draws its own top divider. */
    static final class RowLayout extends LinearLayout {
        RowLayout(Context c){super(c);setWillNotDraw(false);}
        protected void onDraw(Canvas c){super.onDraw(c);drawRowDivider(this,c);}
    }
    /** A game-style navigation row: bold slate label, optional teal value on the right, grey chevron. */
    static final class RowButton extends Button {
        private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
        private String value;
        private String drawnValue;
        RowButton(Context c){super(c);setSingleLine(false);setMaxLines(Integer.MAX_VALUE);setAllCaps(false);setGravity(Gravity.CENTER_VERTICAL|Gravity.START);setStateListAnimator(null);paint.setTypeface(FeedTheme.medium());}
        private int dp(float x){return FeedUi.dp(getContext(),x);}
        void setValue(String v){if(v!=null&&v.equals(value))return;value=v;updateReserve();invalidate();}
        /** Keeps room on the right for the value and chevron so a long label wraps instead of overlapping. */
        void updateReserve(){updateReserve(getWidth());}
        private void updateReserve(int width){
            paint.setTextSize(getTextSize()*0.94f);
            int budget=width>0?Math.max(0,(width-dp(38))*2/5):dp(120);
            drawnValue=value==null?null:android.text.TextUtils.ellipsize(value,new android.text.TextPaint(paint),budget,android.text.TextUtils.TruncateAt.END).toString();
            int reserve=dp(26)+(drawnValue==null?0:Math.round(paint.measureText(drawnValue))+dp(12));
            if(getPaddingRight()!=reserve)setPadding(0,dp(6),reserve,dp(6));
        }
        protected void onMeasure(int widthSpec,int heightSpec){
            updateReserve(View.MeasureSpec.getSize(widthSpec));super.onMeasure(widthSpec,heightSpec);
        }
        protected void onDraw(Canvas c){super.onDraw(c);drawRowDivider(this,c);
            float cy=getHeight()/2f,right=getWidth()-dp(4);
            paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(dp(1.8f));paint.setStrokeCap(Paint.Cap.ROUND);paint.setStrokeJoin(Paint.Join.ROUND);paint.setColor(FeedTheme.chevron());
            float h=dp(6);c.drawLine(right-h,cy-h,right,cy,paint);c.drawLine(right,cy,right-h,cy+h,paint);
            if(value!=null){paint.setStyle(Paint.Style.FILL);paint.setTextSize(getTextSize()*0.94f);paint.setTextAlign(Paint.Align.RIGHT);paint.setColor(isEnabled()?FeedTheme.value():muted());
                c.drawText(drawnValue==null?"":drawnValue,right-dp(18),cy-(paint.descent()+paint.ascent())/2f,paint);}
        }
    }

    static void styleTree(Context context,View view){
        if(view instanceof TextView){TextView t=(TextView)view;
            t.setTextColor(new android.content.res.ColorStateList(new int[][]{new int[]{-android.R.attr.state_enabled},new int[]{}},new int[]{muted(),ink()}));
            if(!(view instanceof Button))t.setPadding(0,dp(context,3),0,dp(context,3));
        }
        if(view instanceof RowButton){RowButton b=(RowButton)view;b.setTextSize(FeedTheme.Size.ROW_TEXT_SP);b.setTypeface(FeedTheme.bold());b.setMinHeight(dp(context,FeedTheme.Size.ROW_HEIGHT_DP));b.setMinimumHeight(dp(context,FeedTheme.Size.ROW_HEIGHT_DP));b.setBackground(FeedTheme.rowRipple(context));b.updateReserve();}
        else if(view instanceof Button&&!(view instanceof CompoundButton)){Button b=(Button)view;b.setBackground(FeedTheme.secondaryPill(context));b.setTypeface(FeedTheme.bold());b.setTextColor(FeedTheme.value());b.setStateListAnimator(null);FeedTheme.elevate(b,1.5f);}
        if(view instanceof CompoundButton){FeedTheme.styleCompound((CompoundButton)view);
            if(!(view instanceof Switch)){TextView t=(TextView)view;t.setTextSize(FeedTheme.Size.ROW_TEXT_SP-1);t.setTypeface(FeedTheme.medium());t.setMinHeight(dp(context,44));t.setPadding(dp(context,8),dp(context,6),0,dp(context,6));}}
        if(view instanceof Switch){Switch s=(Switch)view;s.setText("");s.setMinHeight(dp(context,48));s.setMinimumHeight(dp(context,48));s.setMinimumWidth(dp(context,48));s.setPadding(dp(context,8),0,0,0);FeedTheme.styleSwitch(s);}
        if(view instanceof EditText)FeedTheme.styleField((EditText)view);
        if(view instanceof ViewGroup){ViewGroup group=(ViewGroup)view;for(int i=0;i<group.getChildCount();i++)styleTree(context,group.getChildAt(i));}
    }
}
