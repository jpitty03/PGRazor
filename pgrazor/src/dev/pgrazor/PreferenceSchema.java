package dev.pgrazor;

import java.util.Arrays;

/** Single source of truth for preference scopes and their keys in settings export/import.
 *  A plugin that persists UI state (overlay positions, map toggles, feed filters...) registers
 *  its keys here once; SettingsBackup writes exactly these keys and SettingsBackupRules
 *  accepts exactly these keys, so exports can never fail to import. */
public final class PreferenceSchema {
    private PreferenceSchema() {}
    public static final String[] SCOPES={"pgrazor-map","pgrazor-overlays","pgrazor-saved-places-v1","pgrazor-livefeed"};
    // Must stay index-aligned with SCOPES.
    public static final String[][] KEYS={
        {"Stops","Gyms","Raids"},
        {"cooldown.fx","cooldown.fy","vgp.fx","vgp.fy","transfer.fx","transfer.fy","tag-catch.fx","tag-catch.fy","gym-feed.fx","gym-feed.fy","local-time.fx","local-time.fy"},
        {"places"},
        {"category","radius","minIv","myShinies","filters_v1","filter_presets_v1"}
    };
    public static boolean has(String scope,String key){
        for(int i=0;i<SCOPES.length;i++)if(SCOPES[i].equals(scope))
            for(String candidate:KEYS[i])if(candidate.equals(key))return true;
        return false;
    }
    public static boolean scope(String scope){return Arrays.asList(SCOPES).contains(scope);}
}
