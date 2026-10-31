package dev.pgrazor;

import android.app.AlertDialog;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.Handler;
import android.os.Looper;
import android.widget.TextView;
import org.json.JSONArray;
import org.json.JSONObject;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.HashMap;

/** Draws and makes tappable the game-loaded nearby points (stops/gyms/raids) over the map.
 *  All map coupling is explicit: a {@link Projector} places lat/lon on screen, a {@link Snapshots}
 *  feeds the point data, a themed context hosts the detail dialogs, and a redraw callback
 *  requests frames. Map gestures, routes and selection stay in the dialog's view. */
final class MapPoiLayer {
    /** Screen placement for a lat/lon against the live viewport, plus the drawable bounds. */
    interface Projector {
        float screenX(double lon);
        float screenY(double lat);
        int width();
        int height();
    }
    /** The nearby-point feed; opening the layer signals the game to keep snapshots fresh. */
    interface Snapshots {
        void setOpen(boolean open);
        String read();
    }

    private final Context context; // asset access and density
    private final Context dialogContext; // themed chrome for POI detail dialogs
    private final Projector projector;
    private final Snapshots snapshots;
    private final Runnable invalidate;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
    // Compact gym defender count has its own paints so the shared marker style is never disturbed.
    private final Paint labelFill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint labelOutline = new Paint(Paint.ANTI_ALIAS_FLAG);
    // Bundled POI art is decoded once and reused every frame; a null entry marks a tried-but-missing
    // asset so the vector fallback renders without re-hitting the AssetManager each draw.
    private final Map<String, Bitmap> markerIcons = new HashMap<>();
    // Raid-boss base-species id -> English game name, read once from the bundled table (null if unreadable).
    private Map<String, String> bossNames;
    private boolean bossNamesLoaded;
    private final List<AlertDialog> poiDialogs = new ArrayList<>();
    private final Runnable poiRefresh = this::refresh;
    private int layers;
    private TextView poiStatus;
    private JSONArray pois = new JSONArray();
    private String lastPois = "";
    private boolean closed;
    private WorldMapDialog.DestinationListener walk, teleport;

    void setDestinationActions(WorldMapDialog.DestinationListener walk, WorldMapDialog.DestinationListener teleport) {
        this.walk = walk; this.teleport = teleport;
    }

    MapPoiLayer(Context context, Context dialogContext, Projector projector, Snapshots snapshots, Runnable invalidate) {
        this.context = context; this.dialogContext = dialogContext; this.projector = projector;
        this.snapshots = snapshots; this.invalidate = invalidate;
        for (String icon : new String[]{"stop.png","gym.png","raid-normal.png","raid-rare.png","raid-legendary.png","raid-primal.png","raid-ultra.png","raid-shadow.png","raid-active.png","raid-mega-active.png","raid-primal-active.png","raid-shadow-active.png"}) loadIcon(icon);
        // White glyphs on a dark outline keep the count legible over any street colour.
        labelFill.setColor(Color.WHITE); labelFill.setTextAlign(Paint.Align.CENTER); labelFill.setFakeBoldText(true);
        labelOutline.setColor(Color.rgb(20, 24, 28)); labelOutline.setTextAlign(Paint.Align.CENTER);
        labelOutline.setStyle(Paint.Style.STROKE); labelOutline.setFakeBoldText(true);
    }

    boolean hasLayers() { return layers != 0; }
    void setLayer(int bit, boolean on) { layers = on ? layers | bit : layers & ~bit; }
    void setStatusView(TextView status) { poiStatus = status; }

    void refresh() {
        if (closed) return; snapshots.setOpen(layers != 0);
        String snapshot = snapshots.read();
        if (!snapshot.equals(lastPois)) { lastPois = snapshot; try { pois = new JSONObject(snapshot).optJSONArray("points"); } catch (Exception e) { pois = null; } if (pois == null) pois = new JSONArray(); invalidate.run(); }
        if (poiStatus != null) { String message = layers == 0 ? "Nearby layers off" : "Nearby locations loaded by the game (" + pois.length() + ")";
            if (layers != 0 && snapshot.isEmpty()) message = "Waiting for nearby game locations";
            else if (layers != 0) try { if (new JSONObject(snapshot).has("error")) message = "Nearby layers unavailable"; } catch (Exception ignored) {}
            poiStatus.setText(message);
        }
        invalidate.run(); main.removeCallbacks(poiRefresh); main.postDelayed(poiRefresh, 1000);
    }

    void draw(Canvas canvas) {
        if (layers == 0) return; float radius = dp(11); long now = System.currentTimeMillis();
        for (int i = 0; i < pois.length(); i++) { JSONObject point = pois.optJSONObject(i); if (point == null || !poiVisible(point, now)) continue;
            double lat = point.optDouble("lat", Double.NaN), lon = point.optDouble("lon", Double.NaN); if (!Double.isFinite(lat) || !Double.isFinite(lon) || Math.abs(lat) > 90 || Math.abs(lon) > 180) continue;
            float x = projector.screenX(lon), y = projector.screenY(lat);
            float extent = "stop".equals(point.optString("kind")) ? radius : dp(27);
            if (x < -radius - 3 || x > projector.width() + radius + 3 || y < -extent || y > projector.height() + radius + 3) continue;
            drawMarker(canvas, point, x, y, radius, now);
        }
    }

    /** Shared layer/time gate so only on-screen, currently drawn markers can be tapped. */
    private boolean poiVisible(JSONObject point, long now) {
        boolean stop = "stop".equals(point.optString("kind"));
        boolean showRaid = point.optLong("raidEnd") > now && (layers & 4) != 0;
        return stop ? (layers & 1) != 0 : ((layers & 2) != 0 || showRaid);
    }

    private void drawMarker(Canvas canvas, JSONObject point, float x, float y, float radius, long now) {
        boolean showRaid = point.optLong("raidEnd") > now && (layers & 4) != 0;
        boolean stop = "stop".equals(point.optString("kind"));
        paint.setStyle(Paint.Style.FILL);
        if (showRaid) {
            paint.setColor(Color.WHITE); canvas.drawCircle(x, y, radius + 3, paint);
            if (!point.optBoolean("egg")) { // a hatched raid shows a solid accent ring; an egg stays plain
                paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(Math.max(2, radius / 5f)); paint.setColor(Color.rgb(242, 110, 30));
                canvas.drawCircle(x, y, radius + 2, paint); paint.setStyle(Paint.Style.FILL);
            }
            drawIconOrFallback(canvas, loadIcon(raidIcon(point)), x, y, radius, point.optBoolean("egg") ? "E" : "R", Color.rgb(151, 65, 205));
            if (point.optBoolean("egg")) { paint.setColor(Color.rgb(65, 45, 85)); canvas.drawCircle(x + radius * .8f, y + radius * .8f, radius * .45f, paint); paint.setColor(Color.WHITE); paint.setTextAlign(Paint.Align.CENTER); paint.setTextSize(radius * .65f); canvas.drawText("E", x + radius * .8f, y + radius * 1.02f, paint); }
        } else if (stop) {
            Bitmap stopIcon = loadIcon("stop.png");
            if (stopIcon != null && stopIcon.getWidth() > 0 && stopIcon.getHeight() > 0) {
                // PokeMiners pokestop_near art, aspect-fit within the marker diameter and centred; no white backing or halo.
                float scale = Math.min(2 * radius / stopIcon.getWidth(), 2 * radius / stopIcon.getHeight());
                float halfW = stopIcon.getWidth() * scale / 2f, halfH = stopIcon.getHeight() * scale / 2f;
                canvas.drawBitmap(stopIcon, null, new RectF(x - halfW, y - halfH, x + halfW, y + halfH), paint);
            } else {
                // Fallback only if the asset can't be decoded: vector cyan disc with a thin white halo and white "S".
                paint.setColor(Color.WHITE); canvas.drawCircle(x, y, radius + 2, paint);
                paint.setColor(Color.rgb(20, 157, 208)); canvas.drawCircle(x, y, radius, paint);
                paint.setColor(Color.WHITE); paint.setTextAlign(Paint.Align.CENTER); paint.setTextSize(radius * 1.25f); canvas.drawText("S", x, y + radius * .43f, paint);
            }
        } else { // gym: a team-colour ring sits behind the disc
            paint.setColor(Color.WHITE); canvas.drawCircle(x, y, radius + 3, paint);
            paint.setColor(teamColor(point.optString("team", null))); canvas.drawCircle(x, y, radius + 1, paint);
            drawIconOrFallback(canvas, loadIcon("gym.png"), x, y, radius * 0.92f, "G", Color.rgb(223, 124, 35));
        }
        if (!stop) drawDefenderLabel(canvas, point, x, y, radius); // gyms and gym-hosted raids; never stops
    }

    /** "(6-slots)/6" defenders just below a gym/raid marker; invalid or missing slots show "--/6". */
    private void drawDefenderLabel(Canvas canvas, JSONObject point, float x, float y, float radius) {
        int slots = point.optInt("slots", -1);
        String text = (slots >= 0 && slots <= 6) ? ((6 - slots) + "/6") : "--/6"; // 0 slots open = full (6/6), 6 open = empty (0/6)
        float textSize = Math.max(dp(9), radius * 0.78f);
        labelFill.setTextSize(textSize); labelOutline.setTextSize(textSize); labelOutline.setStrokeWidth(Math.max(2f, textSize / 5f));
        float baseline = y + radius + dp(3) + textSize; // sits beneath the white marker halo, within the tap reach
        canvas.drawText(text, x, baseline, labelOutline);
        canvas.drawText(text, x, baseline, labelFill);
    }

    private void drawIconOrFallback(Canvas canvas, Bitmap icon, float x, float y, float radius, String letter, int fallbackColor) {
        if (icon != null) {
            RectF bounds = new RectF(x - radius, y - radius, x + radius, y + radius);
            canvas.drawBitmap(icon, null, bounds, paint); return;
        }
        paint.setStyle(Paint.Style.FILL); paint.setColor(fallbackColor); canvas.drawCircle(x, y, radius, paint);
        paint.setColor(Color.WHITE); paint.setTextAlign(Paint.Align.CENTER); paint.setTextSize(radius * 1.25f); canvas.drawText(letter, x, y + radius * .43f, paint);
    }

    /** Asset name for a raid cue, chosen from the game-resolved tier text (never the icon file name). */
    private static String raidIcon(JSONObject point) {
        String tier = point.optString("tier", ""); String n = tier.toLowerCase(Locale.US);
        boolean egg = point.optBoolean("egg");
        if (n.contains("shadow")) return egg ? "raid-shadow.png" : "raid-shadow-active.png";
        if (n.contains("primal")) return egg ? "raid-primal.png" : "raid-primal-active.png";
        if (n.contains("ultra")) return "raid-ultra.png";
        if (n.contains("mega")) return "raid-mega-active.png";
        if (!egg) return "raid-active.png";
        if (tier.matches("_?[1-5]")) {
            int level = tier.charAt(tier.length() - 1) - '0';
            return level >= 5 ? "raid-legendary.png" : level >= 3 ? "raid-rare.png" : "raid-normal.png";
        }
        return "raid-active.png"; // Unknown/special tier: generic cue, never guess egg rarity.
    }
    /** Team colour keyed on the game-resolved constant name, so no numeric team id is assumed. */
    private static int teamColor(String name) {
        if (name == null) return Color.rgb(150, 150, 150);
        String n = name.toLowerCase(Locale.US);
        if (n.contains("mystic") || n.contains("blue")) return Color.rgb(29, 120, 230);
        if (n.contains("valor") || n.contains("red")) return Color.rgb(221, 60, 60);
        if (n.contains("instinct") || n.contains("yellow")) return Color.rgb(240, 190, 30);
        return Color.rgb(150, 150, 150);
    }
    private Bitmap loadIcon(String name) {
        if (markerIcons.containsKey(name)) return markerIcons.get(name);
        Bitmap bitmap = null;
        try (InputStream in = context.getAssets().open("pgrazor/map-icons/" + name)) { bitmap = BitmapFactory.decodeStream(in); }
        catch (IOException | RuntimeException ignored) { bitmap = null; }
        markerIcons.put(name, bitmap); return bitmap;
    }

    /** Visible markers within the tap radius, nearest first; respects the active layers. */
    List<JSONObject> hits(float px, float py) {
        List<JSONObject> hits = new ArrayList<>(); final List<Double> distances = new ArrayList<>();
        long now = System.currentTimeMillis(); float reach = dp(18);
        for (int i = 0; i < pois.length(); i++) { JSONObject point = pois.optJSONObject(i); if (point == null || !poiVisible(point, now)) continue;
            double lat = point.optDouble("lat", Double.NaN), lon = point.optDouble("lon", Double.NaN); if (!Double.isFinite(lat) || !Double.isFinite(lon) || Math.abs(lat) > 90 || Math.abs(lon) > 180) continue;
            double dx = px - projector.screenX(lon), dy = py - projector.screenY(lat), distance = Math.hypot(dx, dy);
            boolean labelHit = !"stop".equals(point.optString("kind")) && Math.abs(dx) <= dp(16) && dy >= 0 && dy <= dp(27);
            if (distance <= reach || labelHit) { hits.add(point); distances.add(distance); }
        }
        List<Integer> order = new ArrayList<>(); for (int i = 0; i < hits.size(); i++) order.add(i);
        java.util.Collections.sort(order, (a, b) -> Double.compare(distances.get(a), distances.get(b)));
        List<JSONObject> sorted = new ArrayList<>(); for (int i : order) sorted.add(hits.get(i)); return sorted;
    }

    void openDetails(List<JSONObject> hits) {
        if (closed || hits.isEmpty()) return;
        if (hits.size() == 1) { showPoiDetails(hits.get(0)); return; }
        String[] labels = new String[hits.size()]; for (int i = 0; i < labels.length; i++) labels[i] = poiShortLabel(hits.get(i));
        AlertDialog chooser = new AlertDialog.Builder(dialogContext).setTitle("Nearby here (" + hits.size() + ")")
            .setItems(labels, (d, which) -> showPoiDetails(hits.get(which))).setNegativeButton("Close", null).create();
        registerPoiDialog(chooser); chooser.show();
    }
    private String poiShortLabel(JSONObject point) {
        long now = System.currentTimeMillis();
        if (point.optLong("raidEnd") > now) { String tier = point.optString("tier", ""); return "Raid" + (tier.isEmpty() ? "" : " " + tier) + (point.optBoolean("egg") ? " (egg)" : ""); }
        if ("stop".equals(point.optString("kind"))) return "PokéStop";
        String team = point.optString("team", ""); return "Gym" + (team.isEmpty() ? "" : " (" + team + ")");
    }
    private void registerPoiDialog(AlertDialog dialog) {
        poiDialogs.add(dialog); dialog.setOnDismissListener(d -> poiDialogs.remove(dialog));
    }
    private void showPoiDetails(JSONObject initial) {
        if (closed) return;
        final String id = initial.optString("id", "");
        final TextView body = new TextView(dialogContext);
        int pad = dp(18); body.setPadding(pad, pad, pad, pad); body.setTextSize(14);
        final double latitude = initial.optDouble("lat", Double.NaN), longitude = initial.optDouble("lon", Double.NaN);
        final boolean valid = Double.isFinite(latitude) && Double.isFinite(longitude) && Math.abs(latitude) <= 90 && Math.abs(longitude) <= 180;
        AlertDialog.Builder builder = new AlertDialog.Builder(dialogContext).setTitle(poiTitle(initial)).setView(body).setNegativeButton("Close", null);
        if (walk != null) builder.setNeutralButton("Walk", null);
        if (teleport != null) builder.setPositiveButton("Teleport", null);
        final AlertDialog dialog = builder.create();
        dialog.setOnShowListener(d -> {
            MapTheme.styleButtons(dialog);
            if (walk != null) {
                dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setEnabled(valid);
                dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener(v -> {
                    if (!closed && walk.onDestination(latitude, longitude)) dialog.dismiss();
                });
            }
            if (teleport != null) {
                dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(valid);
                dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
                    if (!closed && teleport.onDestination(latitude, longitude)) dialog.dismiss();
                });
            }
        });
        final Runnable[] refresh = new Runnable[1];
        refresh[0] = () -> {
            if (closed || !dialog.isShowing()) return;
            JSONObject live = id.isEmpty() ? null : findPoi(id);
            dialog.setTitle(poiTitle(live != null ? live : initial));
            body.setText(poiDetailsText(live != null ? live : initial, live == null));
            main.postDelayed(refresh[0], 1000);
        };
        dialog.setOnDismissListener(d -> { poiDialogs.remove(dialog); main.removeCallbacks(refresh[0]); });
        poiDialogs.add(dialog); dialog.show(); refresh[0].run();
    }
    private JSONObject findPoi(String id) {
        for (int i = 0; i < pois.length(); i++) { JSONObject point = pois.optJSONObject(i); if (point != null && id.equals(point.optString("id"))) return point; }
        return null;
    }
    private static String poiTitle(JSONObject point) {
        long now = System.currentTimeMillis();
        if (point.optLong("raidEnd") > now) return "Raid";
        return "stop".equals(point.optString("kind")) ? "PokéStop" : "Gym";
    }
    /** Read-only; never changes the selected destination. "Unknown"/"unavailable" stay explicit. */
    private String poiDetailsText(JSONObject p, boolean gone) {
        StringBuilder sb = new StringBuilder();
        double lat = p.optDouble("lat", Double.NaN), lon = p.optDouble("lon", Double.NaN);
        if (Double.isFinite(lat) && Double.isFinite(lon)) sb.append(String.format(Locale.US, "%.6f, %.6f\n\n", lat, lon));
        long now = System.currentTimeMillis();
        boolean stop = "stop".equals(p.optString("kind"));
        if (stop) sb.append("PokéStop loaded nearby by the game.\n");
        else {
            String team = p.optString("team", "");
            sb.append("Team: ").append(team.isEmpty() ? "Unknown" : team).append('\n');
            // Six defenders per gym: official Pokemon GO Help Center, Battling at Gyms (verified 2026-10-21).
            int slots = p.optInt("slots", -1);
            if (slots >= 0 && slots <= 6) sb.append("Defenders: ").append(6 - slots).append("/6 (" + slots + " slots open)\n");
            else sb.append("Defenders: unavailable\n");
        }
        if (p.optLong("raidEnd") > now) {
            sb.append('\n'); boolean egg = p.optBoolean("egg"); String tier = p.optString("tier", "");
            sb.append("Raid tier: ").append(tier.isEmpty() ? "Unknown" : tier.replace("_", " ").replaceAll("([0-9])([A-Za-z])", "$1 $2").trim()).append('\n');
            if (egg) {
                long start = p.optLong("raidStart", 0);
                sb.append("Egg — boss not yet revealed");
                if (start > now) sb.append("; hatches in ").append(countdown(start - now));
                sb.append('\n');
            } else {
                int boss = p.optInt("boss", 0);
                String name = boss > 0 ? bossName(boss) : null; // base species only; forms (e.g. Mega X/Y) not inferred
                sb.append("Boss: ").append(boss > 0 ? (name != null ? name : ("#" + boss)) : "unknown").append('\n');
            }
            long end = p.optLong("raidEnd", 0); if (end > now) sb.append("Ends in ").append(countdown(end - now)).append('\n');
        }
        if (p.optLong("raidEnd") > 0 && p.optLong("raidEnd") <= now) sb.append("\nRaid ended.\n");
        if (gone) sb.insert(0, "Last known data - no fresh nearby snapshot.\n\n");
        return sb.toString().trim();
    }
    private static String countdown(long millis) { long total = Math.max(0, millis) / 1000; return String.format(Locale.US, "%d:%02d", total / 60, total % 60); }

    /** English name for a base-species id, or null to fall back to #id; the table is read once. */
    private String bossName(int id) {
        if (!bossNamesLoaded) { bossNamesLoaded = true; bossNames = loadBossNames(); } // one asset read per map, not per refresh
        return bossNames == null ? null : bossNames.get(Integer.toString(id));
    }
    /** Parse the bundled id->name table once; any missing/oversize/malformed file leaves #id fallbacks. */
    private Map<String, String> loadBossNames() {
        try (InputStream in = context.getAssets().open("pgrazor/map-icons/pokemon-names.json")) {
            ByteArrayOutputStream out = new ByteArrayOutputStream(); byte[] buffer = new byte[8192]; int count;
            while ((count = in.read(buffer)) != -1) { if (out.size() + count > 2 * 1024 * 1024) throw new IOException("Oversize name table"); out.write(buffer, 0, count); }
            JSONObject json = new JSONObject(new String(out.toByteArray(), "UTF-8"));
            Map<String, String> map = new HashMap<>();
            for (Iterator<String> keys = json.keys(); keys.hasNext();) {
                String key = keys.next(); String value = json.optString(key, "");
                if (!value.isEmpty()) map.put(key, value);
            }
            return map;
        } catch (IOException | org.json.JSONException | RuntimeException e) { return null; }
    }

    void close() {
        if (closed) return;
        closed = true;
        main.removeCallbacksAndMessages(null);
        for (AlertDialog dialog : new ArrayList<>(poiDialogs)) dialog.dismiss();
        poiDialogs.clear();
        snapshots.setOpen(false);
    }

    private int dp(float value) { return Math.round(value * context.getResources().getDisplayMetrics().density); }
}
