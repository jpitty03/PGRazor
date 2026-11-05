package dev.pgrazor;

import android.app.Activity;
import android.app.AlertDialog;

/** The quick menu's Feeds entry: pick which live feed service to open (each keeps its own sign-in). */
final class FeedChooser {
    private FeedChooser() {}

    static void show(Activity activity) {
        String[] labels = {"LivePokeMap\nNearby Pokémon, stops, gyms, raids and power spots",
                           "PoGoCoordinates\nPokémon search by IV, PvP rank and more; sign in with Discord"};
        AlertDialog choose = ChoiceList.attach(new AlertDialog.Builder(MapTheme.wrap(activity)).setTitle("Feeds")
            .setNegativeButton("Cancel", null).create(), labels, which -> {
                if (which == 0) LiveFeedDialog.show(activity); else PogoFeedDialog.show(activity);
            });
        DialogLifecycle.bind(activity, choose, null);
        choose.setOnShowListener(d -> MapTheme.styleButtons(choose));
        choose.show();
    }
}
