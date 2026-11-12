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
 * Full-screen PoGoCoordinates feed. Shows a login form while the saved session is unsigned, then a
 * themed, filterable list of nearby live results fetched around the current {@link Control} engine
 * coordinates (never a remote account's view). Tapping a row opens a read-only detail child dialog
 * whose Map / Walk / Teleport actions are the only things that can move the location.
 *
 * <p>Networking (login/fetch) is delegated to {@link PogoFeedClient} and runs on a single serialized
 * background {@link ExecutorService}; parsing happens off the main thread inside the client. A
 * request generation counter rejects any response that arrives after the filters, a logout, or a
 * close changed what the UI expects, so stale data never lands. All timers, the connection and the
 * executor are torn down when the dialog is dismissed or the activity leaves the foreground via
 * {@link DialogLifecycle}.
 */
public final class PogoFeedDialog {
    private PogoFeedDialog() {}

    /** PGRazorX: the site's search modes, shown as the top row; Filters follows the selected one. */
    private static final PogoFeedModes.Mode[] MODES = PogoFeedModes.Mode.values();
    /** Results of the other modes (Raids, Quests, ...), kept per mode for the process lifetime. */
    private static final java.util.EnumMap<PogoFeedModes.Mode, List<PogoFeedData.Entry>> MODE_STORE =
        new java.util.EnumMap<>(PogoFeedModes.Mode.class);
    private static final java.util.EnumMap<PogoFeedModes.Mode, Long> MODE_UPDATED = new java.util.EnumMap<>(PogoFeedModes.Mode.class);
    /** Legacy preset radii, only used to migrate the old saved choice to the free km value. */
    private static final double[] LEGACY_RADII = {1, 5, 25, 100, 0};
    /** The site's own maximum radius; anything larger is searched as Global. */
    private static final double MAX_RADIUS_KM = PogoFeedClient.MAX_SEARCH_RADIUS_KM;
    /** Auto-refresh choices picked from the Auto refresh list; 0 = "Off" (manual, Refresh button only). */
    private static final int[] REFRESH_INTERVALS_SEC = {30, 60, 300, 900, 1800, 0};
    private static final String[] REFRESH_LABELS = {"30s", "1m", "5m", "15m", "30m", "Off"};
    /** PGRazorX: the same choices, spelled out for the Auto refresh picker. */
    private static final String[] REFRESH_NAMES = {"30 seconds", "1 minute", "5 minutes", "15 minutes", "30 minutes", "Off (manual)"};
    private static final int DEFAULT_REFRESH_SECONDS = 300;
    /** A 429 always backs off at least this long regardless of a shorter server Retry-After. */
    private static final long MIN_BACKOFF_MS = 30_000L;
    /** Pause between the searches of a multi-search refresh (IV → GL → UL). */
    private static final long SEARCH_GAP_MS = 10_000L;
    /** Found-Pokémon list shared by every opening of the feed (main thread only). */
    private static final List<PogoFeedData.Entry> STORE = new ArrayList<>();
    /** PGRazorX: Pokémon first found by the latest refresh (its IV/GL/UL searches); cleared when the next one starts. */
    private static final java.util.Set<String> NEW_IDS = new java.util.HashSet<>();
    private static boolean storeLoaded;
    private static final String STORE_KEY = "found_pokemon_v1";

    /** Entry point wired by the coordinator from the quick menu / {@link Pad}. */
    public static void show(Activity activity) {
        if (activity == null || activity.isFinishing() || activity.isDestroyed()) return;
        Context ui = FeedTheme.wrap(activity);
        LinearLayout root = new LinearLayout(ui);
        root.setOrientation(LinearLayout.VERTICAL);
        AlertDialog dialog = new AlertDialog.Builder(ui)
            .setTitle("PoGoCoordinates")
            .setView(root)
            .setNegativeButton("Close", null)
            .create();
        Controller controller = new Controller(activity, ui, dialog, root);
        bindLifecycle(activity, dialog, controller);
        dialog.setOnShowListener(d -> controller.attach());
        dialog.show();
    }

    /**
     * Like {@link DialogLifecycle#bind}, but while the Discord sign-in page is showing a pause of the
     * game (e.g. Android's "Save password?" / autofill sheet, or the keyboard switching apps) does not
     * dismiss the dialog, so the half-finished login is not lost. Everything else behaves as before.
     */
    private static void bindLifecycle(Activity owner, AlertDialog dialog, Controller controller) {
        android.app.Application app = owner.getApplication();
        android.app.Application.ActivityLifecycleCallbacks callbacks = new android.app.Application.ActivityLifecycleCallbacks() {
            public void onActivityPaused(Activity a) { if (a == owner && !controller.keepOpenWhilePaused()) dialog.dismiss(); }
            public void onActivityDestroyed(Activity a) { if (a == owner) dialog.dismiss(); }
            public void onActivityCreated(Activity a, android.os.Bundle b) {}
            public void onActivityStarted(Activity a) {}
            public void onActivityResumed(Activity a) {}
            public void onActivityStopped(Activity a) {}
            public void onActivitySaveInstanceState(Activity a, android.os.Bundle b) {}
        };
        app.registerActivityLifecycleCallbacks(callbacks);
        dialog.setOnDismissListener(d -> { app.unregisterActivityLifecycleCallbacks(callbacks); controller.close(); });
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
            Thread t = new Thread(task, "pgrazor-pogofeed"); t.setDaemon(true); return t;
        });
        private final PogoFeedClient client;
        private final PogoFeedIcons icons;
        private volatile org.json.JSONObject speciesNames;
        private final List<AlertDialog> children = new ArrayList<>();
        private final Runnable tick = this::tick;

        // All of the following are touched on the main thread only.
        /** The found-Pokémon list. Lives in {@link #STORE} so closing the feed doesn't empty it;
         *  only Log out (or each Pokémon's despawn) removes rows. */
        private List<PogoFeedData.Entry> entries = STORE;
        private final List<PogoFeedData.Entry> visible = new ArrayList<>();
        /** PGRazorX: section title shown above visible[i] when a section starts there (Pokémon mode), else null. */
        private final List<String> sections = new ArrayList<>();
        private HashMap<PogoFeedData.Entry, Double> distCache = new HashMap<>();
        private final FeedAdapter adapter = new FeedAdapter();
        private boolean closed;
        private boolean showingLogin;
        private int generation;               // bumped on radius/min-IV change, logout and close
        private boolean fetchInFlight, loginInFlight;
        private long lastFetchStartedElapsed; // SystemClock.elapsedRealtime
        private long lastUpdated;             // System.currentTimeMillis of last applied snapshot
        private long backoffUntilElapsed;
        private int refreshSeconds = DEFAULT_REFRESH_SECONDS;
        private String note = "", lastError;
        private PogoFeedModes.Mode mode = PogoFeedModes.Mode.POKEMON;
        /** Search radius in km typed by the user; 0 = Global (no location). */
        private double radiusKm;
        private int refreshIntervalIndex = REFRESH_INTERVALS_SEC.length - 1; // Off (default)
        /** Manual mode: no automatic refresh at all (any interval, normal or PvP league searches);
         *  searches only run from the Refresh button or when the filters/radius are changed. */
        private boolean manualMode;
        private PogoFeedFilters filters;      // immutable; replaced wholesale on Apply
        private Button[] modeButtons;
        private Button globalBtn;
        private EditText radiusInput;
        private TextView feedStatus;

        Controller(Activity activity, Context ui, AlertDialog dialog, LinearLayout root) {
            this.activity = activity; this.ui = ui; this.dialog = dialog; this.root = root;
            prefs = AccountProfiles.preferences(activity,"pgrazor-pogofeed");
            mode = PogoFeedModes.Mode.byName(prefs.getString("feedMode", "POKEMON"));
            if (prefs.contains("radiusKm")) {
                radiusKm = Math.max(0, prefs.getFloat("radiusKm", 0f));
            } else { // migrate the old preset buttons (default was Global)
                radiusKm = LEGACY_RADII[clamp(prefs.getInt("radius", 4), 0, LEGACY_RADII.length - 1)];
                prefs.edit().putFloat("radiusKm", (float) radiusKm).apply();
            }
            refreshIntervalIndex = clamp(prefs.getInt("refreshIntervalIndex", REFRESH_INTERVALS_SEC.length - 1) /* default: Off */, 0, REFRESH_INTERVALS_SEC.length - 1);
            if (prefs.getBoolean("manualMode", false)) { // migrate the old separate Auto/Manual toggle
                refreshIntervalIndex = REFRESH_INTERVALS_SEC.length - 1;
                prefs.edit().remove("manualMode").putInt("refreshIntervalIndex", refreshIntervalIndex).apply();
            }
            manualMode = REFRESH_INTERVALS_SEC[refreshIntervalIndex] == 0;
            refreshSeconds = REFRESH_INTERVALS_SEC[refreshIntervalIndex] > 0 ? REFRESH_INTERVALS_SEC[refreshIntervalIndex] : DEFAULT_REFRESH_SECONDS;
            filters = loadFilters();
            loadStore();
            entries = listFor(mode);
            // The session constructor must not touch the keystore on the main thread; token use is
            // confined to login()/fetch() which run on the network worker.
            client = new PogoFeedClient(LiveFeedSession.pogoCoordinates(activity));
            icons = new PogoFeedIcons(activity);
            showingLogin = !client.signedIn();
            net.execute(() -> {
                try (java.io.InputStream in = PgAssets.open(activity, "pgrazor/map-icons/pokemon-names.json")) {
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
            FeedTheme.fullScreen(dialog);
            dialog.getWindow().clearFlags(android.view.WindowManager.LayoutParams.FLAG_ALT_FOCUSABLE_IM);
            dialog.getWindow().setSoftInputMode(android.view.WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
            render();
            FeedTheme.styleButtons(dialog);
            // Game page: big upper-case title, no hairline, and our round close instead of the button bar.
            FeedTheme.pageTitle(dialog);
            int barId = ui.getResources().getIdentifier("buttonPanel", "id", "android");
            View bar = barId == 0 ? null : dialog.getWindow().findViewById(barId);
            if (bar != null) bar.setVisibility(View.GONE);
            main.removeCallbacks(tick);
            main.postDelayed(tick, 1000);
        }

        // ------------------------------------------------------------------ rendering

        private void render() {
            root.removeAllViews();
            feedStatus = null; modeButtons = null; globalBtn = null; radiusInput = null;
            radiusRow = filtersRow = intervalRow = null; feedCards.clear(); feedHeaders.clear();
            if (showingLogin) buildLogin(); else buildFeed();
            // The game's round close button, centred at the bottom of the page.
            FeedTheme.CloseButton close = new FeedTheme.CloseButton(ui);
            close.setOnClickListener(v -> dialog.dismiss());
            LinearLayout.LayoutParams cl = new LinearLayout.LayoutParams(dp(58), dp(58));
            cl.gravity = Gravity.CENTER_HORIZONTAL; cl.topMargin = dp(10); cl.bottomMargin = dp(8);
            root.addView(close, cl);
            FeedTheme.style(root); FeedTheme.styleGameButtons(root);
            if (!showingLogin) { styleFeedPage(); highlightAll(); }
        }

        private void buildLogin() {
            root.setPadding(0, 0, 0, 0);

            LinearLayout topBar = new LinearLayout(ui);
            topBar.setOrientation(LinearLayout.HORIZONTAL);
            topBar.setPadding(dp(12), dp(8), dp(12), dp(8));
            topBar.setGravity(Gravity.CENTER_VERTICAL);
            TextView prompt = label("Sign in with Discord on PoGoCoordinates:", 13);
            loginPrompt = prompt;
            Button doneBtn = button("Continue to Feed");
            topBar.addView(prompt, new LinearLayout.LayoutParams(0, -2, 1f));
            // "Continue to Feed" sits centred in the space beside the prompt.
            FrameLayout doneSlot = new FrameLayout(ui);
            doneSlot.addView(doneBtn, new FrameLayout.LayoutParams(-2, dp(40), Gravity.CENTER));
            topBar.addView(doneSlot, new LinearLayout.LayoutParams(0, -2, 1f));
            root.addView(topBar);

            if (feedWebView == null) {
                feedWebView = new android.webkit.WebView(ui);
                android.webkit.WebSettings ws = feedWebView.getSettings();
                ws.setJavaScriptEnabled(true);
                ws.setDomStorageEnabled(true);
                ws.setDatabaseEnabled(true);
                ws.setLoadWithOverviewMode(true);
                ws.setUseWideViewPort(true);
                // Keep Discord/hCaptcha popups and target=_blank links inside this same view.
                ws.setJavaScriptCanOpenWindowsAutomatically(true);
                ws.setSupportMultipleWindows(false);
                // Some sign-in pages refuse embedded WebViews ("; wv" in the user agent).
                String agent = ws.getUserAgentString();
                if (agent != null) ws.setUserAgentString(agent.replace("; wv)", ")").replace(" Version/4.0", ""));
                // Discord's login and hCaptcha need cookies, including third-party ones (hcaptcha.com).
                android.webkit.CookieManager cookies = android.webkit.CookieManager.getInstance();
                cookies.setAcceptCookie(true);
                cookies.setAcceptThirdPartyCookies(feedWebView, true);
                // Android's autofill "Save password?" sheet pauses the game; keep it off this page.
                if (android.os.Build.VERSION.SDK_INT >= 26)
                    feedWebView.setImportantForAutofill(View.IMPORTANT_FOR_AUTOFILL_NO_EXCLUDE_DESCENDANTS);
                ws.setSaveFormData(false);
                feedWebView.setFocusable(true);
                feedWebView.setFocusableInTouchMode(true);
                feedWebView.setWebChromeClient(new android.webkit.WebChromeClient());
                feedWebView.setWebViewClient(new android.webkit.WebViewClient() {
                    @Override
                    public boolean shouldOverrideUrlLoading(android.webkit.WebView view, android.webkit.WebResourceRequest request) {
                        if (finishDiscordSignIn(view, request.getUrl().toString())) return true;
                        String scheme = request.getUrl().getScheme();
                        // http(s) stays in the WebView. "Open in Discord app" links (discord://,
                        // intent://) can't complete the sign-in here, so ignore them instead of
                        // showing ERR_UNKNOWN_URL_SCHEME; the web login on the page still works.
                        if ("http".equalsIgnoreCase(scheme) || "https".equalsIgnoreCase(scheme)) return false;
                        showLoginNote("Use the email/password login on the page (opening the Discord app is not supported here).");
                        return true;
                    }

                    @Override
                    public void onReceivedError(android.webkit.WebView view, android.webkit.WebResourceRequest request, android.webkit.WebResourceError error) {
                        super.onReceivedError(view, request, error);
                        if (request.isForMainFrame()) {
                            showLoginNote("Couldn't load " + request.getUrl().getHost() + " (" + error.getDescription() + "). Check the connection, then long-press 'Continue to Feed' to reload.");
                            Control.event("feed-login-error", request.getUrl().getHost() + ": " + error.getDescription());
                        }
                    }

                    @Override
                    public void doUpdateVisitedHistory(android.webkit.WebView view, String url, boolean isReload) {
                        super.doUpdateVisitedHistory(view, url, isReload);
                        // Discord's web app also changes route without a page load after login.
                        logLoginStep(url);
                        if (resumeDiscordAuthorize(view, url)) return;
                        // The site is a single-page app: after /callback it stores the JWT and changes
                        // route without a new page load, so check again on every route change.
                        if (!suppressAutoLogin && url != null && url.startsWith("https://pogocoordinates.com")) scheduleJwtPoll();
                    }

                    @Override
                    public void onPageStarted(android.webkit.WebView view, String url, Bitmap favicon) {
                        super.onPageStarted(view, url, favicon);
                        logLoginStep(url);
                        if (finishDiscordSignIn(view, url)) return;
                        if (resumeDiscordAuthorize(view, url)) return;
                        // The user went to Discord to sign in again: from now on a new JWT is welcome.
                        if (url != null && url.contains("discord.com")) suppressAutoLogin = false;
                    }

                    @Override
                    public void onPageFinished(android.webkit.WebView view, String url) {
                        super.onPageFinished(view, url);
                        if (suppressAutoLogin) {
                            // Just logged out: make sure the site's old token is gone (deleteOrigin is
                            // asynchronous and may not have run yet) and do NOT auto-enter the feed.
                            view.evaluateJavascript("try{localStorage.removeItem('jwt')}catch(e){}", null);
                            return;
                        }
                        // After Discord sign-in the site stores its JWT in localStorage["jwt"].
                        // Grab it and go straight to the feed once it appears.
                        captureJwt(found -> { if (found && showingLogin && !closed) enterFeed(); });
                        if (url != null && url.startsWith("https://pogocoordinates.com/callback")) scheduleJwtPoll();
                    }
                });
                feedWebView.loadUrl(client.getDiscordLoginUrl());
            }
            if (feedWebView.getParent() instanceof ViewGroup) {
                ((ViewGroup) feedWebView.getParent()).removeView(feedWebView);
            }
            root.addView(feedWebView, new LinearLayout.LayoutParams(-1, 0, 1));

            // Continue with the JWT if present; otherwise as a guest (the API allows anonymous search
            // with a lower result cap).
            doneBtn.setOnClickListener(v -> captureJwt(found -> {
                if (!found) toast("Not signed in — continuing as guest (fewer results).");
                enterFeed();
            }));
            // Long-press "Continue" reloads the sign-in page (e.g. after a network error).
            doneBtn.setOnLongClickListener(v -> {
                if (feedWebView != null) { showLoginNote(null); feedWebView.loadUrl(client.getDiscordLoginUrl()); toast("Reloading sign-in page…"); }
                return true;
            });
        }

        private TextView loginPrompt;

        /** Same authorize link the site's "Login with Discord" button builds (fixed state "login:discord"). */
        private static final String DISCORD_AUTHORIZE =
            "https://discord.com/oauth2/authorize?client_id=1499296458753445998"
            + "&redirect_uri=https%3A%2F%2Fpogocoordinates.com%2Fcallback&response_type=code"
            + "&scope=identify+applications.commands&integration_type=1&state=login%3Adiscord";
        /** Last authorize URL actually requested by the page (preferred over the fixed copy above). */
        private String lastAuthorizeUrl;
        private long lastAuthorizeResume;
        private String lastLoggedStep;

        /**
         * After a username/password login, Discord's mobile web often lands on its own home
         * (/app, /channels/@me, /download …) instead of returning to the authorize page, so
         * the OAuth hand-off to pogocoordinates.com/callback never happens. Detect that and
         * send the WebView straight back to the authorize page (now signed in to Discord).
         */
        private boolean resumeDiscordAuthorize(android.webkit.WebView view, String url) {
            if (url == null || closed || !showingLogin) return false;
            android.net.Uri u = android.net.Uri.parse(url);
            String host = u.getHost() == null ? "" : u.getHost();
            String path = u.getPath() == null ? "/" : u.getPath();
            if (!host.equals("discord.com") && !host.endsWith(".discord.com")) return false;
            if (path.startsWith("/oauth2/authorize")) { lastAuthorizeUrl = url; return false; }
            boolean landedHome = path.equals("/") || path.startsWith("/app") || path.startsWith("/channels")
                || path.startsWith("/download") || path.startsWith("/store") || path.startsWith("/activity");
            if (!landedHome) return false;
            long now = SystemClock.elapsedRealtime();
            if (now - lastAuthorizeResume < 4000) return false; // never loop
            lastAuthorizeResume = now;
            String target = lastAuthorizeUrl != null ? lastAuthorizeUrl : DISCORD_AUTHORIZE;
            showLoginNote("Signed in to Discord — now tap Authorize to finish.");
            Control.event("feed-login", "Discord landed on " + path + "; returning to authorize");
            view.stopLoading();
            view.loadUrl(target);
            return true;
        }

        /** Records each sign-in step (host + path only, no codes/tokens) in the Run log. */
        private void logLoginStep(String url) {
            if (url == null) return;
            try {
                android.net.Uri u = android.net.Uri.parse(url);
                String step = u.getHost() + u.getPath();
                if (step.equals(lastLoggedStep)) return;
                lastLoggedStep = step;
                Control.event("feed-login-nav", step);
            } catch (RuntimeException ignored) { }
        }

        /** Shows a sign-in hint/error above the WebView (null restores the default prompt). */
        private void showLoginNote(String note) {
            if (loginPrompt == null || closed) return;
            loginPrompt.setText(note == null ? "Sign in with Discord on PoGoCoordinates:" : note);
        }

        /** Checks for the site's JWT a few times after /callback (the token is saved asynchronously). */
        private void scheduleJwtPoll() {
            for (int i = 1; i <= 8; i++) {
                main.postDelayed(() -> {
                    if (closed || !showingLogin || suppressAutoLogin || feedWebView == null) return;
                    captureJwt(found -> { if (found && showingLogin && !closed) enterFeed(); });
                }, i * 750L);
            }
        }

        /**
         * Fallback sign-in: paste the site's session token from a browser where the Discord login
         * works (pogocoordinates.com keeps it in localStorage["jwt"]).
         */
        private void askForToken() {
            if (closed) return;
            LinearLayout box = new LinearLayout(ui);
            box.setOrientation(LinearLayout.VERTICAL);
            int pad = dp(18);
            box.setPadding(pad, dp(8), pad, 0);
            TextView help = label("1. In Chrome (PC or phone) sign in at pogocoordinates.com with Discord.\n"
                + "2. PC: press F12 → Console and run:  copy(localStorage.jwt)\n"
                + "   Phone: in the address bar type  javascript:prompt('token',localStorage.jwt)  and copy the text.\n"
                + "3. Paste it below.", 12);
            box.addView(help);
            EditText input = new EditText(ui);
            input.setHint("eyJ…");
            input.setSingleLine(true);
            input.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD);
            noAutofill(input);
            box.addView(input);
            AlertDialog ask = new AlertDialog.Builder(ui)
                .setTitle("Sign in with a token")
                .setView(box)
                .setPositiveButton("Use token", null)
                .setNegativeButton("Cancel", null)
                .create();
            trackOwn(ask);
            ask.setOnShowListener(d -> {
                FeedTheme.style(box); FeedTheme.styleGameButtons(box); FeedTheme.styleButtons(ask);
                ask.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
                    String token = input.getText().toString().trim().replaceAll("^\"|\"$", "");
                    if (!token.startsWith("eyJ") || token.split("\\.").length != 3) { input.setError("That doesn't look like a token (it starts with eyJ…)."); return; }
                    net.execute(() -> {
                        boolean ok;
                        try { client.saveSession(token); ok = true; } catch (Exception e) { ok = false; }
                        final boolean saved = ok;
                        main.post(() -> {
                            if (closed) return;
                            if (!saved) { toast("Couldn't save the token."); return; }
                            ask.dismiss(); suppressAutoLogin = false;
                            toast("Signed in with token.");
                            enterFeed();
                        });
                    });
                });
            });
            ask.show();
        }

        /** True while the sign-in page is open: a pause must not close the dialog mid-login. */
        boolean keepOpenWhilePaused() { return showingLogin && !closed; }

        private void enterFeed() {
            if (closed) return;
            showingLogin = false;
            lastError = null; note = ""; lastUpdated = mode == PogoFeedModes.Mode.POKEMON ? 0 : updatedFor(mode); backoffUntilElapsed = 0;
            render(); // the stored list is kept; only Log out empties it
        }

        /** Reads localStorage["jwt"] from the login WebView and persists it off the UI thread. */
        private boolean exchangingCode;

        /**
         * Discord's redirect back to pogocoordinates.com/callback?code=…: instead of loading the site
         * (which some networks block by name), stop there and swap the code for the session at the API
         * host. True when the URL was the callback and is handled here.
         */
        private boolean finishDiscordSignIn(android.webkit.WebView view, String url) {
            if (url == null || !url.startsWith(PogoFeedClient.CALLBACK)) return false;
            android.net.Uri uri = android.net.Uri.parse(url);
            String code = uri.getQueryParameter("code"), state = uri.getQueryParameter("state");
            if (code == null || state == null || !state.endsWith(":discord")) return false;
            view.stopLoading();
            if (exchangingCode || closed) return true;
            exchangingCode = true;
            showLoginNote("Finishing sign-in…");
            final int g = generation;
            net.execute(() -> {
                String error = null;
                try { client.exchangeDiscordCode(code); }
                catch (PogoFeedClient.FeedException fe) { error = safeMessage(fe, "Sign-in failed. Try again."); }
                catch (Exception e) { error = "Couldn't reach PoGoCoordinates to finish signing in."; }
                final String failure = error;
                main.post(() -> {
                    exchangingCode = false;
                    if (closed) return;
                    if (g != generation) { net.execute(client::logout); return; }
                    Control.event("feed-login", failure == null ? "PoGoCoordinates signed in" : "PoGoCoordinates sign-in failed");
                    if (failure != null) {
                        showLoginNote(failure + " Long-press 'Continue to Feed' to start again.");
                        return;
                    }
                    suppressAutoLogin = false;
                    toast("Signed in to PoGoCoordinates.");
                    if (showingLogin) enterFeed();
                });
            });
            return true;
        }

        private void captureJwt(java.util.function.Consumer<Boolean> done) {
            if (closed || suppressAutoLogin || feedWebView == null) { done.accept(false); return; }
            feedWebView.evaluateJavascript(
                "(function(){try{return localStorage.getItem('jwt')||''}catch(e){return ''}})()",
                value -> {
                    String jwt = "";
                    try {
                        if (value != null && value.startsWith("\""))
                            jwt = String.valueOf(new org.json.JSONTokener(value).nextValue());
                    } catch (Exception ignored) { }
                    final String token = jwt.trim();
                    // A JWT is three base64url segments starting with "eyJ".
                    if (closed || !token.startsWith("eyJ") || token.split("\\.").length != 3) { done.accept(false); return; }
                    final int g = generation;
                    net.execute(() -> {
                        boolean ok;
                        try { client.saveSession(token); ok = true; } catch (Exception e) { ok = false; }
                        final boolean saved = ok;
                        main.post(() -> {
                            if (g != generation) { net.execute(client::logout); done.accept(false); return; }
                            done.accept(saved);
                        });
                    });
                });
        }

        private void buildFeed() {
            root.setPadding(dp(12), dp(2), dp(12), 0);

            // Search modes as game pills, two rows of three (Pokémon, Raids, Quests / Weather, Rocket, Power Spots).
            modeButtons = new Button[MODES.length];
            for (int r = 0; r < 2; r++) {
                LinearLayout row = new LinearLayout(ui);
                row.setOrientation(LinearLayout.HORIZONTAL);
                row.setPadding(0, r == 0 ? dp(4) : dp(6), 0, 0);
                for (int i = r * 3; i < Math.min(MODES.length, r * 3 + 3); i++) {
                    final PogoFeedModes.Mode m = MODES[i];
                    Button b = segButton(m.label);
                    b.setPadding(dp(4), 0, dp(4), 0); b.setMinWidth(0); b.setMinimumWidth(0); b.setSingleLine(true);
                    FeedUi.fitText(b, 13.5f);
                    b.setOnClickListener(v -> selectMode(m));
                    LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, dp(40), m == PogoFeedModes.Mode.POWER_SPOTS ? 1.3f : 1f);
                    lp.leftMargin = lp.rightMargin = dp(3);
                    row.addView(b, lp);
                    modeButtons[i] = b;
                }
                root.addView(row);
            }

            // SEARCH and RESULTS scroll together under the fixed mode pills: the SEARCH section is the
            // list's header, so scrolling the results moves it out of the way and the list gets the space.
            LinearLayout top = new LinearLayout(ui);
            top.setOrientation(LinearLayout.VERTICAL);
            // SEARCH: radius, filters, auto refresh and the account, as game list rows in a white card.
            top.addView(header("Search", null, null));
            LinearLayout search = card(top);
            radiusRow = FeedUi.button(ui, "Radius");
            radiusRow.setOnClickListener(v -> editRadius());
            filtersRow = FeedUi.button(ui, "Filters");
            filtersRow.setOnClickListener(v -> openFilters());
            intervalRow = FeedUi.button(ui, "Auto refresh");
            intervalRow.setOnClickListener(v -> {
                // PGRazorX: pick the interval from a list; the current one carries a tick.
                String[] names = new String[REFRESH_LABELS.length];
                for (int i = 0; i < names.length; i++)
                    names[i] = REFRESH_NAMES[i] + (i == refreshIntervalIndex ? "   \u2713" : "");
                AlertDialog dlg = FeedTheme.showListDialog(ui, "Auto refresh", names, (d, which) -> {
                    if (which == refreshIntervalIndex) return;
                    boolean wasManual = manualMode;
                    refreshIntervalIndex = which;
                    manualMode = REFRESH_INTERVALS_SEC[refreshIntervalIndex] == 0;
                    if (!manualMode) refreshSeconds = REFRESH_INTERVALS_SEC[refreshIntervalIndex];
                    // Leaving Off restarts the countdown from now instead of firing a search at once.
                    if (wasManual && !manualMode) lastFetchStartedElapsed = SystemClock.elapsedRealtime();
                    prefs.edit().putInt("refreshIntervalIndex", refreshIntervalIndex).apply();
                    updateRows();
                    toast(manualMode ? "Auto refresh off. Use Refresh to search."
                                     : "Auto refresh every " + REFRESH_LABELS[refreshIntervalIndex]);
                    updateStatus();
                });
                trackOwn(dlg);
                dlg.setOnShowListener(x -> {
                    FeedTheme.styleButtons(dlg); FeedTheme.gameFooter(dlg, null);
                    if (dlg.getWindow() != null) tintTick(dlg.getWindow().getDecorView());
                });
                dlg.show();
            });
            Button account = FeedUi.button(ui, "Account");
            ((FeedUi.RowButton) account).setValue("Log out");
            account.setOnClickListener(v -> logout());
            for (Button row : new Button[]{radiusRow, filtersRow, intervalRow, account})
                search.addView(row, new LinearLayout.LayoutParams(-1, -2));
            feedStatus = label("", 12);
            feedStatus.setPadding(0, dp(2), 0, dp(10));
            search.addView(feedStatus);
            updateRows();

            // RESULTS: its own header (with Refresh); the rows below form one white card.
            top.addView(header("Results", "↻ Refresh", this::requestFetch));
            ListView list = new ListView(ui);
            list.setDivider(null); // rows draw their own inset hairline inside the card
            list.setVerticalScrollBarEnabled(false);
            list.setClipToPadding(false);
            list.setPadding(dp(2), 0, dp(2), dp(4));
            // PGRazorX: let the cards' soft shadow draw past their own bounds, so the empty-results card's rounded
            // corners are not left with square, see-through patches where its shadow was cut off.
            list.setClipChildren(false);
            list.addHeaderView(top, null, false);
            // Empty state: a small card in place of the rows (an empty view would hide the SEARCH header too).
            FrameLayout emptyHolder = new FrameLayout(ui);
            emptyCard = new TextView(ui);
            emptyCard.setText("No live results in range.");
            emptyCard.setGravity(Gravity.CENTER);
            emptyCard.setPadding(dp(16), dp(22), dp(16), dp(22));
            emptyHolder.setClipChildren(false); emptyHolder.setClipToPadding(false);
            emptyHolder.addView(emptyCard, new FrameLayout.LayoutParams(-1, -2));
            feedCards.add(emptyCard);
            list.addFooterView(emptyHolder, null, false);
            list.setAdapter(adapter);
            list.setOnItemClickListener((parent, view, pos, id) -> {
                int i = pos - 1; // the SEARCH/RESULTS header is position 0
                if (i >= 0 && i < visible.size()) openDetails(visible.get(i));
            });
            root.addView(list, new LinearLayout.LayoutParams(-1, 0, 1));

            rebuildVisible();
            adapter.notifyDataSetChanged();
            updateStatus();
            if (!manualMode) requestFetch(); // first load immediately on entering the feed (Auto only)
        }

        // ------------------------------------------------------------------ filters

        /** Switches the search mode: its own result list and its own Filters, then one search. */
        private void selectMode(PogoFeedModes.Mode next) {
            if (next == null || next == mode) return;
            mode = next; prefs.edit().putString("feedMode", next.name()).apply();
            generation++;
            entries = listFor(mode);
            lastError = null;
            note = mode == PogoFeedModes.Mode.POKEMON ? "" : PogoFeedModes.summary(mode, PogoFeedModes.load(prefs, mode));
            lastUpdated = updatedFor(mode);
            highlightAll(); updateRows();
            rebuildVisible(); adapter.notifyDataSetChanged(); updateStatus();
            requestFetch();
        }

        /** The result list of a mode: the persisted found-Pokémon list, or the mode's in-memory list. */
        private List<PogoFeedData.Entry> listFor(PogoFeedModes.Mode m) {
            if (m == PogoFeedModes.Mode.POKEMON) return STORE;
            List<PogoFeedData.Entry> list = MODE_STORE.get(m);
            if (list == null) { list = new ArrayList<>(); MODE_STORE.put(m, list); }
            return list;
        }

        private long updatedFor(PogoFeedModes.Mode m) {
            Long t = MODE_UPDATED.get(m);
            return t == null ? 0 : t;
        }

        private Button radiusRow, filtersRow, intervalRow;
        private final List<View> feedCards = new ArrayList<>();
        private final List<TextView> feedHeaders = new ArrayList<>();

        /** Upper-case section header on the page, with an optional teal action on the right. */
        private View header(String title, String action, Runnable onAction) {
            LinearLayout h = new LinearLayout(ui);
            h.setOrientation(LinearLayout.HORIZONTAL);
            h.setGravity(Gravity.CENTER_VERTICAL);
            h.setPadding(dp(8), dp(14), dp(6), dp(6));
            TextView t = new TextView(ui); t.setText(title);
            h.addView(t, new LinearLayout.LayoutParams(0, -2, 1));
            feedHeaders.add(t);
            if (action != null) {
                TextView a = new TextView(ui); a.setText(action); a.setPadding(dp(10), dp(4), dp(4), dp(4));
                a.setClickable(true); a.setFocusable(true); a.setBackground(FeedTheme.rowRipple(ui));
                a.setOnClickListener(v -> onAction.run());
                a.setTag("action");
                h.addView(a, new LinearLayout.LayoutParams(-2, -2));
                feedHeaders.add(a);
            }
            return h;
        }

        /** A white game card added to the page. */
        private TextView emptyCard;

        private LinearLayout card(LinearLayout parent) {
            LinearLayout c = new LinearLayout(ui);
            c.setOrientation(LinearLayout.VERTICAL);
            c.setPadding(dp(FeedTheme.Size.CARD_PADDING_DP), dp(2), dp(FeedTheme.Size.CARD_PADDING_DP), dp(2));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
            lp.leftMargin = lp.rightMargin = dp(2);
            parent.addView(c, lp);
            feedCards.add(c);
            return c;
        }

        /** Game look for the feed page, re-applied after the generic dialog styling. */
        private void styleFeedPage() {
            for (View c : feedCards) { c.setBackground(FeedTheme.cardBackground(ui)); FeedTheme.elevate(c, 2.5f); }
            for (TextView t : feedHeaders) {
                if ("action".equals(t.getTag())) { t.setTextColor(FeedTheme.accent()); t.setTypeface(FeedTheme.bold()); t.setTextSize(13.5f); }
                else FeedTheme.sectionHeader(t);
            }
            for (Button b : new Button[]{radiusRow, filtersRow, intervalRow})
                if (b != null && b.getParent() instanceof View) FeedUi.styleTree(ui, (View) b.getParent());
            if (feedStatus != null) { feedStatus.setTextColor(FeedTheme.muted()); feedStatus.setTextSize(12.5f); }
            if (emptyCard != null) { emptyCard.setTextColor(FeedTheme.muted()); emptyCard.setTextSize(14); emptyCard.setVisibility(visible.isEmpty() ? View.VISIBLE : View.GONE); }
        }

        /** Current values on the SEARCH rows. */
        private void updateRows() {
            if (radiusRow instanceof FeedUi.RowButton)
                ((FeedUi.RowButton) radiusRow).setValue(radiusKm > 0 ? formatKm(radiusKm) + " km" : "Global");
            if (intervalRow instanceof FeedUi.RowButton)
                ((FeedUi.RowButton) intervalRow).setValue(REFRESH_LABELS[refreshIntervalIndex]);
            if (filtersRow instanceof FeedUi.RowButton) {
                String v = mode == PogoFeedModes.Mode.POKEMON ? pokemonSummary(filters) : PogoFeedModes.summary(mode, PogoFeedModes.load(prefs, mode));
                ((FeedUi.RowButton) filtersRow).setValue(v.length() > 28 ? v.substring(0, 27) + "…" : v);
            }
        }

        private String pokemonSummary(PogoFeedFilters f) {
            List<String> p = new ArrayList<>();
            if (f.getMinIv() > 0 || f.getMaxIv() < 100) p.add(f.getMinIv() + (f.getMaxIv() < 100 ? "–" + f.getMaxIv() : "+") + "% IV");
            if (f.isShowPvpGreat() || f.isShowPvpUltra()) p.add((f.isShowPvpGreat() ? "GL" : "") + (f.isShowPvpGreat() && f.isShowPvpUltra() ? "+" : "") + (f.isShowPvpUltra() ? "UL" : ""));
            if (f.isShinyOnly()) p.add("Shiny");
            if (PogoFeedFilters.MODE_ALLOWLIST.equals(f.getSpeciesMode())) p.add(f.getSpeciesList().size() + " species");
            return p.isEmpty() ? "Any" : PogoFeedModes.join(p, " · ");
        }

        /** Radius editor: type km (Set) or search worldwide (Global). */
        private void editRadius() {
            if (closed) return;
            LinearLayout body = new LinearLayout(ui);
            body.setOrientation(LinearLayout.VERTICAL);
            body.setPadding(dp(18), dp(4), dp(18), dp(4));
            radiusInput = new EditText(ui);
            radiusInput.setInputType(InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_DECIMAL);
            radiusInput.setSingleLine(true);
            radiusInput.setHint("km");
            radiusInput.setTextSize(16);
            radiusInput.setImeOptions(android.view.inputmethod.EditorInfo.IME_ACTION_DONE);
            noAutofill(radiusInput);
            if (radiusKm > 0) radiusInput.setText(formatKm(radiusKm));
            body.addView(radiusInput, new LinearLayout.LayoutParams(-1, dp(46)));
            TextView hint = label("Up to " + formatKm(MAX_RADIUS_KM) + " km around your current location. Global searches worldwide.", 12);
            hint.setPadding(0, dp(8), 0, 0);
            body.addView(hint);
            AlertDialog d = new AlertDialog.Builder(ui)
                .setTitle("Search radius")
                .setView(body)
                .setPositiveButton("Set", (x, w) -> applyRadiusInput())
                .setNeutralButton("Global", (x, w) -> { radiusInput.setText(""); setRadius(0); })
                .setNegativeButton("Cancel", null)
                .create();
            trackOwn(d);
            radiusInput.setOnEditorActionListener((v, actionId, ev) -> { applyRadiusInput(); d.dismiss(); return true; });
            d.setOnShowListener(x -> { FeedTheme.style(body); FeedTheme.styleButtons(d); FeedTheme.gameFooter(d, null); });
            d.show();
        }

        /** Reads the typed km value: a positive number sets the radius, empty or 0 means Global. */
        private void applyRadiusInput() {
            if (radiusInput == null) return;
            String raw = radiusInput.getText().toString().trim().replace(',', '.');
            double km;
            if (raw.isEmpty()) km = 0;
            else {
                try { km = Double.parseDouble(raw); } catch (NumberFormatException e) { toast("Type a radius in km, e.g. 2.5"); return; }
                if (!Double.isFinite(km) || km < 0) { toast("Type a radius in km, e.g. 2.5"); return; }
            }
            if (km > MAX_RADIUS_KM) toast("The site allows up to " + formatKm(MAX_RADIUS_KM) + " km — searching Global.");
            android.view.inputmethod.InputMethodManager imm = (android.view.inputmethod.InputMethodManager)
                    activity.getSystemService(Context.INPUT_METHOD_SERVICE);
            if (imm != null) imm.hideSoftInputFromWindow(radiusInput.getWindowToken(), 0);
            setRadius(km);
        }

        /** 0 (or anything above the site's max) = Global. The found list is kept. */
        private void setRadius(double km) {
            if (km > MAX_RADIUS_KM) km = 0;
            if (km == radiusKm) { highlightAll(); return; }
            radiusKm = km; prefs.edit().putFloat("radiusKm", (float) km).apply();
            generation++; highlightAll(); updateRows();
            toast(km > 0 ? "Radius: " + formatKm(km) + " km" : "Radius: Global");
            // The radius only changes what the next searches look for; the list is kept.
            rebuildVisible(); adapter.notifyDataSetChanged();
            requestFetch();
        }

        private void openFilters() {
            if (closed || showingLogin) return;
            if (mode != PogoFeedModes.Mode.POKEMON) { openModeFilters(mode); return; }
            org.json.JSONObject working;
            try {
                working = filters.toViewportJson(); // a fresh, mutable copy the modal edits in place
            } catch (org.json.JSONException e) { toast("Couldn't open the filters."); return; }
            PogoFeedFilterDialog.show(activity, ui, false, working, speciesNames, prefs,
                this::trackOwn, this::applyFilters);
        }

        /** Called by the modal on Apply. Validates the edited JSON through the immutable model; a bad
         *  value is rejected so the modal can stay open, otherwise the new set is saved and refetched. */
        private boolean applyFilters(org.json.JSONObject edited) {
            PogoFeedFilters next;
            try {
                next = PogoFeedFilters.fromJson(edited);
            } catch (org.json.JSONException e) { return false; }
            filters = next;
            // New search criteria do NOT clear the list: found Pokémon stay until their despawn hits 0.
            saveFilters(); updateRows();
            generation++;
            rebuildVisible(); adapter.notifyDataSetChanged();
            requestFetch();
            return true;
        }

        /** Filters of the other modes: loads the mode's option catalog (cached) and opens its dialog. */
        private void openModeFilters(PogoFeedModes.Mode m) {
            final String catalog = PogoFeedModeFilterDialog.catalogPath(m);
            if (catalog != null && PogoFeedModeFilterDialog.cached(catalog) == null) {
                toast("Loading " + m.label + " options…");
                net.execute(() -> {
                    org.json.JSONObject data = null;
                    try { data = client.getJson(catalog); } catch (Exception ignored) { }
                    final org.json.JSONObject got = data;
                    main.post(() -> {
                        if (closed || mode != m) return;
                        if (got != null) PogoFeedModeFilterDialog.cache(catalog, got);
                        showModeFilters(m);
                    });
                });
                return;
            }
            showModeFilters(m);
        }

        private void showModeFilters(PogoFeedModes.Mode m) {
            org.json.JSONObject working = PogoFeedModes.load(prefs, m);
            PogoFeedModeFilterDialog.show(activity, ui, m, working, this::trackOwn, edited -> {
                if (closed) return;
                PogoFeedModes.save(prefs, m, edited);
                if (mode != m) return;
                generation++;
                note = PogoFeedModes.summary(m, edited); updateRows();
                updateStatus();
                requestFetch();
            });
        }

        private PogoFeedFilters loadFilters() {
            try {
                String stored = prefs.getString("filters_v1", null);
                if (stored != null) return PogoFeedFilters.fromJson(new org.json.JSONObject(stored));
                int legacy = clamp(prefs.getInt("minIv", 0), 0, 3);
                org.json.JSONObject initial = new PogoFeedFilters().toViewportJson();
                initial.put("min_iv", new int[]{0,80,90,100}[legacy]);
                return PogoFeedFilters.fromJson(initial);
            } catch (Exception ignored) { } // fall back to defaults on any corrupt/legacy value
            return new PogoFeedFilters();
        }

        private void saveFilters() {
            try {
                prefs.edit().putString("filters_v1", filters.toViewportJson().toString()).apply();
            } catch (org.json.JSONException ignored) { }
        }

        private void highlightAll() {
            if (modeButtons != null) for (int i = 0; i < modeButtons.length; i++) if (modeButtons[i] != null) FeedTheme.gamePill(modeButtons[i], i == mode.ordinal());
            if (globalBtn != null) highlight(new Button[]{globalBtn}, radiusKm > 0 ? -1 : 0);
        }

        private void highlight(Button[] buttons, int selected) {
            if (buttons == null) return;
            for (int i = 0; i < buttons.length; i++) {
                FeedTheme.gameSelected(buttons[i], i == selected);
            }
        }

        // ------------------------------------------------------------------ session

        private void logout() {
            generation++;                 // reject any in-flight login/fetch result
            backoffUntilElapsed = 0; lastError = null; note = ""; lastUpdated = 0;
            STORE.clear(); MODE_STORE.clear(); MODE_UPDATED.clear(); visible.clear(); distCache = new HashMap<>();
            saveStore(); // Log out is the one action that empties the found-Pokémon list
            showingLogin = true;
            // Run the token clear on the same serialized worker so it can never race ahead of an
            // in-flight login save; a later login just queues behind it.
            client.logout(); // invalidate immediately, even if this dialog is closed next
            // Clear again on the serialized worker so a token save still queued there can't win.
            try { net.execute(client::logout); } catch (RuntimeException ignored) {}
            // The site's own logout is just localStorage.removeItem("jwt"); wipe the WebView storage
            // too, and block auto sign-in until the user actually goes through Discord again.
            suppressAutoLogin = true;
            try { android.webkit.WebStorage.getInstance().deleteOrigin("https://pogocoordinates.com"); } catch (RuntimeException ignored) {}
            if (feedWebView != null) {
                feedWebView.evaluateJavascript("try{localStorage.removeItem('jwt')}catch(e){}", null);
                feedWebView.destroy(); feedWebView = null;
            }
            render();
            toast("Logged out.");
        }

        // ------------------------------------------------------------------ fetching

        private android.webkit.WebView feedWebView;
        /** True right after Log out: the login page must not silently re-use a leftover JWT. */
        private boolean suppressAutoLogin;

        private void requestFetch() {
            if (closed || showingLogin || fetchInFlight) return;
            long nowE = SystemClock.elapsedRealtime();
            if (nowE < backoffUntilElapsed) { updateStatus(); return; }
            fetchInFlight = true;
            lastFetchStartedElapsed = nowE;
            final int g = generation;
            // PGRazorX: a radius search is centred on the current location (the spoofed one while the spoof is on,
            // otherwise the real one the game sees), never on a stale spoof point.
            final double lat = Control.currentLatitude(), lon = Control.currentLongitude();
            final double radiusKm = this.radiusKm > 0 ? this.radiusKm : Double.POSITIVE_INFINITY; // ∞ → Global
            if (radiusKm <= PogoFeedClient.MAX_SEARCH_RADIUS_KM && !(Double.isFinite(lat) && Double.isFinite(lon))) {
                fetchInFlight = false;
                note = "Current location not known yet. Move in the game or choose Global.";
                updateStatus();
                return;
            }
            final PogoFeedFilters f = filters;
            if (mode != PogoFeedModes.Mode.POKEMON) { fetchMode(mode, g, lat, lon, radiusKm); return; }
            NEW_IDS.clear(); // the NEW tag lasts until the next refresh
            updateStatus();

            // With PvP leagues selected (and a max rank), each league is searched one by one and its
            // results are merged into the feed as soon as they arrive; otherwise one normal search.
            // Order: IV search (priority, when IV/level filters are set) → Great League → Ultra League.
            final List<String> runs = PogoFeedClient.searchPlan(f);
            final boolean multi = runs.size() > 1;
            net.execute(() -> {
                for (int i = 0; i < runs.size(); i++) {
                    final String league = runs.get(i);
                    final boolean last = i == runs.size() - 1;
                    try {
                        if (i > 0) Thread.sleep(SEARCH_GAP_MS); // 10 s between IV / GL / UL searches
                        PogoFeedData.Snapshot snap = client.fetchLeague(lat, lon, radiusKm, f, league);
                        main.post(() -> onSnapshot(g, snap, last, multi ? (league == null ? "iv" : league) : null));
                    } catch (InterruptedException ie) {
                        return; // dialog closed
                    } catch (PogoFeedClient.FeedException fe) {
                        main.post(() -> onFetchError(g, fe.status, fe.retryAfterMs, safeMessage(fe, "Search failed.")));
                        return;
                    } catch (Exception e) {
                        main.post(() -> onFetchError(g, 0, 0, "Could not read the search results."));
                        return;
                    }
                }
            });
        }

        /** One search in a non-Pokémon mode; its rows replace that mode's list (revealed coords kept). */
        private void fetchMode(PogoFeedModes.Mode m, int g, double lat, double lon, double radiusKm) {
            final org.json.JSONObject spec = PogoFeedModes.load(prefs, m);
            String missing = PogoFeedModes.missing(m, spec);
            if (missing != null) {
                fetchInFlight = false;
                note = missing; updateStatus();
                return;
            }
            updateStatus();
            net.execute(() -> {
                try {
                    PogoFeedData.Snapshot snap = client.fetchMode(m, spec, lat, lon, radiusKm);
                    main.post(() -> onModeSnapshot(g, m, spec, snap));
                } catch (PogoFeedClient.FeedException fe) {
                    main.post(() -> onFetchError(g, fe.status, fe.retryAfterMs, safeMessage(fe, "Search failed.")));
                } catch (Exception e) {
                    main.post(() -> onFetchError(g, 0, 0, "Could not read the search results."));
                }
            });
        }

        private void onModeSnapshot(int g, PogoFeedModes.Mode m, org.json.JSONObject spec, PogoFeedData.Snapshot snap) {
            fetchInFlight = false;
            if (closed || showingLogin) return;
            if (g != generation) { requestFetch(); return; } // mode/filters/radius changed meanwhile
            long now = System.currentTimeMillis();
            List<PogoFeedData.Entry> list = listFor(m);
            HashMap<String, PogoFeedData.Entry> old = new HashMap<>();
            for (PogoFeedData.Entry e : list) if (e != null) old.put(e.id, e);
            list.clear();
            for (PogoFeedData.Entry in : snap.entries) {
                if (in == null || in.expired(now)) continue;
                PogoFeedData.Entry before = old.get(in.id);
                list.add(before != null && before.hasCoords() ? in.withCoords(before.lat, before.lon) : in);
            }
            long at = snap.fetchedAt > 0 ? snap.fetchedAt : now;
            MODE_UPDATED.put(m, at);
            if (m != mode) return;
            lastUpdated = at;
            lastError = null;
            String summary = PogoFeedModes.summary(m, spec);
            note = summary + (snap.note == null || snap.note.isEmpty() ? "" : "\n" + snap.note);
            rebuildVisible(); adapter.notifyDataSetChanged(); updateStatus();
        }

        /**
         * Merges one search result into the feed. Rows accumulate across refreshes (and across PvP
         * leagues) until their despawn timer hits 0; a row already revealed keeps its coordinates.
         */
        private void onSnapshot(int g, PogoFeedData.Snapshot snap, boolean last, String league) {
            if (last) fetchInFlight = false;
            if (closed || showingLogin) return;
            // Results of a search started before a parameter change are still real spawns: add them
            // too, then start the search with the new parameters once this run is over.
            boolean stale = g != generation;
            if (stale && last) main.post(this::requestFetch);
            long now = System.currentTimeMillis();
            int added = 0;
            final List<PogoFeedData.Entry> store = STORE; // Pokémon rows, even if another mode is showing
            if (snap != null && snap.entries != null) {
                HashMap<String, Integer> index = new HashMap<>();
                for (int i = 0; i < store.size(); i++) index.put(store.get(i).id, i);
                for (PogoFeedData.Entry in : snap.entries) {
                    if (in == null || in.expired(now)) continue;
                    Integer at = index.get(in.id);
                    if (at == null) { index.put(in.id, store.size()); store.add(in); NEW_IDS.add(in.id); added++; continue; }
                    PogoFeedData.Entry old = store.get(at);
                    PogoFeedData.Entry merged = in.withPvpFrom(old); // same Pokémon in GL and UL: keep both ranks
                    store.set(at, old.hasCoords() && !merged.hasCoords() ? merged.withCoords(old.lat, old.lon) : merged);
                }
            }
            pruneExpired(now);
            saveStore();
            MODE_UPDATED.put(PogoFeedModes.Mode.POKEMON, snap != null && snap.fetchedAt > 0 ? snap.fetchedAt : now);
            if (mode != PogoFeedModes.Mode.POKEMON) return;
            note = snap != null && snap.note != null ? snap.note : "";
            if (league != null) note = ("iv".equals(league) ? "IV search"
                    : PogoFeedClient.LEAGUE_ULTRA.equals(league) ? "Ultra League" : "Great League")
                    + ": +" + added + " new" + (last ? "" : " · next search in 10s")
                    + (note.isEmpty() ? "" : "\n" + note);
            lastUpdated = snap != null && snap.fetchedAt > 0 ? snap.fetchedAt : now;
            lastError = null;
            rebuildVisible(); adapter.notifyDataSetChanged(); updateStatus();
        }

        /** Drops rows whose despawn countdown reached 0. */
        private void pruneExpired(long now) {
            boolean removed = false;
            for (java.util.Iterator<PogoFeedData.Entry> it = entries.iterator(); it.hasNext(); ) {
                PogoFeedData.Entry e = it.next();
                if (e == null || e.expired(now)) { it.remove(); removed = true; }
            }
            if (removed && entries == STORE) saveStore();
        }

        /** Loads the saved list once per process (survives the app being killed), dropping despawned rows. */
        private void loadStore() {
            if (storeLoaded) return;
            storeLoaded = true;
            STORE.clear();
            try {
                org.json.JSONArray arr = new org.json.JSONArray(prefs.getString(STORE_KEY, "[]"));
                long now = System.currentTimeMillis();
                for (int i = 0; i < arr.length() && STORE.size() < PogoFeedData.MAX_ENTRIES; i++) {
                    org.json.JSONObject o = arr.optJSONObject(i);
                    PogoFeedData.Entry e = o == null ? null : PogoFeedData.Entry.fromJson(o);
                    if (e != null && !e.expired(now)) STORE.add(e);
                }
            } catch (Exception ignored) { }
        }

        /** Persists the list (small JSON in the app-private prefs). */
        private void saveStore() {
            try {
                org.json.JSONArray arr = new org.json.JSONArray();
                for (PogoFeedData.Entry e : STORE) if (e != null) arr.put(e.toJson());
                prefs.edit().putString(STORE_KEY, arr.toString()).apply();
            } catch (Exception ignored) { }
        }

        private void onFetchError(int g, int status, long retryAfterMs, String message) {
            fetchInFlight = false;
            if (closed) return;
            if (status == 429) backoffUntilElapsed = SystemClock.elapsedRealtime() + Math.max(retryAfterMs, MIN_BACKOFF_MS);
            if (g != generation) { requestFetch(); return; }
            if (status == 401) {
                // The client already cleared the stale JWT; send the user back to sign in.
                showingLogin = true;
                lastError = null;
                render();
                toast("Session expired. Please sign in again.");
                return;
            }
            lastError = message;
            updateStatus();
        }

        /** Repeats every second: drops expired rows against the wall clock, refreshes the relative
         *  "updated" time, and polls once a refresh interval has elapsed (never while in flight or
         *  backing off). Keeps the list current between network fetches with no extra requests. */
        private void tick() {
            if (closed) return;
            if (activity.isFinishing() || activity.isDestroyed()) {
                if (dialog.isShowing()) dialog.dismiss(); else close();
                return;
            }
            if (!showingLogin) {
                pruneExpired(System.currentTimeMillis());
                rebuildVisible(); adapter.notifyDataSetChanged(); updateStatus();
                long nowE = SystemClock.elapsedRealtime();
                if (!manualMode && !fetchInFlight && nowE >= backoffUntilElapsed
                    && nowE - lastFetchStartedElapsed >= refreshSeconds * 1000L) requestFetch();
            }
            main.postDelayed(tick, 1000);
        }

        private void updateStatus() {
            if (feedStatus == null) return;
            long nowE = SystemClock.elapsedRealtime();
            if (nowE < backoffUntilElapsed) {
                long s = (backoffUntilElapsed - nowE + 999) / 1000;
                feedStatus.setText("Rate limited by PoGoCoordinates. Retrying in " + s + "s.");
                return;
            }
            StringBuilder sb = new StringBuilder();
            if (lastError != null) sb.append(lastError).append('\n');
            if (lastUpdated == 0) {
                sb.append(fetchInFlight ? "Loading live results…"
                        : manualMode ? "Auto refresh off: tap Refresh to search." : "No results loaded yet.");
            } else {
                long age = Math.max(0, (System.currentTimeMillis() - lastUpdated) / 1000);
                sb.append(visible.size()).append(visible.size() == 1 ? " result" : " results")
                  .append(" · updated ").append(age).append("s ago");
                if (fetchInFlight) sb.append(" · refreshing…");
                else if (manualMode) sb.append(" · auto refresh off");
                else {
                    long next = Math.max(0, refreshSeconds - (SystemClock.elapsedRealtime() - lastFetchStartedElapsed) / 1000);
                    sb.append(" · next in ").append(next / 60).append(':').append(String.format(Locale.US, "%02d", next % 60));
                }
            }
            if (note != null && !note.isEmpty()) sb.append('\n').append(note);
            feedStatus.setText(sb.toString());
        }

        // ------------------------------------------------------------------ list model

        private void rebuildVisible() {
            long now = System.currentTimeMillis();
            // PGRazorX: distances from the current location (the last spoof point only while it is unknown).
            final double clat = Control.currentLatitude(), clon = Control.currentLongitude(), here = Double.isFinite(clat) && Double.isFinite(clon) ? 1 : 0;
            final double elat = here > 0 ? clat : Control.engine.latitude, elon = here > 0 ? clon : Control.engine.longitude;
            final HashMap<PogoFeedData.Entry, Double> dist = new HashMap<>();
            List<PogoFeedData.Entry> list = new ArrayList<>();
            for (PogoFeedData.Entry e : entries) {
                if (e == null || e.expired(now)) continue;               // hide expired every pass
                // The server applies the filters; no client-side IV gate (it would fight the always-show exceptions).
                // Every found Pokémon stays listed until its despawn reaches 0, whatever the current
                // radius/filters: those only shape the next searches. Rows without coordinates
                // (not revealed yet) are listed after the located ones.
                double meters;
                if (e.hasCoords()) meters = distanceMeters(elat, elon, e.lat, e.lon);
                else if (e.revealCode != null) meters = Double.POSITIVE_INFINITY;
                else continue;
                dist.put(e, meters);
                list.add(e);
            }
            sections.clear();
            if (entries == STORE) {
                // PGRazorX: NEWS (found by the latest refresh), UNSEEN (still without coordinates), then
                // SEEN by distance. Rows are kept in found order, so a later index is a newer find.
                final HashMap<PogoFeedData.Entry, Integer> found = new HashMap<>();
                for (int i = 0; i < STORE.size(); i++) found.put(STORE.get(i), i);
                List<PogoFeedData.Entry> fresh = new ArrayList<>(), unseen = new ArrayList<>(), seen = new ArrayList<>();
                for (PogoFeedData.Entry e : list) {
                    if (NEW_IDS.contains(e.id)) fresh.add(e); else if (!e.hasCoords()) unseen.add(e); else seen.add(e);
                }
                java.util.Comparator<PogoFeedData.Entry> newest = (a, b) -> Integer.compare(found.getOrDefault(b, -1), found.getOrDefault(a, -1));
                Collections.sort(fresh, newest); Collections.sort(unseen, newest);
                Collections.sort(seen, (a, b) -> Double.compare(dist.get(a), dist.get(b)));
                visible.clear();
                String[] titles = {"NEWS \u00b7 LAST SEARCH", "UNSEEN", "SEEN \u00b7 BY DISTANCE"};
                List<List<PogoFeedData.Entry>> groups = java.util.Arrays.asList(fresh, unseen, seen);
                for (int k = 0; k < 3; k++) {
                    List<PogoFeedData.Entry> group = groups.get(k);
                    for (int i = 0; i < group.size(); i++) { visible.add(group.get(i)); sections.add(i == 0 ? titles[k] : null); }
                }
            } else {
                // Stable (merge) sort keeps the backend's order among equal distances.
                Collections.sort(list, (a, b) -> Double.compare(dist.get(a), dist.get(b)));
                visible.clear(); visible.addAll(list);
                for (int i = 0; i < list.size(); i++) sections.add(null);
            }
            distCache = dist;
            if (emptyCard != null) emptyCard.setVisibility(visible.isEmpty() ? View.VISIBLE : View.GONE);
        }

        private final class FeedAdapter extends BaseAdapter {
            public int getCount() { return visible.size(); }
            public Object getItem(int position) { return visible.get(position); }
            public long getItemId(int position) { return position; }
            public View getView(int position, View convertView, ViewGroup parent) {
                RowHolder holder;
                if (convertView == null) { holder = buildRow(parent.getContext()); }
                else holder = (RowHolder) convertView.getTag();
                PogoFeedData.Entry e = visible.get(position);
                holder.view.setBackground(rowCard(position == 0, position == visible.size() - 1));
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
                // PGRazorX: country flag from the place, NEW tag for the latest refresh's finds.
                String flag = PogoFeedData.countryFlag(e.details);
                android.text.SpannableStringBuilder title = new android.text.SpannableStringBuilder(flag.isEmpty() ? "" : flag + " ");
                title.append(displayName(e));
                if (entries == STORE && NEW_IDS.contains(e.id)) {
                    title.append("  ");
                    int from = title.length(); title.append("NEW");
                    title.setSpan(new NewTag(dp(8), dp(7)), from, title.length(), android.text.Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
                }
                holder.title.setText(title);
                String section = position < sections.size() ? sections.get(position) : null;
                holder.section.setVisibility(section == null ? View.GONE : View.VISIBLE);
                holder.section.setText(section == null ? "" : section);
                String sub = displaySubtitle(e, now);
                Double d = distCache.get(e);
                StringBuilder line = new StringBuilder();
                if (sub != null && !sub.isEmpty()) line.append(sub);
                if (d != null && Double.isFinite(d)) { if (line.length() > 0) line.append("  ·  "); line.append(formatDistance(d)); }
                else if (!e.hasCoords()) { if (line.length() > 0) line.append("  ·  "); line.append("🔒 tap to get coords"); }
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
            row.setPadding(dp(16), dp(12), dp(14), dp(12));
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
            h.title = new TextView(context); h.title.setTextSize(15.5f); h.title.setTextColor(FeedTheme.ink()); h.title.setTypeface(FeedTheme.bold());
            h.subtitle = new TextView(context); h.subtitle.setTextSize(12.5f); h.subtitle.setTextColor(FeedTheme.muted());
            text.addView(h.title); text.addView(h.subtitle);
            row.addView(text, new LinearLayout.LayoutParams(0, -2, 1));
            // PGRazorX: a section title (NEWS / UNSEEN / SEEN) above the first row of each group.
            LinearLayout outer = new LinearLayout(context);
            outer.setOrientation(LinearLayout.VERTICAL);
            h.section = new TextView(context);
            h.section.setTextSize(11); h.section.setTypeface(FeedTheme.bold()); h.section.setTextColor(FeedTheme.muted());
            h.section.setLetterSpacing(.06f); h.section.setPadding(dp(16), dp(12), dp(16), 0); h.section.setVisibility(View.GONE);
            outer.addView(h.section, new LinearLayout.LayoutParams(-1, -2));
            outer.addView(row, new LinearLayout.LayoutParams(-1, -2));
            outer.setTag(h);
            h.view = outer;
            return h;
        }

        /** One slice of the RESULTS card: rounded top on the first row, rounded bottom on the last,
         *  and an inset hairline above every row but the first. */
        private android.graphics.drawable.Drawable rowCard(boolean first, boolean last) {
            float r = dp(14);
            GradientDrawable bg = new GradientDrawable();
            bg.setColor(FeedTheme.card());
            bg.setCornerRadii(new float[]{first ? r : 0, first ? r : 0, first ? r : 0, first ? r : 0, last ? r : 0, last ? r : 0, last ? r : 0, last ? r : 0});
            if (first) return bg;
            GradientDrawable line = new GradientDrawable();
            line.setColor(FeedTheme.dlgHair());
            android.graphics.drawable.LayerDrawable d = new android.graphics.drawable.LayerDrawable(new android.graphics.drawable.Drawable[]{bg, line});
            d.setLayerInset(1, dp(16), 0, dp(16), 0);
            d.setLayerGravity(1, Gravity.TOP | Gravity.FILL_HORIZONTAL);
            d.setLayerHeight(1, Math.max(1, dp(1)));
            return d;
        }

        private static final class RowHolder {
            View view; ImageView icon; TextView fallback, title, subtitle, section; GradientDrawable fallbackShape;
        }

        /** PGRazorX: small orange pill with white bold text, drawn inline after a row's title. */
        private static final class NewTag extends android.text.style.ReplacementSpan {
            private final int pad, radius;
            NewTag(int pad, int radius) { this.pad = pad; this.radius = radius; }
            private android.graphics.Paint small(android.graphics.Paint p) {
                android.graphics.Paint q = new android.graphics.Paint(p); q.setTextSize(p.getTextSize() * .66f); q.setFakeBoldText(true); return q;
            }
            @Override public int getSize(android.graphics.Paint p, CharSequence t, int start, int end, android.graphics.Paint.FontMetricsInt fm) {
                return Math.round(small(p).measureText(t, start, end)) + 2 * pad;
            }
            @Override public void draw(android.graphics.Canvas c, CharSequence t, int start, int end, float x, int top, int y, int bottom, android.graphics.Paint p) {
                android.graphics.Paint q = small(p);
                float w = q.measureText(t, start, end) + 2 * pad, h = q.getTextSize() * 1.45f, mid = y - p.getTextSize() * .36f;
                q.setColor(Color.rgb(255, 138, 61));
                c.drawRoundRect(x, mid - h / 2, x + w, mid + h / 2, radius, radius, q);
                q.setColor(Color.WHITE);
                c.drawText(t, start, end, x + pad, mid + q.getTextSize() * .36f, q);
            }
        }

        /** Meaningful, text-independent accessibility label for a row's leading icon. */
        private String iconDescription(PogoFeedData.Entry e, long now) {
            if (e == null || e.kind == null) return "Result";
            if (e.kind == PogoFeedData.Kind.RAID)
                return e.raidHatchAt > now ? "Raid egg" : "Raid";
            return kindLabel(e.kind) + " " + displayName(e);
        }

        // ------------------------------------------------------------------ detail + actions

        private void openDetails(PogoFeedData.Entry e) {
            if (closed) return;
            long now = System.currentTimeMillis();
            if (e.expired(now)) { toast("That result just expired."); rebuildVisible(); adapter.notifyDataSetChanged(); return; }
            LinearLayout body = new LinearLayout(ui);
            body.setOrientation(LinearLayout.VERTICAL);
            int pad = dp(18);
            body.setPadding(pad, dp(8), pad, 0);
            TextView info = label(detailText(e, now), 14);
            body.addView(info);
            if (!e.hasCoords()) { openLockedDetails(e, body); return; }
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
                FeedTheme.style(body); FeedTheme.styleGameButtons(body); FeedTheme.styleButtons(detail);
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

        /** Detail for a row without coordinates: one "Get coords" button that calls /reveal on demand. */
        private void openLockedDetails(PogoFeedData.Entry e, LinearLayout body) {
            Button get = button("Get coords");
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, dp(48));
            lp.topMargin = dp(14);
            body.addView(get, lp);
            AlertDialog detail = new AlertDialog.Builder(ui)
                .setTitle(displayName(e))
                .setView(body)
                .setNegativeButton("Close", null)
                .create();
            trackOwn(detail);
            detail.setOnShowListener(d -> {
                FeedTheme.style(body); FeedTheme.styleGameButtons(body); FeedTheme.styleButtons(detail);
                get.setOnClickListener(v -> {
                    get.setEnabled(false); get.setText("Getting coords…");
                    final String id = e.id.startsWith("shiny:") ? e.id.substring(6) : e.id.startsWith("pokemon:") ? e.id.substring(8) : e.id;
                    net.execute(() -> {
                        double[] ll = null; String err = null;
                        try { ll = client.revealOne(id, e.revealCode); }
                        catch (PogoFeedClient.FeedException fe) { err = safeMessage(fe, "Couldn't get the coordinates."); }
                        catch (Exception ex) { err = "Couldn't get the coordinates."; }
                        final double[] got = ll; final String error = err;
                        main.post(() -> {
                            if (closed) return;
                            if (got == null) {
                                toast(error);
                                if (detail.isShowing()) { get.setEnabled(true); get.setText("Get coords"); }
                                return;
                            }
                            PogoFeedData.Entry located = e.withCoords(got[0], got[1]);
                            int i = entries.indexOf(e);
                            if (i >= 0) { entries.set(i, located); saveStore(); }
                            rebuildVisible(); adapter.notifyDataSetChanged(); updateStatus();
                            detail.dismiss();
                            openDetails(located);
                        });
                    });
                });
            });
            detail.show();
        }

        /** Walk/Teleport only after a fresh validity, readiness and foreground check. */
        private void applyMove(PogoFeedData.Entry e, boolean teleport, AlertDialog detail) {
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

        private void openMap(PogoFeedData.Entry e, AlertDialog detail) {
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

        private String detailText(PogoFeedData.Entry e, long now) {
            StringBuilder sb = new StringBuilder();
            sb.append(kindLabel(e.kind)).append('\n');
            String sub = displaySubtitle(e, now);
            if (sub != null && !sub.isEmpty()) sb.append(sub).append('\n');
            Double d = distCache.get(e);
            if (d != null && Double.isFinite(d)) sb.append("Distance: ").append(formatDistance(d)).append('\n');
            if (!e.hasCoords()) sb.append("Coordinates locked (lookup limit). Tap Get coords.").append('\n');
            if (Double.isFinite(e.lat) && Double.isFinite(e.lon))
                sb.append(String.format(Locale.US, "%.6f, %.6f\n", e.lat, e.lon));
            if (e.expiresAt > 0) {
                long remaining = e.expiresAt - now;
                sb.append(remaining > 0 ? "Expires in " + countdown(remaining) : "Expired").append('\n');
            }
            return sb.toString().trim();
        }

        private String displayName(PogoFeedData.Entry e) {
            String name = e.name == null || e.name.isEmpty() ? kindLabel(e.kind) : e.name;
            if (name.equals("#" + e.dexId) && speciesNames != null)
                return speciesNames.optString(Integer.toString(e.dexId), name);
            return name;
        }

        private String displaySubtitle(PogoFeedData.Entry e, long now) {
            String text = e.subtitle(now);
            if (e.dexId > 0 && speciesNames != null && e.kind != PogoFeedData.Kind.POKEMON) {
                String name = speciesNames.optString(Integer.toString(e.dexId), "");
                if (!name.isEmpty()) text = text.replace("#" + e.dexId, name);
            }
            return text;
        }

        // ------------------------------------------------------------------ child dialog tracking + teardown

        /** PGRazorX: the Auto refresh list's tick in the theme accent, bold. */
        private void tintTick(View v) {
            if (v instanceof TextView) {
                CharSequence t = ((TextView) v).getText(); int at = t == null ? -1 : t.toString().lastIndexOf('\u2713');
                if (at >= 0) {
                    android.text.SpannableString sp = new android.text.SpannableString(t.toString());
                    sp.setSpan(new android.text.style.ForegroundColorSpan(FeedTheme.accent()), at, at + 1, 0);
                    sp.setSpan(new android.text.style.StyleSpan(android.graphics.Typeface.BOLD), at, at + 1, 0);
                    ((TextView) v).setText(sp);
                }
            } else if (v instanceof android.view.ViewGroup) {
                android.view.ViewGroup g = (android.view.ViewGroup) v;
                for (int i = 0; i < g.getChildCount(); i++) tintTick(g.getChildAt(i));
            }
        }

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
            try { android.webkit.CookieManager.getInstance().flush(); } catch (RuntimeException ignored) {}
            if (feedWebView != null) { try { feedWebView.destroy(); } catch (RuntimeException ignored) {} feedWebView = null; }
        }

        // ------------------------------------------------------------------ view helpers

        private int dp(float v) { return Math.round(v * activity.getResources().getDisplayMetrics().density); }
        private TextView label(String text, int size) { TextView v = new TextView(ui); v.setText(text); v.setTextSize(size); return v; }
        private Button button(String text) { Button b = new Button(ui); b.setText(text); b.setAllCaps(false); b.setTextSize(13.5f); return b; }
        private Button segButton(String text) {
            Button b = new Button(ui); b.setText(text); b.setAllCaps(false); b.setTextSize(13);
            b.setPadding(dp(10), 0, dp(10), 0); b.setMinWidth(dp(52)); return b;
        }
        private void noAutofill(View v) {
            if (android.os.Build.VERSION.SDK_INT >= 26) v.setImportantForAutofill(View.IMPORTANT_FOR_AUTOFILL_NO);
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

    private static String formatKm(double km) {
        return km == Math.rint(km) ? Long.toString((long) km) : String.format(Locale.US, "%.2f", km).replaceAll("0+$", "");
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

    private static String glyphLetter(PogoFeedData.Kind kind) {
        if (kind == null) return "?";
        switch (kind) {
            case POKEMON: return "P";
            case STOP: return "S";
            case GYM: return "G";
            case RAID: return "R";
            case STATION: return "W";
            case QUEST: return "Q";
            case WEATHER: return "☁";
            case ROCKET: return "R";
            default: return "?";
        }
    }

    private static int kindColor(PogoFeedData.Kind kind) {
        if (kind == null) return Color.rgb(120, 120, 120);
        switch (kind) {
            case POKEMON: return Color.rgb(46, 160, 100);
            case STOP: return Color.rgb(30, 120, 210);
            case GYM: return Color.rgb(223, 124, 35);
            case RAID: return Color.rgb(151, 65, 205);
            case STATION: return Color.rgb(0, 150, 160);
            case QUEST: return Color.rgb(30, 120, 210);
            case WEATHER: return Color.rgb(90, 160, 220);
            case ROCKET: return Color.rgb(200, 50, 60);
            default: return Color.rgb(120, 120, 120);
        }
    }

    private static String kindLabel(PogoFeedData.Kind kind) {
        if (kind == null) return "Result";
        switch (kind) {
            case POKEMON: return "Pokémon";
            case STOP: return "PokéStop";
            case GYM: return "Gym";
            case RAID: return "Raid";
            case STATION: return "Power Spot";
            case QUEST: return "Quest";
            case WEATHER: return "Weather";
            case ROCKET: return "Team GO Rocket";
            default: return "Result";
        }
    }

    private static String loginError(PogoFeedClient.FeedException fe) {
        if (fe.status == 401) return "Incorrect username or password.";
        if (fe.status == 429) {
            long s = Math.max(fe.retryAfterMs, MIN_BACKOFF_MS) / 1000;
            return "Too many attempts. Wait " + s + "s and try again.";
        }
        return safeMessage(fe, "Sign-in failed. Please try again.");
    }

    private static String safeMessage(PogoFeedClient.FeedException fe, String fallback) {
        String m = fe.getMessage();
        return m != null && !m.isEmpty() ? m : fallback;
    }
}
