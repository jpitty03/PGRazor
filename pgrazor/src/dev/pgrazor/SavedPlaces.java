package dev.pgrazor;

import android.content.Context;
import android.content.SharedPreferences;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.UUID;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/** Favorites: a small app-private destination library. Call persistence methods off the UI thread. */
public final class SavedPlaces {
    public static final int LIMIT = 50;
    public static final int NAME_LIMIT = 40;
    private static final int JSON_LIMIT = 64 * 1024;
    private final SharedPreferences preferences;

    public static final class Entry {
        public final String id, name;
        public final double latitude, longitude;
        public Entry(String name, double latitude, double longitude) {
            this(UUID.randomUUID().toString(), name, latitude, longitude);
        }
        private Entry(String id, String name, double latitude, double longitude) {
            if (id == null || id.length() == 0 || id.length() > 64) throw new IllegalArgumentException("Invalid favorite ID");
            if (name == null || name.trim().length() == 0) throw new IllegalArgumentException("Enter a name for this place.");
            name = name.trim();
            if (name.length() > NAME_LIMIT) throw new IllegalArgumentException("Names can contain at most 40 characters.");
            for (int i = 0; i < name.length(); i++) if (Character.isISOControl(name.charAt(i))) throw new IllegalArgumentException("Use a name without line breaks or control characters.");
            if (!Double.isFinite(latitude) || !Double.isFinite(longitude) || latitude < -90 || latitude > 90 || longitude < -180 || longitude > 180)
                throw new IllegalArgumentException("Coordinates are outside the valid range.");
            this.id = id; this.name = name; this.latitude = latitude; this.longitude = longitude;
        }
    }
    public static final class Snapshot {
        public final List<Entry> entries;
        public final boolean damaged;
        Snapshot(List<Entry> entries, boolean damaged) { this.entries = Collections.unmodifiableList(entries); this.damaged = damaged; }
    }

    public SavedPlaces(Context context) { preferences = AccountProfiles.preferences(context.getApplicationContext(),"pgrazor-saved-places-v1"); }

    public synchronized Snapshot read() {
        List<Entry> entries = new ArrayList<>();
        boolean damaged = false;
        try {
            String raw = preferences.getString("places", "[]");
            if (raw == null || raw.length() > JSON_LIMIT) return new Snapshot(entries, true);
            JSONArray array = new JSONArray(raw);
            Set<String> names = new HashSet<>(), ids = new HashSet<>();
            if (array.length() > LIMIT) damaged = true;
            // Inspect at most50 records even when a corrupt document advertises a larger collection.
            for (int i = 0; i < Math.min(array.length(), LIMIT); i++) {
                try {
                    JSONObject object = array.getJSONObject(i);
                    Entry entry = new Entry(object.getString("id"), object.getString("name"), object.getDouble("latitude"), object.getDouble("longitude"));
                    String normalized = entry.name.toLowerCase(Locale.ROOT);
                    if (ids.contains(entry.id) || names.contains(normalized)) { damaged = true; continue; }
                    ids.add(entry.id); names.add(normalized); entries.add(entry);
                } catch (JSONException | IllegalArgumentException e) { damaged = true; }
            }
        } catch (JSONException | ClassCastException e) { damaged = true; }
        return new Snapshot(entries, damaged);
    }

    public synchronized void add(Entry entry) throws IOException {
        List<Entry> entries = new ArrayList<>(read().entries);
        if (entries.size() >= LIMIT) throw new IllegalArgumentException("You have 50 favorites. Delete one before adding another.");
        for (Entry existing : entries) if (existing.name.equalsIgnoreCase(entry.name)) throw new IllegalArgumentException("A place with this name already exists. Choose another name.");
        entries.add(entry);
        write(entries);
    }

    public synchronized boolean remove(String id) throws IOException {
        List<Entry> entries = new ArrayList<>(read().entries);
        boolean removed = false;
        for (int i = entries.size() - 1; i >= 0; i--) if (entries.get(i).id.equals(id)) { entries.remove(i); removed = true; }
        if (removed) write(entries);
        return removed;
    }

    /** Renames a place, keeping its position in the list; false when it no longer exists. */
    public synchronized boolean rename(String id, String name) throws IOException {
        List<Entry> entries = new ArrayList<>(read().entries);
        for (int i = 0; i < entries.size(); i++) {
            Entry old = entries.get(i);
            if (!old.id.equals(id)) continue;
            Entry renamed = new Entry(old.id, name, old.latitude, old.longitude);
            for (Entry other : entries)
                if (!other.id.equals(id) && other.name.equalsIgnoreCase(renamed.name))
                    throw new IllegalArgumentException("A place with this name already exists. Choose another name.");
            entries.set(i, renamed);
            write(entries);
            return true;
        }
        return false;
    }

    private void write(List<Entry> entries) throws IOException {
        JSONArray array = new JSONArray();
        try {
            for (Entry entry : entries) {
                JSONObject object = new JSONObject();
                object.put("id", entry.id); object.put("name", entry.name); object.put("latitude", entry.latitude); object.put("longitude", entry.longitude);
                array.put(object);
            }
        } catch (JSONException e) { throw new IOException("Could not encode favorites.", e); }
        String encoded = array.toString();
        if (encoded.length() > JSON_LIMIT) throw new IOException("Favorites exceed the storage limit.");
        SharedPreferences.Editor editor = preferences.edit();
        // Preserve one bounded copy of a damaged document before replacing it with salvaged valid entries.
        if (read().damaged) {
            try {
                String raw = preferences.getString("places", "[]");
                if (raw != null && raw.length() <= JSON_LIMIT) editor.putString("recovered-from", raw);
            } catch (ClassCastException ignored) { }
        }
        if (!editor.putString("places", encoded).commit()) throw new IOException("Could not save places to device storage. Please retry.");
    }
}
