package dev.pgrazor;

import java.text.Normalizer;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.regex.Pattern;

/**
 * The settings search catalogue: pure Java, no Android types, so it runs under the host JVM test
 * suite. The panel registers a view target for each entry; this class owns only the text and the
 * match order.
 *
 * <p>Every entry keeps one pre-normalised haystack (name, description, category, group and
 * synonyms), so a keystroke is a substring scan over a small list rather than a view walk. Matching
 * is case-insensitive, accent-insensitive ({@code pokemon} finds {@code Pokémon}) and requires every
 * query word to appear, which keeps multi-word searches precise.
 */
final class SettingsSearch {
    static final class Entry {
        final int id;
        final String name, description, category, group;
        final int page;
        private final String normalizedName, normalizedDescription, haystack;

        private Entry(int id, String name, String description, String category, String group, int page) {
            this.id = id; this.name = name; this.description = description;
            this.category = category; this.group = group; this.page = page;
            this.normalizedName = normalize(name);
            this.normalizedDescription = normalize(description);
            this.haystack = this.normalizedName + ' ' + this.normalizedDescription + ' '
                + normalize(category) + ' ' + normalize(group) + ' ' + normalize(synonyms(name));
        }
    }

    private final List<Entry> entries = new ArrayList<>();
    private static final Pattern MARKS = Pattern.compile("\\p{M}+");
    private static final Pattern SPACES = Pattern.compile("\\s+");
    private static final Pattern COUNT = Pattern.compile("\\s*\\(\\d+\\)$");

    int add(String name, String group, String category, int page) {
        int id = entries.size();
        entries.add(new Entry(id, name, describe(name, group, category), category, group, page));
        return id;
    }

    int size() { return entries.size(); }

    /** All matches in display order: exact name, name prefix, name substring, then text matches. */
    List<Entry> find(String query) {
        String wanted = normalize(query);
        if (wanted.isEmpty() || entries.isEmpty()) return Collections.emptyList();
        String[] tokens = wanted.split(" ");
        ArrayList<Entry> hits = new ArrayList<>();
        for (int i = 0; i < entries.size(); i++) {
            Entry entry = entries.get(i);
            boolean all = true;
            for (int t = 0; t < tokens.length; t++) {
                if (!entry.haystack.contains(tokens[t])) { all = false; break; }
            }
            if (all) hits.add(entry);
        }
        final String whole = wanted;
        Collections.sort(hits, new Comparator<Entry>() {
            public int compare(Entry a, Entry b) {
                int rank = rank(a, whole) - rank(b, whole);
                if (rank != 0) return rank;
                int length = a.normalizedName.length() - b.normalizedName.length();
                if (length != 0) return length;
                return a.normalizedName.compareTo(b.normalizedName);
            }
        });
        return hits;
    }

    private static int rank(Entry entry, String query) {
        if (entry.normalizedName.equals(query)) return 0;
        if (entry.normalizedName.startsWith(query)) return 1;
        if (entry.normalizedName.contains(query)) return 2;
        if (entry.normalizedDescription.startsWith(query)) return 3;
        if (entry.normalizedDescription.contains(query)) return 4;
        return 5;
    }

    /** Lowercase, strip accents and collapse spaces; {@code Pokémon} and {@code pokemon} compare equal. */
    static String normalize(String value) {
        if (value == null || value.isEmpty()) return "";
        String lower = value.toLowerCase(Locale.ROOT).trim();
        String folded = Normalizer.normalize(lower, Normalizer.Form.NFD);
        return SPACES.matcher(MARKS.matcher(folded).replaceAll("")).replaceAll(" ");
    }

    /** The setting name without its current value: {@code "Throw rating: Great"} becomes {@code "Throw rating"}. */
    static String cleanName(String raw) {
        if (raw == null) return "";
        String name = SPACES.matcher(raw.trim()).replaceAll(" ");
        int split = name.indexOf(": ");
        if (split > 0) name = name.substring(0, split);
        name = COUNT.matcher(name).replaceAll("");
        String renamed = RENAMES.get(normalize(name));
        return renamed == null ? name : renamed;
    }

    private static String describe(String name, String group, String category) {
        String text = DESCRIPTIONS.get(normalize(name));
        if (text != null) return text;
        if ("quick menu entries".equals(normalize(group))) return "Show " + name + " in the quick menu.";
        if (group != null && !group.isEmpty()) return group + " option.";
        return category;
    }

    private static String synonyms(String name) {
        String value = SYNONYMS.get(normalize(name));
        return value == null ? "" : value;
    }

    private static final HashMap<String, String> DESCRIPTIONS = new HashMap<>();
    private static final HashMap<String, String> RENAMES = new HashMap<>();
    private static final HashMap<String, String> SYNONYMS = new HashMap<>();
    private static void put(HashMap<String, String> map, String key, String value) { map.put(normalize(key), value); }

    static {
        put(RENAMES, "Evolve", "Evolve stages");
        put(RENAMES, "Applies to", "Evolve applies to");
        put(RENAMES, "Selected species", "Evolve species");
        put(RENAMES, "Encounter", "Encounter IV display");
        put(RENAMES, "Pokémon stats screen", "Pokémon stats IV display");
        put(RENAMES, "Pokémon storage list", "Storage list IV display");
        put(RENAMES, "Heal", "Heal timing");
        put(RENAMES, "Heal when", "Heal threshold");
        put(RENAMES, "Items", "Healing items");
        put(RENAMES, "Priority", "Incubator priority");
        put(RENAMES, "VGP+", "VGP+ mode");

        put(DESCRIPTIONS, "Location spoofing", "Use the spoofed location instead of the device GPS.");
        put(DESCRIPTIONS, "Coordinates & speed", "Set the latitude, longitude and walking speed.");
        put(DESCRIPTIONS, "World map", "Pick a destination, walk route or nearby point on the map.");
        put(DESCRIPTIONS, "Show joystick", "Show the on-screen movement joystick.");
        put(DESCRIPTIONS, "Idle walking", "Walk in a circle when the joystick is idle. Speed 0 pauses it.");
        put(DESCRIPTIONS, "Game map tap", "Choose what a single or double tap on the game map does.");
        put(DESCRIPTIONS, "Spawn booster (80 m)", "Load nearby Pokémon sooner. Can cause lag in busy areas.");
        put(DESCRIPTIONS, "Cooldown overlay", "Show the estimated catch cooldown timer on the map.");
        put(DESCRIPTIONS, "Cooldown history", "Review recent cooldown actions and locations.");
        put(DESCRIPTIONS, "Show local time", "Show the wall-clock time at the spoofed coordinates.");
        put(DESCRIPTIONS, "Count encounters", "Count wild encounters for this account in a rolling 24 hours.");
        put(DESCRIPTIONS, "Show the count on the map", "Show the encounter count next to the PGRazor title.");
        put(DESCRIPTIONS, "Quick catch", "Speed up or skip the catch summary after a throw.");
        put(DESCRIPTIONS, "Faster catch summary", "Speed up or skip the catch summary after a throw.");
        put(DESCRIPTIONS, "Throw rating", "Set the throw quality used for automatic throws.");
        put(DESCRIPTIONS, "Throw variety", "Pick a random throw quality for each throw.");
        put(DESCRIPTIONS, "Always curveball", "Curve every automatic throw.");
        put(DESCRIPTIONS, "Always hit", "Make automatic throws always land.");
        put(DESCRIPTIONS, "AR+ throw bonus", "Apply the AR+ throw bonus to automatic throws.");
        put(DESCRIPTIONS, "Show catch chance", "Show the catch chance during an encounter.");
        put(DESCRIPTIONS, "Auto ball", "Pick the cheapest ball that reaches your target catch chance.");
        put(DESCRIPTIONS, "Auto ball target", "Catch chance target used by Auto ball.");
        put(DESCRIPTIONS, "Auto throw", "Throw automatically using your ball, berry and throw settings.");
        put(DESCRIPTIONS, "Catch on map (tap Pokémon)", "Catch a tapped wild, lure or incense Pokémon without opening it.");
        put(DESCRIPTIONS, "Catch berries & overlay", "Use berries automatically and show the berry picker overlay.");
        put(DESCRIPTIONS, "Berry priority", "Choose the berry order for automatic catching.");
        put(DESCRIPTIONS, "Transfer below 90% IV", "Transfer new catches below the IV threshold after the catch summary.");
        put(DESCRIPTIONS, "Custom keep list", "Keep specific species, including all forms, from auto-transfer.");
        put(DESCRIPTIONS, "Keep-all overlay button", "Show a floating Keep All button that holds every catch.");
        put(DESCRIPTIONS, "Keep Great League PvP IVs", "Keep catches that rank well in Great League.");
        put(DESCRIPTIONS, "Keep Ultra League PvP IVs", "Keep catches that rank well in Ultra League.");
        put(DESCRIPTIONS, "Keep Master League PvP IVs", "Keep catches that rank well in Master League.");
        put(DESCRIPTIONS, "Evolve before transferring", "Evolve new catches before applying transfer rules.");
        put(DESCRIPTIONS, "Evolve stages", "Try one or all available evolution stages.");
        put(DESCRIPTIONS, "Evolve applies to", "Apply evolution to selected species only or to every transfer.");
        put(DESCRIPTIONS, "Evolve species", "Choose the species to evolve before transferring.");
        put(DESCRIPTIONS, "Encounter IV display", "Show level, IVs and PvP rank on the catch screen.");
        put(DESCRIPTIONS, "Pokémon stats IV display", "Show IV details on the Pokémon stats screen.");
        put(DESCRIPTIONS, "Storage list IV display", "Show IV details in the Pokémon storage list.");
        put(DESCRIPTIONS, "Mass Evolve", "Evolve many Pokémon in one run.");
        put(DESCRIPTIONS, "VGP overlay", "Show the Virtual Go Plus button and status on the map.");
        put(DESCRIPTIONS, "VGP+ mode", "Extended Virtual Go Plus ball, berry and throw control.");
        put(DESCRIPTIONS, "VGP+ auto ball", "Pick a ball by catch chance for VGP+.");
        put(DESCRIPTIONS, "VGP+ auto ball target", "Catch chance target used by VGP+ auto ball.");
        put(DESCRIPTIONS, "Standard ball", "Ball used by VGP+ when auto ball is off.");
        put(DESCRIPTIONS, "Standard berry", "Berry used by VGP+ when automatic berries are on.");
        put(DESCRIPTIONS, "High CP threshold", "Use the premium ball and berry above this CP.");
        put(DESCRIPTIONS, "High level threshold", "Use the premium ball and berry above this level.");
        put(DESCRIPTIONS, "Curveball", "Curve VGP+ throws.");
        put(DESCRIPTIONS, "Catch speed", "How long VGP+ waits before each throw and scan.");
        put(DESCRIPTIONS, "Auto-spin Gym discs", "Spin nearby Gym discs while VGP is running.");
        put(DESCRIPTIONS, "Candy species (Pinap)", "Species that receive Pinap berries.");
        put(DESCRIPTIONS, "Catch only shinies", "Open every Pokémon but throw only at shinies.");
        put(DESCRIPTIONS, "Catch only", "Catch only the selected species.");
        put(DESCRIPTIONS, "Nearby Pokémon", "Let Virtual Go Plus catch nearby Pokémon.");
        put(DESCRIPTIONS, "Nearby PokéStops", "Let Virtual Go Plus spin nearby PokéStops.");
        put(DESCRIPTIONS, "Push notifications", "Notify when Virtual Go Plus acts.");
        put(DESCRIPTIONS, "Stop on cooldown (estimated)", "Pause Virtual Go Plus during the estimated cooldown.");
        put(DESCRIPTIONS, "Quick spin", "Spin PokéStops automatically.");
        put(DESCRIPTIONS, "Auto-transfer items", "Trim inventory items down to limits you choose.");
        put(DESCRIPTIONS, "Smart battling", "Fill teams with counters and swap to the best counter.");
        put(DESCRIPTIONS, "Smart healing", "Heal and revive automatically between battles.");
        put(DESCRIPTIONS, "Heal timing", "Choose when Smart healing runs.");
        put(DESCRIPTIONS, "Heal threshold", "Choose how much HP missing starts healing.");
        put(DESCRIPTIONS, "Healing items", "Use smart potion choices or heal fully.");
        put(DESCRIPTIONS, "Revive fainted Pokémon", "Revive fainted Pokémon during Smart healing.");
        put(DESCRIPTIONS, "Gym Slotter & Berry Feeder", "Place defenders and feed gyms automatically.");
        put(DESCRIPTIONS, "Battle until defeated", "Repeat a gym battle until the gym is defeated.");
        put(DESCRIPTIONS, "Remote Gym Feeding", "Feed the gyms you defend once with surplus berries.");
        put(DESCRIPTIONS, "Auto-Raid", "Find and complete nearby raids.");
        put(DESCRIPTIONS, "Auto quick attack", "Use fast attacks automatically in battles.");
        put(DESCRIPTIONS, "Auto charged attack", "Use charged attacks automatically in battles.");
        put(DESCRIPTIONS, "Auto Max attack", "Use Max attacks automatically in Dynamax battles.");
        put(DESCRIPTIONS, "Auto dodge", "Dodge automatically in battles.");
        put(DESCRIPTIONS, "Auto shield", "Use shields automatically in Team Rocket battles.");
        put(DESCRIPTIONS, "Auto incubate", "Hatch eggs with the unlimited incubator.");
        put(DESCRIPTIONS, "Incubator priority", "Hatch the shortest or longest distance egg first.");
        put(DESCRIPTIONS, "Auto open gifts", "Open friend gifts automatically.");
        put(DESCRIPTIONS, "Auto send gifts", "Send friend gifts automatically.");
        put(DESCRIPTIONS, "Add an owned sticker", "Add an owned sticker when sending gifts.");
        put(DESCRIPTIONS, "Send & Open Gifts", "Open and send friend gifts in one batch.");
        put(DESCRIPTIONS, "Run the Buddy Emotion Engine", "Build buddy mood with snapshots and petting.");
        put(DESCRIPTIONS, "Open gifts your buddy brings back", "Open gifts returned by your buddy.");
        put(DESCRIPTIONS, "Show EP overlay", "Show the buddy excitement status overlay.");
        put(DESCRIPTIONS, "Feeding priority", "Choose the berry order used by the buddy engine.");
        put(DESCRIPTIONS, "Reserve per berry", "Keep this many of each berry before feeding.");
        put(DESCRIPTIONS, "Shundo Sniper (Experimental)", "Teleport to nearby spawns looking for a shundo.");
        put(DESCRIPTIONS, "Shiny Scanner (Experimental)", "Check nearby wild Pokémon for shinies without catching.");
        put(DESCRIPTIONS, "Daily Incense Session", "Walk a 15-minute incense session at 10.5 km/h.");
        put(DESCRIPTIONS, "Research", "Auto-claim, delete and catch field research.");
        put(DESCRIPTIONS, "Halloween 2026", "Event preferences for Halloween 2026.");
        put(DESCRIPTIONS, "Go Wild November 2026", "Event preferences for GO Wild Area 2026.");
        put(DESCRIPTIONS, "Dark theme", "Switch the whole interface between the dark and light palette.");
        put(DESCRIPTIONS, "UI scale", "Resize the joystick and map overlays.");
        put(DESCRIPTIONS, "Skip cutscenes", "Skip repeatable cutscenes and hatch animations.");
        put(DESCRIPTIONS, "Lower resolution", "Draw the map and Pokémon at half resolution.");
        put(DESCRIPTIONS, "Limit to 20 FPS", "Cap the game at 20 frames per second.");
        put(DESCRIPTIONS, "Low graphics quality", "Turn off shadows, fog, reflections and other heavy effects.");
        put(DESCRIPTIONS, "Accounts", "Switch, add or manage game accounts.");
        put(DESCRIPTIONS, "Export / import settings", "Back up or restore every saved setting.");
        put(DESCRIPTIONS, "Run log / export", "Open the run log and export it as text.");
        put(DESCRIPTIONS, "Diagnostics", "Camera and catch or spin capture diagnostics.");

        put(DESCRIPTIONS, "Keep legendary / mythical / Ultra Beasts", "Never auto-transfer legendary, mythical or Ultra Beast Pokémon.");
        put(DESCRIPTIONS, "Keep shiny Pokemon", "Never auto-transfer shiny Pokémon.");
        put(DESCRIPTIONS, "Keep event Pokemon", "Never auto-transfer Pokémon caught during an event.");
        put(DESCRIPTIONS, "Keep background Pokemon", "Never auto-transfer Pokémon with a special background.");
        put(DESCRIPTIONS, "Keep XXS", "Never auto-transfer XXS Pokémon.");
        put(DESCRIPTIONS, "Keep XXL", "Never auto-transfer XXL Pokémon.");
        put(DESCRIPTIONS, "Keep 0% IV (0/0/0)", "Never auto-transfer a 0% IV Pokémon.");
        put(DESCRIPTIONS, "Keep costume Pokemon", "Never auto-transfer costume Pokémon.");

        put(SYNONYMS, "Location spoofing", "fake gps mock location teleport");
        put(SYNONYMS, "Coordinates & speed", "lat lon latitude longitude kmh walking speed");
        put(SYNONYMS, "World map", "map place search pin route");
        put(SYNONYMS, "Auto ball", "ball pokeball great ultra catch rate");
        put(SYNONYMS, "Auto throw", "throw catch automatic");
        put(SYNONYMS, "Transfer below 90% IV", "transfer iv appraise candy");
        put(SYNONYMS, "Custom keep list", "keep species list transfer");
        put(SYNONYMS, "Keep Great League PvP IVs", "pvp rank great league");
        put(SYNONYMS, "Keep Ultra League PvP IVs", "pvp rank ultra league");
        put(SYNONYMS, "Keep Master League PvP IVs", "pvp rank master league");
        put(SYNONYMS, "Encounter IV display", "iv overlay catch screen stats");
        put(SYNONYMS, "Pokémon stats IV display", "iv overlay stats screen");
        put(SYNONYMS, "Storage list IV display", "iv overlay storage list");
        put(SYNONYMS, "VGP overlay", "go plus virtual vgp button");
        put(SYNONYMS, "Quick spin", "pokestop spin stop");
        put(SYNONYMS, "Smart healing", "heal potion revive");
        put(SYNONYMS, "Auto incubate", "egg incubator hatch");
        put(SYNONYMS, "Dark theme", "light mode appearance");
        put(SYNONYMS, "UI scale", "size resize scale overlays");
        put(SYNONYMS, "Limit to 20 FPS", "fps frames battery performance");
        put(SYNONYMS, "Lower resolution", "resolution battery performance graphics");
        put(SYNONYMS, "Low graphics quality", "graphics quality battery performance shadows");
        put(SYNONYMS, "Export / import settings", "backup restore settings");
        put(SYNONYMS, "Run log / export", "log txt diagnostics export");
        put(SYNONYMS, "Shiny Scanner (Experimental)", "shiny scan scanner notification");
        put(SYNONYMS, "Shundo Sniper (Experimental)", "shundo sniper teleport shiny");
    }
}
