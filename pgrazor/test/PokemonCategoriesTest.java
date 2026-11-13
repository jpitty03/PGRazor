package dev.pgrazor;

public final class PokemonCategoriesTest {
    private static void expect(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
    private static boolean in(int group, int id) { return PokemonCategories.matches(group, id, false); }
    public static void main(String[] args) {
        expect(in(PokemonCategories.STAGE_2, 2) && !in(PokemonCategories.STAGE_3, 2), "Ivysaur is Stage 2");
        expect(in(PokemonCategories.STAGE_3, 3) && !in(PokemonCategories.STAGE_2, 3), "Venusaur is Stage 3");
        expect(!in(PokemonCategories.STAGE_2, 1) && !in(PokemonCategories.STAGE_3, 1), "Bulbasaur is unevolved");
        expect(in(PokemonCategories.STAGE_2, 25) && in(PokemonCategories.STAGE_3, 26), "Baby Pichu counts as first stage");
        expect(in(PokemonCategories.STAGE_2, 196), "Branched Eevee evolution is Stage 2");
        expect(!in(PokemonCategories.STAGE_2, 150), "Mega evolution does not increase species stage");
        expect(in(PokemonCategories.LEGENDARY, 150), "Mewtwo is Legendary");
        expect(in(PokemonCategories.MEGA, 150), "Legendary and Mega can overlap");
        expect(!in(PokemonCategories.MYTHICAL, 150), "Mewtwo is not Mythical");
        expect(in(PokemonCategories.MYTHICAL, 151), "Mew is Mythical");
        expect(!in(PokemonCategories.LEGENDARY, 151), "Mythical stays separate");
        expect(in(PokemonCategories.MYTHICAL, 719) && in(PokemonCategories.MEGA, 719), "Diancie overlaps Mythical/Mega");
        expect(in(PokemonCategories.ULTRA_BEAST, 793) && !in(PokemonCategories.LEGENDARY, 793), "Ultra Beasts stay separate");
        expect(in(PokemonCategories.PRIMAL, 382) && in(PokemonCategories.LEGENDARY, 382), "Kyogre overlaps Primal/Legendary");
        expect(!in(PokemonCategories.MEGA, 382), "Primal is not Mega");
        expect(in(PokemonCategories.MEGA, 6) && !in(PokemonCategories.LEGENDARY, 6), "Charizard is Mega-capable");
        expect(!in(PokemonCategories.MEGA, 4), "Pre-evolution not automatically included");
        expect(PokemonCategories.matches(PokemonCategories.ALL, 1999, false), "Unknown saved IDs remain browsable");
        expect(PokemonCategories.matches(PokemonCategories.SELECTED, 1999, true), "Unknown saved IDs can be reviewed");
        expect(!PokemonCategories.matches(PokemonCategories.SELECTED, 1999, false), "Selected requires actual selection");
        expect(!in(PokemonCategories.ALL, 0) && !in(PokemonCategories.ALL, 2001), "Invalid IDs excluded");
        expect(!in(99, 150), "Unknown category does not broaden results");
        expect(in(PokemonCategories.LEGENDARY, 150) && SmartCatchPreferences.matchesSpecies(150, "Mewtwo", "mEw"), "Name filter intersects category");
        expect(!in(PokemonCategories.MYTHICAL, 150) && SmartCatchPreferences.matchesSpecies(150, "Mewtwo", "#150"), "Dex search cannot bypass category");
        System.out.println("PASS: species category overlap, Mega/Primal distinction, unknown IDs, search intersection");
    }
}
