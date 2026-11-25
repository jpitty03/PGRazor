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
 * decides whether it may run and shows what it found. Nothing is thrown unless the player presses Catch it.
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
    // The found shiny's place on the screen, and a popup waiting to be shown for it.
    private static volatile float targetX, targetY; private static volatile long targetAt;
    private static String[] popup;       // {title, message, "found" or "result"}
    private static String foundText = ""; // the found popup's message, to show it again from the badge
    private static long catchSeq;
    private static boolean catching, gone;
    private static volatile JSONObject names;

    public static void read(Properties p) { synchronized (LOCK) { prefs.read(p); session++; nativeStatus = ""; foundAt = 0; } }
    public static void write(Properties p) { synchronized (LOCK) { prefs.write(p); } }
    public static boolean enabled() { synchronized (LOCK) { return prefs.enabled; } }

    /** Why the scanner is waiting although it is on; null when nothing else is in its way. */
    private static String blocked() {
        if (Control.vgpEnabled()) return "Paused while VGP+ is on";
        if (ShundoSniper.active()) return "Paused while Shundo Sniper runs";
        if (AutoRaid.running()) return "Paused while Auto-Raid runs";
        if (AutoRocket.running()) return "Paused while Auto Rocket runs";
        return null;
    }
    /** Read by the native transport (string slot 23). */
    public static String config() {
        // Asks other features first, never while holding LOCK.
        boolean allowed = blocked() == null, count = Control.encounterCounter();
        int encounters = EncounterCounter.total();
        synchronized (LOCK) { return prefs.config(session, allowed, encounters, count, catchSeq); }
    }

    public static void setEnabled(boolean on) {
        synchronized (LOCK) {
            if (prefs.enabled == on) return;
            prefs.enabled = on; session++; nativeStatus = ""; checked = shinies = 0;
            if (!on) { foundAt = 0; popup = null; catching = gone = false; }
        }
        Control.saveSettings();
    }

    static void setTarget(boolean visible, double x, double y) {
        if (!visible || !Double.isFinite(x) || !Double.isFinite(y) || x < 0 || x > 1 || y < 0 || y > 1) { targetAt = 0; return; }
        targetX = (float) x; targetY = (float) y; targetAt = SystemClock.elapsedRealtime();
    }
    /** Whether the shiny symbol should be drawn: the scanner is on and its place was reported in the last second. */
    static boolean targetVisible() {
        long at = targetAt;
        return at != 0 && SystemClock.elapsedRealtime() - at < 1000 && enabled();
    }
    static float targetX() { return targetX; }
    static float targetY() { return targetY; }
    /** A popup that has not been shown yet as {title, message, "found" or "result"}; null when there is none. Shown once. */
    static String[] takePopup() { synchronized (LOCK) { String[] next = popup; popup = null; return next; } }
    /** Shows the found shiny's popup again (from the map badge). */
    static void showFound() { synchronized (LOCK) { if (foundAt != 0) popup = new String[]{"✨ Shiny found", foundText, "found"}; } }
    /** The player asked to catch the found shiny: the native scanner feeds a berry and throws. */
    static void requestCatch() {
        String blocked = blocked();
        synchronized (LOCK) {
            if (foundAt == 0 || catching) return;
            if (blocked != null) { popup = new String[]{"Cannot catch yet", blocked + ". Stop it, then tap the shiny badge.", "result"}; return; }
            catchSeq++; catching = true;
        }
    }
    static boolean catching() { synchronized (LOCK) { return catching; } }

    /** One "shiny-scan" event from the native scanner. */
    public static void publish(String detail) {
        boolean shiny = false, notify = false, stop = false; int species = 0; String notice = "";
        try {
            JSONObject o = new JSONObject(detail);
            String phase = o.optString("phase");
            // Every encounter the game made counts, and so does a check that was sent but never answered.
            if ("encounter".equals(phase) || o.optBoolean("sent")) EncounterCounter.count(o.optString("id"));
            synchronized (LOCK) {
                if (o.optLong("session") != session) return;
                if ("gone".equals(phase)) { gone = true; return; }
                // Caught or fled through the game's own encounter: nothing left to show.
                if ("closed".equals(phase)) { foundAt = 0; catching = gone = false; return; }
                if ("catch".equals(phase)) {
                    String result = o.optString("result"), said = o.optString("message");
                    String who = "Shiny " + name(foundSpecies);
                    if ("throwing".equals(result)) { catching = true; return; }
                    if ("opened".equals(result)) { catching = false; return; }  // the game takes it from here
                    catching = false;
                    if ("caught".equals(result)) { foundAt = 0; popup = new String[]{"✨ Shiny caught", who + ". " + said + ".", "result"}; }
                    else if ("fled".equals(result)) { foundAt = 0; popup = new String[]{"Shiny fled", who + ". " + said + ".", "result"}; }
                    else popup = new String[]{"Catch did not finish", who + ". " + said + ". Tap the shiny badge to try again.", "result"};
                    return;
                }
                if ("status".equals(phase) || "error".equals(phase)) { nativeStatus = o.optString("message"); return; }
                if (!"checked".equals(phase) && !"failed".equals(phase)) return;
                if (!"checked".equals(phase)) return;
                checked++;
                if (o.optBoolean("shiny")) {
                    species = o.optInt("species"); double lat = o.optDouble("lat"), lon = o.optDouble("lon");
                    if (species > 0 && Double.isFinite(lat) && Double.isFinite(lon) && Math.abs(lat) <= 90 && Math.abs(lon) <= 180) {
                        shiny = true; shinies++; notify = prefs.notify; stop = prefs.stopWalking; catching = gone = false;
                        foundSpecies = species; foundCp = o.optInt("cp"); foundLat = lat; foundLon = lon; foundAt = SystemClock.elapsedRealtime();
                        org.json.JSONArray iv = o.optJSONArray("iv");
                        for (int i = 0; i < 3; i++) foundIv[i] = iv == null ? -1 : iv.optInt(i, -1);
                        foundText = "Shiny " + name(species) + (foundCp > 0 ? " · CP " + foundCp : "")
                            + (foundIv[0] >= 0 && foundIv[1] >= 0 && foundIv[2] >= 0 ? " · IV " + foundIv[0] + "/" + foundIv[1] + "/" + foundIv[2] : "")
                            + "\n\n" + (stop ? "Walking stopped. " : "") + "Marked with ✨ on the map."
                            + "\nCatch it: opens the encounter" + (prefs.hold ? ", or throws for you if it has left the map." : ".")
                            + "\nDismiss: lets it go.";
                        notice = foundText.substring(0, foundText.indexOf('\n'));
                        popup = new String[]{"✨ Shiny found", foundText, "found"};
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
            if (notify) notifyPhone(notice);
            if (stop) MAIN.post(ShinyScanner::stopWalking);
        }
    }

    /** Ends the current walk, autowalk route or in-game Route so the player stays next to the shiny. */
    private static void stopWalking() {
        try {
            if (Control.engine.walking) Control.event("walk-cancelled", "Shiny found");
            Control.engine.stop();
            if (Control.gameRoutesRunning()) Control.stopGameRoutes();
            Control.saveState();
        } catch (RuntimeException error) { Control.event("shiny-scan-diagnostic", "Could not stop walking: " + error.getClass().getSimpleName()); }
    }
    private static boolean mayNotify(Context context) {
        return android.os.Build.VERSION.SDK_INT < 33
            || context.checkSelfPermission("android.permission.POST_NOTIFICATIONS") == android.content.pm.PackageManager.PERMISSION_GRANTED;
    }
    /** A phone notification with sound for a found shiny; does nothing without the notification permission. */
    private static void notifyPhone(String text) {
        Context context = app;
        if (context == null || !mayNotify(context)) return;
        try {
            android.app.NotificationManager manager = (android.app.NotificationManager) context.getSystemService(Context.NOTIFICATION_SERVICE);
            manager.createNotificationChannel(new android.app.NotificationChannel("pgrazor-shiny", "PGRazor shiny found", android.app.NotificationManager.IMPORTANCE_HIGH));
            manager.notify(7302, new android.app.Notification.Builder(context, "pgrazor-shiny").setSmallIcon(android.R.drawable.star_on)
                .setContentTitle("✨ Shiny found").setContentText(text).setAutoCancel(true).build());
        } catch (RuntimeException error) { Control.event("shiny-scan-notification-error", error.toString()); }
    }
    private static void askToNotify(Activity activity) {
        if (!mayNotify(activity)) activity.requestPermissions(new String[]{"android.permission.POST_NOTIFICATIONS"}, 7302);
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
        int species, cp; double lat, lon; long at; int[] iv; boolean held, left, throwing;
        synchronized (LOCK) {
            if (!prefs.enabled || foundAt == 0) return "";
            species = foundSpecies; cp = foundCp; lat = foundLat; lon = foundLon; at = foundAt; iv = foundIv.clone();
            held = prefs.hold; left = gone; throwing = catching;
        }
        double here = Control.currentLatitude(), hereLon = Control.currentLongitude();
        double away = Double.isFinite(here) && Double.isFinite(hereLon) ? meters(here, hereLon, lat, lon) : Double.NaN;
        long age = SystemClock.elapsedRealtime() - at;
        // A held shiny stays until it is caught or dismissed; otherwise the badge goes after a while or once far away.
        if (!held && !throwing && (age < 0 || age > FOUND_TTL_MS || away > FOUND_FAR_METERS)) { dismissFound(); return ""; }
        String label = "✨ Shiny " + name(species) + (cp > 0 ? " · CP " + cp : "");
        if (iv[0] >= 0 && iv[1] >= 0 && iv[2] >= 0) label += " · " + iv[0] + "/" + iv[1] + "/" + iv[2];
        if (throwing) label += " · catching…";
        else if (left) label += " · left the map";
        else if (!Double.isNaN(away)) label += " · " + Math.round(away) + " m";
        return label;
    }
    /** Lets the found shiny go: the badge goes and a new session tells the native scanner to stop holding it. */
    static void dismissFound() { synchronized (LOCK) { if (foundAt == 0) return; foundAt = 0; catching = gone = false; session++; } }

    /** One line for the dialog and the settings summary. */
    public static String status() {
        String blocked = blocked();
        int encounters = EncounterCounter.total();
        synchronized (LOCK) {
            if (!prefs.enabled) return "Off";
            String counts = checked + " checked, " + shinies + (shinies == 1 ? " shiny" : " shinies") + " · "
                + encounters + (prefs.dailyLimit > 0 ? "/" + prefs.dailyLimit : "") + " in 24 h";
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
        description.setText("Checks wild Pokémon within 80 m for shinies. Each check uses one encounter.");
        body.addView(description);
        final TextView status = new TextView(ui); body.addView(status);
        final CheckBox on = new CheckBox(ui); on.setText("Scan for shinies"); body.addView(on);
        final CheckBox notifyBox = new CheckBox(ui); notifyBox.setText("Phone notification"); body.addView(notifyBox);
        final CheckBox stopBox = new CheckBox(ui); stopBox.setText("Stop walking when found"); body.addView(stopBox);
        final CheckBox holdBox = new CheckBox(ui); holdBox.setText("Hold it (pause checks until caught or dismissed)"); body.addView(holdBox);
        final Button speed = button(body), species = button(body), limit = button(body);
        TextView note = new TextView(ui);
        note.setText("Pauses while VGP+, Shundo Sniper or Auto-Raid runs.");
        body.addView(note);

        final boolean[] refreshing = {false};
        final Runnable update = () -> {
            refreshing[0] = true;
            status.setText(status());
            synchronized (LOCK) {
                on.setChecked(prefs.enabled); notifyBox.setChecked(prefs.notify); stopBox.setChecked(prefs.stopWalking); holdBox.setChecked(prefs.hold);
                speed.setText("Speed: " + ShinyScanPreferences.speedSummary(prefs.speed));
                species.setText("Species: " + speciesSummary(prefs.species));
                limit.setText("Encounter limit: " + ShinyScanPreferences.limitSummary(prefs.dailyLimit));
            }
            refreshing[0] = false;
        };
        on.setOnCheckedChangeListener((v, checked) -> { if (!refreshing[0]) { setEnabled(checked); if (checked && notifyBox.isChecked()) askToNotify(activity); update.run(); } });
        notifyBox.setOnCheckedChangeListener((v, checked) -> {
            if (refreshing[0]) return;
            synchronized (LOCK) { prefs.notify = checked; }
            Control.saveSettings(); if (checked) askToNotify(activity); update.run();
        });
        stopBox.setOnCheckedChangeListener((v, checked) -> { if (refreshing[0]) return; synchronized (LOCK) { prefs.stopWalking = checked; } Control.saveSettings(); update.run(); });
        holdBox.setOnCheckedChangeListener((v, checked) -> { if (refreshing[0]) return; synchronized (LOCK) { prefs.hold = checked; } Control.saveSettings(); update.run(); });
        speed.setOnClickListener(v -> {
            String[] labels = new String[ShinyScanPreferences.SPEED_LABELS.length];
            String[] hints = {"1 check every 2 s", "About 1 a second", "Up to 3 a second"};
            for (int i = 0; i < labels.length; i++) labels[i] = ShinyScanPreferences.SPEED_LABELS[i] + "\n" + hints[i];
            choose(activity, "Scan speed", labels, index -> { synchronized (LOCK) { prefs.speed = index; } Control.saveSettings(); update.run(); });
        });
        limit.setOnClickListener(v -> {
            final int[] choices = ShinyScanPreferences.LIMIT_CHOICES;
            String[] labels = new String[choices.length];
            for (int i = 0; i < choices.length; i++)
                labels[i] = choices[i] == 0 ? "No limit" : choices[i] + " in 24 hours";
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
