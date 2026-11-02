package dev.pgrazor;

import android.content.Context;

/** UI-only, per-account preferences; no gameplay engine changes. */
final class OverlayTextSettings {
    private OverlayTextSettings() {}
    static boolean hidden(Context context,String overlay){
        return AccountProfiles.preferences(context.getApplicationContext(),"pgrazor-overlays")
            .getBoolean(overlay+".textHidden",false);
    }
    static void setHidden(Context context,String overlay,boolean hidden){
        AccountProfiles.preferences(context.getApplicationContext(),"pgrazor-overlays")
            .edit().putBoolean(overlay+".textHidden",hidden).apply();
    }
}
