package dev.pgrazor;

import java.util.List;

/** Host-JVM checks for the settings search catalogue. No Android runtime is involved. */
public final class SettingsSearchTest {
    public static void main(String[] args) {
        normalization();
        cleanNames();
        ranking();
        matching();
        synonyms();
        emptyQueries();
        System.out.println("SettingsSearch: normalization, cleaning, ranking, synonyms, multi-token, category and empty states pass");
    }

    private static void normalization() {
        eq("pokemon go", SettingsSearch.normalize("  Pokémon GO  "));
        eq("pokemon", SettingsSearch.normalize("POKÉMON"));
        eq("favorites", SettingsSearch.normalize("Favorites"));
        eq("", SettingsSearch.normalize(null));
        eq("", SettingsSearch.normalize("   "));
    }

    private static void cleanNames() {
        eq("Throw rating", SettingsSearch.cleanName("Throw rating: Great"));
        eq("Evolve stages", SettingsSearch.cleanName("Evolve: all available stages"));
        eq("Evolve applies to", SettingsSearch.cleanName("Applies to: current transfer rules"));
        eq("Evolve species", SettingsSearch.cleanName("Selected species (3)"));
        eq("Auto ball target", SettingsSearch.cleanName("  Auto ball target: 90%  "));
        eq("", SettingsSearch.cleanName(null));
    }

    private static void ranking() {
        SettingsSearch search = new SettingsSearch();
        int first = search.add("Auto ball target", "Catching", "Catching", 1);
        int exact = search.add("Auto ball", "Catching", "Catching", 1);
        search.add("Auto throw", "Catching", "Catching", 1);
        eq("0", Integer.toString(first));
        eq("1", Integer.toString(exact));
        eq("3", Integer.toString(search.size()));

        List<SettingsSearch.Entry> hits = search.find("auto ball");
        eq("3", Integer.toString(hits.size()));
        eq("Auto ball", hits.get(0).name);
        eq("Auto ball target", hits.get(1).name);
        // Description matches rank after every name match, so a broader row stays last.
        eq("Auto throw", hits.get(2).name);
        eq("1", Integer.toString(hits.get(0).page));

        // A token that is not in a name still matches its description or synonyms.
        List<SettingsSearch.Entry> descriptionHits = search.find("cheapest ball");
        eq("1", Integer.toString(descriptionHits.size()));
        eq("Auto ball", descriptionHits.get(0).name);
    }

    private static void matching() {
        SettingsSearch catalogue = new SettingsSearch();
        catalogue.add("Dark theme", "Interface", "Display & Performance", 4);
        catalogue.add("Location spoofing", "Location & movement", "Location & Map", 0);
        catalogue.add("Show joystick", "Location & movement", "Location & Map", 0);

        List<SettingsSearch.Entry> theme = catalogue.find("DARK");
        eq("1", Integer.toString(theme.size()));
        eq("Dark theme", theme.get(0).name);

        // Category text is searchable.
        List<SettingsSearch.Entry> performance = catalogue.find("performance");
        eq("1", Integer.toString(performance.size()));
        eq("Dark theme", performance.get(0).name);

        // Group text is searchable, and every token must match.
        List<SettingsSearch.Entry> movement = catalogue.find("location movement");
        eq("2", Integer.toString(movement.size()));
        List<SettingsSearch.Entry> joystick = catalogue.find("movement joystick");
        eq("1", Integer.toString(joystick.size()));
        eq("Show joystick", joystick.get(0).name);

        // Case and accents fold for every surface, including names.
        SettingsSearch accented = new SettingsSearch();
        accented.add("Pokémon stats IV display", "Display", "Catching", 2);
        eq("1", Integer.toString(accented.find("POKEMON").size()));
        eq("1", Integer.toString(accented.find("pokémon").size()));
    }

    private static void synonyms() {
        SettingsSearch search = new SettingsSearch();
        search.add("Location spoofing", "Location & movement", "Location & Map", 0);
        search.add("Dark theme", "Interface", "Display & Performance", 4);
        eq("Location spoofing", single(search.find("fake gps")));
        eq("Location spoofing", single(search.find("mock location")));
        eq("Dark theme", single(search.find("light mode")));
    }

    private static void emptyQueries() {
        SettingsSearch search = new SettingsSearch();
        search.add("Auto ball", "Catching", "Catching", 1);
        eq("0", Integer.toString(search.find("").size()));
        eq("0", Integer.toString(search.find("   ").size()));
        eq("0", Integer.toString(search.find(null).size()));
        eq("0", Integer.toString(search.find("helicopter").size()));
        eq("1", Integer.toString(search.find("ball").size()));
    }

    private static String single(List<SettingsSearch.Entry> hits) {
        eq("1", Integer.toString(hits.size()));
        return hits.get(0).name;
    }

    private static void eq(String expected, String actual) {
        if (!expected.equals(actual)) throw new AssertionError("expected <" + expected + "> but was <" + actual + ">");
    }
}
