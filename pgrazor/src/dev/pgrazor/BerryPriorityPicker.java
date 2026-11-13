package dev.pgrazor;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.graphics.*;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.widget.*;
import java.io.InputStream;

/** Shared persistent berry priority editor, available on the map or in an encounter. */
final class BerryPriorityPicker {
    static final int[] IDS={0,701,703,705,706,707};
    static final String[] NAMES={"Off","Razz Berry","Nanab Berry","Pinap Berry","Golden Razz Berry","Silver Pinap Berry"};
    private final Activity activity;
    private final Runnable changed;
    private final Bitmap[] icons=new Bitmap[IDS.length];
    private AlertDialog picker;
    private LinearLayout[] rows;
    private TextView[] labels,ranks;
    private Context pickerContext;
    BerryPriorityPicker(Activity activity,Runnable changed){
        this.activity=activity;this.changed=changed;
        for(int i=1;i<IDS.length;i++)try(InputStream in=PgAssets.open(activity,"pgrazor/map-icons/berry-"+IDS[i]+".png")){icons[i]=BitmapFactory.decodeStream(in);}catch(Exception ignored){}
    }
    private int dp(float n){return MapTheme.dp(activity,n);}
    private int pickerDp(float n){return MapTheme.dp(pickerContext,n);}
    void close(){if(picker!=null)picker.dismiss();}
    static String summary(){
        StringBuilder names=new StringBuilder();
        for(int id:BerryPriority.parse(Control.berryPriority()))for(int i=1;i<IDS.length;i++)if(IDS[i]==id){
            if(names.length()>0)names.append(" \u2192 ");names.append(NAMES[i]);break;
        }
        return names.length()==0?"Off":names.toString();
    }
    void show(){
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
            row.setOnClickListener(v->{Control.selectBerryPriority(id);refresh();changed.run();});
        }
        ScrollView scroll=new ScrollView(ui);scroll.addView(body);
        picker=new AlertDialog.Builder(ui).setTitle("Berry priority").setView(scroll).setPositiveButton("Done",null).create();
        picker.setOnDismissListener(d->{picker=null;rows=null;labels=null;ranks=null;pickerContext=null;});
        picker.show();
        int baseWidth=Math.min(dp(400),activity.getResources().getDisplayMetrics().widthPixels-dp(48));
        if(picker.getWindow()!=null)picker.getWindow().setLayout(Math.max(1,Math.round(baseWidth*scale)),android.view.ViewGroup.LayoutParams.WRAP_CONTENT);
        MapTheme.styleButtons(picker);refresh();
    }
    void refresh(){
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
