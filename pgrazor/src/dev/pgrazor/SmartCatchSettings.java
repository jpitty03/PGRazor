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
public final class SmartCatchSettings {
    private SmartCatchSettings() {}
    private static final SmartCatchPreferences preferences=new SmartCatchPreferences();
    private static final int[] BERRIES={0,701,703,705,706,708};
    private static final String[] BERRY_NAMES={"Off","Razz","Nanab","Pinap","Golden Razz","Silver Pinap"};
    public static void init(Context context) {}
    public static void read(Properties p){preferences.read(p);}
    public static void write(Properties p){preferences.write(p);}
    public static String config(){return preferences.config();}
    /** Gym-disc spinning follows VGP's actual run/cooldown gate, not catch availability. */
    public static boolean gymSpinActive(){return preferences.enabled()&&preferences.spinGyms()&&Control.vgpEnabled();}
    public static String normalizeSpecies(String value){return SmartCatchPreferences.normalizeSpecies(value);}
    static String candySpecies(){return preferences.candySpecies();}
    static void setCandySpecies(String value){set("CandySpecies",value);}
    static String catchSpecies(){return preferences.catchSpecies();}
    static void setCatchSpecies(String value){set("CatchSpecies",value);}
    private static String count(String ids,String none){return ids.isEmpty()?none:ids.split(",").length+" selected";}
    private static void set(String key,Object value){preferences.set(key,String.valueOf(value));persist();}
    private static void persist(){Control.saveSettings();}
    public static Runnable populate(Activity activity,LinearLayout body){
        Context ui=body.getContext();
        CheckBox mode=new CheckBox(ui);mode.setText("VGP+");mode.setChecked(preferences.enabled());body.addView(mode);
        LinearLayout options=new LinearLayout(ui);options.setOrientation(LinearLayout.VERTICAL);body.addView(options);
        TextView description=new TextView(ui);description.setText("Use the VGP overlay to start or stop. VGP+ adds ball, berry and throw choices. High CP/level targets use Ultra Balls; set a threshold to 0 to disable it.");options.addView(description);
        TextView standard=row(activity,options,"Standard ball",new String[]{"Poké Ball","Great Ball","Ultra Ball"},preferences.ball()-1,index->{set("Ball",index+1);});
        CheckBox auto=new CheckBox(ui);auto.setText("Auto ball");auto.setChecked(preferences.autoBall());options.addView(auto);
        TextView autoTarget=button(options,"Auto ball target: "+preferences.autoBallTarget()+"%");
        TextView autoHelp=new TextView(ui);autoHelp.setText("Replaces the standard ball: throws the cheapest ball whose catch chance reaches the target, from the server's catch rate with your berry, throw rating and curveball. The Ultra Ball rules below still apply, and a Master Ball is never used.");options.addView(autoHelp);
        autoTarget.setOnClickListener(v->{
            int[] targets=Control.AUTO_BALL_TARGETS;int next=targets[0];
            for(int i=0;i<targets.length;i++)if(targets[i]==preferences.autoBallTarget()){next=targets[(i+1)%targets.length];break;}
            set("AutoBallTarget",next);autoTarget.setText("Auto ball target: "+next+"%");});
        int berryIndex=0;for(int i=0;i<BERRIES.length;i++)if(BERRIES[i]==preferences.berry())berryIndex=i;
        row(activity,options,"Standard berry",BERRY_NAMES,berryIndex,index->{set("Berry",BERRIES[index]);});
        CheckBox special=new CheckBox(ui);special.setText("Shiny or IV ≥90%: Ultra Ball + Golden Razz");special.setChecked(preferences.premium());options.addView(special);
        special.setOnCheckedChangeListener((v,on)->{set("Premium",on);});
        input(activity,options,"High CP threshold (0–10000)",String.valueOf(preferences.highCp()),value->{int n=bounded(value,10000);set("HighCp",n);},true);
        input(activity,options,"High level threshold (0–50)",String.valueOf(preferences.highLevel()),value->{int n=bounded(value,50);set("HighLevel",n);},true);
        TextView rating=row(activity,options,"Throw rating",new String[]{"Default","Nice","Great","Excellent"},preferences.grade(),index->{set("Grade",index);});
        Runnable[] afterVariety={null};
        row(activity,options,"Throw variety",Control.THROW_VARIETY_LABELS,preferences.variety(),index->{set("Variety",index);if(afterVariety[0]!=null)afterVariety[0].run();});
        TextView varietyHelp=new TextView(ui);varietyHelp.setText("Throw variety picks a rating at random for each throw, with even odds. All includes a plain throw. It replaces the throw rating while on.");options.addView(varietyHelp);
        CheckBox curved=new CheckBox(ui);curved.setText("Curveball");curved.setChecked(preferences.curve());options.addView(curved);
        curved.setOnCheckedChangeListener((v,on)->{set("Curve",on);});
        CheckBox gymSpin=new CheckBox(ui);gymSpin.setText("Auto-spin Gym discs");gymSpin.setChecked(preferences.spinGyms());options.addView(gymSpin);
        TextView gymSpinHelp=new TextView(ui);gymSpinHelp.setText("While VGP is running on the map, spin nearby Gym discs of any team within 80 m when the item bag has room.");options.addView(gymSpinHelp);
        gymSpin.setOnCheckedChangeListener((v,on)->{if(on!=preferences.spinGyms())set("SpinGyms",on);});
        TextView candy=button(options,"Candy species (Pinap): "+count(preferences.candySpecies(),"None"));
        candy.setOnClickListener(v->SmartCatchSpeciesDialog.show(activity,()->candy.setText("Candy species (Pinap): "+count(preferences.candySpecies(),"None"))));
        TextView only=button(options,"Catch only: "+count(preferences.catchSpecies(),"All species"));
        TextView onlyHelp=new TextView(ui);onlyHelp.setText("When species are selected, VGP+ catches only those and leaves every other Pokémon on the map, shiny or not.");options.addView(onlyHelp);
        only.setOnClickListener(v->SmartCatchSpeciesDialog.showCatchOnly(activity,()->only.setText("Catch only: "+count(preferences.catchSpecies(),"All species"))));
        Runnable refresh=()->{mode.setChecked(preferences.enabled());gymSpin.setChecked(preferences.spinGyms());auto.setChecked(preferences.autoBall());SettingsUi.enabled(mode,Control.vgpOverlay());SettingsUi.enabled(options,Control.vgpOverlay()&&preferences.enabled());if(Control.vgpOverlay()&&preferences.enabled()){SettingsUi.enabled(autoTarget,preferences.autoBall());SettingsUi.enabled(standard,!preferences.autoBall());SettingsUi.enabled(rating,preferences.variety()==0);}};
        afterVariety[0]=refresh;
        auto.setOnCheckedChangeListener((v,on)->{if(on!=preferences.autoBall())set("AutoBall",on);refresh.run();});
        mode.setOnCheckedChangeListener((v,on)->{if(on!=preferences.enabled())set("Enabled",on);refresh.run();});
        refresh.run();return refresh;
    }
    public static void show(Activity activity){
        if(activity==null||activity.isFinishing()||activity.isDestroyed())return;
        Context ui=MapTheme.wrap(activity);LinearLayout body=new LinearLayout(ui);body.setOrientation(LinearLayout.VERTICAL);
        int pad=MapTheme.dp(ui,16);body.setPadding(pad,pad,pad,pad);Runnable refresh=populate(activity,body);
        ScrollView scroll=new ScrollView(ui);scroll.addView(body);
        AlertDialog dialog=new AlertDialog.Builder(ui).setTitle("VGP+").setView(scroll).setPositiveButton("Done",null).create();
        DialogLifecycle.bind(activity,dialog,null);dialog.setOnShowListener(d->{MapTheme.style(body);MapTheme.styleButtons(dialog);refresh.run();});dialog.show();
    }
    private interface Choice {void set(int index);}
    private interface Value {void set(String value);}
    private static int bounded(String value,int max){
        try{int n=Integer.parseInt(value.trim());if(n>=0&&n<=max)return n;}catch(NumberFormatException ignored){}
        throw new IllegalArgumentException("Enter a whole number from 0 to "+max+".");
    }
    private static TextView button(LinearLayout body,String text){
        android.widget.Button b=new android.widget.Button(body.getContext());b.setText(text);b.setAllCaps(false);body.addView(b);return b;
    }
    private static TextView row(Activity a,LinearLayout body,String title,String[] names,int selected,Choice save){
        TextView button=button(body,title+": "+names[selected]);final int[] current={selected};
        button.setOnClickListener(v->{
            AlertDialog d=new AlertDialog.Builder(MapTheme.wrap(a)).setTitle(title).setSingleChoiceItems(names,current[0],(dialog,index)->{save.set(index);current[0]=index;button.setText(title+": "+names[index]);dialog.dismiss();}).setNegativeButton("Cancel",null).create();
            DialogLifecycle.bind(a,d,null);d.setOnShowListener(x->MapTheme.styleButtons(d));d.show();
        });
        return button;
    }
    private static void input(Activity a,LinearLayout body,String title,String initial,Value save,boolean numeric){
        TextView button=button(body,title+": "+(initial.isEmpty()?"None":initial));final String[] current={initial};
        button.setOnClickListener(v->{
            Context ui=MapTheme.wrap(a);EditText edit=new EditText(ui);edit.setSingleLine(true);edit.setText(current[0]);
            edit.setInputType(numeric?InputType.TYPE_CLASS_NUMBER:InputType.TYPE_CLASS_TEXT);
            AlertDialog d=new AlertDialog.Builder(ui).setTitle(title).setView(edit).setPositiveButton("Save",null).setNegativeButton("Cancel",null).create();
            DialogLifecycle.bind(a,d,null);d.setOnShowListener(x->{MapTheme.style(edit);MapTheme.styleButtons(d);d.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(w->{
                try{String value=edit.getText().toString();save.set(value);current[0]=numeric?value.trim():normalizeSpecies(value);button.setText(title+": "+(current[0].isEmpty()?"None":current[0]));d.dismiss();}
                catch(IllegalArgumentException e){edit.setError(e.getMessage());}
            });});d.show();
        });
    }
}

/** Feature-specific entry points; all species lists share PokemonSpeciesDialog. */
final class SmartCatchSpeciesDialog {
    private SmartCatchSpeciesDialog() {}
    static void show(Activity activity, Runnable onSaved) {
        PokemonSpeciesDialog.show(activity, "Candy species (Pinap)", "species", 200,
            SmartCatchSettings::candySpecies, SmartCatchSettings::setCandySpecies, onSaved,
            "None (no candy species selected)", null);
    }
    static void showCatchOnly(Activity activity, Runnable onSaved) {
        PokemonSpeciesDialog.show(activity, "Catch only these species", "species", 200,
            SmartCatchSettings::catchSpecies, SmartCatchSettings::setCatchSpecies, onSaved,
            "All species (no selection)", null);
    }
    static void show(Activity activity, String title, String noun, int max,
                     java.util.function.Supplier<String> load, java.util.function.Consumer<String> save, Runnable onSaved) {
        PokemonSpeciesDialog.show(activity, title, noun, max, load, save, onSaved, "All species (no selection)", null);
    }
}
