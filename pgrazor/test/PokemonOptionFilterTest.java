package dev.pgrazor;

/**
 * Exercises {@link PokemonOptionFilter} with the real catalog key shapes: composite raid-boss keys
 * ({@code id|form|variant}) and bare Dex keys. Run by the coordinator as a plain Java main.
 */
public final class PokemonOptionFilterTest {
    private static void expect(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
    private static final int ALL = PokemonCategories.ALL, LEGENDARY = PokemonCategories.LEGENDARY,
        MYTHICAL = PokemonCategories.MYTHICAL, MEGA = PokemonCategories.MEGA, SELECTED = PokemonCategories.SELECTED;

    public static void main(String[] args) {
        // Dex id comes from the whole numeric key or the part before the first bar.
        expect(PokemonOptionFilter.dexOf("150") == 150, "Bare key is its own Dex id");
        expect(PokemonOptionFilter.dexOf("6|0|mega") == 6, "Composite key reads the Dex before the first bar");
        expect(PokemonOptionFilter.dexOf("150|3|shadow") == 150, "Form and variant do not change the Dex");
        expect(PokemonOptionFilter.dexOf("") == -1 && PokemonOptionFilter.dexOf((String) null) == -1, "No Dex id when the key is empty");
        expect(PokemonOptionFilter.dexOf("kecleon") == -1, "Non-numeric key has no Dex id");

        // Variant preservation: the two Charizard variants are distinct keys that both resolve to #6
        // and are kept together, never flattened to one Dex selection.
        String normal = "6|0|normal", mega = "6|0|mega";
        expect(PokemonOptionFilter.visible(MEGA, "", "Charizard", normal, false), "Mega-capable keeps the normal variant");
        expect(PokemonOptionFilter.visible(MEGA, "", "Charizard", mega, false), "Mega-capable keeps the mega variant");
        expect(PokemonOptionFilter.visible(ALL, "#6", "Charizard", normal, false)
            && PokemonOptionFilter.visible(ALL, "#6", "Charizard", mega, false), "#6 shows both Charizard variants");

        // A form or variant id in a composite key must not satisfy a Dex search.
        expect(!PokemonOptionFilter.visible(ALL, "#0", "Charizard", "6|0|mega", false), "#0 never matches a form id");
        expect(PokemonOptionFilter.visible(ALL, "#0", "Charizard", "60|0|normal", false), "#0 does match the real Dex #60");
        expect(PokemonOptionFilter.visible(ALL, "char", "Charizard", "6|0|mega", false), "Name search still matches the label");

        // Category and search intersect: a Legendary name search keeps Mewtwo but drops a Mythical.
        expect(PokemonOptionFilter.visible(LEGENDARY, "mew", "Mewtwo", "150", false), "Legendary + name keeps Mewtwo");
        expect(!PokemonOptionFilter.visible(MYTHICAL, "mew", "Mewtwo", "150", false), "Mewtwo is not Mythical");
        expect(PokemonOptionFilter.visible(MYTHICAL, "mew", "Mew", "151", false), "Mythical + name keeps Mew");
        expect(!PokemonOptionFilter.visible(LEGENDARY, "mew", "Mew", "151", false), "Mew is not Legendary");
        expect(PokemonOptionFilter.visible(LEGENDARY, "#150", "Mewtwo", "150", false), "Legendary + #150 keeps Mewtwo");
        expect(!PokemonOptionFilter.visible(MYTHICAL, "#150", "Mewtwo", "150", false), "A Dex search cannot bypass the category");

        // Unknown or out-of-range Dex ids: browsable under All, never under a special group.
        expect(PokemonOptionFilter.visible(ALL, "", "Mystery Boss", "9999|0|normal", false), "All keeps an unknown saved boss");
        expect(!PokemonOptionFilter.visible(LEGENDARY, "", "Mystery Boss", "9999|0|normal", false), "Special groups skip invalid species");

        // Selected view: only the currently selected options, unknown Dex ids included.
        expect(PokemonOptionFilter.visible(SELECTED, "", "Charizard", mega, true), "Selected shows a selected option");
        expect(!PokemonOptionFilter.visible(SELECTED, "", "Charizard", mega, false), "Selected hides an unselected option");
        expect(PokemonOptionFilter.visible(SELECTED, "", "Mystery Boss", "9999|0|normal", true), "Selected preserves an unknown selected option");
        expect(!PokemonOptionFilter.visible(SELECTED, "char", "Eevee", "133|0|normal", true), "Selected still honours the search box");

        // Real /quests/catalog-pairs rows: id is an opaque packed species/form selection ID;
        // pokemon_id is the Dex number. Never replace id with pokemon_id in the saved selection.
        String[] aggron = {"Aggron", "306000000", "306"};
        expect(PokemonOptionFilter.dexOf(aggron) == 306, "Quest metadata supplies the Dex");
        expect(PokemonOptionFilter.visible(MEGA, "", aggron, false), "Quest Mega Energy includes Aggron");
        expect(PokemonOptionFilter.visible(PokemonCategories.STAGE_3, "#306", aggron, false), "Quest stage and Dex search use metadata");
        expect(!PokemonOptionFilter.visible(ALL, "#000", aggron, false), "Packed suffix is not a Dex search hit");
        expect(aggron[1].equals("306000000"), "Matching preserves the packed selection key");
        expect(PokemonOptionFilter.dexOf(new String[]{"Charizard", "6|0|mega"}) == 6, "Raid options fall back to composite Dex");

        System.out.println("PASS: Dex extraction, variant preservation, category/search intersection, unknown IDs, Selected and #Dex search");
    }
}
