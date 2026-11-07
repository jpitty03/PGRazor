package dev.pgrazor;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.graphics.*;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.view.View;
import android.widget.*;
import java.io.InputStream;

/** Compact icon/count display and persistent, ordered catch-berry picker. */
final class BerryOverlay extends View {
    private static final int[] IDS={0,701,703,705,706,707};
    private static final String[] NAMES={"Off","Razz Berry","Nanab Berry","Pinap Berry","Golden Razz Berry","Silver Pinap Berry"};
    private final Activity activity;
    private final Bitmap[] icons=new Bitmap[IDS.length];
    private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG|Paint.FILTER_BITMAP_FLAG);
    private AlertDialog picker;
    private LinearLayout[] rows;
    private TextView[] labels,ranks;
    private final Runnable changed;
    private Context pickerContext;

    BerryOverlay(Activity activity,Runnable changed){
        super(activity);this.activity=activity;this.changed=changed;setClickable(true);setFocusable(true);
        for(int i=1;i<IDS.length;i++)try(InputStream in=PgAssets.open(activity, "pgrazor/map-icons/berry-"+IDS[i]+".png")){icons[i]=BitmapFactory.decodeStream(in);}catch(Exception ignored){}
        setOnClickListener(v->showPicker());
    }
    private int dp(float n){return MapTheme.dp(activity,n);}
    private int pickerDp(float n){return MapTheme.dp(pickerContext,n);}
    void applyUiScale(float scale){setPivotX(getWidth());setPivotY(getHeight()/2f);setScaleX(scale);setScaleY(scale);}
    protected void onSizeChanged(int w,int h,int oldw,int oldh){super.onSizeChanged(w,h,oldw,oldh);setPivotX(w);setPivotY(h/2f);}
    private int index(int id){for(int i=0;i<IDS.length;i++)if(IDS[i]==id)return i;return 0;}
    void refresh(){
        boolean visible=Control.encounterBerryVisible();setVisibility(visible?VISIBLE:GONE);
        if(!visible&&picker!=null)picker.dismiss();
        int id=Control.autoBerrySelected();
        setContentDescription("Catch berry: "+NAMES[index(id)]+(id==0?"":", "+Control.berryCount(id)+" available")+". Tap to choose berry priorities.");
        setEnabled(!Control.encounterBerryBusy());invalidate();refreshPicker();
    }
    void close(){if(picker!=null)picker.dismiss();}
    protected void onDraw(Canvas c){
        super.onDraw(c);float w=getWidth(),h=getHeight();
        paint.setStyle(Paint.Style.FILL);paint.setColor(MapTheme.surface());c.drawRoundRect(0,0,w,h,dp(14),dp(14),paint);
        paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(dp(1));paint.setColor(MapTheme.accent());c.drawRoundRect(dp(0.5f),dp(0.5f),w-dp(0.5f),h-dp(0.5f),dp(14),dp(14),paint);
        paint.setStyle(Paint.Style.FILL);
        int id=Control.autoBerrySelected(),i=index(id);Bitmap icon=icons[i];
        if(icon!=null){float size=dp(35),scale=Math.min(size/icon.getWidth(),size/icon.getHeight()),iw=icon.getWidth()*scale,ih=icon.getHeight()*scale;
            paint.setAlpha(Control.encounterBerryBusy()?140:255);c.drawBitmap(icon,null,new RectF(dp(23)-iw/2,h/2-ih/2,dp(23)+iw/2,h/2+ih/2),paint);paint.setAlpha(255);
        }else{
            paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(dp(2));paint.setColor(MapTheme.ink());
            c.drawCircle(dp(23),h/2,dp(12),paint);c.drawLine(dp(15),h/2+dp(8),dp(31),h/2-dp(8),paint);paint.setStyle(Paint.Style.FILL);
        }
        paint.setColor(MapTheme.ink());paint.setTypeface(Typeface.DEFAULT_BOLD);paint.setTextAlign(Paint.Align.CENTER);
        String count=id==0?"":("("+Math.max(0,Control.berryCount(id))+")");
        paint.setTextSize(dp(count.length()>5?11:13));
        c.drawText(count,dp(65),h/2-(paint.ascent()+paint.descent())/2,paint);
    }
    private void showPicker(){
        if(picker!=null||activity.isFinishing()||activity.isDestroyed())return;
        float scale=Control.uiScalePercent()/100f;
        android.view.ContextThemeWrapper ui=new android.view.ContextThemeWrapper(activity,Control.darkTheme()?android.R.style.Theme_Material_Dialog_Alert:android.R.style.Theme_Material_Light_Dialog_Alert);
        android.content.res.Configuration density=new android.content.res.Configuration();
        density.densityDpi=Math.max(1,Math.round(activity.getResources().getDisplayMetrics().densityDpi*scale));
        ui.applyOverrideConfiguration(density);pickerContext=ui;
        LinearLayout body=new LinearLayout(ui);body.setOrientation(LinearLayout.VERTICAL);body.setPadding(pickerDp(12),pickerDp(4),pickerDp(12),pickerDp(4));
        TextView help=new TextView(ui);help.setText("Choose up to 3. The next choice starts a new list.");help.setTextSize(12);help.setTextColor(MapTheme.ink());help.setPadding(pickerDp(6),0,pickerDp(6),pickerDp(8));body.addView(help);
        rows=new LinearLayout[IDS.length];labels=new TextView[IDS.length];ranks=new TextView[IDS.length];
        for(int i=0;i<IDS.length;i++){
            final int id=IDS[i];LinearLayout row=new LinearLayout(ui);row.setGravity(Gravity.CENTER_VERTICAL);row.setPadding(pickerDp(8),pickerDp(3),pickerDp(8),pickerDp(3));rows[i]=row;
            ImageView icon=new ImageView(ui);icon.setScaleType(ImageView.ScaleType.FIT_CENTER);if(icons[i]!=null)icon.setImageBitmap(icons[i]);
            row.addView(icon,new LinearLayout.LayoutParams(pickerDp(40),pickerDp(40)));
            TextView name=new TextView(ui);name.setTextSize(15);name.setPadding(pickerDp(8),0,0,0);labels[i]=name;row.addView(name,new LinearLayout.LayoutParams(0,-2,1));
            TextView rank=new TextView(ui);rank.setGravity(Gravity.CENTER);rank.setTextSize(18);rank.setTypeface(Typeface.DEFAULT_BOLD);ranks[i]=rank;row.addView(rank,new LinearLayout.LayoutParams(pickerDp(34),pickerDp(40)));
            body.addView(row,new LinearLayout.LayoutParams(-1,pickerDp(54)));
            row.setOnClickListener(v->{Control.selectBerryPriority(id);refreshPicker();changed.run();});
        }
        ScrollView scroll=new ScrollView(ui);scroll.addView(body);
        picker=new AlertDialog.Builder(ui).setTitle("Berry priority").setView(scroll).setPositiveButton("Done",null).create();
        picker.setOnDismissListener(d->{picker=null;rows=null;labels=null;ranks=null;pickerContext=null;});
        picker.show();
        int baseWidth=Math.min(dp(400),activity.getResources().getDisplayMetrics().widthPixels-dp(48));
        if(picker.getWindow()!=null)picker.getWindow().setLayout(Math.max(1,Math.round(baseWidth*scale)),android.view.ViewGroup.LayoutParams.WRAP_CONTENT);
        MapTheme.styleButtons(picker);refreshPicker();
    }
    private void refreshPicker(){
        if(picker==null||rows==null)return;
        int[] priority=BerryPriority.parse(Control.berryPriority());
        for(int i=0;i<IDS.length;i++){
            int rank=0;for(int j=0;j<priority.length;j++)if(priority[j]==IDS[i])rank=j+1;
            boolean selected=rank>0||(IDS[i]==0&&priority.length==0);
            labels[i].setText(NAMES[i]+(IDS[i]==0?"":" ("+Math.max(0,Control.berryCount(IDS[i]))+")"));
            labels[i].setTextColor(MapTheme.ink());ranks[i].setText(rank==0?"":String.valueOf(rank));ranks[i].setTextColor(MapTheme.accent());
            GradientDrawable bg=new GradientDrawable();bg.setCornerRadius(pickerDp(10));bg.setColor(MapTheme.surface());if(selected)bg.setStroke(Math.max(1,pickerDp(1)),MapTheme.accent());rows[i].setBackground(bg);
            // No stock gating: priorities are retained and become usable when inventory replenishes.
            rows[i].setContentDescription(NAMES[i]+(rank==0?"":", priority "+rank));
        }
    }
}
