package dev.pgrazor;

import java.util.Properties;

public final class ResearchPreferencesTest {
    private static void require(boolean ok, String what) { if (!ok) throw new AssertionError(what); }
    private static void refused(Runnable r, String what) {
        try { r.run(); } catch (IllegalArgumentException expected) { return; }
        throw new AssertionError(what);
    }

    public static void main(String[] args) {
        ResearchPreferences p = new ResearchPreferences();
        p.read(new Properties());
        require(!p.claim() && !p.remove() && p.config().equals("R2|0|0|0|"), "defaults are off");

        require(p.observe("CHALLENGE_CATCH_EASY", "Catch 5 Pokémon", "Item"), "new task stored");
        require(!p.observe("CHALLENGE_CATCH_EASY", "Catch 5 Pokémon", "Item"), "unchanged task ignored");
        require(p.observe("CHALLENGE_CATCH_EASY", "Catch 6\tPokémon\n", "Item") && p.tasks("catch 6 pok").size() == 1, "text is cleaned and refreshed");
        require(!p.observe("lower_case", "x", "y") && !p.observe("", "x", "y"), "invalid templates ignored");
        p.observe("HALLOWEEN_26_NICE_THROWS_P2", "Make 6 Nice Throws", "Encounter: Litwick");
        require(p.tasks("").size() == 2 && p.tasks("litwick").size() == 1 && p.tasks("nice").get(0)[0].equals("HALLOWEEN_26_NICE_THROWS_P2"), "search");
        require(p.tasks("").get(0)[1].equals("Catch 6 Pokémon"), "sorted by task text");

        p.select("HALLOWEEN_26_NICE_THROWS_P2", true);
        p.setClaim(true);
        require(p.config().equals("R2|0|1|0|"), "selection is sent only while removal is on");
        p.setRemove(true);
        require(p.config().equals("R2|1|1|0|HALLOWEEN_26_NICE_THROWS_P2"), "removal sends sorted selection");
        p.setCatchEncounters(true);
        require(p.config().equals("R2|1|1|1|HALLOWEEN_26_NICE_THROWS_P2"), "encounter catching flag");
        refused(() -> p.select("bad template", true), "invalid selection refused");

        Properties saved = new Properties();
        p.write(saved);
        ResearchPreferences copy = new ResearchPreferences();
        copy.read(saved);
        require(copy.config().equals(p.config()) && copy.tasks("").size() == 2 && copy.selected("HALLOWEEN_26_NICE_THROWS_P2"), "round trip");

        Properties corrupt = new Properties();
        corrupt.setProperty("researchSelected", "GOOD_ONE,bad one,,GOOD_TWO");
        corrupt.setProperty("researchCatalog", "GOOD_ONE\tTask\tReward\nbroken line\nbad\tx\ty");
        ResearchPreferences repaired = new ResearchPreferences();
        repaired.read(corrupt);
        require(repaired.selectedCount() == 2 && repaired.tasks("").size() == 1, "corrupt entries dropped");

        ResearchPreferences full = new ResearchPreferences();
        full.read(new Properties());
        full.observe("KEEP_ME", "Keep", "");
        full.select("KEEP_ME", true);
        for (int i = 0; i < ResearchPreferences.MAX_TEMPLATES + 5; i++) full.observe("T_" + i, "Task " + i, "");
        require(full.tasks("").size() == ResearchPreferences.MAX_TEMPLATES && full.tasks("keep").size() == 1, "bounded catalog keeps selected tasks");
        System.out.println("ResearchPreferencesTest passed");
    }
}
