package dev.pgrazor;

import android.app.*;
import android.widget.*;
import android.view.Gravity;
import java.util.Locale;

/** Account chooser; authentication remains in the game's Google/PTC sign-in flow. */
final class AccountsDialog {
    static void show(Activity activity) {
        // PGRazorX: the same look as Auto-transfer items: centred muted note, accounts in a mint block with hairlines,
        // compact game pills, and the game action bar at the bottom.
        android.content.Context ui=SettingsUi.dialogContext(activity);
        int pad=MapTheme.dp(activity,16),small=MapTheme.dp(activity,8);
        LinearLayout body=new LinearLayout(ui);body.setOrientation(1);body.setPadding(pad,small,pad,small);
        TextView explanation=new TextView(ui);
        explanation.setText("Each account keeps its settings, location and saved sign-in. Switching restarts the game. Sign in once per account; expired sessions may require sign-in again.");
        body.addView(explanation);
        LinearLayout rows=new LinearLayout(ui);rows.setOrientation(1);
        rows.setPadding(MapTheme.dp(activity,12),MapTheme.dp(activity,2),MapTheme.dp(activity,12),MapTheme.dp(activity,2));
        rows.setBackground(MapTheme.mintBlock(ui));rows.setClipToOutline(true);
        final int maxHeight=MapTheme.dp(activity,340);
        ScrollView scroll=new ScrollView(ui){@Override protected void onMeasure(int w,int h){super.onMeasure(w,android.view.View.MeasureSpec.makeMeasureSpec(maxHeight,android.view.View.MeasureSpec.AT_MOST));}};
        scroll.addView(rows);
        LinearLayout.LayoutParams listLayout=new LinearLayout.LayoutParams(-1,-2);listLayout.topMargin=small;listLayout.bottomMargin=small;body.addView(scroll,listLayout);
        AccountProfileStore store=AccountProfiles.store();
        java.util.ArrayList<Button> switches=new java.util.ArrayList<>();
        java.util.LinkedHashMap<Button,String> deletions=new java.util.LinkedHashMap<>();
        if(store!=null)for(AccountProfileStore.Profile profile:store.list()){
            final boolean current=profile.id.equals(AccountProfiles.currentId());
            LinearLayout row=new LinearLayout(ui);row.setOrientation(1);row.setPadding(0,small,0,small);
            if(rows.getChildCount()>0){android.view.View line=new android.view.View(ui);line.setBackgroundColor(MapTheme.dlgMintLine());rows.addView(line,new LinearLayout.LayoutParams(-1,Math.max(1,MapTheme.dp(activity,1))));}
            TextView label=new TextView(ui);label.setText(profile.label+(current?" (current)":""));label.setTextSize(16);label.setTypeface(MapTheme.bold());row.addView(label);
            try{android.util.AtomicFile f=new android.util.AtomicFile(store.dataFile(profile.id,"pgrazor-location-state-v1.txt"));
                LocationState location=LocationState.decode(new String(f.readFully(),java.nio.charset.StandardCharsets.UTF_8));
                TextView coords=new TextView(ui);coords.setText(String.format(Locale.US,"%.5f, %.5f",location.latitude,location.longitude));coords.setTextSize(12);coords.setTag("muted");row.addView(coords);
            }catch(Exception ignored){}
            LinearLayout actions=new LinearLayout(ui);actions.setOrientation(0);actions.setPadding(0,MapTheme.dp(activity,6),0,0);row.addView(actions);
            Button rename=new Button(ui);rename.setText("Rename");
            Button change=new Button(ui);change.setText("Switch");change.setEnabled(AccountProfiles.canSwitch()&&!current);
            Button delete=new Button(ui);delete.setText("Delete");delete.setEnabled(AccountProfiles.canDelete(profile.id));
            for(Button b:new Button[]{rename,change,delete}){b.setAllCaps(false);b.setMinWidth(0);b.setMinimumWidth(0);
                LinearLayout.LayoutParams lp=new LinearLayout.LayoutParams(0,MapTheme.dp(activity,40),1);lp.setMarginEnd(b==delete?0:small);actions.addView(b,lp);}
            rename.setOnClickListener(v->{EditText name=new EditText(ui);name.setSingleLine(true);name.setText(profile.label);name.setSelectAllOnFocus(true);
                field(activity,ui,"Account name",null,name,"Save",value->AccountProfiles.rename(profile.id,value,()->label.setText(value+(current?" (current)":""))));});
            if(!current)switches.add(change);
            change.setOnClickListener(v->confirm(activity,profile));rows.addView(row);
            deletions.put(delete,profile.id);
            delete.setContentDescription(current?"Switch accounts before deleting the current account":"Delete saved account");
            delete.setOnClickListener(v->ask(activity,ui,"Delete "+profile.label+"?",
                "Remove this saved sign-in, settings, location, favorites and routes from this device? Your Pokémon GO account will not be deleted.",
                "Delete",()->AccountProfiles.delete(activity,profile.id,()->{switches.remove(change);deletions.remove(delete);int at=rows.indexOfChild(row);rows.removeView(row);
                    if(at>0&&at-1<rows.getChildCount()&&!(rows.getChildAt(at-1) instanceof LinearLayout))rows.removeViewAt(at-1);else if(at==0&&rows.getChildCount()>0&&!(rows.getChildAt(0) instanceof LinearLayout))rows.removeViewAt(0);})));
        }
        if(rows.getChildCount()==0){TextView none=new TextView(ui);none.setText("No saved accounts yet.");none.setGravity(Gravity.CENTER_HORIZONTAL);none.setTag("muted");rows.addView(none);}
        Button add=new Button(ui);add.setText("Add account");add.setEnabled(AccountProfiles.canSwitch());
        Button reset=new Button(ui);reset.setText("Reset sign-in choices");
        for(Button b:new Button[]{add,reset}){b.setAllCaps(false);LinearLayout.LayoutParams lp=new LinearLayout.LayoutParams(-1,MapTheme.dp(activity,44));lp.topMargin=MapTheme.dp(activity,6);body.addView(b,lp);}
        reset.setOnClickListener(v->ask(activity,ui,"Reset sign-in choices?",
            "The game will restart and ask for your date of birth again. Enter your own date of birth to see the sign-in methods available to you.",
            "Reset",AccountProfiles::resetSignIn));
        add.setOnClickListener(v->{EditText label=new EditText(ui);label.setSingleLine(true);label.setHint("Account name");
            field(activity,ui,"Add account","Create a separate profile, then sign in with Google or PTC.",label,"Continue",name->{
                try{AccountProfiles.add(activity,name.isEmpty()?"New account":name);}catch(Exception error){Toast.makeText(activity,"Could not add account",Toast.LENGTH_SHORT).show();}});});
        TextView status=new TextView(ui);status.setText(AccountProfiles.status());body.addView(status);
        AlertDialog dialog=new AlertDialog.Builder(ui).setTitle("Accounts").setView(body).setPositiveButton("Close",null).create();
        android.os.Handler handler=new android.os.Handler(android.os.Looper.getMainLooper());
        Runnable refresh=new Runnable(){public void run(){
            if(!dialog.isShowing())return;
            status.setText(AccountProfiles.status());
            boolean enabled=AccountProfiles.canSwitch();add.setEnabled(enabled);
            reset.setEnabled(AccountProfiles.canResetSignIn());reset.setVisibility(enabled?android.view.View.GONE:android.view.View.VISIBLE);
            for(Button button:switches)button.setEnabled(enabled);
            for(java.util.Map.Entry<Button,String> entry:deletions.entrySet())entry.getKey().setEnabled(AccountProfiles.canDelete(entry.getValue()));
            handler.postDelayed(this,500);
        }};
        dialog.setOnShowListener(d->{
            SettingsUi.styleTree(ui,body);
            explanation.setGravity(Gravity.CENTER_HORIZONTAL);explanation.setTextColor(MapTheme.dlgMuted());explanation.setTextSize(13);explanation.setPadding(MapTheme.dp(activity,4),MapTheme.dp(activity,4),MapTheme.dp(activity,4),MapTheme.dp(activity,6));
            status.setGravity(Gravity.CENTER_HORIZONTAL);status.setTextColor(MapTheme.dlgMuted());status.setTextSize(13);
            muted(rows);
            MapTheme.styleButtons(dialog);MapTheme.gameFooter(dialog,null);handler.post(refresh);});
        DialogLifecycle.bind(activity,dialog,()->handler.removeCallbacksAndMessages(null));dialog.show();
    }
    /** Secondary lines (coordinates, empty note) in the muted dialog colour. */
    private static void muted(android.view.ViewGroup group){
        for(int i=0;i<group.getChildCount();i++){android.view.View v=group.getChildAt(i);
            if(v instanceof TextView&&"muted".equals(v.getTag()))((TextView)v).setTextColor(MapTheme.dlgMuted());
            if(v instanceof android.view.ViewGroup)muted((android.view.ViewGroup)v);}
    }
    /** A small confirmation in the same style (game buttons and action bar). */
    private static void ask(Activity activity,android.content.Context ui,String title,String message,String action,Runnable yes){
        AlertDialog d=new AlertDialog.Builder(ui).setTitle(title).setMessage(message).setNegativeButton("Cancel",null).setPositiveButton(action,(x,w)->yes.run()).create();
        d.setOnShowListener(x->{MapTheme.styleButtons(d);MapTheme.gameFooter(d,null);});d.show();
    }
    /** A one-field dialog (rename / add) in the same style; the trimmed text goes to {@code done}. */
    private static void field(Activity activity,android.content.Context ui,String title,String message,EditText input,String action,java.util.function.Consumer<String> done){
        android.widget.FrameLayout box=new android.widget.FrameLayout(ui);box.setPadding(MapTheme.dp(activity,20),MapTheme.dp(activity,8),MapTheme.dp(activity,20),0);box.addView(input);
        AlertDialog.Builder b=new AlertDialog.Builder(ui).setTitle(title).setView(box).setNegativeButton("Cancel",null).setPositiveButton(action,(x,w)->done.accept(input.getText().toString().trim()));
        if(message!=null)b.setMessage(message);
        AlertDialog d=b.create();d.setOnShowListener(x->{MapTheme.styleField(input);MapTheme.styleButtons(d);MapTheme.gameFooter(d,null);input.requestFocus();});d.show();
    }
    private static void confirm(Activity activity,AccountProfileStore.Profile profile){
        ask(activity,SettingsUi.dialogContext(activity),"Switch to "+profile.label+"?",
            "Movement and automation will stop. The game will restart with this account's saved sign-in and location. Sign in if this account hasn't been saved yet.",
            "Switch",()->AccountProfiles.switchTo(activity,profile.id));
    }
}
