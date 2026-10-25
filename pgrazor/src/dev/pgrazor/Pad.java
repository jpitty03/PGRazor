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
    private LinearLayout joystickOverlay;
    private BerryOverlay encounterBerry;
    private Button cooldownTimer;
    private TextView vgpActivity;
    private VgpMarker vgpMarker;
    // Draggable, position-persisting handles for the cooldown and VGP overlays.
    private OverlayDrag cooldownDrag, vgpDrag;
    private QuickMenu quickMenu;
    // Draggable, position-persisting Keep All / transfer-rules overlay button.
    private TransferToggle transferToggle;
    private OverlayDrag transferDrag;
    // Compact, draggable map-only pill for the Shiny Scanner: runtime status and the latest shiny species/CP.
    private TextView shinyScannerPill;
    private OverlayDrag shinyScannerDrag;
    // Bundled id->name table, loaded once; used only to label the found shiny in the pill.
    private org.json.JSONObject shinySpeciesNames;
    private boolean shinySpeciesNamesLoaded;

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
    private void applyTheme(){
        settings.applyTheme();
        if(launcher!=null)launcher.invalidate();
        if(joystickOverlay!=null){joystickOverlay.setBackground(null);stick.setAlpha(0.6f);styleTree(joystickOverlay);stick.invalidate();}
        if(encounterBerry!=null)encounterBerry.invalidate();
        if(cooldownTimer!=null)cooldownTimer.setBackground(rounded(Control.darkTheme()?0xcc141b25:0xccf1f5f8,12));
        if(transferToggle!=null)transferToggle.invalidate();
        if(shinyScannerPill!=null){shinyScannerPill.setTextColor(ink());shinyScannerPill.setBackground(rounded(surface(),10));}
        if(quickMenu!=null)quickMenu.applyTheme();
    }
    private void draggable(View handle,View target,boolean clickable){
        handle.setOnTouchListener(new View.OnTouchListener(){float downX,downY,originX,originY;boolean moved;
            public boolean onTouch(View v,MotionEvent e){int a=e.getActionMasked();if(a==MotionEvent.ACTION_DOWN){downX=e.getRawX();downY=e.getRawY();originX=target.getX();originY=target.getY();moved=false;return true;}
                if(a==MotionEvent.ACTION_MOVE){float dx=e.getRawX()-downX,dy=e.getRawY()-downY;if(Math.hypot(dx,dy)>dp(6))moved=true;if(moved){target.setX(Math.max(0,Math.min(parent.getWidth()-OverlayDrag.visualWidth(target),originX+dx)));target.setY(Math.max(0,Math.min(parent.getHeight()-OverlayDrag.visualHeight(target),originY+dy)));}return true;}
                if(a==MotionEvent.ACTION_UP){if(!moved&&clickable)v.performClick();return true;}return true;}
        });
    }
    private final class RazorIcon extends View {
        private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
        RazorIcon(Context c){super(c);setContentDescription("Open PGRazor settings. Drag to reposition.");setFocusable(true);setClickable(true);}
        protected void onDraw(Canvas c){super.onDraw(c);float scale=Math.min(getWidth(),getHeight())/48f;c.save();c.scale(scale,scale);paint.setColor(surface());c.drawCircle(24,24,23,paint);paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(1.5f);paint.setColor(accent());c.drawCircle(24,24,22,paint);paint.setStyle(Paint.Style.FILL);
            Path r=new Path();r.moveTo(13,11);r.lineTo(29,11);r.lineTo(36,18);r.lineTo(29,26);r.lineTo(36,37);r.lineTo(26,37);r.lineTo(19,26);r.lineTo(19,37);r.lineTo(12,37);r.lineTo(17,19);r.lineTo(24,19);r.lineTo(23,22);r.lineTo(27,22);r.lineTo(30,18);r.lineTo(27,16);r.lineTo(12,16);r.close();c.drawPath(r,paint);c.restore();}
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
        });

        stick = new Stick(host);
        joystickOverlay = new LinearLayout(host); joystickOverlay.setOrientation(LinearLayout.VERTICAL);
        TextView grip = text("• • •", 12, muted()); grip.setGravity(Gravity.CENTER); joystickOverlay.addView(grip, new LinearLayout.LayoutParams(-1, dp(28)));
        joystickOverlay.addView(stick, new LinearLayout.LayoutParams(dp(136), dp(136))); draggable(grip, joystickOverlay, false);

        // The settings panel is a full-screen modal; it consumes taps so the game behind it stays inert.
        parent.addView(settings.view(), new FrameLayout.LayoutParams(-1, -1));

        FrameLayout.LayoutParams joyLayout=new FrameLayout.LayoutParams(dp(136),-2,Gravity.LEFT|Gravity.BOTTOM);joyLayout.leftMargin=dp(16);joyLayout.bottomMargin=dp(96);parent.addView(joystickOverlay,joyLayout);

        // Compact catching-berry overlay; the popup chooses owned berries and Off.
        encounterBerry=new BerryOverlay(activity,this::refresh);
        FrameLayout.LayoutParams berryLayout=new FrameLayout.LayoutParams(dp(90),dp(50),Gravity.RIGHT|Gravity.CENTER_VERTICAL);berryLayout.rightMargin=dp(10);parent.addView(encounterBerry,berryLayout);encounterBerry.setVisibility(View.GONE);

        cooldownTimer=button("History  --:--");cooldownTimer.setTextSize(14);cooldownTimer.setAllCaps(false);cooldownTimer.setTextColor(accent());FrameLayout.LayoutParams timerLayout=new FrameLayout.LayoutParams(dp(165),dp(44),Gravity.TOP|Gravity.RIGHT);timerLayout.topMargin=dp(44);timerLayout.rightMargin=dp(12);parent.addView(cooldownTimer,timerLayout);cooldownTimer.setOnClickListener(v->Cooldown.show(activity));cooldownDrag=OverlayDrag.attach(parent,cooldownTimer,"cooldown");

        // Window attachment: the panel pads itself inside the system bars/IME as insets arrive.
        View panelView=settings.view();
        panelView.setOnApplyWindowInsetsListener((view,insets) -> {settings.applyInsets(insets);return insets;});
        panelView.requestApplyInsets();

        vgpToggle=new VgpToggle(host);FrameLayout.LayoutParams vgpButtonLayout=new FrameLayout.LayoutParams(dp(54),dp(54),Gravity.TOP|Gravity.LEFT);vgpButtonLayout.leftMargin=dp(12);vgpButtonLayout.topMargin=dp(156);parent.addView(vgpToggle,vgpButtonLayout);vgpToggle.setOnClickListener(v->{Control.setVgpEnabled(!Control.vgpEnabled());refresh();});
        vgpActivity=text("VGP",12,ink());vgpActivity.setPadding(dp(10),dp(6),dp(10),dp(6));vgpActivity.setMaxLines(2);vgpActivity.setEllipsize(android.text.TextUtils.TruncateAt.END);
        FrameLayout.LayoutParams vgpLayout=new FrameLayout.LayoutParams(dp(220),-2,Gravity.TOP|Gravity.LEFT);vgpLayout.leftMargin=dp(12);vgpLayout.topMargin=dp(216);parent.addView(vgpActivity,vgpLayout);vgpActivity.setVisibility(View.GONE);
        // The toggle is the drag handle; the status pill follows it as one coherent unit. Tap still flips VGP.
        vgpDrag=OverlayDrag.attach(parent,vgpToggle,vgpActivity,"vgp");
        vgpMarker=new VgpMarker(host);parent.addView(vgpMarker,new FrameLayout.LayoutParams(dp(38),dp(46)));vgpMarker.setVisibility(View.GONE);
        // Compact Keep All / transfer-rules overlay; tap toggles Keep All, drag repositions (persists transfer.fx/fy).
        transferToggle=new TransferToggle(host);FrameLayout.LayoutParams transferLayout=new FrameLayout.LayoutParams(dp(54),dp(54),Gravity.TOP|Gravity.LEFT);transferLayout.leftMargin=dp(12);transferLayout.topMargin=dp(300);parent.addView(transferToggle,transferLayout);transferToggle.setOnClickListener(v->{Control.setTransferKeepAll(!Control.transferKeepAll());refresh();});
        transferDrag=OverlayDrag.attach(parent,transferToggle,"transfer");transferToggle.setVisibility(View.GONE);
        // Themed status pill for the Shiny Scanner; the pill is its own drag handle (persists shiny.fx/fy).
        shinyScannerPill=text("Shiny Scanner",12,ink());shinyScannerPill.setPadding(dp(10),dp(6),dp(10),dp(6));shinyScannerPill.setMaxLines(2);shinyScannerPill.setEllipsize(android.text.TextUtils.TruncateAt.END);
        FrameLayout.LayoutParams shinyLayout=new FrameLayout.LayoutParams(dp(210),-2,Gravity.TOP|Gravity.LEFT);shinyLayout.leftMargin=dp(12);shinyLayout.topMargin=dp(372);parent.addView(shinyScannerPill,shinyLayout);shinyScannerPill.setVisibility(View.GONE);
        shinyScannerDrag=OverlayDrag.attach(parent,shinyScannerPill,"shiny");
        launcher=new RazorIcon(host);FrameLayout.LayoutParams iconLayout=new FrameLayout.LayoutParams(dp(48),dp(48),Gravity.TOP|Gravity.LEFT);iconLayout.leftMargin=dp(12);iconLayout.topMargin=dp(100);parent.addView(launcher,iconLayout);
        launcher.setContentDescription("Open PGRazor quick menu. Drag to reposition.");
        // The R launcher now toggles the compact quick menu; Settings there opens this panel full screen.
        quickMenu=new QuickMenu(host,parent,launcher,new QuickMenu.Listener(){
            public void onMap(){openWorldMap();}
            public double onCycleWalkSpeed(){double s=Control.cycleWalkSpeed();refresh();return s;}
            public void onAutoWalk(){stick.release(true);AutoWalkDialog.show(activity);}
            public void onFeeds(){stick.release(true);LiveFeedDialog.show(activity);}
            public void onFavorites(){QuickLocationDialogs.showFavorites(activity,Pad.this::quickTeleport);}
            public void onTeleport(){QuickLocationDialogs.showTeleport(activity,Pad.this::quickTeleport);}
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
    /** English species name for a dex id from the bundled table (read once); falls back to #id. */
    private String shinySpeciesName(int id){
        if(!shinySpeciesNamesLoaded){shinySpeciesNamesLoaded=true;shinySpeciesNames=loadShinySpeciesNames();}
        if(shinySpeciesNames!=null){String name=shinySpeciesNames.optString(Integer.toString(id),"");if(!name.isEmpty())return name;}
        return "#"+id;
    }
    /** Parse the bundled id->name table once; any missing/oversize/malformed file leaves #id fallbacks. */
    private org.json.JSONObject loadShinySpeciesNames(){
        try(java.io.InputStream in=activity.getAssets().open("pgrazor/map-icons/pokemon-names.json")){
            java.io.ByteArrayOutputStream out=new java.io.ByteArrayOutputStream();byte[] buffer=new byte[8192];int count;
            while((count=in.read(buffer))!=-1){if(out.size()+count>2*1024*1024)throw new java.io.IOException("Oversize name table");out.write(buffer,0,count);}
            return new org.json.JSONObject(new String(out.toByteArray(),"UTF-8"));
        }catch(java.io.IOException|org.json.JSONException|RuntimeException ignored){return null;}
    }
    private final class VgpToggle extends View {
        private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint art=new Paint(Paint.ANTI_ALIAS_FLAG|Paint.FILTER_BITMAP_FLAG);
        private final RectF dst=new RectF();
        // Exact supplied PokeMiners Plus icons, decoded once with alpha intact; a null falls back to the vector glyph.
        private final Bitmap connected=decodeMapIcon("vgp-connected.png"),disconnected=decodeMapIcon("vgp-disconnected.png");
        VgpToggle(Context context){super(context);setFocusable(true);setClickable(true);}
        protected void onDraw(Canvas canvas){super.onDraw(canvas);boolean on=Control.vgpEnabled();canvas.save();canvas.scale(getWidth()/54f,getHeight()/54f);
            paint.setStyle(Paint.Style.FILL);paint.setColor(surface());canvas.drawRoundRect(1,1,53,53,14,14,paint);
            Bitmap icon=on?connected:disconnected;
            if(icon!=null&&icon.getWidth()>0&&icon.getHeight()>0){float box=32f,ratio=Math.min(box/icon.getWidth(),box/icon.getHeight()),w=icon.getWidth()*ratio,h=icon.getHeight()*ratio;dst.set(27-w/2f,21-h/2f,27+w/2f,21+h/2f);canvas.drawBitmap(icon,null,dst,art);}
            else{paint.setColor(on?Color.rgb(33,188,125):Color.rgb(126,134,148));canvas.drawCircle(27,20,13,paint);paint.setColor(Color.WHITE);paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(2.5f);canvas.drawArc(19,12,35,28,-55,290,false,paint);canvas.drawLine(27,8,27,20,paint);}
            paint.setStyle(Paint.Style.FILL);paint.setColor(on?Color.rgb(33,188,125):muted());paint.setTextSize(11);paint.setTypeface(Typeface.DEFAULT_BOLD);paint.setTextAlign(Paint.Align.CENTER);canvas.drawText(on?"ON":"OFF",27,50,paint);
            canvas.restore();}
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
     * nothing auto-transfers), XFER (Keep All off and the auto-transfer master on, so the rules run),
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
            paint.setStyle(Paint.Style.FILL);paint.setColor(surface());canvas.drawRoundRect(1,1,53,53,14,14,paint);
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
        String state=keep?"Keep All on; no catches are auto-transferred":(master?"Normal transfer rules apply":"Auto transfer is off");
        return "Transfer protection: "+state+". Tap to "+(keep?"use normal transfer rules":"keep every catch")+". Drag to reposition.";
    }

    private void applyMapScale(){
        if(launcher==null||launcher.getWidth()==0||parent.getWidth()==0)return;
        int percent=Control.uiScalePercent();if(appliedUiScale==percent)return;
        appliedUiScale=percent;float scale=percent/100f;
        for(View view:new View[]{launcher,joystickOverlay,cooldownTimer,vgpToggle,vgpActivity,vgpMarker,transferToggle,shinyScannerPill}){
            if(view==null)continue;
            float fx=view.getX()/Math.max(1,parent.getWidth()-OverlayDrag.visualWidth(view));
            float fy=view.getY()/Math.max(1,parent.getHeight()-OverlayDrag.visualHeight(view));
            view.setPivotX(0);view.setPivotY(0);view.setScaleX(scale);view.setScaleY(scale);
            view.setX(Math.max(0,Math.min(1,fx))*Math.max(0,parent.getWidth()-OverlayDrag.visualWidth(view)));
            view.setY(Math.max(0,Math.min(1,fy))*Math.max(0,parent.getHeight()-OverlayDrag.visualHeight(view)));
        }
        if(cooldownDrag!=null)cooldownDrag.refreshBounds();if(vgpDrag!=null)vgpDrag.refreshBounds();
        if(transferDrag!=null)transferDrag.refreshBounds();
        if(shinyScannerDrag!=null)shinyScannerDrag.refreshBounds();
        if(encounterBerry!=null)encounterBerry.applyUiScale(scale);
        if(quickMenu!=null)quickMenu.applyScale(scale);
    }
    private void refresh() {
        applyMapScale();
        if(vgpToggle!=null){vgpToggle.setVisibility(Control.vgpOverlayVisible()&&!settings.isVisible()&&!(quickMenu!=null&&quickMenu.isShowing())?View.VISIBLE:View.GONE);vgpToggle.setContentDescription("Virtual Go Plus "+(Control.vgpEnabled()?"on; tap to turn off":"off; tap to turn on")+". "+Control.vgpStatus());vgpToggle.invalidate();}

        // Pair the status line with the toggle so the cooldown shutoff reason is visible even when VGP is off.
        if(vgpActivity!=null){vgpActivity.setVisibility(Control.vgpOverlayVisible()&&!settings.isVisible()&&!(quickMenu!=null&&quickMenu.isShowing())?View.VISIBLE:View.GONE);vgpActivity.setText("VGP  |  "+Control.vgpStatus());vgpActivity.setTextColor(ink());vgpActivity.setBackground(rounded(surface(),10));}
        if(vgpMarker!=null){boolean show=Control.vgpTargetVisible()&&!settings.isVisible();vgpMarker.setVisibility(show?View.VISIBLE:View.GONE);if(show){vgpMarker.setX(Control.vgpTargetX()*parent.getWidth()-dp(19)*vgpMarker.getScaleX());vgpMarker.setY(Control.vgpTargetY()*parent.getHeight()-dp(36)-dp(46)*vgpMarker.getScaleY());vgpMarker.invalidate();}}
        if(transferToggle!=null){transferToggle.setVisibility(Control.transferOverlayVisible()&&!settings.isVisible()&&!(quickMenu!=null&&quickMenu.isShowing())?View.VISIBLE:View.GONE);transferToggle.setContentDescription(transferToggleDescription());transferToggle.invalidate();}
        // Map-only pill: hidden in settings and the quick menu; shows the latest shiny or a concise status.
        if(shinyScannerPill!=null){
            boolean show=Control.shinyScannerOverlayVisible()&&!settings.isVisible()&&!(quickMenu!=null&&quickMenu.isShowing());
            shinyScannerPill.setVisibility(show?View.VISIBLE:View.GONE);
            if(show){
                String label;
                if(Control.shinyScannerResultVisible())label="✨ "+shinySpeciesName(Control.shinyScannerSpecies())+"  ·  CP "+Control.shinyScannerCp();
                else label="Shiny Scanner  |  "+Control.shinyScannerStatus();
                shinyScannerPill.setText(label);shinyScannerPill.setContentDescription(label);
                shinyScannerPill.setTextColor(ink());shinyScannerPill.setBackground(rounded(surface(),10));
            }
        }

        if(cooldownTimer!=null){cooldownTimer.setVisibility(Control.cooldownOverlay()?View.VISIBLE:View.GONE);cooldownTimer.setText("☰  "+Cooldown.label());cooldownTimer.setTextColor(Cooldown.remaining()>0?Color.rgb(255,186,95):accent());}
        if (lastEnabled != Control.engine.enabled) { lastEnabled = Control.engine.enabled; stick.invalidate(); }
        if(encounterBerry!=null)encounterBerry.refresh();
        if(quickMenu!=null)quickMenu.onRefresh();
        settings.refresh();
    }
    private final Runnable tick = new Runnable() {
        public void run() {
            if (closed) return;
            Control.checkLease();
            long now = SystemClock.elapsedRealtime();
            stick.applyMotion(now);
            boolean wasWalking = Control.engine.walking;
            MotionEngine.RouteProgress before=Control.engine.routeProgress();
            Control.engine.step((now-lastTick)/1000.0); lastTick = now;
            MotionEngine.RouteProgress after=Control.engine.routeProgress();
            if (before.total>1 && after.total==before.total && after.visited>before.visited)
                Control.event("route-progress", "visited=" + after.visited + "/" + after.total);
            if (wasWalking && !Control.engine.walking && Control.engine.enabled) {
                Control.event(before.total>1 ? "route-completed" : "walk-arrived", String.format(Locale.US,"lat=%.7f lon=%.7f; holding destination",Control.engine.latitude,Control.engine.longitude));
                Control.saveState();
            }
            Control.checkpoint(now);
            refresh();
            if (now-lastLog >= 1000 && Control.engine.enabled) {
                lastLog = now;
                Control.event("position", String.format(Locale.US, "lat=%.7f lon=%.7f speed=%.3f walking=%s reads=%d/%d", Control.engine.latitude, Control.engine.longitude, Control.engine.metersPerSecond(), Control.engine.walking, Control.latitudeReads, Control.longitudeReads));
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
        EditText lat = input(Double.toString(Control.engine.latitude), "Latitude (-90 to 90)");
        EditText lon = input(Double.toString(Control.engine.longitude), "Longitude (-180 to 180)");
        EditText speed = input(Double.toString(Control.engine.speedKmh), "Speed (0.5–30 km/h)");
        for (EditText field : new EditText[]{lat, lon, speed}) form.addView(field);
        AlertDialog dialog = new AlertDialog.Builder(dialogContext()).setTitle("Coordinates & speed").setView(form).setNegativeButton("Cancel",null).setPositiveButton("Apply",null).create();
        dialog.setOnShowListener(d -> dialog.getButton(-1).setOnClickListener(v -> {
            try {
                // Validate all fields before committing any state.
                MotionEngine check = new MotionEngine(); check.position(Double.parseDouble(lat.getText().toString().trim()), Double.parseDouble(lon.getText().toString().trim())); check.speed(Double.parseDouble(speed.getText().toString().trim()));
                Control.engine.position(check.latitude, check.longitude); Control.engine.speed(check.speedKmh);
                Control.saveState();
                Control.event("coordinates-set", check.latitude + "," + check.longitude + " speed=" + check.speedKmh); refresh(); dialog.dismiss();
            } catch (IllegalArgumentException error) { speed.setError("Enter valid coordinates and speed (0.5–30)."); }
        })); dialog.show();styleTree(form);
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
    public void close() {
        if (closed) return;
        closed = true;if(stopRouteDialog!=null)stopRouteDialog.dismiss(); stick.release(true); Control.suspendForBackground(); handler.removeCallbacks(tick);
        if(quickMenu!=null){quickMenu.destroy();quickMenu=null;}
        if(cooldownDrag!=null){cooldownDrag.detach();cooldownDrag=null;}if(vgpDrag!=null){vgpDrag.detach();vgpDrag=null;}if(transferDrag!=null){transferDrag.detach();transferDrag=null;}if(shinyScannerDrag!=null){shinyScannerDrag.detach();shinyScannerDrag=null;}
        settings.close();
        parent.removeView(settings.view());parent.removeView(launcher);parent.removeView(joystickOverlay);if(encounterBerry!=null){encounterBerry.close();parent.removeView(encounterBerry);}if(cooldownTimer!=null)parent.removeView(cooldownTimer);if(vgpToggle!=null)parent.removeView(vgpToggle);if(vgpActivity!=null)parent.removeView(vgpActivity);if(vgpMarker!=null)parent.removeView(vgpMarker);if(transferToggle!=null)parent.removeView(transferToggle);if(shinyScannerPill!=null)parent.removeView(shinyScannerPill);
        settings.view().setOnApplyWindowInsetsListener(null);
        Control.event("panel-closed", "Runtime suspended on activity change; desiredEnabled=" + Control.desiredEnabled() + "; movement stopped");
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
            float cx=getWidth()/2f, cy=getHeight()/2f, radius=Math.min(cx,cy)-dp(12);
            paint.setColor(Control.darkTheme()?0xff202e3e:0xffe1e9ef); canvas.drawCircle(cx,cy,radius,paint);
            paint.setColor(Color.rgb(72,102,125)); paint.setStrokeWidth(dp(1));
            canvas.drawLine(cx-radius,cy,cx+radius,cy,paint); canvas.drawLine(cx,cy-radius,cx,cy+radius,paint);
            paint.setColor(Control.engine.enabled?accent():muted());
            canvas.drawCircle(cx+x*radius*.65f,cy+y*radius*.65f,dp(22),paint);
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
            float radius=Math.min(getWidth(),getHeight())/2f-dp(12);
            x=(e.getX(index)-getWidth()/2f)/radius; y=(e.getY(index)-getHeight()/2f)/radius;
            float length=(float)Math.max(1,Math.hypot(x,y)); x/=length; y/=length;
            if(Math.hypot(x,y)<0.08) x=y=0;
            applyMotion(SystemClock.elapsedRealtime()); invalidate(); return true;
        }
    }
}
