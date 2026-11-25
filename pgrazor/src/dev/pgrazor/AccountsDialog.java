package dev.pgrazor;

import android.app.*;
import android.widget.*;
import android.view.Gravity;
import java.util.Locale;

/** Account chooser; authentication remains in the game's Google/PTC sign-in flow. */
final class AccountsDialog {
    static void show(Activity activity) {
        android.content.Context ui=MapTheme.wrap(activity);
        LinearLayout body=new LinearLayout(ui);body.setOrientation(1);int pad=MapTheme.dp(activity,16);body.setPadding(pad,pad,pad,pad);
        TextView status=new TextView(ui);status.setText(AccountProfiles.status());status.setTextSize(16);body.addView(status);
        TextView explanation=new TextView(ui);
        explanation.setText("Each account keeps its settings, location and saved sign-in. Switching restarts the game. Sign in once per account; expired sessions may require sign-in again.");
        explanation.setPadding(0,pad,0,pad);body.addView(explanation);
        // Global settings: every account uses the main account's settings; each keeps its own for later.
        LinearLayout globalRow=new LinearLayout(ui);globalRow.setOrientation(0);globalRow.setGravity(Gravity.CENTER_VERTICAL);
        TextView globalLabel=new TextView(ui);globalLabel.setText("Global settings");globalLabel.setTextSize(16);globalRow.addView(globalLabel,new LinearLayout.LayoutParams(0,-2,1));
        Switch global=new Switch(ui);global.setContentDescription("Global settings");global.setChecked(AccountProfiles.globalSettings());globalRow.addView(global);body.addView(globalRow);
        TextView globalNote=new TextView(ui);globalNote.setText("Every account uses the main account's settings. Each account's own settings come back when this is off.");
        globalNote.setPadding(0,0,0,pad/2);body.addView(globalNote);
        ScrollView scroll=new ScrollView(ui);LinearLayout rows=new LinearLayout(ui);rows.setOrientation(1);scroll.addView(rows);body.addView(scroll,new LinearLayout.LayoutParams(-1,0,1));
        AccountProfileStore store=AccountProfiles.store();
        java.util.ArrayList<Button> switches=new java.util.ArrayList<>();
        java.util.LinkedHashMap<Button,String> deletions=new java.util.LinkedHashMap<>();
        java.util.LinkedHashMap<String,TextView> labels=new java.util.LinkedHashMap<>();
        java.util.LinkedHashMap<String,Button> mains=new java.util.LinkedHashMap<>();
        java.util.LinkedHashMap<String,String> names=new java.util.LinkedHashMap<>();
        Runnable marks=()->{for(java.util.Map.Entry<String,TextView> e:labels.entrySet())e.getValue().setText(title(e.getKey(),names.get(e.getKey())));
            for(java.util.Map.Entry<String,Button> e:mains.entrySet())e.getValue().setVisibility(e.getKey().equals(AccountProfiles.mainId())?android.view.View.GONE:android.view.View.VISIBLE);};
        if(store!=null)for(AccountProfileStore.Profile profile:store.list()){
            LinearLayout row=new LinearLayout(ui);row.setOrientation(1);row.setPadding(0,pad/2,0,pad/2);
            TextView label=new TextView(ui);label.setText(title(profile.id,profile.label));label.setTextSize(17);row.addView(label);
            labels.put(profile.id,label);names.put(profile.id,profile.label);
            try{android.util.AtomicFile f=new android.util.AtomicFile(store.dataFile(profile.id,"pgrazor-location-state-v1.txt"));
                LocationState location=LocationState.decode(new String(f.readFully(),java.nio.charset.StandardCharsets.UTF_8));
                TextView coords=new TextView(ui);coords.setText(String.format(Locale.US,"%.5f, %.5f",location.latitude,location.longitude));row.addView(coords);
            }catch(Exception ignored){}
            LinearLayout actions=new LinearLayout(ui);actions.setGravity(Gravity.END);row.addView(actions);
            Button rename=new Button(ui);rename.setText("Rename");actions.addView(rename);
            rename.setOnClickListener(v->{EditText name=new EditText(ui);name.setSingleLine(true);name.setText(names.get(profile.id));new AlertDialog.Builder(ui).setTitle("Account name").setView(name).setNegativeButton("Cancel",null).setPositiveButton("Save",(d,w)->{String value=name.getText().toString();AccountProfiles.rename(profile.id,value,()->{names.put(profile.id,value);label.setText(title(profile.id,value));});}).show();});
            Button makeMain=new Button(ui);makeMain.setText("Make main");actions.addView(makeMain);mains.put(profile.id,makeMain);
            makeMain.setVisibility(profile.id.equals(AccountProfiles.mainId())?android.view.View.GONE:android.view.View.VISIBLE);
            makeMain.setOnClickListener(v->apply(activity,AccountProfiles.globalSettings(),profile.id,names.get(profile.id),marks,null));
            Button change=new Button(ui);change.setText("Switch");change.setEnabled(AccountProfiles.canSwitch()&&!profile.id.equals(AccountProfiles.currentId()));actions.addView(change);
            if(!profile.id.equals(AccountProfiles.currentId()))switches.add(change);
            change.setOnClickListener(v->confirm(activity,profile));rows.addView(row);
            Button delete=new Button(ui);delete.setText("Delete");delete.setEnabled(AccountProfiles.canDelete(profile.id));actions.addView(delete);deletions.put(delete,profile.id);
            delete.setContentDescription(profile.id.equals(AccountProfiles.currentId())?"Switch accounts before deleting the current account":"Delete saved account");
            delete.setOnClickListener(v->new AlertDialog.Builder(ui).setTitle("Delete "+profile.label+"?")
                .setMessage("Remove this saved sign-in, settings, location, favorites and routes from this device? Your Pokémon GO account will not be deleted.")
                .setNegativeButton("Cancel",null).setPositiveButton("Delete",(d,w)->AccountProfiles.delete(activity,profile.id,()->{switches.remove(change);deletions.remove(delete);rows.removeView(row);labels.remove(profile.id);mains.remove(profile.id);marks.run();})).show());
        }
        global.setOnCheckedChangeListener((v,checked)->{if(checked==AccountProfiles.globalSettings())return;
            String main=AccountProfiles.mainId();apply(activity,checked,main,names.get(main),marks,()->global.setChecked(!checked));});
        Button add=new Button(ui);add.setText("Add account");add.setEnabled(AccountProfiles.canSwitch());body.addView(add);
        Button reset=new Button(ui);reset.setText("Reset sign-in choices");body.addView(reset);
        reset.setOnClickListener(v->new AlertDialog.Builder(ui).setTitle("Reset sign-in choices?")
            .setMessage("The game will restart and ask for your date of birth again. Enter your own date of birth to see the sign-in methods available to you.")
            .setNegativeButton("Cancel",null).setPositiveButton("Reset",(d,w)->AccountProfiles.resetSignIn()).show());
        add.setOnClickListener(v->{EditText label=new EditText(ui);label.setSingleLine(true);label.setHint("Account name");new AlertDialog.Builder(ui).setTitle("Add account").setMessage("Create a separate profile, then sign in with Google or PTC.").setView(label).setNegativeButton("Cancel",null).setPositiveButton("Continue",(d,w)->{try{String name=label.getText().toString().trim();AccountProfiles.add(activity,name.isEmpty()?"New account":name);}catch(Exception error){Toast.makeText(activity,"Could not add account",Toast.LENGTH_SHORT).show();}}).show();});
        AlertDialog dialog=new AlertDialog.Builder(ui).setTitle("Accounts").setView(body).setPositiveButton("Close",null).create();
        android.os.Handler handler=new android.os.Handler(android.os.Looper.getMainLooper());
        Runnable refresh=new Runnable(){public void run(){
            if(!dialog.isShowing())return;
            status.setText(AccountProfiles.status());
            boolean enabled=AccountProfiles.canSwitch();add.setEnabled(enabled);global.setEnabled(enabled);
            for(Button button:mains.values())button.setEnabled(enabled);
            reset.setEnabled(AccountProfiles.canResetSignIn());reset.setVisibility(enabled?android.view.View.GONE:android.view.View.VISIBLE);
            for(Button button:switches)button.setEnabled(enabled);
            for(java.util.Map.Entry<Button,String> entry:deletions.entrySet())entry.getKey().setEnabled(AccountProfiles.canDelete(entry.getValue()));
            handler.postDelayed(this,500);
        }};
        dialog.setOnShowListener(d->{MapTheme.fullScreen(dialog);MapTheme.style(body);MapTheme.styleButtons(dialog);handler.post(refresh);});
        DialogLifecycle.bind(activity,dialog,()->handler.removeCallbacksAndMessages(null));dialog.show();
    }
    private static String title(String id,String label){
        return label+(id.equals(AccountProfiles.currentId())?" (current)":"")+(id.equals(AccountProfiles.mainId())?" · Main":"");
    }
    /** Saves the global-settings choice, asking first when it means restarting with other settings. */
    private static void apply(Activity activity,boolean on,String main,String mainName,Runnable saved,Runnable cancelled){
        if(!AccountProfiles.settingsChangeNeedsRestart(on,main)){AccountProfiles.setGlobalSettings(on,main,saved);return;}
        String message=on?"The game restarts and this account uses "+mainName+"'s settings. This account's own settings are kept for when global settings are off."
            :"The game restarts with this account's own settings.";
        new AlertDialog.Builder(MapTheme.wrap(activity)).setTitle("Restart to load settings?").setMessage(message)
            .setNegativeButton("Cancel",(d,w)->{if(cancelled!=null)cancelled.run();})
            .setOnCancelListener(d->{if(cancelled!=null)cancelled.run();})
            .setPositiveButton("Restart",(d,w)->AccountProfiles.setGlobalSettings(on,main,saved)).show();
    }
    private static void confirm(Activity activity,AccountProfileStore.Profile profile){
        new AlertDialog.Builder(MapTheme.wrap(activity)).setTitle("Switch to "+profile.label+"?")
            .setMessage("Movement and automation will stop. The game will restart with this account's saved sign-in and location. Sign in if this account hasn't been saved yet.")
            .setNegativeButton("Cancel",null).setPositiveButton("Switch",(d,w)->AccountProfiles.switchTo(activity,profile.id)).show();
    }
}
