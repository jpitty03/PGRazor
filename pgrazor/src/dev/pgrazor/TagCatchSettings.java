package dev.pgrazor;
import android.app.Activity;
import android.app.AlertDialog;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Properties;
import java.util.Set;
import java.util.function.Consumer;
import java.util.function.Supplier;
/** VGP+ tag rules: "Catch only" / "Candy species" Pokemon storage tags (whole candy families, kept up to date) and the shared rare rule. */
final class TagCatchSettings {
    private TagCatchSettings() {}
    static final int[] BERRIES={0,701,703,705,706,707};
    static final String[] BERRY_NAMES={"None","Razz","Nanab","Pinap","Golden Razz","Silver Pinap"};
    private static final String[] RARE_NAMES={"Legendary, Mythical, Ultra Beast","Shiny","Shundo (shiny 100%)","100% IV","Nundo (0% IV)","IV at or above threshold","Event costume","Special background","Ultra-rare species list"};
    private static final int[] IV_STEPS={80,90,95,98};
    private static final String DEFAULT_ULTRA="201,480,481,482,610,782,885,996";
    // Defaults match the old fixed rule: shiny or IV >= 90% gets an Ultra Ball and a Golden Razz.
    private static boolean active=true,overlay=true,alsoRare;
    private static int rareMask=2|32,ivMin=90,rareBerry=706,candyBerry=705;
    private static String ultra=DEFAULT_ULTRA,catchTags="",candyTags="",fallbackBerries="";
    private static String sent;

    private static int number(Properties p,String key,int fallback,int min,int max){
        try{int n=Integer.parseInt(p.getProperty(key,""));return n>=min&&n<=max?n:fallback;}catch(NumberFormatException e){return fallback;}
    }
    private static int berry(Properties p,String key,int fallback){int n=number(p,key,fallback,0,708);if(n==708)n=707; /* legacy saves used 708 for Silver Pinap */for(int b:BERRIES)if(b==n)return n;return fallback;}
    private static String tags(Properties p,String key){String v=p.getProperty(key,"");return v.length()>4000?"":v;}
    private static String berryList(String v){try{BerryPriority.parse(v);return v;}catch(IllegalArgumentException e){return "";}}
    static synchronized void read(Properties p){
        active=!"false".equals(p.getProperty("smartCatchTagsActive"));overlay=!"false".equals(p.getProperty("smartCatchTagsOverlay"));
        alsoRare="true".equals(p.getProperty("smartCatchAlsoRare"));
        rareMask=number(p,"smartCatchRareMask",2|32,0,511);ivMin=number(p,"smartCatchRareIvMin",90,1,100);
        rareBerry=berry(p,"smartCatchRareBerry",706);candyBerry=berry(p,"smartCatchCandyBerry",705);
        try{ultra=SmartCatchPreferences.normalizeSpecies(p.getProperty("smartCatchUltraSpecies",DEFAULT_ULTRA));}catch(IllegalArgumentException e){ultra=DEFAULT_ULTRA;}
        catchTags=tags(p,"smartCatchCatchTags");candyTags=tags(p,"smartCatchCandyTags");fallbackBerries=berryList(p.getProperty("smartCatchFallbackBerries",""));
    }
    static synchronized void write(Properties p){
        p.setProperty("smartCatchTagsActive",String.valueOf(active));p.setProperty("smartCatchTagsOverlay",String.valueOf(overlay));
        p.setProperty("smartCatchAlsoRare",String.valueOf(alsoRare));p.setProperty("smartCatchRareMask",String.valueOf(rareMask));
        p.setProperty("smartCatchRareIvMin",String.valueOf(ivMin));p.setProperty("smartCatchRareBerry",String.valueOf(rareBerry));
        p.setProperty("smartCatchCandyBerry",String.valueOf(candyBerry));p.setProperty("smartCatchUltraSpecies",ultra);
        p.setProperty("smartCatchCatchTags",catchTags);p.setProperty("smartCatchCandyTags",candyTags);p.setProperty("smartCatchFallbackBerries",fallbackBerries);
    }
    private static synchronized String config(){return "2;"+(active?1:0)+";"+(alsoRare?1:0)+";"+rareMask+";"+ivMin+";"+rareBerry+";"+candyBerry+";"+ultra+";"+catchTags+";"+candyTags+";"+fallbackBerries;}
    /** Sends the config to native when it changed; called from the overlay refresh. */
    static void push(){
        String c=config();if(c.equals(sent))return;
        try{NativeCore.tagCatchConfig(c);sent=c;}catch(UnsatisfiedLinkError ignored){}
    }
    private static void save(Runnable change){synchronized(TagCatchSettings.class){change.run();}Control.saveSettings();push();}
    static synchronized boolean active(){return active;}
    /** The overlay button shows when tags are selected for Catch only or Candy species. */
    static synchronized boolean overlay(){return overlay&&!(catchTags.isEmpty()&&candyTags.isEmpty());}
    static void setActive(boolean on){save(()->active=on);}
    static synchronized String catchTags(){return catchTags;}
    static synchronized String candyTags(){return candyTags;}
    static void setCatchTags(String v){save(()->catchTags=v);}
    static void setCandyTags(String v){save(()->candyTags=v);}
    /** Ordered fallback list used when the configured rare/candy berry is out of stock. */
    static void setFallbackBerry(int id){save(()->{try{fallbackBerries=BerryPriority.select(fallbackBerries,id);}catch(IllegalArgumentException ignored){}});}
    static String fallbackBerries(){return fallbackBerries;}
    static int count(String tags){return tags.isEmpty()?0:tags.split("\t").length;}
    private static int berryIndex(int id){for(int i=0;i<BERRIES.length;i++)if(BERRIES[i]==id)return i;return 0;}

    /** Multi-choice of the player's own tags; the selection is saved as names and resolved to candy families on the game side. */
    static void pickTags(Activity activity,String title,Supplier<String> load,Consumer<String> store,Runnable after){
        String names;try{names=NativeCore.tagCatchTags();}catch(UnsatisfiedLinkError e){names="";}
        Set<String> all=new LinkedHashSet<>();for(String n:names.split("\n"))if(!n.isEmpty())all.add(n.replace(";"," ").replace("\t"," "));
        String saved=load.get();List<String> chosen=saved.isEmpty()?new ArrayList<>():new ArrayList<>(Arrays.asList(saved.split("\t")));all.addAll(chosen);
        if(all.isEmpty()){android.widget.Toast.makeText(activity,"No tags read yet. Turn on VGP+, open the map and try again in a minute.",android.widget.Toast.LENGTH_LONG).show();return;}
        String[] items=all.toArray(new String[0]);boolean[] checked=new boolean[items.length];
        for(int i=0;i<items.length;i++)checked[i]=chosen.contains(items[i]);
        dialog(activity,new AlertDialog.Builder(MapTheme.wrap(activity)).setTitle(title).setMultiChoiceItems(items,checked,(d,i,c)->checked[i]=c)
            .setPositiveButton("Save",(d,w)->{List<String> keep=new ArrayList<>();for(int i=0;i<items.length;i++)if(checked[i])keep.add(items[i]);store.accept(String.join("\t",keep));if(after!=null)after.run();})
            .setNegativeButton("Cancel",null));
    }

    /** The rare rule and tag options, added to the VGP+ section. Returns its refresh. */
    static Runnable populate(Activity activity,LinearLayout body){
        CheckBox also=new CheckBox(activity);also.setText("Also catch rare Pokémon outside Catch only");body.addView(also);
        also.setOnClickListener(v->save(()->alsoRare=also.isChecked()));
        TextView alsoHelp=new TextView(activity);alsoHelp.setText("Rare Pokémon (the criteria below) are caught even when they are not in Catch only.");body.addView(alsoHelp);
        Button rareList=button(body),iv=button(body),species=button(body),rare=button(body),candy=button(body),fallback=button(body);
        Runnable[] refresh={null};
        rareList.setOnClickListener(v->{
            boolean[] checked=new boolean[RARE_NAMES.length];for(int i=0;i<checked.length;i++)checked[i]=(rareMask&(1<<i))!=0;
            dialog(activity,new AlertDialog.Builder(MapTheme.wrap(activity)).setTitle("Counts as rare").setMultiChoiceItems(RARE_NAMES,checked,(d,i,c)->checked[i]=c)
                .setPositiveButton("Save",(d,w)->{int m=0;for(int i=0;i<checked.length;i++)if(checked[i])m|=1<<i;final int mask=m;save(()->rareMask=mask);refresh[0].run();})
                .setNegativeButton("Cancel",null));
        });
        iv.setOnClickListener(v->{int next=IV_STEPS[0];for(int i=0;i<IV_STEPS.length;i++)if(IV_STEPS[i]==ivMin){next=IV_STEPS[(i+1)%IV_STEPS.length];break;}final int n=next;save(()->ivMin=n);refresh[0].run();});
        species.setOnClickListener(v->PokemonSpeciesDialog.show(activity,"Ultra-rare species","species",200,()->ultra,
            value->{String clean=SmartCatchPreferences.normalizeSpecies(value);save(()->ultra=clean);},refresh[0],"No species selected",null));
        rare.setOnClickListener(v->dialog(activity,new AlertDialog.Builder(MapTheme.wrap(activity)).setTitle("Berry for rare Pokémon")
            .setSingleChoiceItems(BERRY_NAMES,berryIndex(rareBerry),(d,i)->{save(()->rareBerry=BERRIES[i]);d.dismiss();refresh[0].run();}).setNegativeButton("Cancel",null)));
        candy.setOnClickListener(v->dialog(activity,new AlertDialog.Builder(MapTheme.wrap(activity)).setTitle("Berry for candy species")
            .setSingleChoiceItems(BERRY_NAMES,berryIndex(candyBerry),(d,i)->{save(()->candyBerry=BERRIES[i]);d.dismiss();refresh[0].run();}).setNegativeButton("Cancel",null)));
        fallback.setOnClickListener(v->{
            BerryPriorityPicker picker=new BerryPriorityPicker(activity,refresh[0],"Berries when the configured berry is unavailable",
                TagCatchSettings::fallbackBerries,TagCatchSettings::setFallbackBerry);
            picker.show();
        });
        TextView fallbackHelp=new TextView(activity);fallbackHelp.setText("If the berry chosen above is out of stock, these berries are tried in this order before the map's berry priority. Tap Off to clear.");body.addView(fallbackHelp);
        CheckBox button=new CheckBox(activity);button.setText("Overlay button to switch tag rules on and off");body.addView(button);
        button.setOnClickListener(v->save(()->overlay=button.isChecked()));
        TextView help=new TextView(activity);help.setText("Rare uses the criteria above for the Ultra Ball rule and for \"Also catch rare\". Shiny, IV, costume and background need an encounter, so with those VGP+ opens every Pokémon. Tags in Catch only and Candy species cover the whole candy family (Squirtle for a tagged Blastoise; regional forms share candy) and are re-read every minute.");body.addView(help);
        refresh[0]=()->{
            synchronized(TagCatchSettings.class){also.setChecked(alsoRare);button.setChecked(overlay);}
            rareList.setText("Rare: "+Integer.bitCount(rareMask)+" of "+RARE_NAMES.length+" criteria");iv.setText("Rare IV threshold: "+ivMin+"%");
            species.setText("Ultra-rare species: "+(ultra.isEmpty()?0:ultra.split(",").length));
            rare.setText("Rare berry: "+BERRY_NAMES[berryIndex(rareBerry)]);candy.setText("Candy species berry: "+BERRY_NAMES[berryIndex(candyBerry)]);
            fallback.setText("Fallback berries: "+BerryPriorityPicker.summary(fallbackBerries));
        };
        refresh[0].run();return refresh[0];
    }
    private static Button button(LinearLayout body){Button b=new Button(body.getContext());b.setAllCaps(false);body.addView(b);return b;}
    private static void dialog(Activity a,AlertDialog.Builder builder){AlertDialog d=builder.create();DialogLifecycle.bind(a,d,null);d.setOnShowListener(x->MapTheme.styleButtons(d));d.show();}
}
