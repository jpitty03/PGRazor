package dev.pgrazor;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.text.InputFilter;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.inputmethod.EditorInfo;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.IOException;
import java.util.List;
import dev.pgrazor.PlaceSearchClient.Place;
import android.view.inputmethod.InputMethodManager;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

/**
 * A place lookup dialog for the world map. The user types a place name and taps Search (or the
 * keyboard's Search action); matches are shown as a scrollable list and selecting one reports its
 * coordinates back through {@link Selection}. Selecting a result never moves anything on its own —
 * the caller decides what to do with the coordinates (the world-map integration centres the map).
 *
 * <p>Networking is owned by a per-dialog {@link PlaceSearchClient} called on a background thread. Each
 * dialog owns its own client and single-thread executor, both torn down when the dialog closes or
 * the activity leaves the foreground, so a slow lookup cannot leak past the UI that started it.
 */
public final class MapSearchDialog {
    private MapSearchDialog() {}

    /** Reports a chosen result's coordinates; the caller decides how to use them. */
    public interface Selection { void select(double latitude, double longitude); }

    private static final int QUERY_LIMIT = 200;

    public static AlertDialog show(Activity activity, double biasLat, double biasLon, Selection callback) {
        if (callback == null) throw new IllegalArgumentException("callback required");
        // The bias only ranks results, so an out-of-range value falls back to the prime meridian
        // rather than refusing to open the search dialog.
        double lat = (Double.isFinite(biasLat) && Math.abs(biasLat) <= 90) ? biasLat : 0;
        double lon = (Double.isFinite(biasLon) && Math.abs(biasLon) <= 180) ? biasLon : 0;

        Context ui = MapTheme.wrap(activity);
        LinearLayout layout = new LinearLayout(ui);
        layout.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(activity, 12);
        layout.setPadding(pad, pad, pad, pad);

        layout.addView(label(ui, "Search near the map area. Add a city for places farther away.", 13));

        EditText input = new EditText(ui);
        input.setHint("Places, parks, landmarks or addresses");
        input.setSingleLine(true);
        // No live autocomplete: results appear only on an explicit Search action.
        input.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_CAP_WORDS | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        input.setImeOptions(EditorInfo.IME_ACTION_SEARCH);
        input.setFilters(new InputFilter[] { new InputFilter.LengthFilter(QUERY_LIMIT) });

        Button searchButton = button(ui, "Search");
        LinearLayout inputRow = new LinearLayout(ui);
        inputRow.setGravity(Gravity.CENTER_VERTICAL);
        inputRow.addView(input, new LinearLayout.LayoutParams(0, -2, 1));
        inputRow.addView(searchButton, new LinearLayout.LayoutParams(dp(activity, 96), dp(activity, 48)));
        layout.addView(inputRow);

        TextView status = label(ui, "", 12);
        status.setPadding(0, dp(activity, 6), 0, dp(activity, 6));
        layout.addView(status);

        ScrollView scroller = new ScrollView(ui);
        LinearLayout resultList = new LinearLayout(ui);
        resultList.setOrientation(LinearLayout.VERTICAL);
        scroller.addView(resultList);
        layout.addView(scroller, new LinearLayout.LayoutParams(-1, 0, 1));

        TextView attribution = label(ui, "Search: Photon / OpenStreetMap contributors", 11);
        attribution.setPadding(0, dp(activity, 6), 0, 0);
        layout.addView(attribution);
        layout.addView(label(ui, "Results may be incomplete.", 11));

        Controller controller = new Controller(activity, lat, lon, input, searchButton, status, resultList, callback);

        searchButton.setOnClickListener(v -> controller.runSearch());
        input.setOnEditorActionListener((v, actionId, event) -> {
            if (actionId == EditorInfo.IME_ACTION_SEARCH) { controller.runSearch(); return true; }
            return false;
        });

        AlertDialog dialog = new AlertDialog.Builder(ui).setTitle("Find a place").setView(layout)
            .setNegativeButton("Close", null).create();
        controller.dialog = dialog;
        DialogLifecycle.bind(activity, dialog, controller::close);
        dialog.setOnShowListener(d -> {
            MapTheme.fullScreen(dialog); MapTheme.style(layout); MapTheme.styleButtons(dialog);
            if (controller.client == null) {
                searchButton.setEnabled(false);
                status.setText("Place search is unavailable right now.");
            } else {
                status.setText("Type a place and tap Search.");
            }
            input.requestFocus();
        });
        dialog.show();
        return dialog;
    }

    /** Owns the per-dialog client, executor and result rendering; nothing here touches teleport/walk/control. */
    private static final class Controller {
        final Activity activity;
        final Context ui;
        final double biasLat, biasLon;
        final EditText input;
        final Button searchButton;
        final TextView status;
        final LinearLayout resultList;
        final Selection callback;
        final Handler main = new Handler(Looper.getMainLooper());
        final ThreadPoolExecutor executor =
            new ThreadPoolExecutor(1, 1, 0, TimeUnit.MILLISECONDS, new LinkedBlockingQueue<Runnable>());
        final PlaceSearchClient client;
        AlertDialog dialog;
        /** Bumped on each new search and on close so stale background completions are ignored. */
        int generation;
        boolean closed;

        Controller(Activity activity, double biasLat, double biasLon, EditText input, Button searchButton,
                   TextView status, LinearLayout resultList, Selection callback) {
            this.activity = activity; this.ui = MapTheme.wrap(activity);
            this.biasLat = biasLat; this.biasLon = biasLon;
            this.input = input; this.searchButton = searchButton; this.status = status;
            this.resultList = resultList; this.callback = callback;
            this.client = new PlaceSearchClient();
        }

        void runSearch() {
            if (closed || client == null || !searchButton.isEnabled()) return;
            String query = input.getText().toString().trim();
            if (query.length() < 2) { status.setText("Enter at least two characters to search."); return; }
            InputMethodManager keyboard=(InputMethodManager)activity.getSystemService(Context.INPUT_METHOD_SERVICE);
            if(keyboard!=null)keyboard.hideSoftInputFromWindow(input.getWindowToken(),0);
            // A new query supersedes any in-flight lookup and clears the previous matches immediately.
            final int gen = ++generation;
            resultList.removeAllViews();
            status.setText("Searching…");
            searchButton.setEnabled(false);
            final PlaceSearchClient c = client;
            final double lat = biasLat, lon = biasLon;
            try {
                executor.execute(() -> {
                    try {
                        List<Place> found = c.search(query, lat, lon);
                        main.post(() -> deliver(gen, found, null));
                    } catch (IOException e) {
                        main.post(() -> deliver(gen, null, "Search is unavailable or busy. Wait a moment, check your connection, and try again."));
                    } catch (RuntimeException e) {
                        main.post(() -> deliver(gen, null, "Search could not be completed. Please try again."));
                    }
                });
            } catch (java.util.concurrent.RejectedExecutionException shuttingDown) {
                // Dialog is closing; nothing more to do.
            }
        }

        private void deliver(int gen, List<Place> found, String error) {
            if (closed || gen != generation) return; // superseded or torn down
            searchButton.setEnabled(true);
            resultList.removeAllViews();
            if (error != null) { status.setText(error); return; }
            if (found == null || found.isEmpty()) {
                status.setText("No matching places found. Try a broader name or add a city.");
                return;
            }
            status.setText(found.size() == 1 ? "1 match" : found.size() + " matches");
            for (Place place : found) {
                if (place == null) continue;
                resultList.addView(resultRow(place));
            }
        }

        private View resultRow(Place place) {
            LinearLayout row = new LinearLayout(ui);
            row.setOrientation(LinearLayout.VERTICAL);
            int v = dp(activity, 10), h = dp(activity, 6);
            row.setPadding(h, v, h, v);
            row.setClickable(true); row.setFocusable(true);
            row.addView(label(ui, place.name == null ? "" : place.name, 15));
            if (place.detail != null && !place.detail.isEmpty()) {
                TextView detail = label(ui, place.detail, 12);
                row.addView(detail);
            }
            final double lat = place.latitude, lon = place.longitude;
            row.setContentDescription("Show " + (place.name == null ? "place" : place.name) + " on the map");
            // Selecting only reports coordinates; the world-map integration decides to centre the map.
            row.setOnClickListener(w -> { if(closed)return; callback.select(lat, lon); if (dialog != null) dialog.dismiss(); });
            return row;
        }

        void close() {
            if (closed) return;
            closed = true;
            generation++; // invalidate any pending completion
            main.removeCallbacksAndMessages(null);
            executor.shutdownNow(); // interrupt an in-flight lookup
            final PlaceSearchClient c = client;
            if (c != null) {
                // close() may touch network/IO, so keep it off the main thread.
                new Thread(() -> { try { c.close(); } catch (Throwable ignored) {} }, "pgrazor-place-search-close").start();
            }
        }
    }

    private static int dp(Context c, float value) { return Math.round(value * c.getResources().getDisplayMetrics().density); }
    private static TextView label(Context c, String text, int size) { TextView v = new TextView(c); v.setText(text); v.setTextSize(size); return v; }
    private static Button button(Context c, String text) { Button b = new Button(c); b.setText(text); b.setAllCaps(false); b.setTextSize(13); b.setPadding(0, 0, 0, 0); return b; }
}
