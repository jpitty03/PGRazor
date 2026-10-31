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
public final class BuddyEmotionSettings {
    private BuddyEmotionSettings() {}
    private static final BuddyEmotionPreferences preferences=new BuddyEmotionPreferences();
    private static final int[] BERRIES={701,703,705,706,708};
    private static final String[] BERRY_LABELS={"Razz Berry","Nanab Berry","Pinap Berry","Golden Razz Berry (premium)","Silver Pinap Berry (premium)"};
    private static volatile String status="Off", savedHistory="", metric="EP: waiting";
    public static String status(){return metric+"\n"+status;}
    public static String history(){return savedHistory;}
    public static void publish(String json){try{
        org.json.JSONObject event=new org.json.JSONObject(json);status=event.optString("message","Waiting");
        int ep=event.optInt("ep",-1);metric=ep<0?"EP: waiting":"EP: "+ep+(event.optBoolean("excited",false)?" | Excited":"");
        long seconds=event.optLong("maintenanceSeconds",0);
        if(seconds>0)metric+=" | Maintenance ~"+(seconds/60)+"m "+(seconds%60)+"s";
        String account=event.optString("account",""), h=event.optString("history","");
        if(AccountProfiles.matchesAutomationIdentity(account)&&h.matches("[0-9]+(;[0-9]+){5}")&&!h.startsWith("0;")&&h.length()<=256){
            String next=account+"|"+h;if(!next.equals(savedHistory)){savedHistory=next;Control.saveSettings();}
        }
    }catch(Exception ignored){}}
    public static void read(Properties p){preferences.read(p);String h=p.getProperty("buddyEmotionHistory","");savedHistory=h.length()<=256?h:"";status="Waiting for buddy data";metric="EP: waiting";}
    public static void write(Properties p){preferences.write(p);p.setProperty("buddyEmotionHistory",savedHistory);}
    public static String config(){return preferences.config();}
    public static boolean enabled(){return preferences.enabled();}
    public static boolean overlayEnabled(){return preferences.enabled()&&preferences.overlay();}
    private static void set(String key,Object value){preferences.set(key,String.valueOf(value));persist();}
    private static void persist(){Control.saveSettings();}
    /** Builds the same account-aware form used inline and in the modal. */
    public static Runnable populate(Activity activity,LinearLayout body){
        Context ui=body.getContext();
        TextView description=new TextView(ui);
        description.setText("Builds buddy mood with snapshots and petting while the game is open on the map. EP comes from the game; maintenance is an estimate. Walking and battles remain manual.");body.addView(description);
        CheckBox master=new CheckBox(ui);master.setText("Run the Buddy Emotion Engine");body.addView(master);
        TextView progress=new TextView(ui);body.addView(progress);
        TextView priority=button(body,"");
        TextView reserve=button(body,"");
        CheckBox gifts=new CheckBox(ui);gifts.setText("Open gifts your buddy brings back");body.addView(gifts);
        CheckBox overlay=new CheckBox(ui);overlay.setText("Show EP overlay");body.addView(overlay);
        TextView explanation=new TextView(ui);explanation.setText("Feeds the first selected berry with stock above its reserve. The reserve applies separately to every berry. Golden Razz and Silver Pinap are optional premium choices; none are selected by default. Clear the list to disable feeding.");body.addView(explanation);
        final boolean[] refreshing={false};
        Runnable refresh=()->{
            refreshing[0]=true;
            boolean on=preferences.enabled();master.setChecked(on);gifts.setChecked(preferences.gifts());
            overlay.setChecked(preferences.overlay());
            progress.setText(status());priority.setText("Feeding priority: "+priorityLabel(preferences.berries()));
            reserve.setText("Reserve per berry: "+preferences.reserve());
            for(android.view.View v:new android.view.View[]{priority,reserve,gifts,overlay,explanation}){v.setEnabled(on);v.setAlpha(on?1f:0.45f);}
            refreshing[0]=false;
        };
        master.setOnCheckedChangeListener((v,on)->{if(!refreshing[0]){set("Enabled",on);refresh.run();}});
        gifts.setOnCheckedChangeListener((v,on)->{if(!refreshing[0])set("Gifts",on);});
        overlay.setOnCheckedChangeListener((v,on)->{if(!refreshing[0])set("Overlay",on);});
        priority.setOnClickListener(v->priorityPicker(activity,refresh));
        reserve.setOnClickListener(v->reservePicker(activity,refresh));
        refresh.run();return refresh;
    }
    public static void show(Activity activity){
        if(activity==null||activity.isFinishing()||activity.isDestroyed())return;
        Context ui=MapTheme.wrap(activity);LinearLayout body=new LinearLayout(ui);body.setOrientation(LinearLayout.VERTICAL);
        int pad=MapTheme.dp(ui,16);body.setPadding(pad,pad,pad,pad);
        Runnable update=populate(activity,body);ScrollView scroll=new ScrollView(ui);scroll.addView(body);
        AlertDialog dialog=new AlertDialog.Builder(ui).setTitle("Buddy Emotion Engine").setView(scroll).setPositiveButton("Done",null).create();
        android.os.Handler handler=new android.os.Handler(android.os.Looper.getMainLooper());
        Runnable refresh=new Runnable(){public void run(){if(dialog.isShowing()){update.run();handler.postDelayed(this,1000);}}};
        DialogLifecycle.bind(activity,dialog,()->handler.removeCallbacks(refresh));
        dialog.setOnShowListener(d->{MapTheme.style(body);MapTheme.styleButtons(dialog);dialog.getWindow().setBackgroundDrawable(new android.graphics.drawable.ColorDrawable(MapTheme.surface()));handler.post(refresh);});dialog.show();
    }
    private static TextView button(LinearLayout body,String text){
        android.widget.Button b=new android.widget.Button(body.getContext());b.setText(text);b.setAllCaps(false);body.addView(b);return b;
    }
    private static String priorityLabel(String value){
        if(value.isEmpty())return "No feeding";
        StringBuilder result=new StringBuilder();
        for(String id:value.split(","))for(int i=0;i<BERRIES.length;i++)if(id.equals(String.valueOf(BERRIES[i]))){
            if(result.length()>0)result.append(" > ");result.append(BERRY_LABELS[i]);
        }
        return result.toString();
    }
    private static boolean sameAccount(String initial){return java.util.Objects.equals(initial,AccountProfiles.currentId());}
    private static void priorityPicker(Activity activity,Runnable refresh){
        Context ui=MapTheme.wrap(activity);String account=AccountProfiles.currentId();
        java.util.ArrayList<String> order=new java.util.ArrayList<>();
        if(!preferences.berries().isEmpty())java.util.Collections.addAll(order,preferences.berries().split(","));
        LinearLayout body=new LinearLayout(ui);body.setOrientation(LinearLayout.VERTICAL);
        TextView help=new TextView(ui);help.setText("Tap berries in the order to try them. Uncheck and reselect a berry to move it to the end. Premium berries require an explicit selection.");body.addView(help);
        CheckBox[] rows=new CheckBox[BERRIES.length];boolean[] syncing={false};
        Runnable redraw=()->{syncing[0]=true;for(int i=0;i<rows.length;i++){
            int position=order.indexOf(String.valueOf(BERRIES[i]));rows[i].setChecked(position>=0);
            rows[i].setText((position<0?"":(position+1)+". ")+BERRY_LABELS[i]);
        }syncing[0]=false;};
        for(int i=0;i<rows.length;i++){final String id=String.valueOf(BERRIES[i]);rows[i]=new CheckBox(ui);body.addView(rows[i]);rows[i].setOnCheckedChangeListener((v,on)->{
            if(syncing[0])return;if(on){if(!order.contains(id))order.add(id);}else order.remove(id);redraw.run();
        });}
        LinearLayout actions=new LinearLayout(ui);actions.setOrientation(LinearLayout.HORIZONTAL);body.addView(actions);
        TextView clear=button(actions,"Clear"),reset=button(actions,"Reset");
        for(TextView action:new TextView[]{clear,reset}){
            action.setLayoutParams(new LinearLayout.LayoutParams(0,LinearLayout.LayoutParams.WRAP_CONTENT,1f));
            action.setMinHeight(MapTheme.dp(ui,48));
        }
        clear.setOnClickListener(v->{order.clear();redraw.run();});
        reset.setOnClickListener(v->{order.clear();java.util.Collections.addAll(order,"703","701","705");redraw.run();});
        redraw.run();ScrollView scroll=new ScrollView(ui);scroll.addView(body);
        AlertDialog d=new AlertDialog.Builder(ui).setTitle("Feeding priority").setView(scroll).setPositiveButton("Save",null).setNegativeButton("Cancel",null).create();
        DialogLifecycle.bind(activity,d,null);d.setOnShowListener(x->{MapTheme.style(body);MapTheme.styleButtons(d);d.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v->{
            if(sameAccount(account)&&preferences.enabled())set("Berries",android.text.TextUtils.join(",",order));refresh.run();d.dismiss();
        });});d.show();
    }
    private static void reservePicker(Activity activity,Runnable refresh){
        Context ui=MapTheme.wrap(activity);String account=AccountProfiles.currentId();EditText edit=new EditText(ui);
        edit.setSingleLine(true);edit.setText(String.valueOf(preferences.reserve()));edit.setInputType(InputType.TYPE_CLASS_NUMBER);
        AlertDialog d=new AlertDialog.Builder(ui).setTitle("Reserve per berry (0-1000)").setView(edit).setPositiveButton("Save",null).setNegativeButton("Cancel",null).create();
        DialogLifecycle.bind(activity,d,null);d.setOnShowListener(x->{MapTheme.style(edit);MapTheme.styleButtons(d);d.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v->{
            if(!sameAccount(account)||!preferences.enabled()){refresh.run();d.dismiss();return;}
            try{int n=Integer.parseInt(edit.getText().toString().trim());if(n<0||n>1000)throw new NumberFormatException();set("Reserve",n);refresh.run();d.dismiss();}
            catch(NumberFormatException invalid){edit.setError("Enter a whole number from 0 to 1000.");}
        });});d.show();
    }
}
