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
    private static final Object lock=new Object();
    private static volatile String status="Gym automation off",guardReason="";
    public static void publish(String detail){try{status=new org.json.JSONObject(detail).optString("message","Waiting");}catch(Exception ignored){}}
    public static String status(){return guardReason.isEmpty()?status:guardReason;}
    /** Short, stable label for Pad's status pill beneath the icon. */
    public static String overlayStatus(){
        stopForCooldown();
        if(!guardReason.isEmpty())return guardReason.contains("cooldown")?"Cooldown":"Choose Deploy or Feed";
        if(!preferences.running())return "Off";
        String raw=status.toLowerCase(java.util.Locale.US);
        if(raw.contains("paused")||raw.contains("failure"))return "Paused";
        if(raw.contains("deploying"))return "Deploying defender";
        if(raw.contains("deployed"))return "Defender deployed";
        if(raw.contains("feeding"))return "Feeding berries";
        if(raw.contains("berry accepted"))return "Berry accepted";
        if(raw.contains("berry declined"))return "Berry declined";
        if(raw.contains("selecting"))return "Selecting defender";
        if(raw.contains("reserve"))return "Berry reserves reached";
        if(raw.contains("waiting")||raw.contains("nearby")||raw.contains("nearest")
            ||raw.contains("eligible"))return "Waiting for gym";
        if(raw.contains("rejected")||raw.contains("declined"))return "Action declined";
        return "Starting";
    }
    public static void init(Context context) {}
    public static void read(Properties p){synchronized(lock){preferences.read(p);guardReason="";status="Gym automation off";}}
    public static void write(Properties p){preferences.write(p);}
    public static String config(){stopForCooldown();return preferences.config();}
    public static boolean enabled(){stopForCooldown();return preferences.running()&&(preferences.deploy()||preferences.feed());}
    public static boolean running(){stopForCooldown();return preferences.running();}
    public static boolean feedEnabled(){stopForCooldown();return preferences.effectiveFeed();}
    public static boolean deployEnabled(){stopForCooldown();return preferences.effectiveDeploy();}
    public static boolean overlayEnabled(){return preferences.overlay();}
    /** Stop the whole run on cooldown while retaining its selected actions. */
    public static boolean stopForCooldown(){
        if(!preferences.running()||Cooldown.remaining()<=0)return false;
        boolean changed=false;
        synchronized(lock){
            if(preferences.stopForCooldown(true)){
                guardReason="Gym automation stopped: cooldown active. Tap to restart after it ends.";
                changed=true;
            }
        }
        return changed;
    }
    /** Tap control for both selected actions; no automatic resume after cooldown. */
    public static boolean toggleRunning(){return setRunning(!running());}
    private static boolean setRunning(boolean on){
        stopForCooldown();
        boolean cooldownActive=Cooldown.remaining()>0;
        synchronized(lock){
            if(!on){preferences.stop();status="Gym automation stopped";return false;}
            if(cooldownActive){
                guardReason="Gym automation is off: cooldown active. Try again after it ends.";
                return false;
            }
            if(!preferences.start()){
                guardReason="Select Deploy or Feed in Gym settings before starting.";
                return false;
            }
            guardReason="";status="Gym automation running";return true;
        }
    }
    private static void set(String key,Object value){
        synchronized(lock){
            boolean wasRunning=preferences.running();preferences.set(key,String.valueOf(value));
            if(wasRunning&&!preferences.running())status="Gym automation stopped: select Deploy or Feed to run.";
            if(("Deploy".equals(key)||"Feed".equals(key))&&(preferences.deploy()||preferences.feed())
                &&guardReason.startsWith("Select Deploy"))guardReason="";
        }
        persist();
    }
    private static void persist(){Control.saveSettings();}
    public static void show(Activity activity){
        if(activity==null||activity.isFinishing()||activity.isDestroyed())return;
        Context ui=MapTheme.wrap(activity);
        LinearLayout body=new LinearLayout(ui);body.setOrientation(LinearLayout.VERTICAL);
        int pad=MapTheme.dp(ui,16);body.setPadding(pad,pad,pad,pad);
        TextView description=new TextView(ui);
        TextView progress=new TextView(ui);progress.setText(status());body.addView(progress);
        description.setText("Acts only within 40 m of a Gym while the game is on the map screen. It slots eligible #Gym-tagged Pokémon first; the lowest-CP fallback is used only when enabled and no eligible #Gym defender is available, and it never picks favorite, buddy, or protected Pokémon. Feeding uses Nanab before Razz and only above the reserves you set. It obeys the game's own server limits, so some actions may be declined. No dust or coin payout is promised.");body.addView(description);
        CheckBox master=new CheckBox(ui);master.setText("Run Gym Slotter & Berry Feeder");body.addView(master);
        CheckBox deploy=new CheckBox(ui);deploy.setText("Auto-deploy to open Gym slots");deploy.setChecked(preferences.deploy());body.addView(deploy);

        CheckBox feed=new CheckBox(ui);feed.setText("Auto-feed berries to your Gym defenders");feed.setChecked(preferences.feed());body.addView(feed);
        CheckBox overlay=new CheckBox(ui);overlay.setText("Show gym automation overlay");overlay.setChecked(preferences.overlay());body.addView(overlay);

        CheckBox fallback=new CheckBox(ui);fallback.setText("Allow lowest-CP fallback when no eligible #Gym defenders");fallback.setChecked(preferences.fallback());body.addView(fallback);
        fallback.setOnCheckedChangeListener((v,on)->{set("Fallback",on);});
        LinearLayout reserves=new LinearLayout(ui);reserves.setOrientation(LinearLayout.VERTICAL);body.addView(reserves);
        input(activity,reserves,"Nanab Berry reserve (0–1000)",String.valueOf(preferences.nanabReserve()),value->{int n=bounded(value,1000);set("NanabReserve",n);});
        input(activity,reserves,"Razz Berry reserve (0–1000)",String.valueOf(preferences.razzReserve()),value->{int n=bounded(value,1000);set("RazzReserve",n);});
        final boolean[] refreshing={false};
        Runnable update=()->{
            refreshing[0]=true;master.setChecked(running());deploy.setChecked(preferences.deploy());
            feed.setChecked(preferences.feed());overlay.setChecked(preferences.overlay());
            SettingsUi.enabled(fallback,preferences.deploy());SettingsUi.enabled(reserves,preferences.feed());
            progress.setText(status());refreshing[0]=false;
        };
        master.setOnCheckedChangeListener((v,on)->{if(!refreshing[0]){setRunning(on);update.run();}});
        deploy.setOnCheckedChangeListener((v,on)->{if(!refreshing[0]){set("Deploy",on);update.run();}});
        feed.setOnCheckedChangeListener((v,on)->{if(!refreshing[0]){set("Feed",on);update.run();}});
        overlay.setOnCheckedChangeListener((v,on)->{if(!refreshing[0]){set("Overlay",on);update.run();}});
        body.removeView(feed);body.addView(feed,body.indexOfChild(fallback)+1);
        body.removeView(overlay);body.addView(overlay,body.indexOfChild(feed)+1);
        update.run();
        ScrollView scroll=new ScrollView(ui);scroll.addView(body);
        AlertDialog dialog=new AlertDialog.Builder(ui).setTitle("Gym Slotter & Berry Feeder").setView(scroll).setPositiveButton("Done",null).create();
        android.os.Handler handler=new android.os.Handler(android.os.Looper.getMainLooper());
        Runnable refresh=new Runnable(){public void run(){if(dialog.isShowing()){update.run();handler.postDelayed(this,1000);}}};
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
