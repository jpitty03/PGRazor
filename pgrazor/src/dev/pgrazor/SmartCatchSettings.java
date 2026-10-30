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
import android.text.Editable;
import android.text.TextWatcher;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.ListView;
import java.util.ArrayList;
import java.util.List;
import org.json.JSONObject;
import java.util.Properties;
import java.util.TreeSet;

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
    public static String normalizeSpecies(String value){return SmartCatchPreferences.normalizeSpecies(value);}
    static String candySpecies(){return preferences.candySpecies();}
    static void setCandySpecies(String value){set("CandySpecies",value);}
    private static void set(String key,Object value){preferences.set(key,String.valueOf(value));persist();}
    private static void persist(){Control.saveSettings();}
    public static void show(Activity activity){
        if(activity==null||activity.isFinishing()||activity.isDestroyed())return;
        Context ui=MapTheme.wrap(activity);
        LinearLayout body=new LinearLayout(ui);body.setOrientation(LinearLayout.VERTICAL);
        int pad=MapTheme.dp(ui,16);body.setPadding(pad,pad,pad,pad);
        TextView description=new TextView(ui);
        description.setText("Use normal VGP Start/Stop; this mode replaces only its catch strategy and does not start VGP automatically. High CP or level thresholds select Ultra Balls; 0 disables that threshold. Choose up to 200 candy species for Pinap. Selected balls and berries require stock; Master Balls are never used.");body.addView(description);
        CheckBox mode=new CheckBox(ui);mode.setText("Use Smart Background Catcher");mode.setChecked(preferences.enabled());body.addView(mode);
        mode.setOnCheckedChangeListener((v,on)->{set("Enabled",on);});
        row(activity,body,"Standard ball",new String[]{"Poké Ball","Great Ball","Ultra Ball"},preferences.ball()-1,index->{set("Ball",index+1);});
        int berryIndex=0;for(int i=0;i<BERRIES.length;i++)if(BERRIES[i]==preferences.berry())berryIndex=i;
        row(activity,body,"Standard berry",BERRY_NAMES,berryIndex,index->{set("Berry",BERRIES[index]);});
        CheckBox special=new CheckBox(ui);special.setText("Shiny or IV ≥90%: Ultra Ball + Golden Razz");special.setChecked(preferences.premium());body.addView(special);
        special.setOnCheckedChangeListener((v,on)->{set("Premium",on);});
        input(activity,body,"High CP threshold (0–10000)",String.valueOf(preferences.highCp()),value->{int n=bounded(value,10000);set("HighCp",n);},true);
        input(activity,body,"High level threshold (0–50)",String.valueOf(preferences.highLevel()),value->{int n=bounded(value,50);set("HighLevel",n);},true);
        row(activity,body,"Throw rating",new String[]{"Default","Nice","Great","Excellent"},preferences.grade(),index->{set("Grade",index);});
        CheckBox curved=new CheckBox(ui);curved.setText("Curveball");curved.setChecked(preferences.curve());body.addView(curved);
        curved.setOnCheckedChangeListener((v,on)->{set("Curve",on);});
        TextView candy=button(body,"Candy species (Pinap): "+(preferences.candySpecies().isEmpty()?"None":preferences.candySpecies().split(",").length+" selected"));
        candy.setOnClickListener(v->SmartCatchSpeciesDialog.show(activity,()->candy.setText("Candy species (Pinap): "+(preferences.candySpecies().isEmpty()?"None":preferences.candySpecies().split(",").length+" selected"))));
        ScrollView scroll=new ScrollView(ui);scroll.addView(body);
        AlertDialog dialog=new AlertDialog.Builder(ui).setTitle("Smart Background Catcher").setView(scroll).setPositiveButton("Done",null).create();
        DialogLifecycle.bind(activity,dialog,null);dialog.setOnShowListener(d->{MapTheme.style(body);MapTheme.styleButtons(dialog);});dialog.show();
    }
    private interface Choice {void set(int index);}
    private interface Value {void set(String value);}
    private static int bounded(String value,int max){
        try{int n=Integer.parseInt(value.trim());if(n>=0&&n<=max)return n;}catch(NumberFormatException ignored){}
        throw new IllegalArgumentException("Enter a whole number from 0 to "+max+".");
    }
    private static TextView button(LinearLayout body,String text){
        android.widget.Button b=new android.widget.Button(body.getContext());b.setText(text);body.addView(b);return b;
    }
    private static void row(Activity a,LinearLayout body,String title,String[] names,int selected,Choice save){
        TextView button=button(body,title+": "+names[selected]);final int[] current={selected};
        button.setOnClickListener(v->{
            AlertDialog d=new AlertDialog.Builder(MapTheme.wrap(a)).setTitle(title).setSingleChoiceItems(names,current[0],(dialog,index)->{save.set(index);current[0]=index;button.setText(title+": "+names[index]);dialog.dismiss();}).setNegativeButton("Cancel",null).create();
            DialogLifecycle.bind(a,d,null);d.setOnShowListener(x->MapTheme.styleButtons(d));d.show();
        });
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

// Searchable, offline picker adapted from TransferKeepDialog; edits only candy preferences.
final class SmartCatchSpeciesDialog {
    private SmartCatchSpeciesDialog() {}

    /** Display range mirrors the known National Dex; selected ids beyond it are still shown and kept. */
    private static final int DISPLAY_MIN = 1, DISPLAY_MAX = 1025;
    private static final String NAMES_ASSET = "pgrazor/map-icons/pokemon-names.json";
    private static final int NAMES_LIMIT = 2 * 1024 * 1024;

    public static void show(Activity activity, Runnable onSaved) {
        if (activity == null || activity.isFinishing() || activity.isDestroyed()) return;
        new Picker(activity, onSaved).open();
    }

    private static final class Picker {
        private final Activity activity;
        private final Context ui;
        private final Runnable onSaved;
        private final TreeSet<Integer> selected = new TreeSet<>();
        private final List<Integer> all = new ArrayList<>();
        private final List<Integer> shown = new ArrayList<>();
        private volatile JSONObject names;   // dex-id string -> name; filled off the main thread
        private Adapter adapter;
        private TextView count;
        private EditText search;
        private AlertDialog dialog;
        private boolean closed;

        Picker(Activity activity, Runnable onSaved) {
            this.activity = activity; this.ui = MapTheme.wrap(activity); this.onSaved = onSaved;
        }

        private int dp(float v) { return MapTheme.dp(ui, v); }

        void open() {
            // Seed from the saved canonical list (already validated and bounded by Control).
            String saved = SmartCatchSettings.candySpecies();
            if (!saved.isEmpty()) for (String token : saved.split(",")) {
                try { int id = Integer.parseInt(token.trim()); if (id >= 1 && id <= 2000) selected.add(id); }
                catch (NumberFormatException ignored) {}
            }
            // Displayed ids: the dex range plus any saved selection beyond it, so nothing is lost.
            TreeSet<Integer> ids = new TreeSet<>();
            for (int id = DISPLAY_MIN; id <= DISPLAY_MAX; id++) ids.add(id);
            ids.addAll(selected);
            all.addAll(ids); shown.addAll(ids);

            LinearLayout body = new LinearLayout(ui); body.setOrientation(LinearLayout.VERTICAL);
            int pad = dp(14); body.setPadding(pad, dp(8), pad, 0);
            search = new EditText(ui); search.setHint("Search name or #dex"); search.setSingleLine(true);
            search.setInputType(InputType.TYPE_CLASS_TEXT);
            body.addView(search, new LinearLayout.LayoutParams(-1, dp(48)));
            count = new TextView(ui); count.setTextSize(12); count.setPadding(0, dp(4), 0, dp(4)); body.addView(count);
            ListView list = new ListView(ui);
            adapter = new Adapter();
            list.setAdapter(adapter);
            list.setOnItemClickListener((parent, view, position, rowId) -> {
                int id = shown.get(position);
                if (!selected.remove(id)) {
                    if(selected.size()>=200){android.widget.Toast.makeText(activity,"Choose at most 200 candy species.",android.widget.Toast.LENGTH_SHORT).show();return;}
                    selected.add(id);
                }
                adapter.notifyDataSetChanged(); updateCount();
            });
            int listHeight = Math.max(dp(120), Math.min(dp(420),
                activity.getResources().getDisplayMetrics().heightPixels - dp(260)));
            body.addView(list, new LinearLayout.LayoutParams(-1, listHeight));
            updateCount();

            search.addTextChangedListener(new TextWatcher() {
                public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
                public void onTextChanged(CharSequence s, int a, int b, int c) {}
                public void afterTextChanged(Editable e) { applyFilter(e.toString()); }
            });

            dialog = new AlertDialog.Builder(ui)
                .setTitle("Candy species (Pinap)")
                .setView(body)
                .setPositiveButton("Save", (d, w) -> commit())
                .setNeutralButton("Clear all", null)   // click overridden below so it does not dismiss
                .setNegativeButton("Cancel", null)
                .create();
            DialogLifecycle.bind(activity,dialog,()->closed=true);
            dialog.setOnShowListener(d -> {
                MapTheme.style(body); MapTheme.styleButtons(dialog);
                // Clear only empties the selection; the user can still Save or Cancel afterwards.
                dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener(v -> {
                    selected.clear(); adapter.notifyDataSetChanged(); updateCount();
                });
            });
            dialog.show();
            loadNames();
        }

        private void applyFilter(String query) {
            shown.clear();
            for (int id : all) {
                if(SmartCatchPreferences.matchesSpecies(id,name(id),query))shown.add(id);
            }
            adapter.notifyDataSetChanged();
        }

        private void updateCount() { count.setText(selected.size() + "/200 candy species selected"); }

        private void commit() {
            StringBuilder sb = new StringBuilder();
            for (int id : selected) { if (sb.length() > 0) sb.append(','); sb.append(id); }
            SmartCatchSettings.setCandySpecies(sb.toString());
            if (onSaved != null) onSaved.run();
        }

        /** Reads the bundled name table off the main thread; labels and name search refresh on arrival. */
        private void loadNames() {
            new Thread(() -> {
                JSONObject parsed = null;
                try (java.io.InputStream in = activity.getAssets().open(NAMES_ASSET)) {
                    java.io.ByteArrayOutputStream bytes = new java.io.ByteArrayOutputStream();
                    byte[] buffer = new byte[4096]; int read; boolean oversize = false;
                    while ((read = in.read(buffer)) != -1) {
                        if (bytes.size() + read > NAMES_LIMIT) { oversize = true; break; }
                        bytes.write(buffer, 0, read);
                    }
                    if (!oversize) parsed = new JSONObject(new String(bytes.toByteArray(), java.nio.charset.StandardCharsets.UTF_8));
                } catch (Exception ignored) {}
                final JSONObject result = parsed;
                activity.runOnUiThread(() -> {
                    if (closed || activity.isFinishing() || activity.isDestroyed() || result == null) return;
                    names = result;
                    applyFilter(search == null ? "" : search.getText().toString()); // name search now resolves
                });
            }, "pgrazor-candy-names").start();
        }

        private String name(int id) {
            JSONObject table = names;
            return table == null ? "" : table.optString(Integer.toString(id), "");
        }

        private final class Adapter extends BaseAdapter {
            public int getCount() { return shown.size(); }
            public Object getItem(int position) { return shown.get(position); }
            public long getItemId(int position) { return shown.get(position); }
            public View getView(int position, View convertView, ViewGroup parent) {
                CheckBox cb = convertView instanceof CheckBox ? (CheckBox) convertView : new CheckBox(ui);
                if (convertView == null) {
                    cb.setTextColor(MapTheme.ink());
                    cb.setButtonTintList(android.content.res.ColorStateList.valueOf(MapTheme.accent()));
                    cb.setClickable(false); cb.setFocusable(false); // the row click owns the toggle
                    cb.setMinHeight(dp(48)); cb.setPadding(dp(8), dp(8), dp(8), dp(8));
                }
                int id = shown.get(position);
                String name = name(id);
                cb.setText(name.isEmpty() ? ("#" + id) : ("#" + id + "  " + name));
                cb.setChecked(selected.contains(id));
                return cb;
            }
        }
    }
}
