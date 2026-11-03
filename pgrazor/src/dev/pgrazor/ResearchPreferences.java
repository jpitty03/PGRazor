package dev.pgrazor;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Properties;
import java.util.TreeSet;

/**
 * Pure Java field research preferences and native wire serialization. Tasks are identified by the
 * game's exact quest template key; the catalog lists the field research tasks the game has handed
 * out while PGRazor was running, with the game's own task text and reward.
 */
public final class ResearchPreferences {
    static final int MAX_TEMPLATES = 300, MAX_TEMPLATE = 96, MAX_TEXT = 160;
    private boolean claim, remove, catchEncounters;
    private final TreeSet<String> selected = new TreeSet<>();
    /** Catalog categories, in display order: item reward, Pokemon encounter reward, game-tagged task. */
    public static final String ITEM = "i", ENCOUNTER = "e", EVENT = "v";
    public static final String[] CATEGORIES = {ITEM, ENCOUNTER, EVENT};
    // template -> {task, reward, category}; insertion order, so the oldest unselected entry is dropped first.
    private final LinkedHashMap<String, String[]> catalog = new LinkedHashMap<>();

    public static boolean validTemplate(String t) {
        if (t == null || t.isEmpty() || t.length() > MAX_TEMPLATE) return false;
        for (int i = 0; i < t.length(); i++) {
            char c = t.charAt(i);
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
        }
        return true;
    }
    private static String clean(String s) {
        if (s == null) return "";
        String out = s.replace('\t', ' ').replace('\n', ' ').replace('\r', ' ').trim();
        return out.length() > MAX_TEXT ? out.substring(0, MAX_TEXT) : out;
    }
    /**
     * The native category, or for an older catalog entry without one, read from its reward line:
     * a game tag after " · " other than Sponsored is an event, then the reward's kind.
     */
    private static String category(String c, String reward) {
        if (ITEM.equals(c) || ENCOUNTER.equals(c) || EVENT.equals(c)) return c;
        int tag = reward.lastIndexOf(" · ");
        if (tag >= 0 && !reward.substring(tag).toLowerCase(Locale.US).contains("sponsor")) return EVENT;
        return reward.contains("ncounter") ? ENCOUNTER : ITEM;
    }

    public synchronized void read(Properties p) {
        claim = "true".equals(p.getProperty("researchClaim"));
        remove = "true".equals(p.getProperty("researchRemove"));
        catchEncounters = "true".equals(p.getProperty("researchCatch"));
        selected.clear(); catalog.clear();
        for (String t : p.getProperty("researchSelected", "").split(",", -1))
            if (validTemplate(t) && selected.size() < MAX_TEMPLATES) selected.add(t);
        // Categories saved before format 2 were derived without the event tag, so they are re-read.
        boolean categorized = "2".equals(p.getProperty("researchCatalogFormat"));
        for (String line : p.getProperty("researchCatalog", "").split("\n", -1)) {
            String[] parts = line.split("\t", -1);
            if ((parts.length == 3 || parts.length == 4) && validTemplate(parts[0]) && catalog.size() < MAX_TEMPLATES)
                catalog.put(parts[0], new String[]{clean(parts[1]), clean(parts[2]),
                    category(categorized && parts.length == 4 ? parts[3] : "", clean(parts[2]))});
        }
    }
    public synchronized void write(Properties p) {
        p.setProperty("researchClaim", String.valueOf(claim));
        p.setProperty("researchRemove", String.valueOf(remove));
        p.setProperty("researchCatch", String.valueOf(catchEncounters));
        p.setProperty("researchSelected", String.join(",", selected));
        StringBuilder out = new StringBuilder();
        for (Map.Entry<String, String[]> e : catalog.entrySet()) {
            if (out.length() > 0) out.append('\n');
            out.append(e.getKey()).append('\t').append(e.getValue()[0]).append('\t').append(e.getValue()[1])
                .append('\t').append(e.getValue()[2]);
        }
        p.setProperty("researchCatalog", out.toString());
        p.setProperty("researchCatalogFormat", "2");
    }
    /** {@code R2|remove|claim|catch|T1,T2,...}: selected templates are sent only while removal is on. */
    public synchronized String config() {
        return "R2|" + (remove ? 1 : 0) + "|" + (claim ? 1 : 0) + "|" + (catchEncounters ? 1 : 0) + "|"
            + (remove ? String.join(",", selected) : "");
    }

    public synchronized boolean claim() { return claim; }
    public synchronized boolean remove() { return remove; }
    public synchronized void setClaim(boolean on) { claim = on; }
    public synchronized void setRemove(boolean on) { remove = on; }
    public synchronized boolean catchEncounters() { return catchEncounters; }
    public synchronized void setCatchEncounters(boolean on) { catchEncounters = on; }
    public synchronized boolean selected(String template) { return selected.contains(template); }
    public synchronized int selectedCount() { return selected.size(); }
    public synchronized void select(String template, boolean on) {
        if (!validTemplate(template)) throw new IllegalArgumentException("Unknown research task.");
        if (!on) { selected.remove(template); return; }
        if (!selected.contains(template) && selected.size() >= MAX_TEMPLATES)
            throw new IllegalArgumentException("Select at most " + MAX_TEMPLATES + " tasks.");
        selected.add(template);
    }
    /** Selects or clears every listed template, or none of them when the selection would overflow. */
    public synchronized void selectAll(List<String> templates, boolean on) {
        for (String t : templates) if (!validTemplate(t)) throw new IllegalArgumentException("Unknown research task.");
        if (!on) { selected.removeAll(templates); return; }
        TreeSet<String> next = new TreeSet<>(selected);
        next.addAll(templates);
        if (next.size() > MAX_TEMPLATES) throw new IllegalArgumentException("Select at most " + MAX_TEMPLATES + " tasks.");
        selected.addAll(templates);
    }
    /** Adds or refreshes one observed task. Returns true when the stored catalog changed. */
    public synchronized boolean observe(String template, String task, String reward, String category) {
        if (!validTemplate(template)) return false;
        String[] entry = {clean(task).isEmpty() ? template : clean(task), clean(reward), category(category, clean(reward))};
        String[] old = catalog.get(template);
        if (old != null && java.util.Arrays.equals(old, entry)) return false;
        if (old == null && catalog.size() >= MAX_TEMPLATES) {
            String evict = null;
            for (String key : catalog.keySet()) if (!selected.contains(key)) { evict = key; break; }
            if (evict == null) return false;
            catalog.remove(evict);
        }
        catalog.put(template, entry);
        return true;
    }
    /** Catalog rows {template, task, reward, category} matching the query, sorted by task text. */
    public synchronized List<String[]> tasks(String query) {
        String q = query == null ? "" : query.trim().toLowerCase(Locale.US);
        List<String[]> out = new ArrayList<>();
        for (Map.Entry<String, String[]> e : catalog.entrySet()) {
            String[] v = e.getValue();
            String haystack = (v[0] + " " + v[1] + " " + e.getKey()).toLowerCase(Locale.US);
            if (q.isEmpty() || haystack.contains(q)) out.add(new String[]{e.getKey(), v[0], v[1], v[2]});
        }
        out.sort((a, b) -> a[1].compareToIgnoreCase(b[1]) != 0 ? a[1].compareToIgnoreCase(b[1]) : a[0].compareTo(b[0]));
        return out;
    }
}
