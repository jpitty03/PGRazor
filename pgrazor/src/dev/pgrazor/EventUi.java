package dev.pgrazor;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.view.View;

/** Theme bridge for the independent events package. */
public final class EventUi {
    private EventUi() {}
    public static Context context(Activity owner) { return MapTheme.wrap(owner); }
    public static void style(AlertDialog dialog, View content) {
        if (content != null) MapTheme.style(content);
        MapTheme.styleButtons(dialog);
        MapTheme.gameFooter(dialog, null);
    }
}
