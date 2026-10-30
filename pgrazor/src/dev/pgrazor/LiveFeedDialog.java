package dev.pgrazor;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.HorizontalScrollView;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;
import android.widget.Toast;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Full-screen LivePokeMap feed. Shows a login form while the saved session is unsigned, then a
 * themed, filterable list of nearby live results fetched around the current {@link Control} engine
 * coordinates (never a remote account's view). Tapping a row opens a read-only detail child dialog
 * whose Map / Walk / Teleport actions are the only things that can move the location.
 *
 * <p>Networking (login/fetch) is delegated to {@link LiveFeedClient} and runs on a single serialized
 * background {@link ExecutorService}; parsing happens off the main thread inside the client. A
 * request generation counter rejects any response that arrives after the filters, a logout, or a
 * close changed what the UI expects, so stale data never lands. All timers, the connection and the
 * executor are torn down when the dialog is dismissed or the activity leaves the foreground via
 * {@link DialogLifecycle}.
 */
public final class LiveFeedDialog {
    private LiveFeedDialog() {}

    private static final String[] CATEGORIES = {"All", "Pokémon", "Stops", "Gyms", "Raids", "Power Spots"};
    private static final double[] RADII = {1, 3, 5};
    private static final String[] RADIUS_LABELS = {"1 km", "3 km", "5 km"};
    private static final int DEFAULT_REFRESH_SECONDS = 15;
    /** A 429 always backs off at least this long regardless of a shorter server Retry-After. */
    private static final long MIN_BACKOFF_MS = 30_000L;

    /** Entry point wired by the coordinator from the quick menu / {@link Pad}. */
    public static void show(Activity activity) {
        if (activity == null || activity.isFinishing() || activity.isDestroyed()) return;
        Context ui = MapTheme.wrap(activity);
        LinearLayout root = new LinearLayout(ui);
        root.setOrientation(LinearLayout.VERTICAL);
        AlertDialog dialog = new AlertDialog.Builder(ui)
            .setTitle("LivePokeMap")
            .setView(root)
            .setNegativeButton("Close", null)
            .create();
        Controller controller = new Controller(activity, ui, dialog, root);
        DialogLifecycle.bind(activity, dialog, controller::close, ()->controller.showingLogin,
            controller::pauseLogin,controller::resumeLogin);
        dialog.setOnShowListener(d -> controller.attach());
        dialog.show();
    }

    /** Owns all screen state, the single network worker, the poll/expiry clock and teardown. */
    private static final class Controller {
        private final Activity activity;
        private final Context ui;
        private final AlertDialog dialog;
        private final LinearLayout root;
        private final SharedPreferences prefs;
        private final Handler main = new Handler(Looper.getMainLooper());
        private final ExecutorService net = Executors.newSingleThreadExecutor(task -> {
            Thread t = new Thread(task, "pgrazor-livefeed"); t.setDaemon(true); return t;
        });
        private final LiveFeedClient client;
        private final LiveFeedIcons icons;
        private volatile org.json.JSONObject speciesNames;
        private final List<AlertDialog> children = new ArrayList<>();
        private final Runnable tick = this::tick;

        // All of the following are touched on the main thread only.
        private final List<LiveFeedData.Entry> entries = new ArrayList<>();
        private final List<LiveFeedData.Entry> visible = new ArrayList<>();
        private HashMap<LiveFeedData.Entry, Double> distCache = new HashMap<>();
        private final FeedAdapter adapter = new FeedAdapter();
        private boolean closed;
        private boolean showingLogin;
        private boolean loginPaused, renderOnResume;
        private int generation;               // bumped on radius/min-IV change, logout and close
        private boolean fetchInFlight, loginInFlight;
        private long lastFetchStartedElapsed; // SystemClock.elapsedRealtime
        private long lastUpdated;             // System.currentTimeMillis of last applied snapshot
        private long backoffUntilElapsed;
        private int refreshSeconds = DEFAULT_REFRESH_SECONDS;
        private String note = "", lastError;
        private int categoryIndex, radiusIndex = 1;
        private boolean myShinies;            // false = Nearby viewport, true = My Shinies feed
        private LiveFeedFilters filters;      // immutable; replaced wholesale on Apply
        private Button[] catButtons, radiusButtons, tabButtons;
        private TextView feedStatus;

        Controller(Activity activity, Context ui, AlertDialog dialog, LinearLayout root) {
            this.activity = activity; this.ui = ui; this.dialog = dialog; this.root = root;
            prefs = AccountProfiles.preferences(activity,"pgrazor-livefeed");
            categoryIndex = clamp(prefs.getInt("category", 0), 0, CATEGORIES.length - 1);
            radiusIndex = clamp(prefs.getInt("radius", 1), 0, RADII.length - 1);
            myShinies = prefs.getBoolean("myShinies", false);
            filters = loadFilters();
            // The session constructor must not touch the keystore on the main thread; token use is
            // confined to login()/fetch() which run on the network worker.
            client = new LiveFeedClient(new LiveFeedSession(activity));
            icons = new LiveFeedIcons(activity);
            showingLogin = !client.signedIn();
            net.execute(() -> {
                try (java.io.InputStream in = activity.getAssets().open("pgrazor/map-icons/pokemon-names.json")) {
                    java.io.ByteArrayOutputStream bytes = new java.io.ByteArrayOutputStream();
                    byte[] buffer = new byte[4096]; int count;
                    while ((count = in.read(buffer)) != -1) {
                        if (bytes.size() + count > 2 * 1024 * 1024) return;
                        bytes.write(buffer, 0, count);
                    }
                    speciesNames = new org.json.JSONObject(bytes.toString("UTF-8"));
                } catch (Exception ignored) { }
            });
        }

        void attach() {
            MapTheme.fullScreen(dialog);
            dialog.getWindow().clearFlags(android.view.WindowManager.LayoutParams.FLAG_ALT_FOCUSABLE_IM);
            dialog.getWindow().setSoftInputMode(android.view.WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
            render();
            MapTheme.styleButtons(dialog);
            main.removeCallbacks(tick);
            main.postDelayed(tick, 1000);
        }

        private void pauseLogin(){loginPaused=true;main.removeCallbacks(tick);}
        private void resumeLogin(){
            if(closed||!loginPaused)return;
            loginPaused=false;
            if(renderOnResume){renderOnResume=false;render();}
            main.removeCallbacks(tick);main.postDelayed(tick,1000);
        }

        // ------------------------------------------------------------------ rendering

        private void render() {
            root.removeAllViews();
            feedStatus = null; catButtons = radiusButtons = tabButtons = null;
            if (showingLogin) buildLogin(); else buildFeed();
            MapTheme.style(root);
            if (!showingLogin) highlightAll();
        }

        private void buildLogin() {
            int pad = dp(18);
            root.setPadding(0, 0, 0, 0);
            android.widget.ScrollView scroll = new android.widget.ScrollView(ui);
            scroll.setFillViewport(true);
            LinearLayout form = new LinearLayout(ui);
            form.setOrientation(LinearLayout.VERTICAL);
            form.setPadding(pad, dp(8), pad, dp(24));
            form.setFocusableInTouchMode(true);
            scroll.addView(form, new android.widget.ScrollView.LayoutParams(-1, -2));
            root.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1));
            TextView intro = label("Sign in with your LivePokeMap account.", 14);
            intro.setPadding(0, 0, 0, dp(20));
            form.addView(intro);
            final EditText username = new EditText(ui);
            username.setHint("Username");
            username.setSingleLine(true);
            username.setInputType(InputType.TYPE_CLASS_TEXT);
            username.setId(View.generateViewId());
            username.setAutofillHints(View.AUTOFILL_HINT_USERNAME);
            username.setImportantForAutofill(View.IMPORTANT_FOR_AUTOFILL_YES);
            username.setImeOptions(android.view.inputmethod.EditorInfo.IME_ACTION_NEXT);
            addLoginField(form, username);
            final EditText password = new EditText(ui);
            password.setHint("Password");
            password.setSingleLine(true);
            // Let the user's password manager fill credentials; the app stores only its session.
            password.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
            password.setTypeface(android.graphics.Typeface.DEFAULT);
            password.setImeOptions(android.view.inputmethod.EditorInfo.IME_ACTION_DONE);
            password.setId(View.generateViewId());
            password.setAutofillHints(View.AUTOFILL_HINT_PASSWORD);
            password.setImportantForAutofill(View.IMPORTANT_FOR_AUTOFILL_YES);
            addLoginField(form, password);
            final Button login = button("Log in");
            login.setPadding(0, 0, 0, 0);
            form.addView(login, new LinearLayout.LayoutParams(-1, dp(48)));
            final TextView status = label("", 13);
            status.setPadding(0, dp(10), 0, 0);
            form.addView(status);
            form.requestFocus();
            password.setOnEditorActionListener((v, action, event) -> {
                if (action != android.view.inputmethod.EditorInfo.IME_ACTION_DONE) return false;
                login.performClick(); return true;
            });
            login.setOnClickListener(v -> {
                if(loginInFlight||loginPaused)return;
                String u = username.getText().toString().trim();
                String p = password.getText().toString();
                // Clear the password field immediately; it stays only in the local that is handed to
                // the worker, never logged or saved by PGRazor.
                password.setText("");
                if (u.isEmpty() || p.isEmpty()) { status.setText("Enter your username and password."); return; }
                if (loginInFlight) return;
                loginInFlight = true;
                login.setEnabled(false);
                status.setText("Signing in…");
                submitLogin(u, p, login, username, status);
            });
        }

        private void addLoginField(LinearLayout form, EditText field) {
            field.setTextSize(16);
            field.setGravity(Gravity.CENTER_VERTICAL);
            field.setPadding(dp(14), dp(12), dp(14), dp(12));
            field.setHintTextColor(MapTheme.muted());
            GradientDrawable background = new GradientDrawable();
            background.setColor(MapTheme.dark() ? 0xff202b39 : 0xffedf2f6);
            background.setCornerRadius(dp(9));
            background.setStroke(dp(1), MapTheme.muted());
            field.setBackground(background);
            field.setBackgroundTintList(null);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, dp(56));
            lp.bottomMargin = dp(16);
            form.addView(field, lp);
        }

        private void submitLogin(String username, String password, Button login, EditText userField, TextView status) {
            final int g = generation;
            net.execute(() -> {
                try {
                    client.login(username, password); // saves the session on this worker; serialized with logout
                    main.post(() -> {
                        loginInFlight = false;
                        if (closed) return;
                        userField.setText(""); // credentials cleared on completion
                        if (g != generation) return; // superseded by a logout or close
                        showingLogin = false;
                        lastError = null; note = ""; lastUpdated = 0; backoffUntilElapsed = 0;
                        entries.clear();
                        if(loginPaused)renderOnResume=true;
                        else render(); // buildFeed() kicks off the first fetch
                    });
                } catch (LiveFeedClient.FeedException fe) {
                    final String msg = loginError(fe);
                    main.post(() -> {
                        loginInFlight = false;
                        if (closed || g != generation) return; // superseded; old login views are gone
                        login.setEnabled(true); status.setText(msg);
                    });
                } catch (Exception e) {
                    main.post(() -> {
                        loginInFlight = false;
                        if (closed || g != generation) { return; }
                        login.setEnabled(true);
                        status.setText("Couldn't reach LivePokeMap. Check your connection and try again.");
                    });
                }
            });
        }

        private void buildFeed() {
            int pad = dp(14);
            root.setPadding(pad, dp(6), pad, 0);

            LinearLayout toolbar = new LinearLayout(ui);
            toolbar.setOrientation(LinearLayout.HORIZONTAL);
            Button logout = button("Log out"), refresh = button("Refresh"), filterBtn = button("Filters");
            toolbar.addView(logout, new LinearLayout.LayoutParams(0, dp(44), 1));
            toolbar.addView(refresh, new LinearLayout.LayoutParams(0, dp(44), 1));
            toolbar.addView(filterBtn, new LinearLayout.LayoutParams(0, dp(44), 1));
            root.addView(toolbar);
            logout.setOnClickListener(v -> logout());
            refresh.setOnClickListener(v -> requestFetch());
            filterBtn.setOnClickListener(v -> openFilters());

            // Feed source tabs: Nearby viewport vs the account's My Shinies list.
            LinearLayout tabRow = new LinearLayout(ui);
            tabRow.setOrientation(LinearLayout.HORIZONTAL);
            final String[] tabs = {"Nearby", "My Shinies"};
            tabButtons = new Button[2];
            for (int i = 0; i < 2; i++) {
                final boolean shinies = i == 1;
                Button b = segButton(tabs[i]);
                b.setOnClickListener(v -> selectTab(shinies));
                tabRow.addView(b, new LinearLayout.LayoutParams(0, dp(40), 1));
                tabButtons[i] = b;
            }
            root.addView(tabRow);

            if (myShinies) {
                TextView caption = label("My Shinies uses the friend code set up on the LivePokeMap website.", 12);
                caption.setPadding(0, dp(6), 0, dp(4));
                root.addView(caption);
            } else {
                TextView around = label("Around current location", 13);
                around.setPadding(0, dp(6), 0, dp(2));
                root.addView(around);

                // Categories: a compact horizontally scrollable segmented control.
                HorizontalScrollView catScroll = new HorizontalScrollView(ui);
                catScroll.setHorizontalScrollBarEnabled(false);
                LinearLayout catRow = new LinearLayout(ui);
                catRow.setOrientation(LinearLayout.HORIZONTAL);
                catButtons = new Button[CATEGORIES.length];
                for (int i = 0; i < CATEGORIES.length; i++) {
                    final int index = i;
                    Button b = segButton(CATEGORIES[i]);
                    b.setOnClickListener(v -> selectCategory(index));
                    catRow.addView(b, new LinearLayout.LayoutParams(-2, dp(40)));
                    catButtons[i] = b;
                }
                catScroll.addView(catRow);
                root.addView(catScroll);

                LinearLayout radiusRow = new LinearLayout(ui);
                radiusRow.setOrientation(LinearLayout.HORIZONTAL);
                radiusRow.setGravity(Gravity.CENTER_VERTICAL);
                radiusRow.addView(label("Radius", 13), new LinearLayout.LayoutParams(dp(58), -2));
                radiusButtons = new Button[RADII.length];
                for (int i = 0; i < RADII.length; i++) {
                    final int index = i;
                    Button b = segButton(RADIUS_LABELS[i]);
                    b.setOnClickListener(v -> selectRadius(index));
                    radiusRow.addView(b, new LinearLayout.LayoutParams(0, dp(40), 1));
                    radiusButtons[i] = b;
                }
                root.addView(radiusRow);
            }

            feedStatus = label("", 12);
            feedStatus.setPadding(0, dp(2), 0, dp(4));
            root.addView(feedStatus);

            FrameLayout listHolder = new FrameLayout(ui);
            ListView list = new ListView(ui);
            list.setAdapter(adapter);
            list.setOnItemClickListener((parent, view, pos, id) -> {
                if (pos >= 0 && pos < visible.size()) openDetails(visible.get(pos));
            });
            TextView empty = label(myShinies ? "No shinies to show." : "No live results in range.", 14);
            empty.setGravity(Gravity.CENTER);
            empty.setPadding(0, dp(24), 0, 0);
            listHolder.addView(list, new FrameLayout.LayoutParams(-1, -1));
            listHolder.addView(empty, new FrameLayout.LayoutParams(-1, -2, Gravity.CENTER_HORIZONTAL));
            list.setEmptyView(empty);
            root.addView(listHolder, new LinearLayout.LayoutParams(-1, 0, 1));

            rebuildVisible();
            adapter.notifyDataSetChanged();
            updateStatus();
            requestFetch(); // first load immediately on entering the feed
        }

        // ------------------------------------------------------------------ filters

        private void selectCategory(int index) {
            if (index == categoryIndex) return;
            categoryIndex = index; prefs.edit().putInt("category", index).apply();
            highlightAll();
            rebuildVisible(); adapter.notifyDataSetChanged(); updateStatus();
            // Category is a pure display filter: no refetch, so in-flight data stays valid.
        }

        private void selectRadius(int index) {
            if (index == radiusIndex) return;
            radiusIndex = index; prefs.edit().putInt("radius", index).apply();
            generation++; highlightAll();
            rebuildVisible(); adapter.notifyDataSetChanged();
            requestFetch();
        }

        /** Switch feed source. Bumps the generation so any in-flight fetch is dropped and no two
         *  polling scopes overlap, clears the old scope's rows, and re-renders for the new layout. */
        private void selectTab(boolean shinies) {
            if (shinies == myShinies) return;
            myShinies = shinies; prefs.edit().putBoolean("myShinies", shinies).apply();
            generation++;
            entries.clear(); visible.clear(); distCache = new HashMap<>();
            note = ""; lastError = null; lastUpdated = 0;
            refreshSeconds = DEFAULT_REFRESH_SECONDS;
            render(); // buildFeed() kicks off the first fetch for the new scope
        }

        private void openFilters() {
            if (closed || showingLogin) return;
            org.json.JSONObject working;
            try {
                working = filters.toViewportJson(); // a fresh, mutable copy the modal edits in place
            } catch (org.json.JSONException e) { toast("Couldn't open the filters."); return; }
            LiveFeedFilterDialog.show(activity, ui, myShinies, working, speciesNames, prefs,
                this::trackOwn, this::applyFilters);
        }

        /** Called by the modal on Apply. Validates the edited JSON through the immutable model; a bad
         *  value is rejected so the modal can stay open, otherwise the new set is saved and refetched. */
        private boolean applyFilters(org.json.JSONObject edited) {
            LiveFeedFilters next;
            try {
                next = LiveFeedFilters.fromJson(edited);
            } catch (org.json.JSONException e) { return false; }
            filters = next;
            entries.clear(); visible.clear(); distCache = new HashMap<>();
            saveFilters();
            generation++;
            rebuildVisible(); adapter.notifyDataSetChanged();
            requestFetch();
            return true;
        }

        private LiveFeedFilters loadFilters() {
            try {
                String stored = prefs.getString("filters_v1", null);
                if (stored != null) return LiveFeedFilters.fromJson(new org.json.JSONObject(stored));
                int legacy = clamp(prefs.getInt("minIv", 0), 0, 3);
                org.json.JSONObject initial = new LiveFeedFilters().toViewportJson();
                initial.put("min_iv", new int[]{0,80,90,100}[legacy]);
                return LiveFeedFilters.fromJson(initial);
            } catch (Exception ignored) { } // fall back to defaults on any corrupt/legacy value
            return new LiveFeedFilters();
        }

        private void saveFilters() {
            try {
                prefs.edit().putString("filters_v1", filters.toViewportJson().toString()).apply();
            } catch (org.json.JSONException ignored) { }
        }

        private void highlightAll() {
            highlight(tabButtons, myShinies ? 1 : 0);
            highlight(catButtons, categoryIndex);
            highlight(radiusButtons, radiusIndex);
        }

        private void highlight(Button[] buttons, int selected) {
            if (buttons == null) return;
            for (int i = 0; i < buttons.length; i++) {
                boolean on = i == selected;
                buttons[i].setTextColor(on ? MapTheme.accent() : MapTheme.ink());
                buttons[i].setTypeface(null, on ? android.graphics.Typeface.BOLD : android.graphics.Typeface.NORMAL);
            }
        }

        // ------------------------------------------------------------------ session

        private void logout() {
            generation++;                 // reject any in-flight login/fetch result
            backoffUntilElapsed = 0; lastError = null; note = ""; lastUpdated = 0;
            entries.clear(); visible.clear(); distCache = new HashMap<>();
            showingLogin = true;
            // Run the token clear on the same serialized worker so it can never race ahead of an
            // in-flight login save; a later login just queues behind it.
            client.logout(); // invalidate immediately, even if this dialog is closed next
            render();
        }

        // ------------------------------------------------------------------ fetching

        private void requestFetch() {
            if (closed || showingLogin || fetchInFlight) return;
            long nowE = SystemClock.elapsedRealtime();
            if (nowE < backoffUntilElapsed) { updateStatus(); return; }
            fetchInFlight = true;
            lastFetchStartedElapsed = nowE;
            final int g = generation;
            final boolean shinies = myShinies;
            final double lat = Control.engine.latitude, lon = Control.engine.longitude;
            final double radiusKm = RADII[radiusIndex];
            final LiveFeedFilters f = filters; // immutable snapshot; safe to hand to the worker
            updateStatus();
            net.execute(() -> {
                try {
                    LiveFeedData.Snapshot snap = shinies
                        ? client.fetchMyShinies(lat, lon, f)
                        : client.fetch(lat, lon, radiusKm, f);
                    main.post(() -> onSnapshot(g, snap));
                } catch (LiveFeedClient.FeedException fe) {
                    final int status = fe.status; final long retry = fe.retryAfterMs;
                    final String msg = safeMessage(fe, "Couldn't load live results right now.");
                    main.post(() -> onFetchError(g, status, retry, msg));
                } catch (Exception e) {
                    main.post(() -> onFetchError(g, 0, 0, "Couldn't reach LivePokeMap. Check your connection."));
                }
            });
        }

        private void onSnapshot(int g, LiveFeedData.Snapshot snap) {
            fetchInFlight = false;
            if (closed || showingLogin) return;
            if (g != generation) { requestFetch(); return; } // filters changed under this request
            entries.clear();
            if (snap != null && snap.entries != null) entries.addAll(snap.entries);
            refreshSeconds = snap != null && snap.refreshSeconds > 0 ? snap.refreshSeconds : DEFAULT_REFRESH_SECONDS;
            note = snap != null && snap.note != null ? snap.note : "";
            lastUpdated = snap != null && snap.fetchedAt > 0 ? snap.fetchedAt : System.currentTimeMillis();
            lastError = null;
            rebuildVisible(); adapter.notifyDataSetChanged(); updateStatus();
        }

        private void onFetchError(int g, int status, long retryAfterMs, String message) {
            fetchInFlight = false;
            if (closed) return;
            if (status == 429) backoffUntilElapsed = SystemClock.elapsedRealtime() + Math.max(retryAfterMs, MIN_BACKOFF_MS);
            if (g != generation) { requestFetch(); return; }
            if (status == 401 || !client.signedIn()) {
                // The client clears the session on a 401; drop back to the login form.
                showingLogin = true; lastError = null; entries.clear(); visible.clear();
                render();
                toast("Your LivePokeMap session ended. Please sign in again.");
                return;
            }
            lastError = message;
            updateStatus();
        }

        /** Repeats every second: drops expired rows against the wall clock, refreshes the relative
         *  "updated" time, and polls once a refresh interval has elapsed (never while in flight or
         *  backing off). Keeps the list current between network fetches with no extra requests. */
        private void tick() {
            if (closed || loginPaused) return;
            if (activity.isFinishing() || activity.isDestroyed()) {
                if (dialog.isShowing()) dialog.dismiss(); else close();
                return;
            }
            if (!showingLogin) {
                rebuildVisible(); adapter.notifyDataSetChanged(); updateStatus();
                long nowE = SystemClock.elapsedRealtime();
                if (!fetchInFlight && nowE >= backoffUntilElapsed
                    && nowE - lastFetchStartedElapsed >= refreshSeconds * 1000L) requestFetch();
            }
            main.postDelayed(tick, 1000);
        }

        private void updateStatus() {
            if (feedStatus == null) return;
            long nowE = SystemClock.elapsedRealtime();
            if (nowE < backoffUntilElapsed) {
                long s = (backoffUntilElapsed - nowE + 999) / 1000;
                feedStatus.setText("Rate limited by LivePokeMap. Retrying in " + s + "s.");
                return;
            }
            StringBuilder sb = new StringBuilder();
            if (lastError != null) sb.append(lastError).append('\n');
            if (lastUpdated == 0) {
                sb.append(fetchInFlight ? "Loading live results…" : "No results loaded yet.");
            } else {
                long age = Math.max(0, (System.currentTimeMillis() - lastUpdated) / 1000);
                sb.append(visible.size()).append(visible.size() == 1 ? " result" : " results")
                  .append(" · updated ").append(age).append("s ago");
                if (fetchInFlight) sb.append(" · refreshing…");
            }
            if (note != null && !note.isEmpty()) sb.append('\n').append(note);
            feedStatus.setText(sb.toString());
        }

        // ------------------------------------------------------------------ list model

        private void rebuildVisible() {
            long now = System.currentTimeMillis();
            final double elat = Control.engine.latitude, elon = Control.engine.longitude;
            final HashMap<LiveFeedData.Entry, Double> dist = new HashMap<>();
            List<LiveFeedData.Entry> list = new ArrayList<>();
            for (LiveFeedData.Entry e : entries) {
                if (e == null || e.expired(now)) continue;               // hide expired every pass
                // My Shinies has no category or radius scope; the server's filters are authoritative
                // for both paths, so no client-side IV gate (it would fight the always-show exceptions).
                if (!myShinies && !categoryMatches(e)) continue;
                if (!Double.isFinite(e.lat) || !Double.isFinite(e.lon)) continue;
                double meters = distanceMeters(elat, elon, e.lat, e.lon);
                if (!myShinies && meters > RADII[radiusIndex] * 1000) continue;
                dist.put(e, meters);
                list.add(e);
            }
            // Stable (merge) sort keeps the backend's order among equal distances.
            Collections.sort(list, (a, b) -> Double.compare(dist.get(a), dist.get(b)));
            visible.clear(); visible.addAll(list);
            distCache = dist;
        }

        private boolean categoryMatches(LiveFeedData.Entry e) {
            switch (categoryIndex) {
                case 1: return e.kind == LiveFeedData.Kind.POKEMON;
                case 2: return e.kind == LiveFeedData.Kind.STOP;
                case 3: return e.kind == LiveFeedData.Kind.GYM;
                case 4: return e.kind == LiveFeedData.Kind.RAID;
                case 5: return e.kind == LiveFeedData.Kind.STATION;
                default: return true;
            }
        }

        private final class FeedAdapter extends BaseAdapter {
            public int getCount() { return visible.size(); }
            public Object getItem(int position) { return visible.get(position); }
            public long getItemId(int position) { return position; }
            public View getView(int position, View convertView, ViewGroup parent) {
                RowHolder holder;
                if (convertView == null) { holder = buildRow(parent.getContext()); }
                else holder = (RowHolder) convertView.getTag();
                LiveFeedData.Entry e = visible.get(position);
                long now = System.currentTimeMillis();
                Bitmap icon = icons.iconFor(e, now);
                if (icon != null) {
                    holder.icon.setImageBitmap(icon);
                    holder.icon.setVisibility(View.VISIBLE);
                    holder.fallback.setVisibility(View.GONE);
                } else {
                    holder.icon.setImageDrawable(null);
                    holder.icon.setVisibility(View.GONE);
                    holder.fallbackShape.setColor(kindColor(e.kind));
                    holder.fallback.setText(glyphLetter(e.kind));
                    holder.fallback.setVisibility(View.VISIBLE);
                }
                String description = iconDescription(e, now);
                holder.icon.setContentDescription(description);
                holder.fallback.setContentDescription(description);
                holder.title.setText(displayName(e));
                String sub = displaySubtitle(e, now);
                Double d = distCache.get(e);
                StringBuilder line = new StringBuilder();
                if (sub != null && !sub.isEmpty()) line.append(sub);
                if (d != null) { if (line.length() > 0) line.append("  ·  "); line.append(formatDistance(d)); }
                holder.subtitle.setText(line.toString());
                return holder.view;
            }
        }

        private RowHolder buildRow(Context context) {
            RowHolder h = new RowHolder();
            LinearLayout row = new LinearLayout(context);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(Gravity.CENTER_VERTICAL);
            int pad = dp(10);
            row.setPadding(pad, pad, pad, pad);
            // A 44dp aspect-fit icon; a lettered oval sits behind it and only shows when the bundled
            // asset is missing, so a recycled row never leaks the previous entry's glyph or sprite.
            FrameLayout iconHolder = new FrameLayout(context);
            h.fallback = new TextView(context);
            h.fallback.setGravity(Gravity.CENTER);
            h.fallback.setTextColor(Color.WHITE);
            h.fallback.setTextSize(15);
            h.fallbackShape = new GradientDrawable();
            h.fallbackShape.setShape(GradientDrawable.OVAL);
            h.fallback.setBackground(h.fallbackShape);
            iconHolder.addView(h.fallback, new FrameLayout.LayoutParams(-1, -1));
            h.icon = new ImageView(context);
            h.icon.setScaleType(ImageView.ScaleType.FIT_CENTER);
            h.icon.setAdjustViewBounds(true);
            iconHolder.addView(h.icon, new FrameLayout.LayoutParams(-1, -1));
            LinearLayout.LayoutParams iconLp = new LinearLayout.LayoutParams(dp(44), dp(44));
            iconLp.rightMargin = dp(12);
            row.addView(iconHolder, iconLp);
            LinearLayout text = new LinearLayout(context);
            text.setOrientation(LinearLayout.VERTICAL);
            h.title = new TextView(context); h.title.setTextSize(15.5f); h.title.setTextColor(MapTheme.ink());
            h.subtitle = new TextView(context); h.subtitle.setTextSize(12.5f); h.subtitle.setTextColor(MapTheme.muted());
            text.addView(h.title); text.addView(h.subtitle);
            row.addView(text, new LinearLayout.LayoutParams(0, -2, 1));
            row.setTag(h);
            h.view = row;
            return h;
        }

        private static final class RowHolder {
            View view; ImageView icon; TextView fallback, title, subtitle; GradientDrawable fallbackShape;
        }

        /** Meaningful, text-independent accessibility label for a row's leading icon. */
        private String iconDescription(LiveFeedData.Entry e, long now) {
            if (e == null || e.kind == null) return "Result";
            if (e.kind == LiveFeedData.Kind.RAID)
                return e.raidHatchAt > now ? "Raid egg" : "Raid";
            return kindLabel(e.kind) + " " + displayName(e);
        }

        // ------------------------------------------------------------------ detail + actions

        private void openDetails(LiveFeedData.Entry e) {
            if (closed) return;
            long now = System.currentTimeMillis();
            if (e.expired(now)) { toast("That result just expired."); rebuildVisible(); adapter.notifyDataSetChanged(); return; }
            LinearLayout body = new LinearLayout(ui);
            body.setOrientation(LinearLayout.VERTICAL);
            int pad = dp(18);
            body.setPadding(pad, dp(8), pad, 0);
            TextView info = label(detailText(e, now), 14);
            body.addView(info);
            LinearLayout actions = new LinearLayout(ui);
            actions.setOrientation(LinearLayout.HORIZONTAL);
            actions.setPadding(0, dp(14), 0, 0);
            Button map = button("Map"), walk = button("Walk here"), teleport = button("Teleport");
            actions.addView(map, new LinearLayout.LayoutParams(0, dp(48), 1));
            actions.addView(walk, new LinearLayout.LayoutParams(0, dp(48), 1));
            actions.addView(teleport, new LinearLayout.LayoutParams(0, dp(48), 1));
            body.addView(actions);
            AlertDialog detail = new AlertDialog.Builder(ui)
                .setTitle(displayName(e))
                .setView(body)
                .setNegativeButton("Close", null)
                .create();
            trackOwn(detail);
            detail.setOnShowListener(d -> {
                MapTheme.style(body); MapTheme.styleButtons(detail);
                map.setOnClickListener(v -> openMap(e, detail));
                walk.setOnClickListener(v -> applyMove(e, false, detail));
                teleport.setOnClickListener(v -> {
                    if(closed)return;
                    if(e.expired(System.currentTimeMillis())){toast("That result just expired.");return;}
                    if(!CooldownState.valid(e.lat,e.lon)){toast("That result has no valid location.");return;}
                    AlertDialog confirmation=TeleportDialog.confirm(activity,e.lat,e.lon,(lat,lon)->{
                        applyMove(e,true,detail);return true;
                    });
                    if(confirmation!=null&&!children.contains(confirmation))children.add(confirmation);
                });
            });
            detail.show();
        }

        /** Walk/Teleport only after a fresh validity, readiness and foreground check. */
        private void applyMove(LiveFeedData.Entry e, boolean teleport, AlertDialog detail) {
            if (closed) return;
            long now = System.currentTimeMillis();
            if (e.expired(now)) { toast("That result just expired."); detail.dismiss(); rebuildVisible(); adapter.notifyDataSetChanged(); return; }
            if (!Control.canEnable()) { toast("Location adapter is not ready."); return; }
            // Avoid hasWindowFocus() here: with this detail child open the game activity does not hold
            // window focus, so rely on it still being a live, non-finishing foreground activity.
            if (activity.isFinishing() || activity.isDestroyed()) return;
            if (!Double.isFinite(e.lat) || !Double.isFinite(e.lon)) { toast("That result has no valid location."); return; }
            if (teleport) Control.teleport(e.lat, e.lon); else Control.walkTo(e.lat, e.lon);
            toast(teleport ? "Teleported to the selected result." : "Walking to the selected result.");
            detail.dismiss();   // child
            dialog.dismiss();   // parent, after a confirmed action
        }

        private void openMap(LiveFeedData.Entry e, AlertDialog detail) {
            if (!Double.isFinite(e.lat) || !Double.isFinite(e.lon)) { toast("That result has no valid location."); return; }
            try {
                // Centred on the selected result, with both Teleport and Walk callbacks. We never push
                // this live data into Control.mapPoiSnapshot; the map loads its own game POIs.
                AlertDialog map = WorldMapDialog.show(activity, e.lat, e.lon,
                    (lat, lon) -> { if (!Control.canEnable()) { toast("Location adapter is not ready."); return false; } Control.teleport(lat, lon); return true; },
                    (lat, lon) -> { if (!Control.canEnable()) { toast("Location adapter is not ready."); return false; } Control.walkTo(lat, lon); return true; });
                if (map != null) children.add(map); // dismissed with this dialog on close()
            } catch (RuntimeException ex) {
                toast("Couldn't open the map for that result.");
                return;
            }
            detail.dismiss();
        }

        private String detailText(LiveFeedData.Entry e, long now) {
            StringBuilder sb = new StringBuilder();
            sb.append(kindLabel(e.kind)).append('\n');
            String sub = displaySubtitle(e, now);
            if (sub != null && !sub.isEmpty()) sb.append(sub).append('\n');
            Double d = distCache.get(e);
            if (d != null) sb.append("Distance: ").append(formatDistance(d)).append('\n');
            if (Double.isFinite(e.lat) && Double.isFinite(e.lon))
                sb.append(String.format(Locale.US, "%.6f, %.6f\n", e.lat, e.lon));
            if (e.expiresAt > 0) {
                long remaining = e.expiresAt - now;
                sb.append(remaining > 0 ? "Expires in " + countdown(remaining) : "Expired").append('\n');
            }
            return sb.toString().trim();
        }

        private String displayName(LiveFeedData.Entry e) {
            String name = e.name == null || e.name.isEmpty() ? kindLabel(e.kind) : e.name;
            if (name.equals("#" + e.dexId) && speciesNames != null)
                return speciesNames.optString(Integer.toString(e.dexId), name);
            return name;
        }

        private String displaySubtitle(LiveFeedData.Entry e, long now) {
            String text = e.subtitle(now);
            if (e.dexId > 0 && speciesNames != null && e.kind != LiveFeedData.Kind.POKEMON) {
                String name = speciesNames.optString(Integer.toString(e.dexId), "");
                if (!name.isEmpty()) text = text.replace("#" + e.dexId, name);
            }
            return text;
        }

        // ------------------------------------------------------------------ child dialog tracking + teardown

        private void trackOwn(AlertDialog d) {
            children.add(d);
            d.setOnDismissListener(x -> children.remove(d));
        }

        private void toast(String message) {
            if (!closed) Toast.makeText(activity, message, Toast.LENGTH_SHORT).show();
        }

        void close() {
            if (closed) return;
            closed = true;
            generation++;
            main.removeCallbacksAndMessages(null);
            for (AlertDialog d : new ArrayList<>(children)) { try { d.dismiss(); } catch (RuntimeException ignored) {} }
            children.clear();
            try { client.cancel(); } catch (RuntimeException ignored) {} // disconnect any live connection off the UI
            net.shutdownNow();
        }

        // ------------------------------------------------------------------ view helpers

        private int dp(float v) { return Math.round(v * activity.getResources().getDisplayMetrics().density); }
        private TextView label(String text, int size) { TextView v = new TextView(ui); v.setText(text); v.setTextSize(size); return v; }
        private Button button(String text) { Button b = new Button(ui); b.setText(text); b.setAllCaps(false); b.setTextSize(13.5f); return b; }
        private Button segButton(String text) {
            Button b = new Button(ui); b.setText(text); b.setAllCaps(false); b.setTextSize(13);
            b.setPadding(dp(10), 0, dp(10), 0); b.setMinWidth(dp(52)); return b;
        }
    }

    // ---------------------------------------------------------------------- stateless helpers

    private static int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

    private static double distanceMeters(double lat1, double lon1, double lat2, double lon2) {
        double R = 6371000.0;
        double dLat = Math.toRadians(lat2 - lat1), dLon = Math.toRadians(lon2 - lon1);
        double a = Math.sin(dLat / 2) * Math.sin(dLat / 2)
            + Math.cos(Math.toRadians(lat1)) * Math.cos(Math.toRadians(lat2)) * Math.sin(dLon / 2) * Math.sin(dLon / 2);
        return 2 * R * Math.asin(Math.min(1.0, Math.sqrt(a)));
    }

    private static String formatDistance(double meters) {
        if (!Double.isFinite(meters)) return "";
        if (meters < 1000) return Math.round(meters) + " m";
        return String.format(Locale.US, "%.1f km", meters / 1000);
    }

    private static String countdown(long millis) {
        long total = Math.max(0, millis) / 1000;
        return String.format(Locale.US, "%d:%02d", total / 60, total % 60);
    }

    private static String glyphLetter(LiveFeedData.Kind kind) {
        if (kind == null) return "?";
        switch (kind) {
            case POKEMON: return "P";
            case STOP: return "S";
            case GYM: return "G";
            case RAID: return "R";
            case STATION: return "W";
            default: return "?";
        }
    }

    private static int kindColor(LiveFeedData.Kind kind) {
        if (kind == null) return Color.rgb(120, 120, 120);
        switch (kind) {
            case POKEMON: return Color.rgb(46, 160, 100);
            case STOP: return Color.rgb(30, 120, 210);
            case GYM: return Color.rgb(223, 124, 35);
            case RAID: return Color.rgb(151, 65, 205);
            case STATION: return Color.rgb(0, 150, 160);
            default: return Color.rgb(120, 120, 120);
        }
    }

    private static String kindLabel(LiveFeedData.Kind kind) {
        if (kind == null) return "Result";
        switch (kind) {
            case POKEMON: return "Pokémon";
            case STOP: return "PokéStop";
            case GYM: return "Gym";
            case RAID: return "Raid";
            case STATION: return "Power Spot";
            default: return "Result";
        }
    }

    private static String loginError(LiveFeedClient.FeedException fe) {
        if (fe.status == 401) return "Incorrect username or password.";
        if (fe.status == 429) {
            long s = Math.max(fe.retryAfterMs, MIN_BACKOFF_MS) / 1000;
            return "Too many attempts. Wait " + s + "s and try again.";
        }
        return safeMessage(fe, "Sign-in failed. Please try again.");
    }

    private static String safeMessage(LiveFeedClient.FeedException fe, String fallback) {
        String m = fe.getMessage();
        return m != null && !m.isEmpty() ? m : fallback;
    }
}
