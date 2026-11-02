package dev.pgrazor;

import android.app.*;
import android.content.*;
import android.graphics.*;
import android.graphics.drawable.GradientDrawable;
import android.os.*;
import android.text.InputType;
import android.view.*;
import android.widget.*;
import java.util.Locale;

/**
 * The full-screen PGRazor settings body: every switch, checkbox, status caption and section,
 * plus their construction and per-frame refresh. Extracted from {@link Pad}, which keeps overlay
 * attachment/lifecycle, the launcher/quick menu and the joystick. This panel never touches those
 * overlays directly; the few cross-cutting actions it triggers go through {@link Host} so Pad's
 * internals stay private. Settings keys and the {@link Control} API are unchanged.
 * PGRazorX: game-style layout (brand header, white cards, list rows, round close button).
 */
final class SettingsPanel {
    /** Narrow callback surface back into Pad: everything the settings body needs that lives with the overlays/joystick. */
    interface Host {
        /** Full refresh (overlays + this panel); matches Pad's existing single refresh entry point. */
        void refresh();
        /** Re-theme overlays and this panel after a dark/light toggle. */
        void applyTheme();
        /** Release the location joystick, optionally preserving an in-progress walk. */
        void releaseJoystick(boolean preserveWalk);
        /** Persist the joystick-visible choice and reflect it on the overlay Pad owns. */
        void setJoystickVisible(boolean visible);
        /** Open the shared World map (also used by the quick menu). */
        void openMap();
        /** Open the coordinates & speed editor (tied to engine/joystick state Pad owns). */
        void editLocation();
        /** PGRazorX: re-apply the map overlay opacity after the slider moves. */
        void applyOverlayOpacity();
    }

    private final Activity activity;
    private final Host host;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final java.util.List<AlertDialog> itemDialogs = new java.util.ArrayList<>();
    private final LinearLayout panel, body;
    private final ScrollView bodyScroll;
    private TextView coords, speedValue;
    private View closeIcon, themeIcon;
    private Switch incubatorSwitch;
    private Button incubatorPriority;
    private TextView incubatorStatus;
    private TextView eggStatus,manualStatus;
    private Switch manualSwitch;
    private Switch autoHealSwitch,autoHealHalfSwitch;
    private TextView autoHealStatus;
    private Switch eggSkipSwitch,eggKeepSwitch,eggObserveSwitch;
    // Primary action and cycle/dialog rows.
    private Switch throwRandomSwitch;
    private Button mapTap, throwGrade, itemSettings, cameraSample, interactionCapture, transferKeepList;
    // Boolean settings use proper switches.
    private Switch idleWalkSwitch, locationSwitch, vgpSwitch, joystickSwitch, cooldownSwitch, spawnSwitch, autoTransferSwitch,
        quickSpinSwitch, berrySwitch, quickSummarySwitch, curveSwitch, alwaysHitSwitch, arSwitch, ivCatchSwitch, ivOwnedSwitch, transferOverlaySwitch;
    private TextView quickSummaryLabel, vgpStatus;
    private final CheckBox[] vgpChoices=new CheckBox[3];
    private CheckBox vgpCooldownStop;
    // Status / caption lines (muted, readiness driven).
    private TextView state, steeringStatus, spawnStatus, quickSummaryStatus, transferStatus, quickSpinStatus,
        buddyStatus, giftStatus, itemStatus, rocketStatus, battleStatus, quickBallStatus, ivStatus, cameraStatus, interactionStatus;
    // Multi-option groups keep checkboxes / radios.
    private final Switch[] transferProtection=new Switch[6];
    private final Switch[] giftChoices=new Switch[3];
    private final Switch[] rocketChoices=new Switch[5];
    private final Switch[] battleChoices=new Switch[10];
    private final Switch[] quickMenuChoices=new Switch[QuickMenu.CUSTOMIZABLE.length];
    private Runnable buddyRefresh=()->{},smartRefresh=()->{};
    // Section roots gated by backend readiness, refreshed without recreation.
    private View catchRoot, buddyRoot, giftRoot, itemsRoot, rocketRoot, battleRoot, quickBallRoot;
    private LinearLayout cameraRoot, interactionRoot;
    private final java.util.List<TextView> sectionHeads=new java.util.ArrayList<>();
    private final java.util.List<TextView> captions=new java.util.ArrayList<>();
    // PGRazorX: game-style card/list pieces restyled on every theme change.
    private final java.util.List<View> cards=new java.util.ArrayList<>();
    private final java.util.List<View> dividers=new java.util.ArrayList<>();
    private final java.util.List<TextView> sectionTitles=new java.util.ArrayList<>();
    private final java.util.List<TextView> rowLabels=new java.util.ArrayList<>();
    private final java.util.List<TextView> infoValues=new java.util.ArrayList<>();
    private final java.util.List<View> chevrons=new java.util.ArrayList<>();
    private TextView discordLink;
    private View brandLogo;
    private SeekBar opacitySlider;

    private int dp(float x){return SettingsUi.dp(activity,x);}
    private int ink(){return SettingsUi.ink();}
    private int muted(){return SettingsUi.muted();}
    private int surface(){return SettingsUi.surface();}
    private int accent(){return SettingsUi.accent();}
    private GradientDrawable rounded(int color,int radius){return SettingsUi.rounded(activity,color,radius);}
    private void styleTree(View view){SettingsUi.styleTree(activity,view);}
    private Context dialogContext(){return SettingsUi.dialogContext(activity);}
    private TextView text(String label,int size,int color){return SettingsUi.text(activity,label,size,color);}
    private Button button(String label){return SettingsUi.button(activity,label);}
    private void setOption(Button b,String label,String value,boolean enabled){SettingsUi.setOption(b,label,value,enabled);}

    /** A white game-style card: rows sit directly inside it, separated by inset hairlines. */
    private LinearLayout card(LinearLayout owner){
        LinearLayout c=new LinearLayout(activity);c.setOrientation(LinearLayout.VERTICAL);c.setPadding(dp(MapTheme.Size.CARD_PADDING_DP),dp(2),dp(MapTheme.Size.CARD_PADDING_DP),dp(2));
        LinearLayout.LayoutParams lp=new LinearLayout.LayoutParams(-1,-2);lp.bottomMargin=dp(MapTheme.Size.CARD_GAP_DP);lp.leftMargin=lp.rightMargin=dp(2);owner.addView(c,lp);cards.add(c);return c;
    }
    /** A hairline divider, used between a section header and its rows. */
    private View divider(LinearLayout owner){View line=new View(activity);owner.addView(line,new LinearLayout.LayoutParams(-1,Math.max(1,dp(1))));dividers.add(line);return line;}
    /** Two-column info row ("Position ... 4.71, -74.07"), like the game's Version row. Returns the value view. */
    private TextView infoRow(LinearLayout owner,String label,Runnable onTap){
        SettingsUi.RowLayout r=new SettingsUi.RowLayout(activity);r.setOrientation(LinearLayout.HORIZONTAL);r.setGravity(Gravity.CENTER_VERTICAL);r.setMinimumHeight(dp(MapTheme.Size.ROW_HEIGHT_DP));
        TextView l=new TextView(activity);l.setText(label);r.addView(l,new LinearLayout.LayoutParams(0,-2,0.4f));rowLabels.add(l);
        TextView v=new TextView(activity);v.setGravity(Gravity.END|Gravity.CENTER_VERTICAL);v.setSingleLine(false);v.setMaxLines(Integer.MAX_VALUE);
        LinearLayout.LayoutParams vl=new LinearLayout.LayoutParams(0,-2,0.6f);vl.leftMargin=dp(12);r.addView(v,vl);infoValues.add(v);
        if(onTap!=null){r.setClickable(true);r.setFocusable(true);r.setBackground(MapTheme.rowRipple(activity));r.setOnClickListener(x->onTap.run());}
        owner.addView(r);return v;
    }
    /** A collapsible group. Content is built directly into the returned container, so no post-hoc index moves. */
    private LinearLayout section(LinearLayout owner,String label,boolean open){
        // Top-level sections: an upper-case header on the page (like the game's "QUALITY") above a
        // white card holding the rows; nested ones sit inside their parent card behind a hairline.
        boolean top=owner==body;
        if(top){
            LinearLayout group=new LinearLayout(activity);group.setOrientation(LinearLayout.VERTICAL);
            LinearLayout head=new LinearLayout(activity);head.setOrientation(LinearLayout.HORIZONTAL);head.setGravity(Gravity.CENTER_VERTICAL);
            head.setPadding(dp(10),dp(10),dp(6),dp(6));head.setMinimumHeight(dp(44));head.setClickable(true);head.setFocusable(true);
            TextView title=new TextView(activity);title.setText(label);MapTheme.sectionHeader(title);head.addView(title,new LinearLayout.LayoutParams(0,-2,1f));
            Chevron chev=new Chevron(activity);chev.setRotation(open?90:0);head.addView(chev,new LinearLayout.LayoutParams(dp(28),dp(28)));
            LinearLayout content=new LinearLayout(activity);content.setOrientation(LinearLayout.VERTICAL);
            content.setPadding(dp(MapTheme.Size.CARD_PADDING_DP),dp(2),dp(MapTheme.Size.CARD_PADDING_DP),dp(6));content.setVisibility(open?View.VISIBLE:View.GONE);cards.add(content);
            head.setContentDescription(label+(open?", expanded":", collapsed"));
            new Fold(head,content,chev,label,open); // PGRazorX: same slide for every title
            group.addView(head);
            LinearLayout.LayoutParams cl=new LinearLayout.LayoutParams(-1,-2);cl.bottomMargin=dp(MapTheme.Size.CARD_GAP_DP);group.addView(content,cl);
            LinearLayout.LayoutParams lp=new LinearLayout.LayoutParams(-1,-2);lp.leftMargin=lp.rightMargin=dp(2);owner.addView(group,lp);
            sectionTitles.add(title);chevrons.add(chev);
            return content;
        }
        LinearLayout group=new LinearLayout(activity);group.setOrientation(LinearLayout.VERTICAL);
        if(top){group.setPadding(dp(MapTheme.Size.CARD_PADDING_DP),0,dp(MapTheme.Size.CARD_PADDING_DP),0);cards.add(group);}
        else divider(group);
        LinearLayout head=new LinearLayout(activity);head.setOrientation(LinearLayout.HORIZONTAL);head.setGravity(Gravity.CENTER_VERTICAL);head.setMinimumHeight(dp(MapTheme.Size.ROW_HEIGHT_DP+(top?4:0)));head.setClickable(true);head.setFocusable(true);
        head.setBackground(MapTheme.rowRipple(activity));
        TextView title=new TextView(activity);title.setText(label);title.setTextSize(MapTheme.Size.ROW_TEXT_SP+(top?1:0));head.addView(title,new LinearLayout.LayoutParams(0,-2,1f));
        Chevron chev=new Chevron(activity);chev.setRotation(open?90:0);head.addView(chev,new LinearLayout.LayoutParams(dp(28),dp(28)));
        LinearLayout content=new LinearLayout(activity);content.setOrientation(LinearLayout.VERTICAL);content.setPadding(0,0,0,dp(6));content.setVisibility(open?View.VISIBLE:View.GONE);
        divider(content);
        head.setContentDescription(label+(open?", expanded":", collapsed"));
        new Fold(head,content,chev,label,open); // PGRazorX: same slide for every title
        group.addView(head);group.addView(content);
        LinearLayout.LayoutParams lp=new LinearLayout.LayoutParams(-1,-2);if(top){lp.bottomMargin=dp(MapTheme.Size.CARD_GAP_DP);lp.leftMargin=lp.rightMargin=dp(2);}owner.addView(group,lp);
        sectionTitles.add(title);chevrons.add(chev);
        return content;
    }
    /** A toggle row with a short muted explanation under it, like the game's settings. */
    private Switch switchRow(LinearLayout owner,String label,String description){
        Switch sw=switchRow(owner,label);
        View row=(View)sw.getParent();
        if(row instanceof SettingsUi.RowLayout){
            SettingsUi.RowLayout r=(SettingsUi.RowLayout)row;
            LinearLayout line=new LinearLayout(activity);line.setOrientation(LinearLayout.HORIZONTAL);line.setGravity(Gravity.CENTER_VERTICAL);
            while(r.getChildCount()>0){View c=r.getChildAt(0);ViewGroup.LayoutParams lp=c.getLayoutParams();r.removeViewAt(0);line.addView(c,lp);}
            r.setOrientation(LinearLayout.VERTICAL);r.setPadding(0,dp(8),0,dp(10));
            r.addView(line,new LinearLayout.LayoutParams(-1,-2));
            TextView d=new TextView(activity);d.setText(description);d.setTextSize(MapTheme.Size.CAPTION_SP);d.setPadding(0,dp(2),dp(4),0);
            r.addView(d,new LinearLayout.LayoutParams(-1,-2));captions.add(d);
        }
        return sw;
    }
    private Switch switchRow(LinearLayout owner,String label){
        SettingsUi.RowLayout r=new SettingsUi.RowLayout(activity);r.setOrientation(LinearLayout.HORIZONTAL);r.setGravity(Gravity.CENTER_VERTICAL);r.setMinimumHeight(dp(MapTheme.Size.ROW_HEIGHT_DP));
        TextView t=new TextView(activity);t.setText(label);r.addView(t,new LinearLayout.LayoutParams(0,-2,1f));rowLabels.add(t);
        Switch sw=new Switch(activity);sw.setContentDescription(label);sw.setClickable(true);sw.setMinimumWidth(dp(48));sw.setMinimumHeight(dp(48));r.addView(sw,new LinearLayout.LayoutParams(-2,-2));
        owner.addView(r);return sw;
    }
    private TextView caption(LinearLayout owner,String s){TextView t=new TextView(activity);t.setText(s);t.setTextSize(MapTheme.Size.CAPTION_SP);owner.addView(t);captions.add(t);return t;}

    /**
     * PGRazorX: every section title, top-level or nested, opens and closes the same way: the content slides
     * (height and fade, 180 ms) while the chevron turns. A tap during the slide reverses it from where it is;
     * with system animations off it switches at once.
     */
    private final class Fold {
        private final View head, content, chev; private final String label; private boolean open;
        private android.animation.ValueAnimator anim;
        Fold(View head,View content,View chev,String label,boolean open){
            this.head=head;this.content=content;this.chev=chev;this.label=label;this.open=open;
            head.setOnClickListener(v->toggle());
        }
        void toggle(){
            open=!open;
            head.setContentDescription(label+(open?", expanded":", collapsed"));
            chev.animate().rotation(open?90:0).setDuration(180).start();
            android.animation.ValueAnimator old=anim;anim=null;if(old!=null)old.cancel();
            View parent=(View)content.getParent();
            int width=parent==null?0:parent.getWidth()-parent.getPaddingLeft()-parent.getPaddingRight();
            boolean animate=width>0&&(Build.VERSION.SDK_INT<26||android.animation.ValueAnimator.areAnimatorsEnabled());
            ViewGroup.LayoutParams lp=content.getLayoutParams();
            if(!animate){lp.height=ViewGroup.LayoutParams.WRAP_CONTENT;content.setLayoutParams(lp);content.setAlpha(1f);content.setVisibility(open?View.VISIBLE:View.GONE);return;}
            int from=content.getVisibility()==View.VISIBLE?content.getHeight():0;
            int to=0;
            if(open){
                content.measure(View.MeasureSpec.makeMeasureSpec(width,View.MeasureSpec.EXACTLY),View.MeasureSpec.makeMeasureSpec(0,View.MeasureSpec.UNSPECIFIED));
                to=content.getMeasuredHeight();
                lp.height=from;content.setLayoutParams(lp);content.setVisibility(View.VISIBLE);
            }
            final float a0=content.getAlpha(),a1=open?1f:0f;
            final android.animation.ValueAnimator va=android.animation.ValueAnimator.ofInt(from,to);
            va.setDuration(180);
            va.setInterpolator(open?new android.view.animation.DecelerateInterpolator():new android.view.animation.AccelerateInterpolator());
            va.addUpdateListener(x->{lp.height=(Integer)x.getAnimatedValue();content.setAlpha(a0+(a1-a0)*x.getAnimatedFraction());content.setLayoutParams(lp);});
            va.addListener(new android.animation.AnimatorListenerAdapter(){@Override public void onAnimationEnd(android.animation.Animator x){
                if(anim!=va)return; // replaced by a newer tap
                anim=null;lp.height=ViewGroup.LayoutParams.WRAP_CONTENT;content.setLayoutParams(lp);content.setAlpha(1f);
                if(!open)content.setVisibility(View.GONE);
            }});
            anim=va;va.start();
        }
    }
    /** Grey right-pointing list chevron; rotated 90 degrees (pointing down) when its section is open. */
    private final class Chevron extends View {
        private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
        Chevron(Context c){super(c);setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);}
        protected void onDraw(Canvas c){super.onDraw(c);float cx=getWidth()/2f,cy=getHeight()/2f,h=dp(6);
            paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(dp(1.8f));paint.setStrokeCap(Paint.Cap.ROUND);paint.setStrokeJoin(Paint.Join.ROUND);paint.setColor(MapTheme.chevron());
            Path p=new Path();p.moveTo(cx-h/2f,cy-h);p.lineTo(cx+h/2f,cy);p.lineTo(cx-h/2f,cy+h);c.drawPath(p,paint);}
    }
    /**
     * The "PGRAZOR" wordmark: heavy italic letters in deep slate-teal with a mint outline, sliced
     * horizontally by a blade (top half nudged left, bottom half right), long tapered speed streaks on
     * both sides of the cut and flying shards above the end and below the start of the word.
     * Geometry is proportional to the font size (TITLE_SP), so it scales with the title.
     */
    private final class BrandLogo extends View {
        private final Paint text=new Paint(Paint.ANTI_ALIAS_FLAG),fill=new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Path path=new Path();
        private static final String WORD="PGRAZOR";
        BrandLogo(Context c){super(c);setContentDescription("PGRazor");text.setTextAlign(Paint.Align.CENTER);text.setTextSkewX(-0.14f);
            text.setStrokeJoin(Paint.Join.ROUND);}
        private void tri(Canvas c,float x1,float y1,float x2,float y2,float x3,float y3,int color){
            path.reset();path.moveTo(x1,y1);path.lineTo(x2,y2);path.lineTo(x3,y3);path.close();fill.setColor(color);c.drawPath(path,fill);}
        /** A thin kite-shaped shard centred on (x,y), length l and width w (units of k), pointing along angle a (degrees). */
        private void shard(Canvas c,float x,float y,float l,float w,float a,float k,int color){
            double r=Math.toRadians(a);float ux=(float)Math.cos(r),uy=(float)Math.sin(r),px=-uy,py=ux;l*=k*0.9f;w*=k*0.9f;
            path.reset();path.moveTo(x+ux*l,y+uy*l);path.lineTo(x+px*w-ux*l*0.2f,y+py*w-uy*l*0.2f);path.lineTo(x-ux*l,y-uy*l);path.lineTo(x-px*w*0.6f,y-py*w*0.6f);path.close();
            fill.setColor(color);c.drawPath(path,fill);}
        private void half(Canvas c,float top,float bottom,float x,float base,float k,Shader shader){
            c.save();c.clipRect(0,top,getWidth(),bottom);
            text.setShader(null);text.setStyle(Paint.Style.STROKE);text.setStrokeWidth(6*k);text.setColor(MapTheme.dlgOutline());c.drawText(WORD,x,base,text);
            text.setStyle(Paint.Style.FILL);text.setShader(shader);c.drawText(WORD,x,base,text);text.setShader(null);
            c.restore();}
        protected void onDraw(Canvas c){super.onDraw(c);
            float size=android.util.TypedValue.applyDimension(android.util.TypedValue.COMPLEX_UNIT_SP,MapTheme.Size.TITLE_SP*1.3f,getResources().getDisplayMetrics());
            text.setTypeface(MapTheme.brandFont(getContext()));text.setTextSize(size);
            float k=size/74f,w=getWidth(),cx=w/2f,base=getHeight()/2f+size*0.33f,top=base-53*k,cut=base-26*k,gap=2.6f*k,dx=5*k;
            float half=text.measureText(WORD)/2f,left=cx-half+4*k,right=cx+half+4*k;
            int ink=MapTheme.ink(),teal=MapTheme.accent(),mint=MapTheme.dlgOutline(),deep=MapTheme.dark()?0xff8fdcc6:0xff2c6e62;
            // Speed streaks along the cut: a dark blade above and a shorter teal blade below, on both sides.
            float lTip=Math.max(2*k,left-125*k),lTip2=Math.max(6*k,left-85*k),rTip=Math.min(w-2*k,right+125*k);
            tri(c,lTip,cut-1.5f*k,left+24*k,cut-10*k,left+24*k,cut+1*k,ink);
            tri(c,lTip2,cut+3.5f*k,left+24*k,cut+2*k,left+24*k,cut+8*k,teal);
            float rTip2=Math.min(w-6*k,right+85*k);
            tri(c,right-24*k,cut-10*k,rTip,cut-1.5f*k,right-24*k,cut+1*k,ink);
            tri(c,right-24*k,cut+2*k,rTip2,cut+3.5f*k,right-24*k,cut+8*k,teal);
            // The word, sliced: top half nudged left, bottom half nudged right.
            Shader shader=new LinearGradient(0,top,0,base,ink,deep,Shader.TileMode.CLAMP);
            half(c,0,cut-gap,cx-dx,base,k,shader);
            half(c,cut+gap,getHeight(),cx+dx,base,k,shader);
            // The blade's edge through the cut.
            fill.setColor(teal);c.drawRect(left-6*k,cut-0.6f*k,right+6*k,cut+0.6f*k,fill);
            // Shards flying up-right off the end of the word and down-left off its start.
            shard(c,right-20*k,top-3*k,22,5,-35,k,ink);shard(c,right+2*k,top-13*k,16,4,-40,k,ink);shard(c,right+15*k,top+1*k,12,3.5f,-30,k,ink);
            shard(c,right-42*k,top-11*k,10,3,-45,k,ink);shard(c,right+24*k,top-14*k,8,2.5f,-35,k,mint);
            float b=base+5*k;
            shard(c,left+14*k,b+8*k,24,5,145,k,ink);shard(c,left-6*k,b+16*k,16,4,140,k,ink);shard(c,left+36*k,b+17*k,12,3.5f,150,k,ink);
            shard(c,left-18*k,b+3*k,9,3,135,k,ink);shard(c,left+4*k,b+26*k,8,2.5f,145,k,mint);
        }
    }
    /** Round white header button (like the game's Help button): sun in dark mode, moon in light mode. */
    private final class ThemeIcon extends View {
        private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
        ThemeIcon(Context c){super(c);setFocusable(true);setClickable(true);setOutlineProvider(SettingsUi.roundRectOutline(-1));
            setForeground(new android.graphics.drawable.RippleDrawable(android.content.res.ColorStateList.valueOf(0x2236b89e),null,new android.graphics.drawable.ShapeDrawable(new android.graphics.drawable.shapes.OvalShape())));}
        protected void onDraw(Canvas c){super.onDraw(c);float x=getWidth()/2f,y=getHeight()/2f,r=Math.min(x,y);
            paint.setStyle(Paint.Style.FILL);paint.setColor(MapTheme.card());c.drawCircle(x,y,r,paint);
            float u=r/23f;
            if(Control.darkTheme()){
                paint.setColor(0xffffc94d);c.drawCircle(x,y,5*u,paint);paint.setStrokeWidth(2*u);paint.setStrokeCap(Paint.Cap.ROUND);
                for(int i=0;i<8;i++){double a=i*Math.PI/4;c.drawLine(x+(float)Math.cos(a)*8.5f*u,y+(float)Math.sin(a)*8.5f*u,x+(float)Math.cos(a)*11.5f*u,y+(float)Math.sin(a)*11.5f*u,paint);}
            }else{
                paint.setShader(new LinearGradient(x-r,y-r,x+r,y+r,MapTheme.accentLime(),MapTheme.accent(),Shader.TileMode.CLAMP));
                Path moon=new Path();moon.addCircle(x,y,10*u,Path.Direction.CW);
                Path cut=new Path();cut.addCircle(x+6*u,y-4*u,9*u,Path.Direction.CW);moon.op(cut,Path.Op.DIFFERENCE);c.drawPath(moon,paint);paint.setShader(null);
            }
        }
    }
    /** The game's round close button: deep teal disc, white rim, soft inner ring and a white X. */
    private final class CloseIcon extends View {
        private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
        CloseIcon(Context c){super(c);setContentDescription("Close PGRazor settings");setFocusable(true);setClickable(true);setOutlineProvider(SettingsUi.roundRectOutline(-1));
            setForeground(new android.graphics.drawable.RippleDrawable(android.content.res.ColorStateList.valueOf(0x33ffffff),null,new android.graphics.drawable.ShapeDrawable(new android.graphics.drawable.shapes.OvalShape())));}
        protected void onDraw(Canvas c){super.onDraw(c);float cx=getWidth()/2f,cy=getHeight()/2f,r=Math.min(cx,cy);
            paint.setStyle(Paint.Style.FILL);paint.setColor(0xffffffff);c.drawCircle(cx,cy,r,paint);
            paint.setColor(MapTheme.closeFill());c.drawCircle(cx,cy,r*0.86f,paint);
            paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(r*0.05f);paint.setColor(0x66ffffff);c.drawCircle(cx,cy,r*0.72f,paint);
            float x=r*0.26f;paint.setStrokeCap(Paint.Cap.ROUND);paint.setStrokeWidth(r*0.09f);paint.setColor(0xffffffff);
            c.drawLine(cx-x,cy-x,cx+x,cy+x,paint);c.drawLine(cx-x,cy+x,cx+x,cy-x,paint);}
    }

    SettingsPanel(Activity activity, Host host) {
        this.activity = activity; this.host = host;
        panel = new LinearLayout(activity); panel.setOrientation(LinearLayout.VERTICAL); panel.setPadding(dp(12), dp(8), dp(12), dp(12));
        panel.setBackground(MapTheme.pageBackground(activity)); panel.setElevation(dp(12));
        panel.addOnLayoutChangeListener((v,l,t,r,b,ol,ot,or,ob)->{if(r-l!=or-ol||b-t!=ob-ot)applyInsets(panel.getRootWindowInsets());});
        // Game-style header: centred brand wordmark with a round white theme button on the right
        // (where the game shows Help). An equal spacer on the left keeps the title centred.
        int headerButton = dp(MapTheme.Size.HEADER_BUTTON_DP);
        LinearLayout header = new LinearLayout(activity); header.setOrientation(LinearLayout.HORIZONTAL); header.setGravity(Gravity.CENTER_VERTICAL); header.setPadding(dp(4), dp(6), dp(4), dp(10));
        header.addView(new View(activity), new LinearLayout.LayoutParams(headerButton, headerButton));
        brandLogo = new BrandLogo(activity);
        header.addView(brandLogo, new LinearLayout.LayoutParams(0, dp(MapTheme.Size.TITLE_SP * 2.1f), 1f));
        themeIcon = new ThemeIcon(activity); header.addView(themeIcon,new LinearLayout.LayoutParams(headerButton,headerButton));
        themeIcon.setOnClickListener(v -> {Control.setDarkTheme(!Control.darkTheme());host.applyTheme();});
        TextView version = text(BuildVersion.NAME, 12, muted());
        version.setSingleLine(true); version.setGravity(Gravity.CENTER_VERTICAL);
        version.setPadding(dp(4), 0, dp(8), 0);
        version.setContentDescription("PGRazor version " + BuildVersion.NAME);
        captions.add(version);
        version.setGravity(Gravity.CENTER);
        panel.addView(header);
        panel.addView(version,new LinearLayout.LayoutParams(-1,-2));
        body = new LinearLayout(activity); body.setOrientation(LinearLayout.VERTICAL);
        bodyScroll = new ScrollView(activity); bodyScroll.setFillViewport(false); bodyScroll.setVerticalScrollBarEnabled(false); bodyScroll.setClipToPadding(false);
        body.setPadding(dp(2), dp(2), dp(2), dp(8)); body.setClipToPadding(false); body.setClipChildren(false);
        bodyScroll.addView(body, new ScrollView.LayoutParams(-1,-2));
        panel.addView(bodyScroll, new LinearLayout.LayoutParams(-1,0,1));
        // The game's round teal close button, centred at the bottom.
        closeIcon = new CloseIcon(activity);
        LinearLayout.LayoutParams closeLayout = new LinearLayout.LayoutParams(dp(MapTheme.Size.CLOSE_BUTTON_DP), dp(MapTheme.Size.CLOSE_BUTTON_DP));
        closeLayout.gravity = Gravity.CENTER_HORIZONTAL; closeLayout.topMargin = dp(8); closeLayout.bottomMargin = dp(4);
        panel.addView(closeIcon, closeLayout);

        // Always-visible status: position card laid out like the game's "Version" row; tapping opens the editor.
        LinearLayout info = card(body);
        coords = infoRow(info, "Position", host::editLocation);
        speedValue = infoRow(info, "Speed", host::editLocation);
        buildOpacitySection();
        buildLocationSection();
        buildCatchSection();
        buildVgpSection();
        buildIncubationSection();
        buildBuddySection();
        buildGiftsSection();
        buildItemsSection();
        buildRocketSection();
        buildBattlesSection();
        buildAutomationSection();
        buildThrowsSection();
        buildBackupSection();
        buildDiagnosticsSection();
        buildCommunitySection();

        // Closing the full-screen settings panel simply returns to the game (or the quick menu on the next tap).
        closeIcon.setOnClickListener(v -> hide());
        // The settings panel is a full-screen modal now; it consumes taps so the game behind it stays inert.
        panel.setClickable(true);
    }

    /**
     * PGRazorX: "Map overlays" card with two sliders in 5 % steps: the overlays' opacity (30-100 %, live)
     * and their UI scale (25-100 %, applied on release). The panel itself stays fully opaque.
     */
    private void buildOpacitySection() {
        LinearLayout overlays = section(body, "Map overlays", true);
        TextView opacityValue = infoRow(overlays, "Map overlay opacity", null);
        opacityValue.setText(Control.overlayOpacity()+" %");
        SeekBar opacity = new SeekBar(activity); opacity.setMax(14); opacity.setProgress((Control.overlayOpacity()-30)/5);
        opacity.setContentDescription("Opacity of the buttons and joystick shown over the map");
        opacity.setPadding(dp(4), dp(4), dp(4), dp(4)); overlays.addView(opacity, new LinearLayout.LayoutParams(-1, -2)); opacitySlider = opacity;
        rangeLabels(overlays, "30 %", "100 %");
        opacity.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener(){
            public void onProgressChanged(SeekBar s,int p,boolean user){int v=30+p*5;opacityValue.setText(v+" %");if(user){Control.setOverlayOpacity(v);host.applyOverlayOpacity();}}
            public void onStartTrackingTouch(SeekBar s){}
            public void onStopTrackingTouch(SeekBar s){}
        });
        TextView scaleValue = infoRow(overlays, "UI scale", null);
        scaleValue.setText(Control.uiScalePercent()+" %"); uiScaleValue = scaleValue;
        SeekBar scale = new SeekBar(activity); scale.setMax(15); scale.setProgress((Control.uiScalePercent()-25)/5);
        scale.setContentDescription("Size of the buttons and joystick shown over the map");
        scale.setPadding(dp(4), dp(4), dp(4), dp(4)); overlays.addView(scale, new LinearLayout.LayoutParams(-1, -2)); uiScaleSlider = scale;
        rangeLabels(overlays, "25 %", "100 %");
        scale.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener(){
            public void onProgressChanged(SeekBar s,int p,boolean user){scaleValue.setText((25+p*5)+" %");}
            public void onStartTrackingTouch(SeekBar s){}
            public void onStopTrackingTouch(SeekBar s){int v=25+s.getProgress()*5;if(v==Control.uiScalePercent())return;host.releaseJoystick(true);Control.setUiScalePercent(v);host.refresh();}
        });
        caption(overlays, "Size and opacity of the R button, quick menu, cooldown timer, berry picker and joystick.");
    }
    private TextView uiScaleValue;
    private SeekBar uiScaleSlider;
    /** Muted min / max labels under a slider. */
    private void rangeLabels(LinearLayout owner,String min,String max){
        LinearLayout r=new LinearLayout(activity);r.setOrientation(LinearLayout.HORIZONTAL);r.setPadding(dp(2),0,dp(2),dp(6));
        TextView a=new TextView(activity);a.setText(min);r.addView(a,new LinearLayout.LayoutParams(0,-2,1f));
        TextView b=new TextView(activity);b.setText(max);b.setGravity(Gravity.END);r.addView(b,new LinearLayout.LayoutParams(0,-2,1f));
        owner.addView(r);rangeTexts.add(a);rangeTexts.add(b);
    }
    private final java.util.List<TextView> rangeTexts=new java.util.ArrayList<>();

    private void buildLocationSection() {
        // ----- Location & overlays -----
        LinearLayout loc = section(body, "Location & overlays", true);
        LinearLayout menu = section(loc,"Customize Quick Menu",false);
        caption(menu,"Choose which entries appear. This does not turn features on or off. Settings stays visible.");
        for(int i=0;i<QuickMenu.CUSTOMIZABLE.length;i++){
            final int bit=1<<i;
            Switch box=switchRow(menu,QuickMenu.CUSTOMIZABLE[i]);
            box.setChecked(Control.quickMenuVisible(bit));quickMenuChoices[i]=box;
            box.setOnCheckedChangeListener((v,on)->{if(on==Control.quickMenuVisible(bit))return;Control.setQuickMenuVisible(bit,on);host.refresh();});
        }
        locationSwitch=switchRow(loc,"Location spoofing","Moves your in-game position to the joystick or map location.");locationSwitch.setChecked(Control.desiredEnabled());
        locationSwitch.setOnCheckedChangeListener((v,checked)->{if(checked==Control.desiredEnabled())return;host.releaseJoystick(true);Control.setLocationSpoofing(checked);host.refresh();});
        steeringStatus = caption(loc, "Joystick follows the camera");
        Button position = button("Coordinates & speed"); loc.addView(position); position.setOnClickListener(v -> host.editLocation());
        Button map = button("World map"); loc.addView(map);
        map.setOnClickListener(v -> host.openMap());
        mapTap = button("Game map tap"); loc.addView(mapTap);
        mapTap.setOnClickListener(v -> { Control.setMapTapMode((Control.mapTapMode()+1)%4); host.refresh(); });
        spawnSwitch = switchRow(loc, "Spawn booster (80 m)"); spawnSwitch.setChecked(Control.spawnBooster());
        spawnSwitch.setOnCheckedChangeListener((v,checked) -> { if(checked==Control.spawnBooster())return; Control.setSpawnBooster(spawnSwitch.isChecked()); });
        spawnStatus = caption(loc, "");
        caption(loc, "Can cause lag in busy areas with many nearby Pokémon. Turn it off if the game stutters.");
        cooldownSwitch = switchRow(loc, "Cooldown overlay", "Floating cooldown timer on the map."); cooldownSwitch.setChecked(Control.cooldownOverlay());
        cooldownSwitch.setOnCheckedChangeListener((v,checked) -> { if(checked==Control.cooldownOverlay())return; Control.setCooldownOverlay(cooldownSwitch.isChecked()); host.refresh(); });
        Button history = button("Cooldown history"); loc.addView(history); history.setOnClickListener(v -> Cooldown.show(activity));
        idleWalkSwitch = switchRow(loc, "Idle walking");idleWalkSwitch.setChecked(Control.idleWalking());
        idleWalkSwitch.setOnCheckedChangeListener((v,checked)->{if(checked!=Control.idleWalking())Control.setIdleWalking(checked);});
        caption(loc, "When idle, start walking in a circle. Speed 0 pauses it.");
        joystickSwitch = switchRow(loc, "Show joystick"); joystickSwitch.setChecked(Control.joystickVisible());
        joystickSwitch.setOnCheckedChangeListener((v,checked) -> { if(checked==Control.joystickVisible())return; host.setJoystickVisible(checked); });

    }

    private void buildCatchSection() {
        // ----- Catch & summary (gated by catch-summary support) -----
        LinearLayout catchC = section(body, "Catch & summary", false); catchRoot = (View)catchC.getParent();
        LinearLayout sumRow = new SettingsUi.RowLayout(activity); sumRow.setOrientation(LinearLayout.HORIZONTAL); sumRow.setGravity(Gravity.CENTER_VERTICAL); sumRow.setMinimumHeight(dp(MapTheme.Size.ROW_HEIGHT_DP));
        quickSummaryLabel = new TextView(activity); quickSummaryLabel.setTextSize(MapTheme.Size.ROW_TEXT_SP); rowLabels.add(quickSummaryLabel); sumRow.addView(quickSummaryLabel, new LinearLayout.LayoutParams(0,-2,1f));
        quickSummarySwitch = new Switch(activity); quickSummarySwitch.setContentDescription("Quick catch"); sumRow.addView(quickSummarySwitch, new LinearLayout.LayoutParams(-2,-2)); catchC.addView(sumRow);
        quickSummarySwitch.setChecked(Control.quickSummaryEnabled());
        quickSummarySwitch.setOnCheckedChangeListener((v,checked) -> { if(checked==Control.quickSummaryEnabled())return; Control.setQuickSummaryEnabled(quickSummarySwitch.isChecked()); host.refresh(); });
        quickSummaryStatus = caption(catchC, "");
        manualSwitch=switchRow(catchC,"Manual transfer without restrictions","Allows special Pokemon in manual transfer. Check status for warning support. Favorites, eggs and gym defenders remain blocked.");
        manualSwitch.setChecked(Control.manualTransferFree());
        manualSwitch.setOnCheckedChangeListener((v,checked)->{
            if(checked==Control.manualTransferFree())return;
            if(!checked){Control.setManualTransferFree(false);return;}
            AlertDialog confirm=new AlertDialog.Builder(dialogContext()).setTitle("Manual transfer without restrictions?")
                .setMessage("Allows special Pokemon in manual transfer. Favorites, eggs and gym defenders remain protected. The normal confirmation stays.")
                .setNegativeButton("Cancel",(d,w)->manualSwitch.setChecked(false))
                .setPositiveButton("Turn on",(d,w)->{Control.setManualTransferFree(true);host.refresh();}).create();
            confirm.setOnCancelListener(d->manualSwitch.setChecked(false));
            confirm.setOnShowListener(d->MapTheme.styleButtons(confirm));confirm.show();
        });
        manualStatus=caption(catchC,Control.manualTransferStatus());
        LinearLayout txC = section(catchC, "Transfer on catch", false);
        autoTransferSwitch = switchRow(txC, "Transfer below 90% IV"); autoTransferSwitch.setChecked(Control.autoTransferEnabled());
        autoTransferSwitch.setOnCheckedChangeListener((v,checked) -> { if(checked==Control.autoTransferEnabled())return; Control.setAutoTransferEnabled(autoTransferSwitch.isChecked()); host.refresh(); });
        String[] protections={"Keep legendary / mythical / Ultra Beasts","Keep shiny Pokemon","Keep event / costume Pokemon","Keep background Pokemon","Keep XXS","Keep XXL"};
        for(int i=0;i<6;i++){final int bit=1<<i;Switch box=switchRow(txC,protections[i]);box.setChecked((Control.transferProtect()&bit)!=0);transferProtection[i]=box;
            box.setOnCheckedChangeListener((v,on)->{int o=Control.transferProtect(),n=on?o|bit:o&~bit;if(n!=o)Control.setTransferProtect(n);});}
        transferKeepList = button("Custom keep list"); txC.addView(transferKeepList);
        transferKeepList.setOnClickListener(v -> TransferKeepDialog.show(activity, host::refresh));
        transferOverlaySwitch = switchRow(txC, "Keep-all overlay button"); transferOverlaySwitch.setChecked(Control.transferOverlayEnabled());
        transferOverlaySwitch.setOnCheckedChangeListener((v,checked) -> { if(checked==Control.transferOverlayEnabled())return; Control.setTransferOverlayEnabled(transferOverlaySwitch.isChecked()); host.refresh(); });
        caption(txC, "Kept species are never auto-transferred, across all forms. The floating button holds every catch (Keep All) or follows the rules above.");
        caption(txC, "Transfers are permanent. Favorites and unknown data are always kept.");
        transferStatus = caption(txC, "");
        autoHealSwitch=switchRow(catchC,"Auto revive & heal");autoHealSwitch.setChecked(Control.autoHeal());
        autoHealSwitch.setOnCheckedChangeListener((v,on)->Control.setAutoHeal(on));
        autoHealHalfSwitch=switchRow(catchC,"Heal Pokemon below 50% HP");autoHealHalfSwitch.setChecked(Control.autoHealBelowHalf());
        autoHealHalfSwitch.setOnCheckedChangeListener((v,on)->Control.setAutoHealBelowHalf(on));
        caption(catchC,"Only on the map. Heals to full HP after starting; exactly 50% does not trigger healing.");
        autoHealStatus=caption(catchC,Control.autoHealStatus());
        quickSpinSwitch = switchRow(catchC, "Quick spin"); quickSpinSwitch.setChecked(Control.quickSpinEnabled());
        quickSpinSwitch.setOnCheckedChangeListener((v,checked) -> { if(checked==Control.quickSpinEnabled())return; Control.setQuickSpinEnabled(quickSpinSwitch.isChecked()); });
        quickSpinStatus = caption(catchC, "");
        berrySwitch = switchRow(catchC, "Catching berry overlay"); berrySwitch.setChecked(Control.encounterBerryEnabled());
        berrySwitch.setOnCheckedChangeListener((v,checked) -> { if(checked==Control.encounterBerryEnabled())return; Control.setEncounterBerryEnabled(berrySwitch.isChecked()); host.refresh(); });
        ivOwnedSwitch = switchRow(catchC, "Inventory IV/level", "Shows IV and level in your Pokémon list."); ivOwnedSwitch.setChecked(Control.ivOwnedEnabled());
        ivOwnedSwitch.setOnCheckedChangeListener((v,checked) -> { if(checked==Control.ivOwnedEnabled())return; Control.setIvOwnedEnabled(ivOwnedSwitch.isChecked()); host.refresh(); });
        ivStatus = caption(catchC, "");
    }

    private void buildVgpSection() {
        LinearLayout vgpC=section(body,"Virtual Go Plus",false);
        vgpSwitch=switchRow(vgpC,"VGP overlay");vgpSwitch.setChecked(Control.vgpOverlay());
        vgpSwitch.setOnCheckedChangeListener((b,checked)->{if(checked!=Control.vgpOverlay())Control.setVgpOverlay(checked);host.refresh();});
        Switch hideText=switchRow(vgpC,"Hide VGP status text","Hide the status text and keep the VGP icon visible.");
        hideText.setChecked(OverlayTextSettings.hidden(activity,"vgp"));
        hideText.setOnCheckedChangeListener((v,on)->{OverlayTextSettings.setHidden(activity,"vgp",on);host.refresh();});
        String[] vgpLabels={"Nearby Pokémon","Nearby PokéStops","Push notifications"};
        for(int i=0;i<3;i++){final int bit=1<<i;CheckBox box=new CheckBox(activity);box.setText(vgpLabels[i]);box.setChecked((Control.vgpOptions()&bit)!=0);vgpChoices[i]=box;vgpC.addView(box);
            box.setOnClickListener(v->{Control.setVgpOptions(box.isChecked()?Control.vgpOptions()|bit:Control.vgpOptions()&~bit);
                if(bit==4&&box.isChecked()&&Build.VERSION.SDK_INT>=33&&activity.checkSelfPermission("android.permission.POST_NOTIFICATIONS")!=android.content.pm.PackageManager.PERMISSION_GRANTED)activity.requestPermissions(new String[]{"android.permission.POST_NOTIFICATIONS"},7301);
            });
        }
        vgpCooldownStop=new CheckBox(activity);vgpCooldownStop.setText("Stop on cooldown (estimated)");vgpCooldownStop.setContentDescription("Stop Virtual Go Plus during the estimated action cooldown");vgpCooldownStop.setChecked(Control.vgpStopOnCooldown());vgpC.addView(vgpCooldownStop);
        vgpCooldownStop.setOnClickListener(v->{if(vgpCooldownStop.isChecked()!=Control.vgpStopOnCooldown())Control.setVgpStopOnCooldown(vgpCooldownStop.isChecked());host.refresh();});
        vgpStatus=caption(vgpC,Control.vgpStatus());
        smartRefresh=SmartCatchSettings.populate(activity,vgpC);
    }

    private void buildIncubationSection() {
        // ----- Auto incubate eggs -----
        LinearLayout eggsC = section(body, "Auto incubate eggs", false);
        incubatorSwitch = switchRow(eggsC, "Auto incubate");
        incubatorSwitch.setChecked(Control.autoIncubate());
        incubatorSwitch.setOnCheckedChangeListener((v, checked) -> {
            if (checked != Control.autoIncubate()) Control.setAutoIncubate(checked);
        });
        incubatorPriority = button("Priority"); eggsC.addView(incubatorPriority);
        incubatorPriority.setOnClickListener(v -> { Control.setIncubateLongest(!Control.incubateLongest()); host.refresh(); });
        caption(eggsC, "Unlimited incubator only");
        incubatorStatus = caption(eggsC, Control.incubatorStatus());
        eggSkipSwitch=switchRow(eggsC,"Skip hatch");eggSkipSwitch.setChecked(Control.eggSkip());
        eggSkipSwitch.setOnCheckedChangeListener((v,checked)->{if(checked!=Control.eggSkip())Control.setEggSkip(checked);});
        eggKeepSwitch=switchRow(eggsC,"Keep shiny & 100% open");eggKeepSwitch.setChecked(Control.eggKeepSpecial());
        eggKeepSwitch.setOnCheckedChangeListener((v,checked)->{if(checked!=Control.eggKeepSpecial())Control.setEggKeepSpecial(checked);});
        eggObserveSwitch=switchRow(eggsC,"Diagnostic: observe only (no automatic actions)");eggObserveSwitch.setChecked(Control.eggObserveOnly());
        eggObserveSwitch.setOnCheckedChangeListener((v,checked)->{if(checked!=Control.eggObserveOnly())Control.setEggObserveOnly(checked);});
        eggStatus=caption(eggsC,Control.eggHatchStatus());
    }

    private void buildBuddySection() {
        LinearLayout buddyC=section(body,"Buddy Emotion Engine",false);buddyRoot=(View)buddyC.getParent();
        buddyRefresh=BuddyEmotionSettings.populate(activity,buddyC);
        buddyStatus=caption(buddyC,"");
    }

    private void buildGiftsSection() {
        // ----- Friend gifts -----
        LinearLayout giftC = section(body, "Friend gifts", false); giftRoot = (View)giftC.getParent();
        String[] giftLabels={"Auto open gifts","Auto send gifts","Add an owned sticker"};
        for(int i=0;i<3;i++){final int bit=1<<i;Switch box=switchRow(giftC,giftLabels[i]);box.setChecked((Control.giftOptions()&bit)!=0);giftChoices[i]=box;box.setOnCheckedChangeListener((v,on)->{int o=Control.giftOptions(),n=on?o|bit:o&~bit;if(n!=o)Control.setGiftOptions(n);});}

        giftStatus = caption(giftC, "");
    }

    private void buildItemsSection() {
        // ----- Inventory (opens its modal directly) -----
        LinearLayout itemsC = card(body); itemsRoot = itemsC;
        itemSettings = button("Auto-transfer items"); itemsC.addView(itemSettings); itemSettings.setOnClickListener(v -> showItemSettings());
        itemStatus = caption(itemsC, "");
    }

    private void buildAutomationSection() {
        // ----- Automation (one-shot manual tasks; mirrors the quick-menu Automation hub) -----
        LinearLayout autoC = section(body, "Automation", false);
        Button incense = button("Daily Incense Session"); autoC.addView(incense);
        incense.setOnClickListener(v -> AutomationPanel.showIncense(activity));
        Button remoteFeed = button("Remote Gym Feeding"); autoC.addView(remoteFeed);
        remoteFeed.setOnClickListener(v -> AutomationPanel.showRemoteFeed(activity));
        Button gifts = button("Send & Open Gifts"); autoC.addView(gifts);
        gifts.setOnClickListener(v -> GiftBatchDialog.show(activity));
        Button evolve = button("Mass Evolve"); autoC.addView(evolve);
        evolve.setOnClickListener(v -> BatchEvolutionSettings.show(activity));
    }

    private void buildRocketSection() {
        // ----- Team Rocket -----
        LinearLayout rocketC = section(body, "Team Rocket", false); rocketRoot = (View)rocketC.getParent();
        String[] rocketLabels={"Auto quick attack","Auto charged attack","Auto shield"};
        for(int i=0;i<3;i++){final int bit=1<<i;Switch box=switchRow(rocketC,rocketLabels[i]);box.setChecked((Control.rocketOptions()&bit)!=0);rocketChoices[i]=box;
            box.setOnCheckedChangeListener((v,on)->{if((Control.rocketOptions()&8)!=0)return;int o=Control.rocketOptions(),n=on?o|bit:o&~bit;if(n!=o)Control.setRocketOptions(n);});}

        String[] fullLabels={"Full auto Rocket","Fight leaders too"};
        for(int i=3;i<5;i++){final int bit=1<<i;Switch box=switchRow(rocketC,fullLabels[i-3]);box.setChecked((Control.rocketOptions()&bit)!=0);rocketChoices[i]=box;box.setOnCheckedChangeListener((v,on)->{int old=Control.rocketOptions(),value=on?old|bit:old&~bit;if(old!=value)Control.setRocketOptions(value);});}
        caption(rocketC,"Full auto includes attacks and shields, uses your selected party and pauses Quick Spin while leaving a Rocket stop. Rescue throws use your Throws settings.");
        rocketStatus=caption(rocketC,Control.rocketStatus());
    }

    private void buildBattlesSection() {
        LinearLayout battleC = section(body, "Gyms, Raids & Dynamax", false); battleRoot = (View)battleC.getParent();
        LinearLayout gymC = section(battleC, "Gyms", false);
        Button gyms=button("Gym Slotter & Berry Feeder");gymC.addView(gyms);
        gyms.setOnClickListener(v->GymAutomationSettings.show(activity));
        battleChoice(gymC,2,"Auto quick attack");
        battleChoice(gymC,3,"Auto charged attack");
        battleChoice(gymC,8,"Auto dodge");

        LinearLayout raidC = section(battleC, "Raids", false);
        battleChoice(raidC,0,"Auto quick attack");
        battleChoice(raidC,1,"Auto charged attack");
        battleChoice(raidC,7,"Auto dodge");

        LinearLayout maxC = section(battleC, "Dynamax", false);
        battleChoice(maxC,4,"Auto quick attack");
        battleChoice(maxC,5,"Auto charged attack");
        battleChoice(maxC,6,"Auto Max attack");
        battleChoice(maxC,9,"Auto dodge");
        battleStatus = caption(battleC, "");
    }

    private void battleChoice(LinearLayout owner,int index,String label) {
        // Keep the persisted/native option bits unchanged when moving controls between groups.
        final int bit=1<<index;
        Switch box=switchRow(owner,label);
        box.setChecked((Control.battleOptions()&bit)!=0);battleChoices[index]=box;
        box.setOnClickListener(v->Control.setBattleOptions(box.isChecked()?Control.battleOptions()|bit:Control.battleOptions()&~bit));
    }

    private void buildThrowsSection() {
        // ----- Throws & catch animation (gated by ball-animation support) -----
        LinearLayout throwC = section(body, "Throws", false); quickBallRoot = (View)throwC.getParent();

        throwGrade = button("Throw rating"); throwC.addView(throwGrade);
        throwGrade.setOnClickListener(v -> { Control.setThrowGrade((Control.throwFixedGrade()+1)%4); host.refresh(); });
        throwRandomSwitch=switchRow(throwC,"Random rating","Each throw is Nice, Great or Excellent at random (replaces the fixed throw rating).");
        throwRandomSwitch.setChecked(Control.throwRandom());
        throwRandomSwitch.setOnCheckedChangeListener((v,on)->{if(on==Control.throwRandom())return;Control.setThrowRandom(on);host.refresh();});
        curveSwitch = switchRow(throwC, "Always curveball"); curveSwitch.setChecked(Control.throwCurve());
        curveSwitch.setOnCheckedChangeListener((v,checked) -> { if(checked==Control.throwCurve())return; Control.setThrowCurve(curveSwitch.isChecked()); host.refresh(); });
        alwaysHitSwitch = switchRow(throwC, "Always hit"); alwaysHitSwitch.setChecked(Control.throwAlwaysHit());
        alwaysHitSwitch.setOnCheckedChangeListener((v,checked) -> { if(checked==Control.throwAlwaysHit())return; Control.setThrowAlwaysHit(alwaysHitSwitch.isChecked()); host.refresh(); });
        arSwitch = switchRow(throwC, "AR+ throw bonus"); arSwitch.setChecked(Control.throwAr());
        arSwitch.setOnCheckedChangeListener((v,checked) -> { if(checked==Control.throwAr())return; Control.setThrowAr(arSwitch.isChecked()); host.refresh(); });
        caption(throwC, "Wild encounters. Combines with throw rating and curveball; server bonus unverified.");
        ivCatchSwitch = switchRow(throwC, "Catch IV/level", "Shows IV and level during wild encounters."); ivCatchSwitch.setChecked(Control.ivCatchEnabled());
        ivCatchSwitch.setOnCheckedChangeListener((v,checked) -> { if(checked==Control.ivCatchEnabled())return; Control.setIvCatchEnabled(ivCatchSwitch.isChecked()); host.refresh(); });
        quickBallStatus = caption(throwC, "");
    }

    private void buildBackupSection() {
        LinearLayout accountsC=card(body);Button accounts=button("Accounts");accountsC.addView(accounts);
        accounts.setOnClickListener(v->AccountsDialog.show(activity));
        LinearLayout backupC=card(body);Button settingsBackup=button("Export / import settings");backupC.addView(settingsBackup);
        settingsBackup.setOnClickListener(v->activity.startActivity(new Intent(activity,LogActivity.class).putExtra("settingsBackup",true)));
    }

    private void buildDiagnosticsSection() {
        // ----- Diagnostics (status plumbing kept out of everyday settings) -----
        LinearLayout diag = section(body, "Diagnostics", false);
        state = caption(diag, Control.connection);
        for(TextView status:new TextView[]{steeringStatus,spawnStatus,quickSummaryStatus,transferStatus,quickSpinStatus,buddyStatus,giftStatus,itemStatus,rocketStatus,battleStatus,quickBallStatus}){
            ((ViewGroup)status.getParent()).removeView(status);diag.addView(status);
        }
        cameraRoot = new LinearLayout(activity); cameraRoot.setOrientation(LinearLayout.VERTICAL); diag.addView(cameraRoot);
        cameraStatus = caption(cameraRoot, ""); cameraStatus.setMaxLines(3); cameraStatus.setEllipsize(android.text.TextUtils.TruncateAt.END);
        cameraSample = button("Sample camera (10s)"); cameraRoot.addView(cameraSample);
        cameraSample.setOnClickListener(v -> { if (Control.requestCameraSample()) { host.releaseJoystick(false); host.refresh(); } });
        interactionRoot = new LinearLayout(activity); interactionRoot.setOrientation(LinearLayout.VERTICAL); diag.addView(interactionRoot);
        interactionStatus = caption(interactionRoot, ""); interactionStatus.setMaxLines(3); interactionStatus.setEllipsize(android.text.TextUtils.TruncateAt.END);
        interactionCapture = button("Capture catch/spin (60s)"); interactionRoot.addView(interactionCapture);
        interactionCapture.setOnClickListener(v -> {
            if (Control.interactionDiagnosticRunning()) Control.requestInteractionStop();
            else if (Control.requestInteractionTrace()) host.releaseJoystick(false);
            host.refresh();
        });
        Button logs = button("Run log / export"); diag.addView(logs);
        logs.setOnClickListener(v -> activity.startActivity(new Intent(activity, LogActivity.class)));
    }

    /** Community link as its own card row, in the game's teal value colour. */
    private void buildCommunitySection() {
        LinearLayout communityC = card(body);
        Button discord = button("Join our Discord"); communityC.addView(discord); discordLink = discord;
        discord.setContentDescription("Join the PGRazor Discord community");
        discord.setOnClickListener(v -> {
            try { activity.startActivity(new Intent(Intent.ACTION_VIEW, android.net.Uri.parse("https://discord.gg/9QqfkDSQX3"))); }
            catch (android.content.ActivityNotFoundException unavailable) {
                android.widget.Toast.makeText(activity, "No app available to open Discord links", android.widget.Toast.LENGTH_SHORT).show();
            }
        });
    }

    /** The root panel view; Pad owns attachment to the window and insets wiring. */
    View view(){return panel;}
    boolean isVisible(){return panel.getVisibility()==View.VISIBLE;}
    /** Bring the full-screen panel up; Pad applies current insets immediately after. */
    void show(){Control.setRocketAutoPanelOpen(true);Control.setEggSettingsOpen(true);Control.setAutoHealPanelOpen(true);panel.setVisibility(View.VISIBLE);panel.bringToFront();panel.requestApplyInsets();refresh();}
    /** Hidden state matches the original startup and the close affix: returns to the game underneath. */
    void hide(){Control.setRocketAutoPanelOpen(false);Control.setEggSettingsOpen(false);Control.setAutoHealPanelOpen(false);panel.setVisibility(View.GONE);}
    /** Release this panel's dialog polling when its owning activity panel is detached. */
    void close(){
        Control.setRocketAutoPanelOpen(false);Control.setEggSettingsOpen(false);Control.setAutoHealPanelOpen(false);
        for(AlertDialog dialog:new java.util.ArrayList<>(itemDialogs))dialog.dismiss();
        itemDialogs.clear();handler.removeCallbacksAndMessages(null);
    }

    /** Pads the full-screen settings panel inside the status bar, navigation bar, and IME. */
    void applyInsets(WindowInsets insets) {
        if(insets==null)return;
        android.graphics.Rect safe=UiViewport.bounds(panel);
        int left=safe.left,top=safe.top,right=Math.max(0,panel.getWidth()-safe.right),bottom=Math.max(0,panel.getHeight()-safe.bottom);
        int gutter=dp(panel.getWidth()/activity.getResources().getDisplayMetrics().density>=600?24:12);
        panel.setPadding(gutter+left,dp(8)+top,gutter+right,dp(12)+bottom);
    }

    void applyTheme(){
        panel.setBackground(MapTheme.pageBackground(activity));styleTree(panel);
        if(brandLogo!=null)brandLogo.invalidate();
        for(TextView head:sectionHeads){head.setTextColor(MapTheme.value());head.setTypeface(MapTheme.bold());}
        for(TextView title:sectionTitles){title.setTextColor(ink());title.setTypeface(MapTheme.bold());}
        for(TextView label:rowLabels){label.setTextColor(ink());label.setTypeface(MapTheme.bold());label.setTextSize(MapTheme.Size.ROW_TEXT_SP);label.setPadding(0,dp(3),0,dp(3));}
        for(TextView v:infoValues){v.setTextColor(MapTheme.value());v.setTypeface(MapTheme.medium());v.setTextSize(MapTheme.Size.ROW_TEXT_SP-1);}
        for(View c:cards){c.setBackground(MapTheme.cardBackground(activity));MapTheme.elevate(c,2.5f);}
        for(View d:dividers)d.setBackgroundColor(MapTheme.outline());
        for(View chev:chevrons)chev.invalidate();
        for(TextView c:captions){c.setTextColor(muted());c.setTextSize(MapTheme.Size.CAPTION_SP);c.setLineSpacing(dp(2),1f);c.setPadding(0,dp(2),0,dp(8));}
        if(discordLink!=null)discordLink.setTextColor(MapTheme.value());
        if(closeIcon!=null){MapTheme.elevate(closeIcon,5);closeIcon.invalidate();}
        if(themeIcon!=null){themeIcon.setContentDescription(Control.darkTheme()?"Switch to light theme":"Switch to dark theme");MapTheme.elevate(themeIcon,3);themeIcon.invalidate();}
        for(TextView t:rangeTexts){t.setTextColor(muted());t.setTextSize(11.5f);}
        for(SeekBar bar:new SeekBar[]{opacitySlider,uiScaleSlider})if(bar!=null){bar.setProgressTintList(android.content.res.ColorStateList.valueOf(accent()));bar.setThumbTintList(android.content.res.ColorStateList.valueOf(MapTheme.closeFill()));bar.setProgressBackgroundTintList(android.content.res.ColorStateList.valueOf(MapTheme.outline()));}
    }

    private void showItemSettings(){
        Control.requestItemCatalog();
        LinearLayout form=new LinearLayout(activity);form.setOrientation(LinearLayout.VERTICAL);form.setPadding(dp(16),dp(8),dp(16),dp(8));
        CheckBox enabled=new CheckBox(activity);enabled.setText("Enable automatic item transfer");enabled.setChecked(Control.itemTransferEnabled());form.addView(enabled);
        TextView explanation=new TextView(activity);explanation.setText("Check items to trim and tap the maximum to change it. Saving applies to your current bag and future items. A limit of 0 removes all of that item. Unchecked items are kept.");form.addView(explanation);
        final java.util.Map<Integer,Integer> limits=new java.util.HashMap<>();final java.util.Set<Integer> selected=new java.util.HashSet<>();
        for(String row:Control.itemLimits().split(";"))try{String[] b=row.split(":");limits.put(Integer.parseInt(b[0]),Integer.parseInt(b[1]));}catch(RuntimeException ignored){}
        for(String row:Control.itemRules().split(";"))try{String[] b=row.split(":");int id=Integer.parseInt(b[0]);selected.add(id);limits.put(id,Integer.parseInt(b[1]));}catch(RuntimeException ignored){}
        // PGRazorX: a recycled list. Only the rows on screen exist (about a dozen views instead of a CheckBox + EditText
        // per item), so opening and scrolling no longer depend on how many items the bag has. The model lives in
        // these lists; asynchronous native catalog updates refresh only changed rows.
        final java.util.List<Integer> ids=new java.util.ArrayList<>();final java.util.Map<Integer,String> names=new java.util.HashMap<>(),counts=new java.util.HashMap<>();
        final String[] lastCatalog={null};
        final Runnable[] parse=new Runnable[1];
        parse[0]=()->{String catalog=Control.itemCatalog();if(catalog.equals(lastCatalog[0]))return;lastCatalog[0]=catalog;
            for(String row:catalog.split("\n")){String[] b=row.split("\\|",-1);if(b.length!=3)continue;int id;try{id=Integer.parseInt(b[0]);}catch(NumberFormatException invalid){continue;}
                if(!names.containsKey(id))ids.add(id);names.put(id,b[1]);counts.put(id,b[2]);}};
        parse[0].run();
        final ListView list=new ListView(activity);list.setDivider(new android.graphics.drawable.ColorDrawable(MapTheme.dlgMintLine()));list.setDividerHeight(Math.max(1,dp(1)));
        list.setPadding(dp(10),dp(2),dp(10),dp(2));list.setClipToPadding(false);list.setItemsCanFocus(false);list.setSelector(new android.graphics.drawable.ColorDrawable(0));
        list.setBackground(MapTheme.mintBlock(activity));list.setClipToOutline(true);
        LinearLayout.LayoutParams listLayout=new LinearLayout.LayoutParams(-1,dp(380));listLayout.topMargin=dp(6);listLayout.bottomMargin=dp(6);form.addView(list,listLayout);
        final Context fieldContext=dialogContext();
        final BaseAdapter adapter=new BaseAdapter(){
            public int getCount(){return ids.size();}
            public Object getItem(int position){return ids.get(position);}
            public long getItemId(int position){return ids.get(position);}
            public View getView(int position,View convert,ViewGroup parent){
                LinearLayout line=(LinearLayout)convert;
                if(line==null){line=new LinearLayout(activity);line.setOrientation(LinearLayout.HORIZONTAL);line.setGravity(Gravity.CENTER_VERTICAL);line.setPadding(0,dp(4),0,dp(4));
                    CheckBox box=new CheckBox(activity);line.addView(box,new LinearLayout.LayoutParams(0,-2,1));
                    Button cap=new Button(fieldContext);cap.setAllCaps(false);cap.setMinWidth(0);cap.setMinimumWidth(0);line.addView(cap,new LinearLayout.LayoutParams(dp(84),dp(40)));
                    styleTree(line);}
                final int id=ids.get(position);final CheckBox box=(CheckBox)line.getChildAt(0);final Button cap=(Button)line.getChildAt(1);
                box.setOnCheckedChangeListener(null);box.setText(names.get(id)+" ("+counts.get(id)+" in bag)");box.setChecked(selected.contains(id));
                final int limit=limits.containsKey(id)?limits.get(id):100;cap.setText("Max "+limit);cap.setEnabled(enabled.isChecked()&&box.isChecked());cap.setAlpha(box.isChecked()?1f:0.45f);
                cap.setContentDescription("Maximum "+names.get(id)+" to keep: "+limit);
                SettingsUi.enabled(box,enabled.isChecked());
                box.setOnCheckedChangeListener((button,checked)->{if(checked)selected.add(id);else selected.remove(id);cap.setEnabled(checked);cap.setAlpha(checked?1f:0.45f);});
                cap.setOnClickListener(v->editItemLimit(names.get(id),limits.containsKey(id)?limits.get(id):100,value->{limits.put(id,value);notifyDataSetChanged();}));
                return line;
            }};
        list.setAdapter(adapter);enabled.setOnCheckedChangeListener((v,on)->adapter.notifyDataSetChanged());
        TextView status=new TextView(activity);status.setText(Control.itemStatus());status.setGravity(Gravity.CENTER_HORIZONTAL);form.addView(status);
        AlertDialog dialog=new AlertDialog.Builder(dialogContext()).setTitle("Auto-transfer inventory items").setView(form).setNegativeButton("Cancel",null).setPositiveButton("Save & apply",null).create();
        final Runnable[] poll=new Runnable[1];poll[0]=()->{if(!dialog.isShowing())return;Control.requestItemCatalog();
            String before=lastCatalog[0];parse[0].run();if(!java.util.Objects.equals(before,lastCatalog[0]))adapter.notifyDataSetChanged();
            status.setText(Control.itemStatus());handler.postDelayed(poll[0],1000);};
        itemDialogs.add(dialog);
        dialog.setOnDismissListener(d->{handler.removeCallbacks(poll[0]);itemDialogs.remove(dialog);});dialog.show();handler.post(poll[0]);styleTree(enabled);MapTheme.styleButtons(dialog);MapTheme.gameFooter(dialog,null);
        explanation.setGravity(Gravity.CENTER_HORIZONTAL);explanation.setTextColor(MapTheme.dlgMuted());explanation.setTextSize(13);explanation.setPadding(dp(4),dp(4),dp(4),dp(6));
        status.setTextColor(MapTheme.dlgMuted());status.setTextSize(13);
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v->{StringBuilder rules=new StringBuilder(),all=new StringBuilder();
            for(int id:ids){int cap=limits.containsKey(id)?limits.get(id):100;String rule=id+":"+cap+";";all.append(rule);if(selected.contains(id))rules.append(rule);}
            Control.saveItemRules(enabled.isChecked(),rules.toString(),all.toString());dialog.dismiss();host.refresh();});
    }
    /** PGRazorX: the maximum of one item, 0 to 100000, in a small number dialog. */
    private void editItemLimit(String name,int current,java.util.function.IntConsumer done){
        Context ctx=dialogContext();EditText field=new EditText(ctx);field.setInputType(InputType.TYPE_CLASS_NUMBER);field.setSingleLine(true);field.setText(String.valueOf(current));field.setSelectAllOnFocus(true);
        FrameLayout box=new FrameLayout(ctx);box.setPadding(dp(20),dp(8),dp(20),0);box.addView(field);
        AlertDialog d=new AlertDialog.Builder(ctx).setTitle("Maximum "+name+" to keep").setView(box).setNegativeButton("Cancel",null).setPositiveButton("OK",null).create();
        d.show();MapTheme.styleField(field);MapTheme.styleButtons(d);field.requestFocus();
        if(d.getWindow()!=null)d.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_VISIBLE);
        d.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v->{int value;try{value=Integer.parseInt(field.getText().toString().trim());if(value<0||value>100000)throw new NumberFormatException();}catch(NumberFormatException invalid){field.setError("Enter 0 to 100000");return;}done.accept(value);d.dismiss();});
    }

    void refresh() {
        if(!isVisible())return;
        refreshLocationStatus();
        refreshDiagnostics();
        refreshSummaryVisibility();
        refreshTransfer();
        refreshVgpStatus();
        refreshIncubation();
        refreshVgpOptions();
        refreshBuddy();
        refreshGifts();
        refreshBattles();
        refreshRocket();
        refreshItems();
        refreshQuickCatch();
        refreshLocationOptions();
        refreshThrows();
        refreshDependencies();
    }

    private void refreshDependencies(){
        for(CheckBox box:vgpChoices)SettingsUi.enabled(box,Control.vgpOverlay());
        SettingsUi.enabled(vgpCooldownStop,Control.vgpOverlay());smartRefresh.run();
        for(CompoundButton box:transferProtection)SettingsUi.enabled(box,Control.autoTransferEnabled());
        SettingsUi.enabled(transferKeepList,Control.autoTransferEnabled());
        SettingsUi.enabled((View)transferOverlaySwitch.getParent(),Control.autoTransferEnabled());
        SettingsUi.enabled(incubatorPriority,Control.autoIncubate()&&Control.incubatorAvailable());
        SettingsUi.enabled(giftChoices[2],(Control.giftOptions()&2)!=0);
        SettingsUi.enabled((View)joystickSwitch.getParent(),Control.desiredEnabled());
        SettingsUi.enabled(mapTap,Control.desiredEnabled()&&Control.quickBallConfigured());
    }

    private void refreshLocationStatus() {
        if(uiScaleSlider!=null&&!uiScaleSlider.isPressed()){uiScaleSlider.setProgress((Control.uiScalePercent()-25)/5);uiScaleValue.setText(Control.uiScalePercent()+" %");}
        coords.setText((Control.desiredEnabled()?"":"Saved spoof location\n")+String.format(Locale.US, "%.6f, %.6f", Control.engine.latitude, Control.engine.longitude));
        speedValue.setText(String.format(Locale.US, "%.1f km/h", Control.engine.speedKmh));
        MotionEngine.RouteProgress route=Control.engine.routeProgress();
        locationSwitch.setChecked(Control.desiredEnabled());
        String movement=!Control.desiredEnabled()?"DEVICE LOCATION":Control.engine.enabled ? (Control.engine.walking ? "WALKING" : "ON") : "WAITING FOR ADAPTER";
        if (route.total>1 && route.active) movement=String.format(Locale.US,"ROUTE %d/%d  ·  %.0f m left",route.visited+1,route.total,route.remainingMeters);
        else if (route.total>1 && route.visited==route.total) movement="ON  ·  route complete";
        state.setText(Control.connection + "\n" + movement + "  ·  coordinate reads " + Control.latitudeReads + "/" + Control.longitudeReads);
        steeringStatus.setText(Control.cameraSteeringConfigured() ? Control.cameraSteeringStatus() : "Joystick follows the camera");
    }

    private void refreshDiagnostics() {
        cameraRoot.setVisibility(Control.cameraDiagnosticsConfigured() ? View.VISIBLE : View.GONE);
        cameraStatus.setText(Control.cameraDiagnosticStatus());
        cameraSample.setText(Control.cameraDiagnosticRunning() ? "Sampling camera..." : "Sample camera (10s)");
        cameraSample.setEnabled(Control.cameraDiagnosticsReady() && !Control.cameraSamplePending());
        interactionRoot.setVisibility(Control.interactionDiagnosticsConfigured() ? View.VISIBLE : View.GONE);
        interactionStatus.setText(Control.interactionDiagnosticStatus());
        interactionCapture.setText(Control.interactionDiagnosticRunning() ? "Stop capture" : "Capture catch/spin (60s)");
        interactionCapture.setEnabled(Control.interactionDiagnosticRunning() || (Control.interactionDiagnosticsReady() && !Control.interactionTracePending()));
    }

    private void refreshSummaryVisibility() {
        int summaryGate = Control.quickSummaryConfigured() ? View.VISIBLE : View.GONE;
        catchRoot.setVisibility(summaryGate); buddyRoot.setVisibility(summaryGate); giftRoot.setVisibility(summaryGate);
        itemsRoot.setVisibility(summaryGate); rocketRoot.setVisibility(summaryGate); battleRoot.setVisibility(summaryGate);
    }

    private void refreshTransfer() {
        quickSummaryStatus.setText(Control.quickSummaryStatus());
        autoTransferSwitch.setChecked(Control.autoTransferEnabled());
        for(int i=0;i<6;i++)transferProtection[i].setChecked((Control.transferProtect()&(1<<i))!=0);
        if(transferKeepList!=null){String kept=Control.transferKeepSpecies();int n=kept.isEmpty()?0:kept.split(",").length;setOption(transferKeepList,"Custom keep list",n>0?String.valueOf(n):"None",true);}
        if(transferOverlaySwitch!=null){transferOverlaySwitch.setChecked(Control.transferOverlayEnabled());transferOverlaySwitch.setEnabled(Control.autoTransferEnabled());transferOverlaySwitch.setAlpha(1f);}
        manualSwitch.setChecked(Control.manualTransferFree());manualStatus.setText(Control.manualTransferStatus());
        transferStatus.setText(Control.transferStatus());
    }

    private void refreshVgpStatus() {
        vgpSwitch.setChecked(Control.vgpOverlay());vgpStatus.setText(Control.vgpStatus());
    }

    private void refreshIncubation() {
        autoHealSwitch.setChecked(Control.autoHeal());autoHealHalfSwitch.setChecked(Control.autoHealBelowHalf());autoHealStatus.setText(Control.autoHealStatus());
        eggSkipSwitch.setChecked(Control.eggSkip());eggObserveSwitch.setChecked(Control.eggObserveOnly());
        eggKeepSwitch.setChecked(Control.eggKeepSpecial());
        eggStatus.setText(Control.eggHatchStatus());
        incubatorSwitch.setChecked(Control.autoIncubate());
        incubatorSwitch.setEnabled(Control.incubatorAvailable());
        setOption(incubatorPriority, "Priority", (Control.incubateLongest() ? "Longest" : "Shortest") + " distance", incubatorPriority.isEnabled());
        incubatorStatus.setText(Control.autoIncubate() ? Control.incubatorStatus() : "Off");
    }

    private void refreshVgpOptions() {
        for(int i=0;i<3;i++)vgpChoices[i].setChecked((Control.vgpOptions()&(1<<i))!=0);
        vgpCooldownStop.setChecked(Control.vgpStopOnCooldown());
    }

    private void refreshBuddy() {buddyRefresh.run();buddyStatus.setText(BuddyEmotionSettings.status());}

    private void refreshGifts() {
        for(int i=0;i<3;i++)giftChoices[i].setChecked((Control.giftOptions()&(1<<i))!=0);giftStatus.setText(Control.giftStatus());
    }

    private void refreshBattles() {
        for(int i=0;i<10;i++)battleChoices[i].setChecked((Control.battleOptions()&(1<<i))!=0);battleStatus.setText(Control.battleStatus());
    }

    private void refreshRocket() {
        int mask=Control.rocketOptions();boolean full=(mask&8)!=0;
        for(int i=0;i<rocketChoices.length;i++){rocketChoices[i].setChecked((mask&(1<<i))!=0||(full&&i<3));if(i<3)rocketChoices[i].setEnabled(!full);}
        rocketStatus.setText(Control.rocketStatus());
    }

    private void refreshItems() {
        setOption(itemSettings,"Auto-transfer items",Control.itemTransferEnabled()?"On":"Off",itemSettings.isEnabled());itemStatus.setText(Control.itemStatus());
    }

    private void refreshQuickCatch() {
        quickSpinSwitch.setChecked(Control.quickSpinEnabled());quickSpinStatus.setText(Control.quickSpinStatus());
        quickSummaryLabel.setText(Control.quickSummaryAutoDismiss() ? "Quick catch" : "Faster catch summary");
        quickSummarySwitch.setChecked(Control.quickSummaryEnabled());
        quickSummarySwitch.setEnabled(Control.quickSummaryConfigured());
        quickBallRoot.setVisibility(Control.quickBallConfigured() ? View.VISIBLE : View.GONE);
        quickBallStatus.setText(Control.quickBallStatus());
    }

    private void refreshLocationOptions() {
        for(int i=0;i<quickMenuChoices.length;i++)quickMenuChoices[i].setChecked(Control.quickMenuVisible(1<<i));
        idleWalkSwitch.setChecked(Control.idleWalking());idleWalkSwitch.setEnabled(Control.desiredEnabled());idleWalkSwitch.setAlpha(Control.desiredEnabled()?1f:.45f);
        setOption(mapTap,"Game map tap",new String[]{"Do nothing","Walk","Teleport","Walk / double tap teleport"}[Control.mapTapMode()],Control.quickBallConfigured());
        spawnSwitch.setChecked(Control.spawnBooster());spawnSwitch.setEnabled(Control.spawnAllowed());spawnStatus.setText(Control.spawnStatus());
    }

    private void refreshThrows() {
        setOption(throwGrade,"Throw rating",Control.throwRandom()?"Random":new String[]{"Off","Nice","Great","Excellent"}[Control.throwFixedGrade()],Control.quickBallConfigured()&&!Control.throwRandom());
        throwRandomSwitch.setChecked(Control.throwRandom());throwRandomSwitch.setEnabled(Control.quickBallConfigured());
        curveSwitch.setChecked(Control.throwCurve());curveSwitch.setEnabled(Control.quickBallConfigured());
        alwaysHitSwitch.setChecked(Control.throwAlwaysHit());alwaysHitSwitch.setEnabled(Control.throwAlwaysHitReady() || Control.throwAlwaysHit());
        arSwitch.setChecked(Control.throwAr());arSwitch.setEnabled(Control.throwArReady());
        ivCatchSwitch.setChecked(Control.ivCatchEnabled());ivCatchSwitch.setEnabled(Control.quickBallConfigured());
        ivOwnedSwitch.setChecked(Control.ivOwnedEnabled());ivOwnedSwitch.setEnabled(Control.quickBallConfigured());
        ivStatus.setText(Control.ivDisplayStatus());
    }
}
