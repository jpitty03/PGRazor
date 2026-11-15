package dev.pgrazor;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.graphics.drawable.ColorDrawable;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import java.util.Properties;
import org.json.JSONObject;

/**
 * Shiny Scanner, Java half: the settings and the latest shiny found. The native
 * scanner (shiny_scan.cpp) does the checking and reports each one as a "shiny-scan" event; this class only
 * decides whether it may run and shows what it found. Nothing is caught or thrown.
 */
public final class ShinyScanner {
    private ShinyScanner() {}
    /** A found shiny is shown for this long, or until the player moves this far from it. */
    static final long FOUND_TTL_MS = 15 * 60 * 1000L;
    static final double FOUND_FAR_METERS = 300;

    private static final Object LOCK = new Object();
    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    private static final ShinyScanPreferences prefs = new ShinyScanPreferences();
    private static long session = System.currentTimeMillis() & 0x3fffffffL;
    private static String nativeStatus = "";
    private static int checked, shinies;
    private static int foundSpecies, foundCp; private static int[] foundIv = new int[3];
    private static double foundLat, foundLon; private static long foundAt;
    private static volatile Context app;
    private static volatile JSONObject names;

    public static void read(Properties p) { synchronized (LOCK) { prefs.read(p); session++; nativeStatus = ""; foundAt = 0; } }
    public static void write(Properties p) { synchronized (LOCK) { prefs.write(p); } }
    public static boolean enabled() { synchronized (LOCK) { return prefs.enabled; } }

    /** Why the scanner is waiting although it is on; null when nothing else is in its way. */
    private static String blocked() {
        if (Control.vgpEnabled()) return "Paused while VGP+ is on";
        if (ShundoSniper.active()) return "Paused while Shundo Sniper runs";
        if (AutoRaid.running()) return "Paused while Auto-Raid runs";
        return null;
    }
    /** Read by the native transport (string slot 23). */
    public static String config() {
        // Asks other features first, never while holding LOCK.
        boolean allowed = blocked() == null, count = Control.encounterCounter();
        int encounters = EncounterCounter.total();
        synchronized (LOCK) { return prefs.config(session, allowed, encounters, count); }
    }

    public static void setEnabled(boolean on) {
        synchronized (LOCK) {
            if (prefs.enabled == on) return;
            prefs.enabled = on; session++; nativeStatus = ""; checked = shinies = 0;
            if (!on) foundAt = 0;
        }
        Control.saveSettings();
    }

    /** One "shiny-scan" event from the native scanner. */
    public static void publish(String detail) {
        boolean shiny = false; int species = 0;
        try {
            JSONObject o = new JSONObject(detail);
            String phase = o.optString("phase");
            // Every encounter the game made counts, and so does a check that was sent but never answered.
            if ("encounter".equals(phase) || o.optBoolean("sent")) EncounterCounter.count(o.optString("id"));
            synchronized (LOCK) {
                if (o.optLong("session") != session) return;
                if ("status".equals(phase) || "error".equals(phase)) { nativeStatus = o.optString("message"); return; }
                if (!"checked".equals(phase) && !"failed".equals(phase)) return;
                if (!"checked".equals(phase)) return;
                checked++;
                if (o.optBoolean("shiny")) {
                    species = o.optInt("species"); double lat = o.optDouble("lat"), lon = o.optDouble("lon");
                    if (species > 0 && Double.isFinite(lat) && Double.isFinite(lon) && Math.abs(lat) <= 90 && Math.abs(lon) <= 180) {
                        shiny = true; shinies++;
                        foundSpecies = species; foundCp = o.optInt("cp"); foundLat = lat; foundLon = lon; foundAt = SystemClock.elapsedRealtime();
                        org.json.JSONArray iv = o.optJSONArray("iv");
                        for (int i = 0; i < 3; i++) foundIv[i] = iv == null ? -1 : iv.optInt(i, -1);
                    }
                }
            }
        } catch (Exception invalid) {
            Control.event("shiny-scan-diagnostic", "Invalid scanner event: " + invalid.getClass().getSimpleName());
            return;
        }
        if (shiny) {
            final String text = "Shiny " + name(species) + " nearby!";
            Control.event("shiny-scan-found", text);
            Context context = app;
            if (context != null) MAIN.post(() -> Toast.makeText(context, text, Toast.LENGTH_LONG).show());
        }
    }

    static String name(int species) {
        JSONObject table = names;
        String name = table == null ? "" : table.optString(String.valueOf(species), "");
        return name.isEmpty() ? "#" + species : name;
    }
    /** Remembers the app context for the found toast and loads the species names once, off the UI thread. */
    static void prepare(Context context) {
        if (context == null || app != null) return;
        final Context application = context.getApplicationContext();
        app = application;
        new Thread(() -> names = PokemonSpeciesDialog.readNames(application), "pgrazor-shiny-names").start();
    }

    private static double meters(double lat1, double lon1, double lat2, double lon2) {
        double dLat = Math.toRadians(lat2 - lat1), dLon = Math.toRadians(lon2 - lon1);
        double a = Math.sin(dLat / 2) * Math.sin(dLat / 2)
            + Math.cos(Math.toRadians(lat1)) * Math.cos(Math.toRadians(lat2)) * Math.sin(dLon / 2) * Math.sin(dLon / 2);
        return 2 * 6371000 * Math.asin(Math.min(1, Math.sqrt(a)));
    }
    /** The latest shiny for the map badge, e.g. "✨ Shiny Ferroseed · CP 512 · 40 m"; empty when there is none to show. */
    static String foundLabel() {
        int species, cp; double lat, lon; long at; int[] iv;
        synchronized (LOCK) {
            if (!prefs.enabled || foundAt == 0) return "";
            species = foundSpecies; cp = foundCp; lat = foundLat; lon = foundLon; at = foundAt; iv = foundIv.clone();
        }
        double here = Control.currentLatitude(), hereLon = Control.currentLongitude();
        double away = Double.isFinite(here) && Double.isFinite(hereLon) ? meters(here, hereLon, lat, lon) : Double.NaN;
        long age = SystemClock.elapsedRealtime() - at;
        if (age < 0 || age > FOUND_TTL_MS || away > FOUND_FAR_METERS) { dismissFound(); return ""; }
        String label = "✨ Shiny " + name(species) + (cp > 0 ? " · CP " + cp : "");
        if (iv[0] >= 0 && iv[1] >= 0 && iv[2] >= 0) label += " · " + iv[0] + "/" + iv[1] + "/" + iv[2];
        if (!Double.isNaN(away)) label += " · " + Math.round(away) + " m";
        return label;
    }
    static void dismissFound() { synchronized (LOCK) { foundAt = 0; } }

    /** One line for the dialog and the settings summary. */
    public static String status() {
        String blocked = blocked();
        int encounters = EncounterCounter.total();
        synchronized (LOCK) {
            if (!prefs.enabled) return "Off";
            String counts = checked + " checked, " + shinies + (shinies == 1 ? " shiny" : " shinies") + " · "
                + encounters + (prefs.dailyLimit > 0 ? " of " + prefs.dailyLimit : "") + " encounters in 24 h";
            if (blocked != null) return blocked + " · " + counts;
            return (nativeStatus.isEmpty() ? "Waiting for the map" : nativeStatus) + " · " + counts;
        }
    }

    public static void show(Activity activity) {
        if (activity == null || activity.isFinishing() || activity.isDestroyed()) return;
        prepare(activity); EncounterCounter.prepare(activity);
        Context ui = MapTheme.wrap(activity);
        LinearLayout body = new LinearLayout(ui); body.setOrientation(LinearLayout.VERTICAL);
        int pad = MapTheme.dp(ui, 16); body.setPadding(pad, pad, pad, pad);

        TextView description = new TextView(ui);
        description.setText("Checks wild Pokémon within 80 m in the background and tells you when one is shiny. "
            + "Nothing is caught or thrown: tap the Pokémon yourself to catch it. Each Pokémon is checked once, and one you "
            + "have already opened costs nothing. A check is an encounter request, so it counts toward the game's encounter "
            + "allowance; the scanner stops once this account reaches the limit below in any 24 hours, counting every "
            + "encounter PGRazor has seen.");
        body.addView(description);
        final TextView status = new TextView(ui); body.addView(status);
        final CheckBox on = new CheckBox(ui); on.setText("Scan for shinies"); body.addView(on);
        final Button speed = button(body), species = button(body), limit = button(body);
        TextView note = new TextView(ui);
        note.setText("Pauses while VGP+, Shundo Sniper or Auto-Raid is running. Choosing species checks only those, "
            + "which saves the most requests. Faster speeds send more requests at once; if the game stops answering, "
            + "the scanner pauses for 30 seconds by itself.");
        body.addView(note);

        final boolean[] refreshing = {false};
        final Runnable update = () -> {
            refreshing[0] = true;
            status.setText(status());
            synchronized (LOCK) {
                on.setChecked(prefs.enabled);
                speed.setText("Speed: " + ShinyScanPreferences.speedSummary(prefs.speed));
                species.setText("Species: " + speciesSummary(prefs.species));
                limit.setText("Encounter limit: " + ShinyScanPreferences.limitSummary(prefs.dailyLimit));
            }
            refreshing[0] = false;
        };
        on.setOnCheckedChangeListener((v, checked) -> { if (!refreshing[0]) { setEnabled(checked); update.run(); } });
        speed.setOnClickListener(v -> {
            String[] labels = new String[ShinyScanPreferences.SPEED_LABELS.length];
            String[] hints = {"One check every 2 seconds", "About one check a second", "Up to three checks a second"};
            for (int i = 0; i < labels.length; i++) labels[i] = ShinyScanPreferences.SPEED_LABELS[i] + "\n" + hints[i];
            choose(activity, "Scan speed", labels, index -> { synchronized (LOCK) { prefs.speed = index; } Control.saveSettings(); update.run(); });
        });
        limit.setOnClickListener(v -> {
            final int[] choices = ShinyScanPreferences.LIMIT_CHOICES;
            String[] labels = new String[choices.length];
            for (int i = 0; i < choices.length; i++)
                labels[i] = choices[i] == 0 ? "No limit\nKeeps checking whatever the count" : choices[i] + " in 24 hours\nStops checking at " + choices[i] + " encounters";
            choose(activity, "Encounter limit", labels, index -> { synchronized (LOCK) { prefs.dailyLimit = choices[index]; } Control.saveSettings(); update.run(); });
        });
        species.setOnClickListener(v -> PokemonSpeciesDialog.show(activity, "Species to check", "species", AutoRaidPreferences.MAX_BOSSES,
            () -> { synchronized (LOCK) { return prefs.species; } },
            value -> { String list = AutoRaidPreferences.normalizeBosses(value); synchronized (LOCK) { prefs.species = list; } Control.saveSettings(); },
            update, "Every species (no selection)", null));

        update.run();
        ScrollView scroll = new ScrollView(ui); scroll.addView(body);
        AlertDialog dialog = new AlertDialog.Builder(ui).setTitle("Shiny Scanner").setView(scroll)
            .setPositiveButton("Done", null).create();
        Handler handler = new Handler(Looper.getMainLooper());
        Runnable refresh = new Runnable() {
            public void run() { if (dialog.isShowing()) { status.setText(status()); handler.postDelayed(this, 1000); } }
        };
        DialogLifecycle.bind(activity, dialog, () -> handler.removeCallbacks(refresh));
        dialog.setOnShowListener(d -> {
            MapTheme.style(body); MapTheme.styleButtons(dialog);
            dialog.getWindow().setBackgroundDrawable(new ColorDrawable(MapTheme.surface()));
            handler.post(refresh);
        });
        dialog.show();
    }

    private interface Choice { void pick(int index); }
    private static void choose(Activity a, String title, String[] labels, Choice choice) {
        Context ui = MapTheme.wrap(a);
        AlertDialog d = new AlertDialog.Builder(ui).setTitle(title).setNegativeButton("Cancel", null).create();
        ChoiceList.attach(d, labels, choice::pick);
        DialogLifecycle.bind(a, d, null);
        d.setOnShowListener(x -> { MapTheme.styleButtons(d);
            d.getWindow().setBackgroundDrawable(new ColorDrawable(MapTheme.surface()));
        });
        d.show();
    }
    private static String speciesSummary(String list) {
        if (list == null || list.isEmpty()) return "Every species";
        String[] ids = list.split(",");
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < Math.min(3, ids.length); i++) {
            if (i > 0) out.append(", ");
            try { out.append(name(Integer.parseInt(ids[i]))); } catch (NumberFormatException e) { out.append('#').append(ids[i]); }
        }
        if (ids.length > 3) out.append(" +").append(ids.length - 3).append(" more");
        return out.toString();
    }
    private static Button button(LinearLayout body) {
        Button b = new Button(body.getContext()); b.setAllCaps(false); body.addView(b); return b;
    }
}
