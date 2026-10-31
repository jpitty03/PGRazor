package dev.pgrazor;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.text.InputType;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.util.Properties;

/** Account settings are persisted by Control alongside its other properties. */
public final class GymAutomationSettings {
    private GymAutomationSettings() {}
    private static final GymAutomationPreferences preferences=new GymAutomationPreferences();
    private static volatile String status="Off";
    public static void publish(String detail){try{status=new org.json.JSONObject(detail).optString("message","Waiting");}catch(Exception ignored){}}
    public static void init(Context context) {}
    public static void read(Properties p){preferences.read(p);}
    public static void write(Properties p){preferences.write(p);}
    public static String config(){return preferences.config();}
    public static boolean enabled(){return preferences.deploy()||preferences.feed();}
    private static void set(String key,Object value){preferences.set(key,String.valueOf(value));persist();}
    private static void persist(){Control.saveSettings();}
    public static void show(Activity activity){
        if(activity==null||activity.isFinishing()||activity.isDestroyed())return;
        Context ui=MapTheme.wrap(activity);
        LinearLayout body=new LinearLayout(ui);body.setOrientation(LinearLayout.VERTICAL);
        int pad=MapTheme.dp(ui,16);body.setPadding(pad,pad,pad,pad);
        TextView description=new TextView(ui);
        TextView progress=new TextView(ui);progress.setText(status);body.addView(progress);
        description.setText("Acts only within 40 m of a Gym while the game is on the map screen. It slots eligible #Gym-tagged Pokémon first; the lowest-CP fallback is used only when enabled and no eligible #Gym defender is available, and it never picks favorite, buddy, or protected Pokémon. Feeding uses Nanab before Razz and only above the reserves you set. It obeys the game's own server limits, so some actions may be declined. No dust or coin payout is promised.");body.addView(description);
        CheckBox deploy=new CheckBox(ui);deploy.setText("Auto-deploy to open Gym slots");deploy.setChecked(preferences.deploy());body.addView(deploy);

        CheckBox feed=new CheckBox(ui);feed.setText("Auto-feed berries to your Gym defenders");feed.setChecked(preferences.feed());body.addView(feed);

        CheckBox fallback=new CheckBox(ui);fallback.setText("Allow lowest-CP fallback when no eligible #Gym defenders");fallback.setChecked(preferences.fallback());body.addView(fallback);
        fallback.setOnCheckedChangeListener((v,on)->{set("Fallback",on);});
        LinearLayout reserves=new LinearLayout(ui);reserves.setOrientation(LinearLayout.VERTICAL);body.addView(reserves);
        input(activity,reserves,"Nanab Berry reserve (0–1000)",String.valueOf(preferences.nanabReserve()),value->{int n=bounded(value,1000);set("NanabReserve",n);});
        input(activity,reserves,"Razz Berry reserve (0–1000)",String.valueOf(preferences.razzReserve()),value->{int n=bounded(value,1000);set("RazzReserve",n);});
        Runnable update=()->{SettingsUi.enabled(fallback,preferences.deploy());SettingsUi.enabled(reserves,preferences.feed());};
        deploy.setOnCheckedChangeListener((v,on)->{set("Deploy",on);update.run();});
        feed.setOnCheckedChangeListener((v,on)->{set("Feed",on);update.run();});
        body.removeView(feed);body.addView(feed,body.indexOfChild(fallback)+1);
        update.run();
        ScrollView scroll=new ScrollView(ui);scroll.addView(body);
        AlertDialog dialog=new AlertDialog.Builder(ui).setTitle("Gym Slotter & Berry Feeder").setView(scroll).setPositiveButton("Done",null).create();
        android.os.Handler handler=new android.os.Handler(android.os.Looper.getMainLooper());
        Runnable refresh=new Runnable(){public void run(){if(dialog.isShowing()){progress.setText(status);update.run();handler.postDelayed(this,1000);}}};
        DialogLifecycle.bind(activity,dialog,()->handler.removeCallbacks(refresh));
        dialog.setOnShowListener(d->{MapTheme.style(body);MapTheme.styleButtons(dialog);dialog.getWindow().setBackgroundDrawable(new android.graphics.drawable.ColorDrawable(MapTheme.surface()));handler.post(refresh);});dialog.show();
    }
    private interface Value {void set(String value);}
    private static int bounded(String value,int max){
        try{int n=Integer.parseInt(value.trim());if(n>=0&&n<=max)return n;}catch(NumberFormatException ignored){}
        throw new IllegalArgumentException("Enter a whole number from 0 to "+max+".");
    }
    private static TextView button(LinearLayout body,String text){
        android.widget.Button b=new android.widget.Button(body.getContext());b.setText(text);b.setAllCaps(false);body.addView(b);return b;
    }
    private static void input(Activity a,LinearLayout body,String title,String initial,Value save){
        TextView button=button(body,title+": "+initial);final String[] current={initial};
        button.setOnClickListener(v->{
            Context ui=MapTheme.wrap(a);EditText edit=new EditText(ui);edit.setSingleLine(true);edit.setText(current[0]);
            edit.setInputType(InputType.TYPE_CLASS_NUMBER);
            AlertDialog d=new AlertDialog.Builder(ui).setTitle(title).setView(edit).setPositiveButton("Save",null).setNegativeButton("Cancel",null).create();
            DialogLifecycle.bind(a,d,null);d.setOnShowListener(x->{MapTheme.style(edit);MapTheme.styleButtons(d);d.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(w->{
                try{String value=edit.getText().toString();save.set(value);current[0]=value.trim();button.setText(title+": "+current[0]);d.dismiss();}
                catch(IllegalArgumentException e){edit.setError(e.getMessage());}
            });});d.show();
        });
    }
}
