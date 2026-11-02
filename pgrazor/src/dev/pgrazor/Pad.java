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

/** An in-activity panel: no root or system-overlay permission needed. */
public final class Pad {
    final Activity activity;
    private final FrameLayout parent;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Stick stick;
    // The full-screen settings body (switches, captions, sections) owns its own construction/refresh.
    private final SettingsPanel settings;
    private View launcher;
    private VgpToggle vgpToggle;
    private int appliedUiScale;
    private int appliedWindowWidth,appliedWindowHeight;
    private final Rect appliedWindowBounds=new Rect();
    private LinearLayout joystickOverlay;
    /** PGRazorX: joystick overlay width/height in dp. The original was 136; everything inside scales with it. */
    private static final int JOY_DP=MapTheme.Size.JOYSTICK_DP;
    /** Gap between the joystick base and its view edge, proportional to the size (12dp at 136dp). */
    private float joyInset(){return dp(JOY_DP*12f/136f);}
    private BerryOverlay encounterBerry;
    private Button cooldownTimer;
    private TextView vgpActivity, buddyEmotionStatus;
    private OverlayDrag buddyEmotionDrag;
    private GymFeedOverlay gymFeedOverlay;
    private TextView gymAutomationStatus;
    private TextView automationStatus;
    private OverlayDrag automationDrag;
    private OverlayDrag gymFeedDrag;
    private VgpMarker vgpMarker;
    // Draggable, position-persisting handles for the cooldown and VGP overlays.
    private OverlayDrag cooldownDrag, vgpDrag;
    private QuickMenu quickMenu;
    // Draggable, position-persisting Keep All / transfer-rules overlay button.
    private TransferToggle transferToggle;
    private OverlayDrag transferDrag;
    private GameRoutesOverlay gameRoutesOverlay;
    private OverlayDrag gameRoutesDrag;

    void tapEggScreen(long command,long expires){
        if(closed||settings.isVisible()||(quickMenu!=null&&quickMenu.isShowing())||activity.isFinishing()||activity.isDestroyed()||!Control.eggTapValid(command,expires))return;
        final View game=activity.getWindow()==null?null:activity.getWindow().getDecorView();
        if(game==null||!game.isShown()||!game.hasWindowFocus()||game.getWidth()<=0||game.getHeight()<=0){Control.event("egg-hatch-touch","blocked: activity window unavailable");return;}
        final long down=SystemClock.uptimeMillis();final float x=game.getWidth()*.5f,y=game.getHeight()*.42f;
        if(!eggTouch(down,down,MotionEvent.ACTION_DOWN,x,y))return;
        handler.postDelayed(() -> {
            boolean valid=!closed&&!activity.isFinishing()&&!activity.isDestroyed()&&Control.eggTapValid(command,expires)&&game.hasWindowFocus();
            eggTouch(down,SystemClock.uptimeMillis(),valid?MotionEvent.ACTION_UP:MotionEvent.ACTION_CANCEL,x,y);
        },60);
    }
    private boolean eggTouch(long down,long time,int action,float x,float y){
        MotionEvent.PointerProperties pointer=new MotionEvent.PointerProperties();pointer.id=0;pointer.toolType=MotionEvent.TOOL_TYPE_FINGER;
        MotionEvent.PointerCoords coords=new MotionEvent.PointerCoords();coords.x=x;coords.y=y;coords.pressure=action==MotionEvent.ACTION_UP||action==MotionEvent.ACTION_CANCEL?0:1;coords.size=.1f;
        MotionEvent event=MotionEvent.obtain(down,time,action,1,new MotionEvent.PointerProperties[]{pointer},new MotionEvent.PointerCoords[]{coords},0,0,1,1,0,0,InputDevice.SOURCE_TOUCHSCREEN,0);
        Control.markSyntheticTouch();
        try{boolean accepted=activity.dispatchTouchEvent(event);Control.event("egg-hatch-touch","action="+action+"; accepted="+accepted+"; route=Activity.dispatchTouchEvent");return accepted;}
        catch(RuntimeException error){Control.event("egg-hatch-touch","error="+error.getClass().getSimpleName());return false;}
        finally{event.recycle();}
    }
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

    /** Re-theme the overlays Pad owns, then the settings body. Invoked on dark/light toggle. */
    /** Cooldown retains its framed timer treatment, allocated only on theme changes. */
    private void styleStatusOverlay(TextView view,boolean multiline){
        if(view==null)return;
        GradientDrawable wash=MapTheme.shape(activity,MapTheme.accentSoft(),0,10);
        wash.setStroke(dp(2),MapTheme.accent());
        view.setBackground(new android.graphics.drawable.LayerDrawable(new android.graphics.drawable.Drawable[]{
            MapTheme.shape(activity,(MapTheme.card()&0x00ffffff)|0xeb000000,0,10),wash}));
        MapTheme.elevate(view,3);view.setGravity(Gravity.CENTER);view.setTypeface(MapTheme.medium());
        view.setTextColor(accent());view.setPadding(dp(10),multiline?dp(6):0,dp(10),multiline?dp(6):0);
        if(multiline)view.setTextSize(MapTheme.Size.COOLDOWN_TEXT_SP);
    }
    /** Compact status text on the same rounded surface as the VGP OFF tile. */
    private void styleMapStatus(TextView view){
        if(view==null)return;
        view.setBackground(MapTheme.shape(activity,(MapTheme.card()&0x00ffffff)|0xeb000000,MapTheme.outline(),10));MapTheme.elevate(view,3);
        view.setTextSize(10);view.setTypeface(Typeface.DEFAULT);
        view.setTextColor(muted());view.setPadding(dp(4),dp(6),dp(4),dp(6));
        view.setLineSpacing(dp(1),1f);view.setGravity(Gravity.TOP|Gravity.CENTER_HORIZONTAL);
        view.setMaxLines(Integer.MAX_VALUE);view.setEllipsize(null);
        if(view==vgpActivity||view==gymAutomationStatus){
            android.view.ViewGroup.LayoutParams lp=view.getLayoutParams();lp.width=dp(53);view.setLayoutParams(lp);
            view.setMaxWidth(dp(53));view.setMinHeight(dp(112));view.setMinimumHeight(dp(112));
        }
    }
    private CharSequence mapStatusLine(String title,String detail,boolean on){
        String head="\u25CF "+title;
        android.text.SpannableString text=new android.text.SpannableString(head+"\n"+detail);
        text.setSpan(new android.text.style.ForegroundColorSpan(on?accent():muted()),0,1,0);
        text.setSpan(new android.text.style.RelativeSizeSpan(0.8f),0,1,0);
        text.setSpan(new android.text.style.StyleSpan(Typeface.BOLD),2,head.length(),0);
        text.setSpan(new android.text.style.ForegroundColorSpan(muted()),head.length(),text.length(),0);
        return text;
    }
    private void setMapStatus(TextView view,String title,String detail,boolean on){
        String key=title+"\n"+detail+"\n"+on;
        if(!key.equals(view.getTag())){view.setTag(key);view.setText(mapStatusLine(title,detail,on));}
    }
    private void applyTheme(){
        settings.applyTheme();
        if(launcher!=null){MapTheme.elevate(launcher,3);launcher.setOutlineProvider(SettingsUi.roundRectOutline(16f/54f));launcher.invalidate();}
        if(joystickOverlay!=null){joystickOverlay.setBackground(MapTheme.shape(activity,(MapTheme.dlgCard()&0x00ffffff)|0xe6000000,0,24));joystickOverlay.setElevation(0);styleTree(joystickOverlay);stick.invalidate();}
        if(encounterBerry!=null){encounterBerry.setElevation(0);encounterBerry.invalidate();}
        styleStatusOverlay(cooldownTimer,false);
        if(transferToggle!=null){MapTheme.elevate(transferToggle,3);transferToggle.setOutlineProvider(SettingsUi.roundRectOutline(14f/54f));transferToggle.invalidate();}
        if(gameRoutesOverlay!=null){MapTheme.elevate(gameRoutesOverlay,3);gameRoutesOverlay.invalidate();}
        // Status pill backgrounds depend only on the theme; refresh() must not reallocate them every tick.
        styleMapStatus(gymAutomationStatus);
        styleMapStatus(buddyEmotionStatus);
        if(buddyEmotionStatus!=null)buddyEmotionStatus.setText(mapStatusLine("EMOTION",BuddyEmotionSettings.status(),BuddyEmotionSettings.enabled()));
        styleMapStatus(automationStatus);
        if(vgpToggle!=null){MapTheme.elevate(vgpToggle,3);vgpToggle.invalidate();}
        if(gymFeedOverlay!=null){MapTheme.elevate(gymFeedOverlay,3);gymFeedOverlay.invalidate();}
        styleMapStatus(vgpActivity);
        if(vgpActivity!=null)vgpActivity.setText(mapStatusLine("VGP",Control.vgpStatus(),Control.vgpEnabled()));
        if(gymAutomationStatus!=null)gymAutomationStatus.setText(mapStatusLine("GYM",GymAutomationSettings.overlayStatus(),GymAutomationSettings.running()));
        if(quickMenu!=null)quickMenu.applyTheme();
        applyOverlayOpacity();
    }
    /** PGRazorX: the opacity setting affects everything drawn over the map, including the quick menu, but never the settings panel. */
    private void applyOverlayOpacity(){
        float a=Control.overlayOpacity()/100f;
        for(View v:new View[]{launcher,cooldownTimer,buddyEmotionStatus,vgpToggle,vgpActivity,vgpMarker,gymFeedOverlay,gymAutomationStatus,gameRoutesOverlay,transferToggle,encounterBerry,joystickOverlay,automationStatus})if(v!=null)v.setAlpha(a);
        if(quickMenu!=null)quickMenu.setOpacity(a);
    }
    private void draggable(View handle,View target,boolean clickable){
        handle.setOnTouchListener(new View.OnTouchListener(){float downX,downY,originX,originY;boolean moved;
            public boolean onTouch(View v,MotionEvent e){int a=e.getActionMasked();if(a==MotionEvent.ACTION_DOWN){downX=e.getRawX();downY=e.getRawY();originX=target.getX();originY=target.getY();moved=false;return true;}
                if(a==MotionEvent.ACTION_MOVE){float dx=e.getRawX()-downX,dy=e.getRawY()-downY;if(Math.hypot(dx,dy)>dp(6))moved=true;if(moved){Rect safe=UiViewport.bounds(parent);target.setX(UiViewportPolicy.clamp(originX+dx,safe.left,safe.right,OverlayDrag.visualWidth(target)));target.setY(UiViewportPolicy.clamp(originY+dy,safe.top,safe.bottom,OverlayDrag.visualHeight(target)));}return true;}
                if(a==MotionEvent.ACTION_UP){if(!moved&&clickable)v.performClick();return true;}return true;}
        });
    }
    /**
     * Floating launcher (PGRazorX original emblem): a flat 2D sphere cut by a straight dark slash, its two
     * halves sliding apart along the cut, with short speed lines and a spark at the blade tip.
     */
    private final class RazorIcon extends View {
        private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Path top=new Path(),bottom=new Path(),spark=new Path(),topBtn=new Path(),bottomBtn=new Path();
        RazorIcon(Context c){super(c);setContentDescription("Open PGRazor quick menu. Drag to reposition.");setFocusable(true);setClickable(true);setOutlineProvider(SettingsUi.roundRectOutline(16f/54f));
            float r=12.5f,shift=2.6f;
            top.moveTo(24-shift-r,24);top.arcTo(new RectF(24-shift-r,24-r,24-shift+r,24+r),180,180,false);top.close();
            bottom.moveTo(24+shift+r,24);bottom.arcTo(new RectF(24+shift-r,24-r,24+shift+r,24+r),0,180,false);bottom.close();
            float btnRadius=6.0f;
            topBtn.moveTo(24-shift-btnRadius,24);topBtn.arcTo(new RectF(24-shift-btnRadius,24-btnRadius,24-shift+btnRadius,24+btnRadius),180,180,false);topBtn.close();
            bottomBtn.moveTo(24+shift+btnRadius,24);bottomBtn.arcTo(new RectF(24+shift-btnRadius,24-btnRadius,24+shift+btnRadius,24+btnRadius),0,180,false);bottomBtn.close();
            float sx=41.6f,sy=24,ss=3.2f,k=ss*.25f;
            spark.moveTo(sx,sy-ss);spark.lineTo(sx+k,sy-k);spark.lineTo(sx+ss,sy);spark.lineTo(sx+k,sy+k);spark.lineTo(sx,sy+ss);spark.lineTo(sx-k,sy+k);spark.lineTo(sx-ss,sy);spark.lineTo(sx-k,sy-k);spark.close();}
        protected void onDraw(Canvas c){super.onDraw(c);float scale=Math.min(getWidth(),getHeight())/48f;c.save();c.scale(scale,scale);
            int ink=MapTheme.dlgInk(),edge=(MapTheme.closeFill()&0x00ffffff)|0x80000000;
            paint.setShader(null);paint.setStrokeCap(Paint.Cap.ROUND);paint.setStrokeJoin(Paint.Join.ROUND);
            boolean open=quickMenu!=null&&quickMenu.isShowing();if(getTag()==null)getViewTreeObserver().addOnGlobalLayoutListener(()->{if(!Boolean.valueOf(quickMenu!=null&&quickMenu.isShowing()).equals(getTag()))invalidate();});setTag(open);c.save();c.scale(48f/54f,48f/54f);paint.setStyle(Paint.Style.FILL);paint.setColor((MapTheme.card()&0x00ffffff)|0xeb000000);c.drawRoundRect(1,1,53,53,16,16,paint);if(open){paint.setColor(MapTheme.accentSoft());c.drawRoundRect(1,1,53,53,16,16,paint);}
            paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(open?2f:1f);paint.setColor(open?MapTheme.accent():MapTheme.outline());c.drawRoundRect(open?2:1.5f,open?2:1.5f,open?52:52.5f,open?52:52.5f,15,15,paint);c.restore();
            c.saveLayerAlpha(0,0,48,48,open?255:90);paint.setStrokeWidth(1.4f);paint.setColor(MapTheme.accent());
            c.drawLine(35.5f,15.5f,38.0f,15.5f,paint);c.drawLine(34.8f,19.6f,36.8f,19.6f,paint);
            c.drawLine(12.5f,32.5f,10.0f,32.5f,paint);c.drawLine(13.2f,28.4f,11.2f,28.4f,paint);
            paint.setStyle(Paint.Style.FILL);paint.setShader(new LinearGradient(11,11,37,37,MapTheme.accentLime(),MapTheme.accent(),Shader.TileMode.CLAMP));c.drawPath(top,paint);
            paint.setStyle(Paint.Style.STROKE);paint.setShader(null);paint.setStrokeWidth(1.2f);paint.setColor(edge);c.drawPath(top,paint);
            paint.setStyle(Paint.Style.FILL);paint.setShader(new LinearGradient(11,11,37,37,MapTheme.accentLime(),MapTheme.accent(),Shader.TileMode.CLAMP));c.drawPath(bottom,paint);
            paint.setStyle(Paint.Style.STROKE);paint.setShader(null);paint.setStrokeWidth(1.2f);paint.setColor(edge);c.drawPath(bottom,paint);
            float innerBtnRadius=2.6f,shift=2.6f;
            paint.setStyle(Paint.Style.FILL);paint.setColor(Color.WHITE);c.drawPath(topBtn,paint);
            paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(1.1f);paint.setColor(edge);c.drawPath(topBtn,paint);
            paint.setStyle(Paint.Style.FILL);paint.setColor(MapTheme.accent());c.drawArc(24-shift-innerBtnRadius,24-innerBtnRadius,24-shift+innerBtnRadius,24+innerBtnRadius,180,180,true,paint);
            paint.setStyle(Paint.Style.FILL);paint.setColor(Color.WHITE);c.drawPath(bottomBtn,paint);
            paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(1.1f);paint.setColor(edge);c.drawPath(bottomBtn,paint);
            paint.setStyle(Paint.Style.FILL);paint.setColor(MapTheme.accent());c.drawArc(24+shift-innerBtnRadius,24-innerBtnRadius,24+shift+innerBtnRadius,24+innerBtnRadius,0,180,true,paint);
            paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(2.4f);paint.setColor(MapTheme.card());c.drawLine(7.1f,24,40.9f,24,paint);paint.setStrokeWidth(0.8f);paint.setColor(edge);c.drawLine(9f,24,39f,24,paint);
            paint.setStyle(Paint.Style.FILL);paint.setColor(MapTheme.dlgLime());c.drawPath(spark,paint);c.restore();
            c.restore();}
    }

    private long lastTick = SystemClock.elapsedRealtime(), lastLog;
    private boolean closed, lastEnabled;
    private AlertDialog stopRouteDialog;
    public Pad(Activity host) {
        activity = host;
        parent = (FrameLayout)host.getWindow().getDecorView();
        // Build the settings body first (not yet attached); it calls back through the narrow host
        // interface for the few overlay/joystick/engine actions Pad still owns.
        settings = new SettingsPanel(host, new SettingsPanel.Host(){
            public void refresh(){ Pad.this.refresh(); }
            public void applyTheme(){ Pad.this.applyTheme(); }
            public void releaseJoystick(boolean preserveWalk){ stick.release(preserveWalk); }
            public void setJoystickVisible(boolean visible){ stick.release(true); Control.setJoystickVisible(visible); joystickOverlay.setVisibility(visible?View.VISIBLE:View.GONE); }
            public void openMap(){ openWorldMap(); }
            public void editLocation(){ edit(); }
            public void applyOverlayOpacity(){ Pad.this.applyOverlayOpacity(); }
        });

        stick = new Stick(host);
        joystickOverlay = new LinearLayout(host); joystickOverlay.setOrientation(LinearLayout.VERTICAL);
        TextView grip = text("• • •", 12, muted()); grip.setGravity(Gravity.CENTER); joystickOverlay.addView(grip, new LinearLayout.LayoutParams(-1, dp(Math.max(18, JOY_DP * 24 / 92))));
        joystickOverlay.addView(stick, new LinearLayout.LayoutParams(dp(JOY_DP), dp(JOY_DP))); draggable(grip, joystickOverlay, false);

        // The settings panel is a full-screen modal; it consumes taps so the game behind it stays inert.
        parent.addView(settings.view(), new FrameLayout.LayoutParams(-1, -1));

        FrameLayout.LayoutParams joyLayout=new FrameLayout.LayoutParams(dp(JOY_DP),-2,Gravity.LEFT|Gravity.BOTTOM);joyLayout.leftMargin=dp(16);joyLayout.bottomMargin=dp(96);parent.addView(joystickOverlay,joyLayout);

        // Compact catching-berry overlay; the popup chooses owned berries and Off.
        encounterBerry=new BerryOverlay(activity,this::refresh);
        FrameLayout.LayoutParams berryLayout=new FrameLayout.LayoutParams(dp(90),dp(50),Gravity.RIGHT|Gravity.CENTER_VERTICAL);berryLayout.rightMargin=dp(10);parent.addView(encounterBerry,berryLayout);encounterBerry.setVisibility(View.GONE);

        cooldownTimer=SettingsUi.overlayButton(activity,"History  --:--");cooldownTimer.setTextSize(MapTheme.Size.COOLDOWN_TEXT_SP);SettingsUi.fitText(cooldownTimer,MapTheme.Size.COOLDOWN_TEXT_SP);cooldownTimer.setAllCaps(false);cooldownTimer.setTextColor(accent());FrameLayout.LayoutParams timerLayout=new FrameLayout.LayoutParams(dp(MapTheme.Size.COOLDOWN_WIDTH_DP),dp(MapTheme.Size.COOLDOWN_HEIGHT_DP),Gravity.TOP|Gravity.RIGHT);timerLayout.topMargin=dp(44);timerLayout.rightMargin=dp(12);parent.addView(cooldownTimer,timerLayout);cooldownTimer.setOnClickListener(v->Cooldown.show(activity));cooldownDrag=OverlayDrag.attach(parent,cooldownTimer,"cooldown");

        // Window attachment: the panel pads itself inside the system bars/IME as insets arrive.
        View panelView=settings.view();
        panelView.setOnApplyWindowInsetsListener((view,insets) -> {settings.applyInsets(insets);return insets;});
        panelView.requestApplyInsets();

        vgpToggle=new VgpToggle(host);FrameLayout.LayoutParams vgpButtonLayout=new FrameLayout.LayoutParams(dp(54),dp(54),Gravity.TOP|Gravity.LEFT);vgpButtonLayout.leftMargin=dp(12);vgpButtonLayout.topMargin=dp(156);parent.addView(vgpToggle,vgpButtonLayout);vgpToggle.setOnClickListener(v->{Control.setVgpEnabled(!Control.vgpEnabled());refresh();});
        vgpActivity=text("VGP",12,ink());vgpActivity.setPadding(dp(10),dp(6),dp(10),dp(6));vgpActivity.setMaxLines(2);vgpActivity.setEllipsize(android.text.TextUtils.TruncateAt.END);
        FrameLayout.LayoutParams vgpLayout=new FrameLayout.LayoutParams(dp(220),-2,Gravity.TOP|Gravity.LEFT);vgpLayout.leftMargin=dp(12);vgpLayout.topMargin=dp(216);parent.addView(vgpActivity,vgpLayout);vgpActivity.setVisibility(View.GONE);
        buddyEmotionStatus=text("EP: waiting",12,ink());buddyEmotionStatus.setPadding(dp(10),dp(6),dp(10),dp(6));buddyEmotionStatus.setMaxLines(3);
        FrameLayout.LayoutParams buddyEpLayout=new FrameLayout.LayoutParams(dp(240),-2,Gravity.TOP|Gravity.LEFT);buddyEpLayout.leftMargin=dp(12);buddyEpLayout.topMargin=dp(265);parent.addView(buddyEmotionStatus,buddyEpLayout);buddyEmotionStatus.setVisibility(View.GONE);
        buddyEmotionStatus.setOnClickListener(v->BuddyEmotionSettings.show(activity));buddyEmotionDrag=OverlayDrag.attach(parent,buddyEmotionStatus,"buddy-emotion");
        gymFeedOverlay=new GymFeedOverlay(activity);FrameLayout.LayoutParams gymFeedLayout=new FrameLayout.LayoutParams(dp(54),dp(54),Gravity.TOP|Gravity.LEFT);gymFeedLayout.leftMargin=dp(12);gymFeedLayout.topMargin=dp(375);parent.addView(gymFeedOverlay,gymFeedLayout);
        gymAutomationStatus=text("Off",11,ink());gymAutomationStatus.setPadding(dp(6),dp(3),dp(6),dp(3));gymAutomationStatus.setMaxLines(1);gymAutomationStatus.setMaxWidth(dp(165));gymAutomationStatus.setEllipsize(android.text.TextUtils.TruncateAt.END);
        parent.addView(gymAutomationStatus,new FrameLayout.LayoutParams(-2,-2,Gravity.TOP|Gravity.LEFT));
        gymFeedDrag=OverlayDrag.attach(parent,gymFeedOverlay,gymAutomationStatus,"gym-feed");
        automationStatus=text("",11,ink());automationStatus.setPadding(dp(8),dp(5),dp(8),dp(5));automationStatus.setMaxLines(3);
        FrameLayout.LayoutParams automationLayout=new FrameLayout.LayoutParams(dp(220),-2,Gravity.TOP|Gravity.LEFT);automationLayout.leftMargin=dp(12);automationLayout.topMargin=dp(440);
        parent.addView(automationStatus,automationLayout);automationStatus.setVisibility(View.GONE);
        automationStatus.setOnClickListener(v->AutomationPanel.show(activity));automationDrag=OverlayDrag.attach(parent,automationStatus,"automation");
        // The toggle is the drag handle; the status pill follows it as one coherent unit. Tap still flips VGP.
        vgpDrag=OverlayDrag.attach(parent,vgpToggle,vgpActivity,"vgp");
        vgpMarker=new VgpMarker(host);parent.addView(vgpMarker,new FrameLayout.LayoutParams(dp(38),dp(46)));vgpMarker.setVisibility(View.GONE);
        // Compact Keep All / transfer-rules overlay; tap toggles Keep All, drag repositions (persists transfer.fx/fy).
        transferToggle=new TransferToggle(host);FrameLayout.LayoutParams transferLayout=new FrameLayout.LayoutParams(dp(54),dp(54),Gravity.TOP|Gravity.LEFT);transferLayout.leftMargin=dp(12);transferLayout.topMargin=dp(300);parent.addView(transferToggle,transferLayout);transferToggle.setOnClickListener(v->{Control.setTransferKeepAll(!Control.transferKeepAll());refresh();});
        transferDrag=OverlayDrag.attach(parent,transferToggle,"transfer");transferToggle.setVisibility(View.GONE);
        gameRoutesOverlay=new GameRoutesOverlay(activity);
        FrameLayout.LayoutParams routesLayout=new FrameLayout.LayoutParams(dp(48),dp(48),Gravity.TOP|Gravity.RIGHT);
        routesLayout.rightMargin=dp(14);routesLayout.topMargin=dp(100);parent.addView(gameRoutesOverlay,routesLayout);
        gameRoutesOverlay.setVisibility(View.GONE);gameRoutesDrag=OverlayDrag.attach(parent,gameRoutesOverlay,"game-routes");
        launcher=new RazorIcon(host);FrameLayout.LayoutParams iconLayout=new FrameLayout.LayoutParams(dp(MapTheme.Size.LAUNCHER_DP),dp(MapTheme.Size.LAUNCHER_DP),Gravity.TOP|Gravity.LEFT);iconLayout.leftMargin=dp(12);iconLayout.topMargin=dp(100);parent.addView(launcher,iconLayout);
        launcher.setContentDescription("Open PGRazor quick menu. Drag to reposition.");
        // The R launcher now toggles the compact quick menu; Settings there opens this panel full screen.
        quickMenu=new QuickMenu(host,parent,launcher,new QuickMenu.Listener(){
            public void onMap(){openWorldMap();}
            public double onCycleWalkSpeed(){double s=Control.cycleWalkSpeed();refresh();return s;}
            public void onAutoWalk(){stick.release(true);AutoWalkDialog.show(activity);}
            public void onGameRoutes(){stick.release(true);GameRoutesDialog.show(activity);}
            public void onFeeds(){stick.release(true);LiveFeedDialog.show(activity);}
            public void onFavorites(){QuickLocationDialogs.showFavorites(activity,Pad.this::quickTeleport);}
            public void onTeleport(){QuickLocationDialogs.showTeleport(activity,Pad.this::quickTeleport);}
            public void onHotspots(){stick.release(true);HotspotsDialog.showQuick(activity,Pad.this::quickTeleport,Pad.this::quickWalk);}
            public void onAccounts(){stick.release(true);AccountsDialog.show(activity);}
            public void onSettings(){openSettings();}
        });
        launcher.setOnClickListener(v->{if(quickMenu!=null)quickMenu.toggle();});draggable(launcher,launcher,true);

        settings.hide();joystickOverlay.setVisibility(Control.joystickVisible()?View.VISIBLE:View.GONE);applyTheme();
        refresh();parent.post(this::applyMapScale); handler.post(tick);
    }
    /** Decode a bundled map-icon once; returns null (never throws, never hits the network) if unavailable. */
    private Bitmap decodeMapIcon(String name){
        try(java.io.InputStream in=activity.getAssets().open("pgrazor/map-icons/"+name)){return BitmapFactory.decodeStream(in);}
        catch(java.io.IOException|RuntimeException ignored){return null;}
    }

    private final class VgpToggle extends View {
        private final OverlayToggleArt art=new OverlayToggleArt();
        VgpToggle(Context context){super(context);setFocusable(true);setClickable(true);setOutlineProvider(SettingsUi.roundRectOutline(16f/54f));}
        protected void onDraw(Canvas canvas){super.onDraw(canvas);canvas.save();canvas.scale(getWidth()/54f,getHeight()/54f);art.vgp(canvas,Control.vgpEnabled());canvas.restore();}
    }
    private final class VgpMarker extends View {
        private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
        VgpMarker(Context context){super(context);setClickable(false);setFocusable(false);setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);}
        protected void onDraw(Canvas canvas){super.onDraw(canvas);canvas.save();canvas.scale(getWidth()/38f,getHeight()/46f);
            paint.setStyle(Paint.Style.FILL);paint.setColor(Color.WHITE);canvas.drawCircle(19,18,17,paint);
            paint.setColor(Control.vgpTargetSpin()?Color.rgb(42,170,240):Color.rgb(235,68,85));canvas.drawCircle(19,18,14,paint);
            paint.setColor(Color.WHITE);
            if(Control.vgpTargetSpin()){paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(3);canvas.drawCircle(19,18,8,paint);canvas.drawLine(19,5,19,10,paint);canvas.drawLine(19,26,19,31,paint);canvas.drawLine(6,18,11,18,paint);canvas.drawLine(27,18,32,18,paint);}
            else{paint.setStrokeWidth(3);canvas.drawLine(5,18,33,18,paint);canvas.drawCircle(19,18,5,paint);paint.setColor(Color.rgb(50,55,65));canvas.drawCircle(19,18,2.5f,paint);}
            paint.setStyle(Paint.Style.FILL);paint.setColor(Color.WHITE);Path arrow=new Path();arrow.moveTo(13,36);arrow.lineTo(25,36);arrow.lineTo(19,44);arrow.close();canvas.drawPath(arrow,paint);canvas.restore();
        }
    }
    /**
     * Compact overlay for the transfer policy with three clearly distinct states: KEEP (Keep All on,
     * manual catches are kept), XFER (Keep All off and the auto-transfer master on, so the rules run),
     * and OFF (auto-transfer master off, so nothing transfers regardless). Tap toggles Keep All only;
     * it never re-enables the master.
     */
    private final class TransferToggle extends View {
        private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
        TransferToggle(Context c){super(c);setFocusable(true);setClickable(true);}
        protected void onDraw(Canvas canvas){super.onDraw(canvas);
            boolean keep=Control.transferKeepAll(),master=Control.autoTransferEnabled();
            int tint=keep?Color.rgb(33,188,125):(master?Color.rgb(230,150,40):muted());
            canvas.save();canvas.scale(getWidth()/54f,getHeight()/54f);
            paint.setStyle(Paint.Style.FILL);paint.setColor(MapTheme.card());canvas.drawRoundRect(1,1,53,53,14,14,paint);
            // Shield outline frames the state glyph.
            paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(2.4f);paint.setColor(tint);paint.setStrokeCap(Paint.Cap.ROUND);
            Path shield=new Path();shield.moveTo(27,7);shield.lineTo(40,12);shield.lineTo(40,25);shield.cubicTo(40,34,34,39,27,42);shield.cubicTo(20,39,14,34,14,25);shield.lineTo(14,12);shield.close();canvas.drawPath(shield,paint);
            if(keep){canvas.drawLine(21,24,25,29,paint);canvas.drawLine(25,29,34,18,paint);} // check
            else if(master){canvas.drawLine(19,24,33,24,paint);Path a=new Path();a.moveTo(29,19);a.lineTo(35,24);a.lineTo(29,29);canvas.drawPath(a,paint);} // transfer-out arrow
            else{canvas.drawLine(21,24,33,24,paint);} // dash: master off
            paint.setStyle(Paint.Style.FILL);paint.setColor(tint);paint.setTextSize(10);paint.setTypeface(Typeface.DEFAULT_BOLD);paint.setTextAlign(Paint.Align.CENTER);
            canvas.drawText(keep?"KEEP":(master?"XFER":"OFF"),27,51,paint);
            canvas.restore();}
    }
    private String transferToggleDescription(){
        boolean keep=Control.transferKeepAll(),master=Control.autoTransferEnabled();
        String state=keep?"Keep All on; manual catches are kept":(master?"Normal transfer rules apply":"Auto transfer is off");
        return "Manual encounter transfer: "+state+". Tap to "+(keep?"use normal transfer rules":"keep manual catches")+". Drag to reposition.";
    }

    private void applyMapScale(){
        if(launcher==null||launcher.getWidth()==0||parent.getWidth()==0)return;
        Rect safe=UiViewport.bounds(parent);
        int percent=Control.uiScalePercent();
        if(appliedUiScale==percent&&appliedWindowWidth==parent.getWidth()&&appliedWindowHeight==parent.getHeight()&&appliedWindowBounds.equals(safe))return;
        Rect previousSafe=appliedWindowBounds.isEmpty()?safe:new Rect(appliedWindowBounds);
        appliedWindowBounds.set(safe);appliedWindowWidth=parent.getWidth();appliedWindowHeight=parent.getHeight();
        appliedUiScale=percent;float scale=percent/100f;
        for(View view:new View[]{launcher,joystickOverlay,cooldownTimer,buddyEmotionStatus,vgpToggle,vgpActivity,vgpMarker,transferToggle,gameRoutesOverlay,gymFeedOverlay,gymAutomationStatus}){
            if(view==null)continue;
            float x=view.getX(),y=view.getY();
            int oldWidth=OverlayDrag.visualWidth(view),oldHeight=OverlayDrag.visualHeight(view);
            float fitted=UiViewportPolicy.fit(scale,view.getWidth(),view.getHeight(),safe.width(),safe.height());
            view.setPivotX(0);view.setPivotY(0);view.setScaleX(fitted);view.setScaleY(fitted);
            view.setX(UiViewportPolicy.resizePosition(x,previousSafe.left,previousSafe.right,oldWidth,safe.left,safe.right,OverlayDrag.visualWidth(view)));
            view.setY(UiViewportPolicy.resizePosition(y,previousSafe.top,previousSafe.bottom,oldHeight,safe.top,safe.bottom,OverlayDrag.visualHeight(view)));
        }
        if(cooldownDrag!=null)cooldownDrag.refreshBounds();if(vgpDrag!=null)vgpDrag.refreshBounds();
        if(transferDrag!=null)transferDrag.refreshBounds();
        if(gameRoutesDrag!=null)gameRoutesDrag.refreshBounds();
        if(gymFeedDrag!=null)gymFeedDrag.refreshBounds();
        if(buddyEmotionDrag!=null)buddyEmotionDrag.refreshBounds();
        if(encounterBerry!=null)encounterBerry.applyUiScale(scale);
        if(quickMenu!=null)quickMenu.applyScale(scale);
    }
    private void refresh() {
        applyMapScale();
        if(gymFeedOverlay!=null)gymFeedOverlay.refresh(!settings.isVisible()&&!(quickMenu!=null&&quickMenu.isShowing()));
        if(gymAutomationStatus!=null){gymAutomationStatus.setVisibility(OverlayTextSettings.hidden(activity,"gym-feed")?View.GONE:gymFeedOverlay.getVisibility());setMapStatus(gymAutomationStatus,"GYM",GymAutomationSettings.overlayStatus(),GymAutomationSettings.running());gymFeedDrag.refreshBounds();}
        if(vgpToggle!=null){vgpToggle.setVisibility(Control.vgpOverlayVisible()&&!settings.isVisible()&&!(quickMenu!=null&&quickMenu.isShowing())?View.VISIBLE:View.GONE);vgpToggle.setContentDescription("Virtual Go Plus "+(Control.vgpEnabled()?"on; tap to turn off":"off; tap to turn on")+". "+Control.vgpStatus());vgpToggle.invalidate();}

        // Pair the status line with the toggle so the cooldown shutoff reason is visible even when VGP is off.
        if(buddyEmotionStatus!=null){buddyEmotionStatus.setVisibility(BuddyEmotionSettings.overlayEnabled()&&!settings.isVisible()&&!(quickMenu!=null&&quickMenu.isShowing())?View.VISIBLE:View.GONE);setMapStatus(buddyEmotionStatus,"EMOTION",BuddyEmotionSettings.status(),BuddyEmotionSettings.enabled());}
        if(automationStatus!=null){automationStatus.setVisibility(OneShotAutomation.overlayVisible()&&!settings.isVisible()&&!(quickMenu!=null&&quickMenu.isShowing())?View.VISIBLE:View.GONE);setTextIfChanged(automationStatus,OneShotAutomation.overlayStatus());automationStatus.setTextColor(muted());automationDrag.refreshBounds();}
        if(vgpActivity!=null){vgpActivity.setVisibility(!OverlayTextSettings.hidden(activity,"vgp")&&Control.vgpOverlayVisible()&&!settings.isVisible()&&!(quickMenu!=null&&quickMenu.isShowing())?View.VISIBLE:View.GONE);setMapStatus(vgpActivity,"VGP",Control.vgpStatus(),Control.vgpEnabled());}
        if(vgpMarker!=null){boolean show=Control.vgpTargetVisible()&&!settings.isVisible();vgpMarker.setVisibility(show?View.VISIBLE:View.GONE);if(show){vgpMarker.setX(Control.vgpTargetX()*parent.getWidth()-dp(19)*vgpMarker.getScaleX());vgpMarker.setY(Control.vgpTargetY()*parent.getHeight()-dp(36)-dp(46)*vgpMarker.getScaleY());vgpMarker.invalidate();}}
        if(transferToggle!=null){transferToggle.setVisibility(Control.transferOverlayVisible()&&!settings.isVisible()&&!(quickMenu!=null&&quickMenu.isShowing())?View.VISIBLE:View.GONE);transferToggle.setContentDescription(transferToggleDescription());transferToggle.invalidate();}

        String routeReward=Control.takeGameRoutesRewardNotice();
        if(!routeReward.isEmpty())Toast.makeText(activity,routeReward,Toast.LENGTH_LONG).show();
        if(gameRoutesOverlay!=null)gameRoutesOverlay.refresh(!settings.isVisible()&&!(quickMenu!=null&&quickMenu.isShowing()));
        if(cooldownTimer!=null){cooldownTimer.setVisibility(Control.cooldownOverlay()?View.VISIBLE:View.GONE);setTextIfChanged(cooldownTimer,"☰  "+Cooldown.label());cooldownTimer.setTextColor(Cooldown.remaining()>0?MapTheme.warn():accent());cooldownTimer.setTypeface(MapTheme.medium());}
        if (lastEnabled != Control.engine.enabled) { lastEnabled = Control.engine.enabled; stick.invalidate(); }
        if(encounterBerry!=null)encounterBerry.refresh();
        if(quickMenu!=null)quickMenu.onRefresh();
        // The hidden panel is GONE; openSettings() refreshes it immediately after show().
        if(settings.isVisible())settings.refresh();
    }
    /** TextView.setText requests a layout pass even for identical text; this tick runs every 100 ms. */
    private static void setTextIfChanged(TextView view,CharSequence value){
        if(!android.text.TextUtils.equals(view.getText(),value))view.setText(value);
    }
    private long lastUiRefresh;
    private final Runnable tick = new Runnable() {
        public void run() {
            if (closed) return;
            Control.checkLease();
            long now = SystemClock.elapsedRealtime();
            stick.applyMotion(now);
            boolean wasWalking = Control.engine.walking;
            MotionEngine.RouteProgress before=Control.engine.routeProgress();
            Control.stepMotion((now-lastTick)/1000.0,stick.pointer>=0); lastTick = now;
            Control.tickGameRoutes();
            MotionEngine.RouteProgress after=Control.engine.routeProgress();
            if (before.total>1 && after.total==before.total && after.visited>before.visited)
                Control.event("route-progress", "visited=" + after.visited + "/" + after.total);
            if (wasWalking && !Control.engine.walking && Control.engine.enabled) {
                Control.event(before.total>1 ? "route-completed" : "walk-arrived", String.format(Locale.US,"lat=%.7f lon=%.7f; holding destination",Control.engine.latitude,Control.engine.longitude));
                Control.saveState();
            }
            Control.checkpoint(now);
            if(now-lastUiRefresh>=250){lastUiRefresh=now;refresh();}
            if (now-lastLog >= 5000 && Control.engine.enabled) {
                lastLog = now;
                Control.event("position", String.format(Locale.US, "lat=%.7f lon=%.7f speed=%.3f walking=%s idleWalking=%s reads=%d/%d", Control.engine.latitude, Control.engine.longitude, Control.engine.metersPerSecond(), Control.engine.walking, Control.engine.idleWalkingActive(), Control.latitudeReads, Control.longitudeReads));
            }
            handler.postDelayed(this, 100);
        }
    };
    private EditText input(String value, String hint) {
        EditText e = new EditText(dialogContext()); e.setSingleLine(true); e.setHint(hint); e.setText(value);
        e.setInputType(InputType.TYPE_CLASS_NUMBER|InputType.TYPE_NUMBER_FLAG_DECIMAL|InputType.TYPE_NUMBER_FLAG_SIGNED); return e;
    }
    private void edit() {
        if (Control.engine.walking) Control.event("walk-cancelled", "Coordinates editor opened");
        stick.release(); refresh();
        LinearLayout form = new LinearLayout(activity); form.setOrientation(LinearLayout.VERTICAL); form.setPadding(dp(24),dp(8),dp(24),dp(8));
        // Latitude and longitude share one line, so a Google Maps "lat, lon" can be pasted straight in.
        EditText coords = QuickLocationDialogs.coordinateInput(dialogContext(), String.format(Locale.US, "%.6f, %.6f", Control.engine.latitude, Control.engine.longitude));
        EditText speed = input(Double.toString(Control.engine.speedKmh), "Speed (0.5–30 km/h)");
        form.addView(coords); form.addView(QuickLocationDialogs.coordinateHint(dialogContext())); form.addView(speed);
        AlertDialog dialog = new AlertDialog.Builder(dialogContext()).setTitle("Coordinates & speed").setView(form).setNegativeButton("Cancel",null).setPositiveButton("Apply",null).create();
        final AlertDialog[] confirmation = {null};
        DialogLifecycle.bind(activity,dialog,()->{if(confirmation[0]!=null)confirmation[0].dismiss();});
        dialog.setOnShowListener(d -> dialog.getButton(-1).setOnClickListener(v -> {
            try {
                // Validate all fields before committing any state.
                double[] ll = QuickLocationDialogs.parseLatLon(coords.getText().toString());
                String problem = QuickLocationDialogs.latLonProblem(ll);
                if (problem != null) { coords.setError(problem); return; }
                MotionEngine check = new MotionEngine(); check.position(ll[0],ll[1]); check.speed(Double.parseDouble(speed.getText().toString().trim()));
                confirmation[0]=TeleportDialog.confirm(activity,check.latitude,check.longitude,(latitude,longitude)->{
                    if(!quickTeleport(latitude,longitude))return false;
                    Control.engine.speed(check.speedKmh); Control.saveState();
                    Control.event("coordinates-set", latitude + "," + longitude + " speed=" + check.speedKmh);
                    refresh(); dialog.dismiss(); return true;
                });
            } catch (IllegalArgumentException error) { speed.setError("Enter valid coordinates and speed (0.5–30)."); }
        })); dialog.show();styleTree(form);MapTheme.styleButtons(dialog);
    }
    /** A ground tap must never silently replace a planned route. */
    void confirmStopRoute() {
        if(closed||activity.isFinishing()||activity.isDestroyed()||stopRouteDialog!=null)return;
        final long token=Control.engine.activeRouteToken();if(token==0)return;
        AlertDialog dialog=new AlertDialog.Builder(dialogContext())
            .setTitle("Stop route?")
            .setMessage("A route or Autowalk is running. Do you want to stop it?")
            .setNegativeButton("Keep walking",null)
            .setPositiveButton("Stop route",(d,w)->{
                if(!closed&&Control.engine.stopRoute(token)){
                    Control.saveState();Control.event("route-stopped","Confirmed after map tap");refresh();
                }
            }).create();
        stopRouteDialog=dialog;
        DialogLifecycle.bind(activity,dialog,()->{if(stopRouteDialog==dialog)stopRouteDialog=null;});
        dialog.setOnShowListener(d->MapTheme.styleButtons(dialog));
        dialog.show();
    }

    /** Shared World map handler for both the settings list and the quick menu Map row. */
    private void openWorldMap() {
        if(quickMenu!=null)quickMenu.hide();
        stick.release(true);
        Control.event("map-open", "Browsing does not apply a destination; current walk=" + Control.engine.walking);
        WorldMapDialog.show(activity, Control.engine.latitude, Control.engine.longitude, (lat, lon) -> {
            if (!Control.canEnable()) { Toast.makeText(activity,"Location adapter is not ready",Toast.LENGTH_SHORT).show(); return false; }
            try {
                stick.release(); Control.teleport(lat, lon);
                Control.event("map-teleport", String.format(Locale.US,"lat=%.7f lon=%.7f",lat,lon));
                refresh(); return true;
            } catch (IllegalArgumentException error) { Toast.makeText(activity,"Select valid coordinates",Toast.LENGTH_SHORT).show(); return false; }
        }, (lat, lon) -> {
            if (!Control.canEnable()) { Toast.makeText(activity,"Location adapter is not ready",Toast.LENGTH_SHORT).show(); return false; }
            try {
                stick.release(); Control.walkTo(lat, lon);
                Control.event("walk-started", String.format(Locale.US,"targetLat=%.7f targetLon=%.7f speedKmh=%.2f",lat,lon,Control.engine.speedKmh));
                if (!Control.engine.walking) Control.event("walk-arrived", "Already at selected destination");
                refresh(); return true;
            } catch (IllegalArgumentException error) { Toast.makeText(activity,"Select valid coordinates",Toast.LENGTH_SHORT).show(); return false; }
        }, (waypoints, loop) -> {
            if (!Control.canEnable()) { Toast.makeText(activity,"Location adapter is not ready",Toast.LENGTH_SHORT).show(); return false; }
            try {
                stick.release(true); Control.walkRoute(waypoints, loop);
                MotionEngine.RouteProgress progress=Control.engine.routeProgress();
                Control.event("route-started", String.format(Locale.US,"stops=%d visited=%d remainingMeters=%.2f speedKmh=%.2f loop=%s",progress.total,progress.visited,progress.remainingMeters,Control.engine.speedKmh,loop));
                refresh(); return true;
            } catch (IllegalArgumentException error) { Toast.makeText(activity,error.getMessage(),Toast.LENGTH_SHORT).show(); return false; }
        });
    }
    /** Opens the existing settings panel full screen; the game runtime is left untouched. */
    private void openSettings() {
        if(quickMenu!=null)quickMenu.hide();
        stick.release(true);
        settings.show(); settings.applyInsets(parent.getRootWindowInsets());
        Control.event("settings-open", "Full-screen panel; runtime unaffected");
        refresh();
    }
    /** Shared quick teleport for the Favorites and Teleport modals; validates readiness. */
    private boolean quickTeleport(double latitude, double longitude) {
        if (!Control.canEnable()) { Toast.makeText(activity,"Location adapter is not ready",Toast.LENGTH_SHORT).show(); return false; }
        try {
            stick.release(); Control.teleport(latitude, longitude);
            Control.event("quick-teleport", String.format(Locale.US,"lat=%.7f lon=%.7f",latitude,longitude));
            refresh(); return true;
        } catch (IllegalArgumentException error) { Toast.makeText(activity,"Enter valid coordinates",Toast.LENGTH_SHORT).show(); return false; }
    }
    void confirmMapTeleport(double latitude,double longitude,int mode,int epoch) {
        TeleportDialog.confirm(activity,latitude,longitude,(lat,lon)->{
            if(closed||!Control.mapTapAllowed()||mode!=Control.mapTapMode()||epoch!=Control.mapTapEpoch()||Control.engine.activeRouteToken()!=0) {
                Toast.makeText(activity,"Map-tap settings changed; tap the map again.",Toast.LENGTH_SHORT).show();return true;
            }
            if(!quickTeleport(lat,lon))return false;
            Control.event("map-tap-applied",String.format(Locale.US,"mode=%d lat=%.7f lon=%.7f",mode,lat,lon));return true;
        });
    }
    private boolean quickWalk(double latitude,double longitude) {
        if(!Control.canEnable()){Toast.makeText(activity,"Location adapter is not ready",Toast.LENGTH_SHORT).show();return false;}
        try {
            stick.release();Control.walkTo(latitude,longitude);
            Control.event("hotspot-walk",String.format(Locale.US,"lat=%.7f lon=%.7f",latitude,longitude));
            refresh();return true;
        } catch(IllegalArgumentException error){Toast.makeText(activity,"Select valid coordinates",Toast.LENGTH_SHORT).show();return false;}
    }
    private View rocketTouchGame;
    private long rocketTouchDown,rocketTouchCommand,rocketTouchStarted;
    private float rocketTouchX,rocketTouchY;
    private boolean rocketTouching,rocketTouchWindow;
    private View rocketUnityTarget(){
        View content=activity.findViewById(android.R.id.content);if(content==null)return null;
        java.util.ArrayDeque<View> queue=new java.util.ArrayDeque<>();queue.add(content);
        for(int count=0;!queue.isEmpty()&&count<256;count++){
            View view=queue.removeFirst();
            for(Class<?> type=view.getClass();type!=null;type=type.getSuperclass()){
                String name=type.getName();
                if(name.equals("com.unity3d.player.UnityPlayer")||name.equals("com.unity3d.player.UnityPlayerForActivityOrService"))
                    return view.isShown()&&view.hasWindowFocus()&&view.getWidth()>0&&view.getHeight()>0?view:null;
            }
            if(view instanceof ViewGroup){ViewGroup group=(ViewGroup)view;for(int i=0;i<group.getChildCount()&&queue.size()<256;i++)queue.addLast(group.getChildAt(i));}
        }
        return null;
    }
    private boolean sendRocketTouch(int action,float x,float y){
        View game=rocketTouchGame;if(game==null)return false;
        MotionEvent.PointerProperties pointer=new MotionEvent.PointerProperties();pointer.id=0;pointer.toolType=MotionEvent.TOOL_TYPE_FINGER;
        MotionEvent.PointerCoords coords=new MotionEvent.PointerCoords();coords.x=x;coords.y=y;coords.pressure=action==MotionEvent.ACTION_UP||action==MotionEvent.ACTION_CANCEL?0:1;coords.size=.1f;
        MotionEvent e=MotionEvent.obtain(rocketTouchDown,SystemClock.uptimeMillis(),action,1,new MotionEvent.PointerProperties[]{pointer},new MotionEvent.PointerCoords[]{coords},0,0,1,1,0,0,InputDevice.SOURCE_TOUCHSCREEN,0);
        try{boolean accepted=rocketTouchWindow?activity.dispatchTouchEvent(e):game.dispatchTouchEvent(e);if(!accepted)Control.event("rocket-auto-touch-rejected","action="+action+"; target="+game.getClass().getName());return accepted;}
        catch(RuntimeException failure){Control.event("rocket-auto-touch-error",failure.getClass().getSimpleName());return false;}finally{e.recycle();}
    }
    private void cancelRocketSwipe(){if(rocketTouching){rocketTouching=false;sendRocketTouch(MotionEvent.ACTION_CANCEL,rocketTouchX,rocketTouchY);Control.event("rocket-auto-swipe-cancelled","command="+rocketTouchCommand+"; elapsedMs="+(SystemClock.elapsedRealtime()-rocketTouchStarted));}rocketTouchGame=null;}
    private boolean rocketGestureLive(){return !closed&&!activity.isFinishing()&&!activity.isDestroyed()&&!settings.isVisible()&&!(quickMenu!=null&&quickMenu.isShowing())&&rocketTouchGame!=null&&rocketTouchGame.isShown()&&rocketTouchGame.hasWindowFocus()&&Control.rocketAutoGestureValid(rocketTouchCommand,rocketTouchStarted);}
    void cancelRocketSwipeIfInvalid(){if(rocketTouching&&!rocketGestureLive())cancelRocketSwipe();}
    void swipeRocket(long command,long expires){
        if(closed||rocketTouching||!Control.rocketAutoTouchValid(command,expires))return;
        View game=rocketUnityTarget();rocketTouchWindow=game==null;
        if(rocketTouchWindow)game=activity.getWindow()==null?null:activity.getWindow().getDecorView();
        if(game==null||!game.isShown()||!game.hasWindowFocus()||game.getWidth()<=0||game.getHeight()<=0||activity.isFinishing()||activity.isDestroyed()||settings.isVisible()||(quickMenu!=null&&quickMenu.isShowing())){Control.event("rocket-auto-swipe-blocked","Activity window unavailable, unfocused or panel open");return;}
        rocketTouchGame=game;rocketTouchCommand=command;rocketTouchStarted=SystemClock.elapsedRealtime();rocketTouchDown=SystemClock.uptimeMillis();rocketTouchX=game.getWidth()*.5f;rocketTouchY=game.getHeight()*.89f;
        rocketTouching=sendRocketTouch(MotionEvent.ACTION_DOWN,rocketTouchX,rocketTouchY);if(!rocketTouching){rocketTouchGame=null;return;}
        final float from=rocketTouchY,to=game.getHeight()*.45f;
        for(int i=1;i<=8;i++){final int step=i;handler.postDelayed(()->{
            if(!rocketTouching||rocketTouchCommand!=command)return;
            if(!rocketGestureLive()){cancelRocketSwipe();return;}
            rocketTouchY=from+(to-from)*step/8f;
            if(!sendRocketTouch(MotionEvent.ACTION_MOVE,rocketTouchX,rocketTouchY)){cancelRocketSwipe();return;}
            if(step==8){boolean accepted=sendRocketTouch(MotionEvent.ACTION_UP,rocketTouchX,rocketTouchY);rocketTouching=false;rocketTouchGame=null;Control.event("rocket-auto-swipe-complete","accepted="+accepted+"; elapsedMs="+(SystemClock.elapsedRealtime()-rocketTouchStarted));}
        },48+i*16L);}
        Control.event("rocket-auto-swipe","Touchscreen finger gesture started; route="+(rocketTouchWindow?"Activity.dispatchTouchEvent":"Unity view")+"; target="+game.getClass().getName()+"; width="+game.getWidth()+"; height="+game.getHeight()+"; command="+command);
    }
    public void close() {
        if (closed) return;
        cancelRocketSwipe();closed = true;if(stopRouteDialog!=null)stopRouteDialog.dismiss(); stick.release(true); Control.suspendForBackground(); handler.removeCallbacks(tick);
        if(quickMenu!=null){quickMenu.destroy();quickMenu=null;}
        if(cooldownDrag!=null){cooldownDrag.detach();cooldownDrag=null;}if(vgpDrag!=null){vgpDrag.detach();vgpDrag=null;}if(transferDrag!=null){transferDrag.detach();transferDrag=null;}if(gameRoutesDrag!=null){gameRoutesDrag.detach();gameRoutesDrag=null;}
        if(buddyEmotionDrag!=null){buddyEmotionDrag.detach();buddyEmotionDrag=null;}
        if(buddyEmotionStatus!=null)parent.removeView(buddyEmotionStatus);
        if(automationStatus!=null){parent.removeView(automationStatus);automationStatus=null;}
        if(gymFeedDrag!=null){gymFeedDrag.detach();gymFeedDrag=null;}
        if(gymAutomationStatus!=null){parent.removeView(gymAutomationStatus);gymAutomationStatus=null;}
        if(gymFeedOverlay!=null){gymFeedOverlay.close();parent.removeView(gymFeedOverlay);gymFeedOverlay=null;}
        settings.close();
        parent.removeView(settings.view());parent.removeView(launcher);parent.removeView(joystickOverlay);if(encounterBerry!=null){encounterBerry.close();parent.removeView(encounterBerry);}if(cooldownTimer!=null)parent.removeView(cooldownTimer);if(vgpToggle!=null)parent.removeView(vgpToggle);if(vgpActivity!=null)parent.removeView(vgpActivity);if(vgpMarker!=null)parent.removeView(vgpMarker);if(transferToggle!=null)parent.removeView(transferToggle);if(gameRoutesOverlay!=null){gameRoutesOverlay.close();parent.removeView(gameRoutesOverlay);}
        settings.view().setOnApplyWindowInsetsListener(null);
        Control.event("panel-closed", "Panel closed on activity change; desiredEnabled=" + Control.desiredEnabled() + "; location kept; route=" + Control.engine.walking);
    }
    private final class Stick extends View {
        final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        float x, y;
        int pointer = -1;
        long cameraGesture=-1;
        boolean gestureCamera;
        Stick(Context c) { super(c); setContentDescription("Location joystick: drag to move, release to stop"); }
        void release() { release(false); }
        void release(boolean preserveWalk) { x=y=0; pointer=-1; cameraGesture=-1; if (!preserveWalk || !Control.engine.walking) Control.engine.stop(); Control.saveState(); invalidate(); }
        void applyMotion(long now) {
            if (pointer<0) return;
            if (!Control.engine.enabled || !Control.canEnable() || gestureCamera!=Control.cameraRelative()) { release(true); return; }
            boolean wasWalking=Control.engine.walking;
            if (gestureCamera) {
                if (!Control.applyCameraInput(cameraGesture,x,-y,now)) {
                    release(true);
                    Control.event("joystick-released", "Camera unavailable or changed; a new drag is required");
                    return;
                }
            } else Control.engine.input(x,-y);
            if (wasWalking) Control.event("walk-cancelled", "Manual joystick input");
        }
        protected void onDraw(Canvas canvas) {
            float cx=getWidth()/2f, cy=getHeight()/2f, radius=Math.min(cx,cy)-joyInset();
            boolean live=Control.engine.enabled;
            // Flat game palette: mint base with a teal outline, quiet direction ticks, solid teal knob with a white rim.
            paint.setShader(null);
            paint.setStyle(Paint.Style.FILL); paint.setColor(MapTheme.dlgMint()); canvas.drawCircle(cx,cy,radius,paint);
            paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(dp(1.5f)); paint.setColor(MapTheme.dlgOutline()); canvas.drawCircle(cx,cy,radius,paint);
            paint.setStrokeWidth(dp(1)); paint.setColor(MapTheme.dlgMintLine()); canvas.drawCircle(cx,cy,radius*.65f,paint);
            paint.setStyle(Paint.Style.FILL); paint.setColor(MapTheme.dlgOutline());
            for(int i=0;i<4;i++){double a=i*Math.PI/2;float tx=cx+(float)Math.cos(a)*radius*.84f,ty=cy+(float)Math.sin(a)*radius*.84f;canvas.drawCircle(tx,ty,Math.max(dp(1.5f),radius*.04f),paint);}
            // Knob sizes are proportional to the base so the whole joystick scales with JOY_DP.
            float kx=cx+x*radius*.65f, ky=cy+y*radius*.65f;
            paint.setColor(0xffffffff); canvas.drawCircle(kx,ky,radius*.40f,paint);
            paint.setColor(live?MapTheme.dlgTeal():MapTheme.dlgDisabled()); canvas.drawCircle(kx,ky,radius*.33f,paint);
        }
        public boolean onTouchEvent(MotionEvent e) {
            int action=e.getActionMasked();
            if (action==MotionEvent.ACTION_DOWN) {
                getParent().requestDisallowInterceptTouchEvent(true);
                // A deliberate manual takeover cancels even when a fresh camera
                // heading is unavailable. Passive camera loss preserves a walk.
                if (Control.engine.walking) {
                    Control.engine.stop(); Control.saveState();
                    Control.event("walk-cancelled", "Manual joystick touch");
                }
                pointer=e.getPointerId(0); gestureCamera=Control.cameraRelative();
                cameraGesture=gestureCamera ? Control.beginCameraGesture() : -1;
            }
            if (action==MotionEvent.ACTION_UP || action==MotionEvent.ACTION_CANCEL ||
                (action==MotionEvent.ACTION_POINTER_UP && e.getPointerId(e.getActionIndex())==pointer)) {
                getParent().requestDisallowInterceptTouchEvent(false);
                release(pointer<0); Control.event("joystick", "released"); return true;
            }
            int index=e.findPointerIndex(pointer); if(index<0) return true;
            float radius=Math.min(getWidth(),getHeight())/2f-joyInset();
            x=(e.getX(index)-getWidth()/2f)/radius; y=(e.getY(index)-getHeight()/2f)/radius;
            float length=(float)Math.max(1,Math.hypot(x,y)); x/=length; y/=length;
            if(Math.hypot(x,y)<0.08) x=y=0;
            applyMotion(SystemClock.elapsedRealtime()); invalidate(); return true;
        }
    }
}
