package dev.pgrazor;

import java.util.Properties;

public final class AutoRaidPreferencesTest {
    static int checks;
    static void eq(Object expected, Object actual) {
        checks++;
        if (expected == null ? actual != null : !expected.equals(actual))
            throw new AssertionError(expected + " != " + actual);
    }
    static void truth(boolean ok, String what) { checks++; if (!ok) throw new AssertionError(what); }
    static void reject(Runnable action) {
        checks++;
        try { action.run(); } catch (IllegalArgumentException expected) { return; }
        throw new AssertionError("Accepted invalid input");
    }

    public static void main(String[] args) {
        // Defaults, and defaults are launchable.
        AutoRaidPreferences p = new AutoRaidPreferences();
        eq(AutoRaidPreferences.TIER_ANY, p.tierMask); eq("", p.bosses); eq(5, p.maxRaids);
        eq(false, p.stopOnShiny); eq(1, p.minPlayers); eq(60, p.waitSeconds);
        eq(false, p.walk); eq(true, p.freePass); eq(false, p.premiumPass);
        p.validate();

        // Tier summary: no bits is "any"; bits render in display order.
        eq("Any tier", AutoRaidPreferences.tierSummary(AutoRaidPreferences.TIER_ANY));
        eq(AutoRaidPreferences.TIER_LABELS[0] + ", " + AutoRaidPreferences.TIER_LABELS[3],
            AutoRaidPreferences.tierSummary(AutoRaidPreferences.TIER_ONE_STAR | AutoRaidPreferences.TIER_MEGA));
        eq(AutoRaidPreferences.TIER_LABELS[6], AutoRaidPreferences.tierSummary(AutoRaidPreferences.TIER_ULTRA_BEAST));

        // Boss list canonicalisation and rejection of bad input.
        eq("", AutoRaidPreferences.normalizeBosses(""));
        eq("", AutoRaidPreferences.normalizeBosses("   "));
        eq("3,5,150", AutoRaidPreferences.normalizeBosses("150, 5, 3, 5"));
        eq("1,2000", AutoRaidPreferences.normalizeBosses("2000,1"));
        reject(() -> AutoRaidPreferences.normalizeBosses("0"));
        reject(() -> AutoRaidPreferences.normalizeBosses("2001"));
        reject(() -> AutoRaidPreferences.normalizeBosses("abc"));
        reject(() -> AutoRaidPreferences.normalizeBosses("12,,13"));
        reject(() -> AutoRaidPreferences.normalizeBosses(null));

        // validate() rejects each out-of-range field and non-canonical boss lists.
        AutoRaidPreferences bad = new AutoRaidPreferences();
        bad.tierMask = 128; reject(bad::validate);
        bad.tierMask = -1; reject(bad::validate);
        bad.tierMask = AutoRaidPreferences.TIER_MASK_ALL; bad.validate();
        bad.tierMask = AutoRaidPreferences.TIER_ANY;
        bad.maxRaids = 0; reject(bad::validate); bad.maxRaids = 101; reject(bad::validate); bad.maxRaids = 5;
        bad.minPlayers = 0; reject(bad::validate); bad.minPlayers = 21; reject(bad::validate); bad.minPlayers = 1;
        bad.waitSeconds = 14; reject(bad::validate); bad.waitSeconds = 121; reject(bad::validate); bad.waitSeconds = 60;
        bad.bosses = "5,3"; reject(bad::validate);   // valid IDs but not canonical
        bad.bosses = "abc"; reject(bad::validate);   // not parseable
        bad.bosses = null; reject(bad::validate);    // never null in a launchable config
        bad.bosses = "3,5"; bad.validate();

        // Pass selection: at least one allowed pass is required; remote passes do not exist here.
        AutoRaidPreferences noPass = new AutoRaidPreferences();
        noPass.freePass = false; noPass.premiumPass = false; reject(noPass::validate);
        noPass.premiumPass = true; noPass.validate();     // Premium alone is fine
        noPass.premiumPass = false; noPass.freePass = true; noPass.validate();

        // read() repairs each malformed field independently, keeping the valid ones.
        Properties corrupt = new Properties();
        corrupt.setProperty("autoRaidTierMask", "999");        // out of range -> any
        corrupt.setProperty("autoRaidBosses", "7,bad,9");      // malformed -> empty
        corrupt.setProperty("autoRaidMaxRaids", "500");        // out of range -> 5
        corrupt.setProperty("autoRaidMinPlayers", "garbage");  // non-integer -> 1
        corrupt.setProperty("autoRaidWaitSeconds", "10");      // below range -> 60
        corrupt.setProperty("autoRaidStopOnShiny", "yes");     // not boolean -> false
        corrupt.setProperty("autoRaidWalk", "true");           // valid -> true
        corrupt.setProperty("autoRaidFreePass", "false");      // valid -> false
        corrupt.setProperty("autoRaidPremiumPass", "true");    // valid -> true
        AutoRaidPreferences repaired = new AutoRaidPreferences();
        repaired.read(corrupt);
        eq(AutoRaidPreferences.TIER_ANY, repaired.tierMask); eq("", repaired.bosses); eq(5, repaired.maxRaids);
        eq(1, repaired.minPlayers); eq(60, repaired.waitSeconds); eq(false, repaired.stopOnShiny);
        eq(true, repaired.walk); eq(false, repaired.freePass); eq(true, repaired.premiumPass);
        repaired.validate();

        // read() canonicalises a valid-but-unsorted stored boss list.
        Properties unsorted = new Properties();
        unsorted.setProperty("autoRaidBosses", "384, 150, 150");
        AutoRaidPreferences canon = new AutoRaidPreferences();
        canon.read(unsorted);
        eq("150,384", canon.bosses);

        // write()/read() round trip of a fully customised configuration.
        AutoRaidPreferences configured = new AutoRaidPreferences();
        configured.tierMask = AutoRaidPreferences.TIER_FIVE_STAR | AutoRaidPreferences.TIER_SHADOW;
        configured.bosses = AutoRaidPreferences.normalizeBosses("384,150");
        configured.maxRaids = 20; configured.stopOnShiny = true; configured.minPlayers = 4;
        configured.waitSeconds = 90; configured.walk = true; configured.freePass = false; configured.premiumPass = true;
        configured.validate();
        Properties saved = new Properties();
        configured.write(saved);
        eq(9, saved.size());
        AutoRaidPreferences restored = new AutoRaidPreferences();
        restored.read(saved);
        eq(configured.tierMask, restored.tierMask); eq(configured.bosses, restored.bosses);
        eq(configured.maxRaids, restored.maxRaids); eq(configured.stopOnShiny, restored.stopOnShiny);
        eq(configured.minPlayers, restored.minPlayers); eq(configured.waitSeconds, restored.waitSeconds);
        eq(configured.walk, restored.walk); eq(configured.freePass, restored.freePass);
        eq(configured.premiumPass, restored.premiumPass);

        // copy() isolation: mutating the copy leaves the source untouched.
        AutoRaidPreferences source = configured.copy();
        AutoRaidPreferences clone = source.copy();
        clone.tierMask = AutoRaidPreferences.TIER_ANY; clone.bosses = ""; clone.maxRaids = 1;
        clone.stopOnShiny = false; clone.minPlayers = 1; clone.waitSeconds = 15; clone.walk = false;
        clone.freePass = true; clone.premiumPass = false;
        truth(source.tierMask == configured.tierMask && source.bosses.equals(configured.bosses)
            && source.maxRaids == 20 && source.stopOnShiny && source.walk && source.premiumPass,
            "copy does not share mutable state with the source");
        truth(clone.maxRaids == 1 && !clone.premiumPass && clone.bosses.isEmpty(),
            "clone holds its own values");

        System.out.println("AutoRaidPreferences: " + checks + " checks passed");
    }
}
