package dev.pgrazor;

import android.app.Activity;
import android.app.AlertDialog;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Rect;
import android.content.Context;
import android.content.SharedPreferences;
import android.widget.CheckBox;
import org.json.JSONObject;
import android.os.Handler;
import android.os.Looper;
import android.text.Html;
import android.text.InputFilter;
import android.text.InputType;
import android.text.method.LinkMovementMethod;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.ScaleGestureDetector;
import android.view.View;
import android.view.ViewConfiguration;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;
import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

/** In-activity map picker. Merely browsing/selecting does not change location. */
public final class WorldMapDialog {
    /**
     * A GPX route imported or picked for the route builder. The system file picker pauses the host
     * and dismisses this dialog, so the choice is stashed here and restored as the draft when the
     * route builder is reopened. Cleared when the route is started or the draft is cleared.
     */
    static volatile GpxRoute pendingRoute;

    public interface DestinationListener {
        /** Return true after applying the destination, or false to keep the picker open. */
        boolean onDestination(double latitude, double longitude);
    }
    public interface RouteListener {
        /** Coordinates are copied in visit order; loop repeats from the first stop. Return true only after accepting the route. */
        boolean onRoute(double[][] waypoints, boolean loop);
    }

    public static AlertDialog show(Activity activity, double latitude, double longitude, DestinationListener listener) {
        return show(activity, latitude, longitude, listener, null);
    }

    public static AlertDialog show(Activity activity, double latitude, double longitude, DestinationListener listener, DestinationListener walk) {
        return show(activity,latitude,longitude,listener,walk,null);
    }

    public static AlertDialog show(Activity activity, double latitude, double longitude, DestinationListener listener, DestinationListener walk, RouteListener route) {
        if (!Double.isFinite(latitude) || !Double.isFinite(longitude) || latitude < -90 || latitude > 90 || longitude < -180 || longitude > 180)
            throw new IllegalArgumentException("Invalid map coordinates");
        Context ui = MapTheme.wrap(activity); // themed chrome; the map view itself keeps the real Activity
        ResponsiveLayout layout = new ResponsiveLayout(activity);
        layout.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(activity, 12);
        layout.setPadding(pad, pad, pad, pad);
        TextView hint = label(ui, "Blue dot: you. Red pin: destination. Drag to pan; pinch to zoom.", 13);
        layout.addView(hint);
        TextView selected = label(ui, "", 15);
        selected.setPadding(0, dp(activity, 8), 0, dp(activity, 8));
        LinearLayout selectionRow = new LinearLayout(ui);
        selectionRow.setGravity(Gravity.CENTER_VERTICAL);
        layout.addView(selected);
        Button saved = button(ui, "Saved places");
        selectionRow.addView(saved, new LinearLayout.LayoutParams(0, dp(activity, 48), 1));
        Button hotspots = button(ui, "Hotspots");
        selectionRow.addView(hotspots, new LinearLayout.LayoutParams(0, dp(activity, 48), 1));
        Button routeMode = button(ui, "Route"); routeMode.setContentDescription("Open route builder");
        routeMode.setVisibility(route == null ? View.GONE : View.VISIBLE);
        selectionRow.addView(routeMode,new LinearLayout.LayoutParams(dp(activity, 64),dp(activity, 48)));
        layout.addView(selectionRow);
        TextView attribution = label(ui, "", 12);
        attribution.setGravity(Gravity.RIGHT);
        attribution.setPadding(0, 0, 0, dp(activity, 6));
        attribution.setText(Html.fromHtml("© <a href=\"https://www.openstreetmap.org/copyright\">OpenStreetMap contributors</a>", Html.FROM_HTML_MODE_LEGACY));
        attribution.setMovementMethod(LinkMovementMethod.getInstance());
        layout.addView(attribution);
        TextView status = label(ui, "Loading map…", 12);
        MapView map = new MapView(activity, latitude, longitude, selected, status);
        final AlertDialog[] hotspotsDialog = {null}, cooldownDialog = {null};
        hotspots.setOnClickListener(v -> {
            if(hotspotsDialog[0]!=null && hotspotsDialog[0].isShowing())return;
            hotspotsDialog[0]=HotspotsDialog.show(activity,spot->{map.select(spot.latitude,spot.longitude);});
        });
        Button search = button(ui, "Search places, parks or addresses");
        layout.addView(search, new LinearLayout.LayoutParams(-1, dp(activity, 48)));
        final AlertDialog[] searchDialog = {null};
        search.setOnClickListener(v -> {
            if(searchDialog[0]!=null && searchDialog[0].isShowing()) return;
            searchDialog[0]=MapSearchDialog.show(activity, MapProjection.latitude(map.centerY),
                MapProjection.longitude(map.centerX), (lat,lon) -> map.select(lat,lon));
        });
        LinearLayout layers=new LinearLayout(ui);
        SharedPreferences preferences=AccountProfiles.preferences(activity,"pgrazor-map");
        String[] names={"Stops","Gyms","Raids"};
        for(int i=0;i<names.length;i++){
            final int bit=1<<i;CheckBox toggle=new CheckBox(ui);toggle.setText(names[i]);toggle.setTextSize(13);
            boolean enabled=preferences.getBoolean(names[i],false);if(enabled)map.setPoiLayer(bit,true);toggle.setChecked(enabled);
            final String key=names[i];toggle.setOnCheckedChangeListener((b,on)->{map.setPoiLayer(bit,on);preferences.edit().putBoolean(key,on).apply();map.refreshPois();});
            layers.addView(toggle,new LinearLayout.LayoutParams(0,-2,1));
        }
        layout.addView(layers);
        TextView coverage=label(ui,"Nearby locations loaded by the game",12);layout.addView(coverage);map.setPoiStatus(coverage);
        map.refreshPois();
        PlaceUi places = new PlaceUi(activity, ui, map, saved);
        saved.setOnClickListener(v -> places.openLibrary());
        layout.map = map;
        // The map fills the space left by the controls (see ResponsiveLayout); this is only a baseline height.
        layout.addView(map, new LinearLayout.LayoutParams(-1, dp(activity, 240)));
        CheckBox loop = new CheckBox(ui); loop.setText("Loop route"); loop.setTextSize(13);
        loop.setContentDescription("Repeat the route from the first stop after the last"); loop.setVisibility(View.GONE);
        LinearLayout routeTools = new LinearLayout(ui);
        routeTools.setGravity(Gravity.CENTER_VERTICAL);
        routeTools.addView(loop,new LinearLayout.LayoutParams(0,dp(activity,48),1));
        Button current=button(ui,"My location");
        current.setContentDescription("Center map on your current in-game location");
        current.setOnClickListener(v -> map.centerCurrentLocation());
        routeTools.addView(current,new LinearLayout.LayoutParams(dp(activity,112),dp(activity,48)));
        Button clearRoute=button(ui,"Clear route");
        clearRoute.setContentDescription("Clear all stops from the route draft");
        clearRoute.setVisibility(View.GONE);
        routeTools.addView(clearRoute,new LinearLayout.LayoutParams(dp(activity,112),dp(activity,48)));
        layout.addView(routeTools);
        LinearLayout actions = new LinearLayout(ui);
        Button globe = button(ui, "World"), center = button(ui, "Selected"), retry = button(ui, "Retry");
        // The GPX menu button only exists when the route builder is available, and stays hidden until route mode.
        Button gpx = route == null ? null : button(ui, "GPX");
        if (gpx != null) { gpx.setContentDescription("Save, import, or export a GPX route"); gpx.setVisibility(View.GONE); }
        Button[] row = gpx == null ? new Button[] {globe, center, retry} : new Button[] {globe, center, retry, gpx};
        for (Button b : row) actions.addView(b, new LinearLayout.LayoutParams(0, dp(activity, 48), 1));
        globe.setOnClickListener(v -> map.worldView());
        center.setOnClickListener(v -> map.centerSelection()); retry.setOnClickListener(v -> map.retry());
        layout.addView(actions);
        layout.addView(status);
        RouteUi routes = route == null ? null : new RouteUi(activity,ui,map,routeMode,hint,globe,center,retry,gpx,clearRoute,loop,route,walk != null);
        if(routes!=null)routes.reopen=()->WorldMapDialog.show(activity,Control.engine.latitude,Control.engine.longitude,listener,walk,route);
        AlertDialog.Builder builder = new AlertDialog.Builder(ui).setTitle("World map").setView(layout)
            .setNegativeButton("Cancel", null).setPositiveButton("Teleport", null);
        if (walk != null) builder.setNeutralButton("Walk here", null);
        AlertDialog dialog = builder.create();
        DialogLifecycle.bind(activity, dialog, () -> { if(hotspotsDialog[0]!=null)hotspotsDialog[0].dismiss(); if(cooldownDialog[0]!=null)cooldownDialog[0].dismiss(); if(searchDialog[0]!=null)searchDialog[0].dismiss(); if(routes!=null)routes.close(); places.close(); map.close(); });
        dialog.setOnShowListener(d -> {
            MapTheme.fullScreen(dialog); MapTheme.style(layout); MapTheme.styleButtons(dialog);
            if(routes!=null)routes.attach(dialog);
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
                if(routes!=null && routes.editing) { routes.start(); return; }
                if(cooldownDialog[0]!=null && cooldownDialog[0].isShowing())return;
                cooldownDialog[0]=TeleportDialog.confirm(activity,map.latitude,map.longitude,(lat,lon)->{
                    if(!listener.onDestination(lat,lon))return false;
                    dialog.dismiss();return true;
                });
            });
            if (walk != null) dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener(v -> {
                if (walk.onDestination(map.latitude, map.longitude)) dialog.dismiss();
            });
        });
        dialog.show();
        return dialog;
    }

    private static int dp(Context c, float value) { return Math.round(value * c.getResources().getDisplayMetrics().density); }
    private static TextView label(Context c, String text, int size) { TextView v = new TextView(c); v.setText(text); v.setTextSize(size); return v; }
    private static Button button(Context c, String text) { Button b = new Button(c); b.setText(text); b.setAllCaps(false); b.setTextSize(13); b.setPadding(0, 0, 0, 0); return b; }

    /** Route mode reuses the map toolbar and dialog action rather than consuming another row. */
    private static final class RouteUi {
        /** Manual builder cap; imported GPX routes may be larger, up to {@link #MAX}. */
        static final int LIMIT=50;
        static final int MAX=GpxCodec.MAX_POINTS;
        final Activity activity;
        final Context ui;
        final MapView map;
        final Button mode,add,undo,stops,gpx,clearRoute;
        final TextView hint;
        final CheckBox loop;
        final RouteListener listener;
        final boolean hasWalk;
        final List<double[]> draft=new ArrayList<>();
        final List<AlertDialog> dialogs=new ArrayList<>();
        AlertDialog parent;
        boolean editing;
        /** The current draft came from a GPX import/selection, so it may exceed the manual LIMIT. */
        boolean imported;
        String routeName="Route";
        Runnable reopen;
        boolean closed;
        RouteUi(Activity activity,Context ui,MapView map,Button mode,TextView hint,Button add,Button undo,Button stops,Button gpx,Button clearRoute,CheckBox loop,RouteListener listener,boolean hasWalk) {
            this.activity=activity;this.ui=ui;this.map=map;this.mode=mode;this.hint=hint;this.loop=loop;
            this.add=add;this.undo=undo;this.stops=stops;this.gpx=gpx;this.clearRoute=clearRoute;this.listener=listener;this.hasWalk=hasWalk;
            clearRoute.setOnClickListener(v -> clearDraft());
            mode.setOnClickListener(v -> {editing=!editing;if(editing)maybeLoadPending();refresh();});
            add.setOnClickListener(v -> {if(editing)addStop();else map.worldView();});
            undo.setOnClickListener(v -> {if(editing)undo();else map.centerSelection();});
            stops.setOnClickListener(v -> {if(editing)showStops();else map.retry();});
            gpx.setOnClickListener(v -> {if(editing)GpxRouteDialog.menu(activity,this::sourceRoute,this::onGpxChosen);});
        }
        void attach(AlertDialog parent) {this.parent=parent;if(pendingRoute!=null){editing=true;maybeLoadPending();}refresh();}
        void refresh() {
            if(draft.isEmpty())imported=false;
            boolean big=draft.size()>LIMIT;
            mode.setText(editing ? "Map" : "Route");
            mode.setContentDescription(editing ? "Leave route builder without starting" : "Open route builder");
            hint.setText(!editing ? "Blue dot: you. Red pin: destination. Tap to select; drag to pan."
                : big ? "GPX route: "+draft.size()+" stops. Tap Start to walk it in order."
                : "Route: "+draft.size()+"/"+LIMIT+" stops. Tap map, then Add stop.");
            add.setText(editing ? "Add stop" : "World"); add.setContentDescription(editing ? "Add selected destination as next route stop" : "Show world map");
            undo.setText(editing ? "Undo" : "Selected"); undo.setContentDescription(editing ? "Undo last route stop" : "Center selected destination");
            stops.setText(editing ? "Stops ("+draft.size()+")" : "Retry"); stops.setContentDescription(editing ? "View ordered route stops and clear route" : "Retry map tiles");
            gpx.setText("GPX"); gpx.setVisibility(editing ? View.VISIBLE : View.GONE);
            clearRoute.setVisibility(editing ? View.VISIBLE : View.GONE);clearRoute.setEnabled(!draft.isEmpty());
            add.setEnabled(!editing || draft.size()<LIMIT);undo.setEnabled(!editing || !draft.isEmpty());
            loop.setVisibility(editing ? View.VISIBLE : View.GONE); // route-only toggle; defaults to stopping at the last stop
            if(parent!=null) {
                parent.setTitle(editing ? "Build route" : "World map");
                Button action=parent.getButton(AlertDialog.BUTTON_POSITIVE);
                action.setText(editing ? "Start route" : "Teleport");
                action.setContentDescription(editing ? "Start route in numbered stop order" : "Teleport to selected destination");
                action.setEnabled(!editing || (draft.size()>=(imported?1:2) && draft.size()<=MAX));
                Button walk=parent.getButton(AlertDialog.BUTTON_NEUTRAL);
                if(walk!=null && hasWalk)walk.setVisibility(editing ? View.GONE : View.VISIBLE);
            }
            map.setRoute(draft);
        }
        void addStop() {
            if(draft.size()>=LIMIT)return;
            double lat=map.latitude,lon=map.longitude;
            if(!Double.isFinite(lat) || !Double.isFinite(lon) || Math.abs(lat)>90 || Math.abs(lon)>180) {
                Toast.makeText(activity,"Select valid coordinates for this stop.",Toast.LENGTH_SHORT).show();return;
            }
            draft.add(new double[]{lat,lon});refresh();
        }
        void undo() {if(!draft.isEmpty())draft.remove(draft.size()-1);refresh();}
        void clearDraft() {draft.clear();imported=false;routeName="Route";WorldMapDialog.pendingRoute=null;refresh();}
        void showStops() {
            AlertDialog.Builder builder=new AlertDialog.Builder(ui).setTitle("Route stops ("+draft.size()+(draft.size()>LIMIT?"":"/"+LIMIT)+")")
                .setNegativeButton("Close",null).setPositiveButton("Clear route",null);
            if(draft.isEmpty())builder.setMessage("Select a destination on the map and tap Add stop. Add at least two stops before starting.");
            else {
                String[] rows=new String[draft.size()];
                for(int i=0;i<rows.length;i++)rows[i]=String.format(Locale.US,"%d.  %.6f, %.6f",i+1,draft.get(i)[0],draft.get(i)[1]);
                builder.setItems(rows,(d,index) -> {double[] stop=draft.get(index);map.select(stop[0],stop[1]);});
            }
            AlertDialog dialog=builder.create();dialogs.add(dialog);
            dialog.setOnDismissListener(d -> dialogs.remove(dialog));
            dialog.setOnShowListener(d -> {
                Button clear=dialog.getButton(AlertDialog.BUTTON_POSITIVE);clear.setEnabled(!draft.isEmpty());
                clear.setOnClickListener(v -> {clearDraft();dialog.dismiss();});
            });
            dialog.show();
        }
        /** Load a chosen GPX route as the current draft; it stays an explicit Start, never auto-walks. */
        void onGpxChosen(GpxRoute route) {
            WorldMapDialog.pendingRoute=route; // survive the picker round-trip / dialog dismissal
            if(route==null)return;
            if(closed){if(reopen!=null)GpxRouteDialog.whenResumed(activity,reopen);return;}
            routeName=route.name;
            if(!editing)editing=true;
            apply(route.points());
            refresh();
            if(!draft.isEmpty())map.select(draft.get(0)[0],draft.get(0)[1]); // bring the route into view
        }
        /** Restore an imported/selected route when the builder is reopened, without clobbering a manual draft. */
        void maybeLoadPending() {
            GpxRoute pending=WorldMapDialog.pendingRoute;
            if(pending!=null && draft.isEmpty()){routeName=pending.name;apply(pending.points());if(!draft.isEmpty())map.select(draft.get(0)[0],draft.get(0)[1]);}
        }
        private void apply(double[][] points) {
            draft.clear();
            for(double[] p:points)draft.add(new double[]{p[0],p[1]});
            imported=true;
        }
        /** The current draft offered to the GPX menu for Save/Export; a placeholder name is renamed on Save. */
        GpxRoute sourceRoute() {
            if(draft.isEmpty())return null;
            try {GpxRoute current=new GpxRoute(routeName,toArray());WorldMapDialog.pendingRoute=current;return current;}
            catch(IllegalArgumentException invalid){return null;}
        }
        private double[][] toArray() {
            double[][] points=new double[draft.size()][2];
            for(int i=0;i<points.length;i++) {points[i][0]=draft.get(i)[0];points[i][1]=draft.get(i)[1];}
            return points;
        }
        void start() {
            if(draft.size()<(imported?1:2) || draft.size()>MAX)return;
            if(listener.onRoute(toArray(),loop.isChecked())){WorldMapDialog.pendingRoute=null;parent.dismiss();}
        }
        void close() {closed=true;for(AlertDialog dialog:new ArrayList<>(dialogs))dialog.dismiss();draft.clear();}
    }

    /** Child dialogs share the map's activity; selecting a saved place never invokes Teleport. */
    private static final class PlaceUi {
        final Activity activity;
        final Context ui;
        final MapView map;
        final Button opener;
        final Handler main = new Handler(Looper.getMainLooper());
        final ThreadPoolExecutor storage = new ThreadPoolExecutor(1, 1, 0, TimeUnit.MILLISECONDS, new LinkedBlockingQueue<Runnable>(8));
        final List<AlertDialog> dialogs = new ArrayList<>();
        SavedPlaces places;
        boolean closed;
        PlaceUi(Activity activity, Context ui, MapView map, Button opener) { this.activity = activity; this.ui = ui; this.map = map; this.opener = opener; }
        private SavedPlaces store() { if (places == null) places = new SavedPlaces(activity); return places; }
        private void show(AlertDialog dialog) {
            if (closed) return;
            dialogs.add(dialog);
            dialog.setOnDismissListener(d -> dialogs.remove(dialog));
            dialog.show();
        }
        void openLibrary() {
            if (closed) return;
            opener.setEnabled(false);
            storage.execute(() -> {
                try {
                    SavedPlaces.Snapshot snapshot = store().read();
                    main.post(() -> { if (!closed) { opener.setEnabled(true); showLibrary(snapshot); } });
                } catch (RuntimeException e) {
                    main.post(() -> { if (!closed) { opener.setEnabled(true); Toast.makeText(activity, "Saved places could not be opened.", Toast.LENGTH_LONG).show(); } });
                }
            });
        }
        private void showLibrary(SavedPlaces.Snapshot snapshot) {
            AlertDialog.Builder builder = new AlertDialog.Builder(ui).setTitle("Saved places (" + snapshot.entries.size() + "/50)")
                .setNegativeButton("Close", null).setPositiveButton("Save selected", null);
            final AlertDialog[] holder = new AlertDialog[1];
            if (snapshot.entries.isEmpty()) builder.setMessage(snapshot.damaged ? "Saved places could not be read. You can save new destinations here." : "No saved places yet. Save the selected map destination to find it again later.");
            else {
                String[] names = new String[snapshot.entries.size()];
                for (int i = 0; i < names.length; i++) {
                    SavedPlaces.Entry entry = snapshot.entries.get(i);
                    names[i] = String.format(Locale.US, "%s\n%.6f, %.6f", entry.name, entry.latitude, entry.longitude);
                }
                builder.setItems(names, (dialog, which) -> showPlace(snapshot.entries.get(which)));
            }
            holder[0] = builder.create();
            holder[0].setOnShowListener(d -> holder[0].getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> showSave(holder[0])));
            show(holder[0]);
            if (snapshot.damaged && !snapshot.entries.isEmpty()) Toast.makeText(activity, "Some saved places could not be read. Valid entries are shown.", Toast.LENGTH_LONG).show();
        }
        private void showSave(AlertDialog library) {
            final double latitude = map.latitude, longitude = map.longitude;
            LinearLayout form = new LinearLayout(ui); form.setOrientation(LinearLayout.VERTICAL);
            int pad = dp(activity, 20); form.setPadding(pad, 0, pad, 0);
            form.addView(label(ui, String.format(Locale.US, "%.6f, %.6f", latitude, longitude), 14));
            EditText name = new EditText(ui); name.setHint("Place name (up to 40 characters)"); name.setSingleLine(true);
            name.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_CAP_SENTENCES);
            name.setFilters(new InputFilter[] {new InputFilter.LengthFilter(SavedPlaces.NAME_LIMIT)});
            form.addView(name);
            AlertDialog dialog = new AlertDialog.Builder(ui).setTitle("Save selected place").setView(form)
                .setNegativeButton("Cancel", null).setPositiveButton("Save", null).create();
            dialog.setOnShowListener(d -> dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
                final SavedPlaces.Entry entry;
                try { entry = new SavedPlaces.Entry(name.getText().toString(), latitude, longitude); }
                catch (IllegalArgumentException e) { name.setError(e.getMessage()); return; }
                dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(false);
                storage.execute(() -> {
                    try {
                        store().add(entry);
                        main.post(() -> {
                            if (!closed && dialog.isShowing()) {
                                dialog.dismiss(); library.dismiss(); Toast.makeText(activity, "Saved " + entry.name, Toast.LENGTH_SHORT).show(); openLibrary();
                            }
                        });
                    } catch (IOException | RuntimeException e) {
                        main.post(() -> { if (!closed && dialog.isShowing()) { dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(true); name.setError(e.getMessage()); } });
                    }
                });
            }));
            show(dialog);
        }
        private void showPlace(SavedPlaces.Entry entry) {
            AlertDialog dialog = new AlertDialog.Builder(ui).setTitle(entry.name)
                .setMessage(String.format(Locale.US, "%.6f, %.6f\n\nSelect shows this destination on the map. Choose Teleport or Walk here there to apply it.", entry.latitude, entry.longitude))
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Select on map", (d, which) -> map.select(entry.latitude, entry.longitude))
                .setNeutralButton("Delete", null).create();
            dialog.setOnShowListener(d -> dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener(v -> confirmDelete(entry, dialog)));
            show(dialog);
        }
        private void confirmDelete(SavedPlaces.Entry entry, AlertDialog details) {
            AlertDialog confirm = new AlertDialog.Builder(ui).setTitle("Delete saved place?")
                .setMessage(entry.name + " will be removed from your saved places. The current location stays unchanged.")
                .setNegativeButton("Cancel", null).setPositiveButton("Delete", null).create();
            confirm.setOnShowListener(d -> confirm.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
                confirm.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(false);
                storage.execute(() -> {
                    try {
                        store().remove(entry.id);
                        main.post(() -> { if (!closed && confirm.isShowing()) { confirm.dismiss(); details.dismiss(); openLibrary(); } });
                    } catch (IOException | RuntimeException e) {
                        main.post(() -> {
                            if (!closed && confirm.isShowing()) {
                                confirm.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(true);
                                Toast.makeText(activity, "Could not delete place. Please retry.", Toast.LENGTH_LONG).show();
                            }
                        });
                    }
                });
            }));
            show(confirm);
        }
        void close() {
            closed = true;
            for (AlertDialog dialog : new ArrayList<>(dialogs)) dialog.dismiss();
            main.removeCallbacksAndMessages(null);
            storage.shutdown(); // Finish any explicitly requested persistence write; callbacks ignore the closed UI.
        }
    }

    /** Give the map the space left after controls, rather than guessing from physical screen orientation. */
    private static final class ResponsiveLayout extends LinearLayout {
        View map;
        final int maximumMapHeight;
        ResponsiveLayout(Activity activity) { super(activity); maximumMapHeight = dp(activity, 420); }
        protected void onMeasure(int widthSpec, int heightSpec) {
            if (map != null) {
                int reserved = getPaddingTop() + getPaddingBottom();
                for (int i = 0; i < getChildCount(); i++) {
                    View child = getChildAt(i);
                    if (child == map || child.getVisibility() == View.GONE) continue;
                    measureChildWithMargins(child, widthSpec, 0, MeasureSpec.makeMeasureSpec(0, MeasureSpec.UNSPECIFIED), 0);
                    LinearLayout.LayoutParams params = (LinearLayout.LayoutParams)child.getLayoutParams();
                    reserved += child.getMeasuredHeight() + params.topMargin + params.bottomMargin;
                }
                // Fill the space left by the controls so the actions stay visible; never a fixed height that pushes them off screen.
                int mode = MeasureSpec.getMode(heightSpec);
                int available;
                if (mode == MeasureSpec.UNSPECIFIED) available = maximumMapHeight;
                else {
                    available = Math.max(0, MeasureSpec.getSize(heightSpec) - reserved);

                }
                map.getLayoutParams().height = available;
            }
            super.onMeasure(widthSpec, heightSpec);
        }
    }

    private static final class MapView extends View {
        private final TextView selectionLabel;
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
        private final int tileSize, slop;
        private final MapTileSource tiles;
        private final MapPoiLayer poiLayer;
        private List<double[]> route = Collections.emptyList();
        private volatile boolean closed;
        private double centerX, centerY, latitude, longitude;
        private int zoom = 14;
        private double zoomScale=1;
        private final ScaleGestureDetector pinch;
        private float downX, downY, previousX, previousY;
        private boolean dragged, multiTouch;
        private final Runnable locationRefresh = new Runnable() {
            public void run() {
                if(closed) return;
                invalidate(); // Refresh the live player marker without requesting new tiles.
                postDelayed(this, 250);
            }
        };

        MapView(Activity activity, double latitude, double longitude, TextView selection, TextView status) {
            super(activity);
            this.latitude = latitude; this.longitude = longitude;
            centerX = MapProjection.x(longitude); centerY = MapProjection.y(latitude);
            selectionLabel = selection;
            tileSize = dp(activity, 256); slop = ViewConfiguration.get(activity).getScaledTouchSlop();
            tiles = new MapTileSource(new MapTileSource.Host() {
                public int zoom() { return zoom; }
                public void onTilesChanged() { invalidate(); }
            }, status, new File(activity.getCacheDir(), "pgrazor-map-tiles-v1"));
            poiLayer = new MapPoiLayer(activity, MapTheme.wrap(activity), new MapPoiLayer.Projector() {
                public float screenX(double lon) { return projectX(lon); }
                public float screenY(double lat) { return projectY(lat); }
                public int width() { return getWidth(); }
                public int height() { return getHeight(); }
            }, new MapPoiLayer.Snapshots() {
                public void setOpen(boolean open) { Control.setMapPoiOpen(open); }
                public String read() { return Control.mapPoiSnapshot(); }
            }, this::invalidate);
            setContentDescription("World map. Blue dot is your current location; red pin is the destination. Drag to pan, pinch to zoom, tap to select destination.");
            // Use the activity hardware renderer; tile I/O stays on the worker.
            pinch=new ScaleGestureDetector(activity,new ScaleGestureDetector.SimpleOnScaleGestureListener(){
                public boolean onScaleBegin(ScaleGestureDetector detector){multiTouch=true;return true;}
                public boolean onScale(ScaleGestureDetector detector){
                    double old=world(),minimum=Math.max(1,Math.min(getWidth(),getHeight()));
                    double next=Math.max(minimum,Math.min(tileSize*Math.pow(2,19),old*detector.getScaleFactor()));
                    double dx=detector.getFocusX()-getWidth()/2.0,dy=detector.getFocusY()-getHeight()/2.0;
                    double anchorX=MapProjection.offsetX(centerX,dx,old),anchorY=MapProjection.offsetY(centerY,dy,old);
                    zoom=Math.max(0,Math.min(19,(int)Math.floor(Math.log(next/tileSize)/Math.log(2))));
                    zoomScale=next/((zoom==0?minimum:tileSize)*(1<<zoom));
                    centerX=MapProjection.offsetX(anchorX,-dx,next);centerY=MapProjection.offsetY(anchorY,-dy,next);
                    viewportChanged();return true;
                }
            });
            updateSelection();
        }

        void setPoiLayer(int bit, boolean on) { poiLayer.setLayer(bit, on); }
        void setPoiStatus(TextView view) { poiLayer.setStatusView(view); }
        void refreshPois() { poiLayer.refresh(); }

        private void updateSelection() {
            selectionLabel.setText(String.format(Locale.US, "Selected: %.6f, %.6f", latitude, longitude));
        }
        private double displayedTileSize() { return (zoom == 0 ? Math.max(1, Math.min(getWidth(), getHeight())) : tileSize)*zoomScale; }
        private double world() { return MapProjection.worldPixels(zoom, displayedTileSize()); }
        void zoom(int amount) { int next = Math.max(0, Math.min(19, zoom + amount)); if (next != zoom) { zoom = next; viewportChanged(); } }
        void worldView() { zoomScale=1; zoom = 0; centerX = .5; centerY = .5; viewportChanged(); }
        void centerCurrentLocation() {
            double lat=Control.engine.latitude,lon=Control.engine.longitude;
            if(!Double.isFinite(lat)||!Double.isFinite(lon)||Math.abs(lat)>90||Math.abs(lon)>180)return;
            zoomScale=1;if(zoom==0)zoom=14;
            centerX=MapProjection.x(lon);centerY=MapProjection.y(lat);viewportChanged();
        }
        void centerSelection() { zoomScale=1; if (zoom == 0) zoom = 14; centerX = MapProjection.x(longitude); centerY = MapProjection.y(latitude); viewportChanged(); }
        void select(double latitude, double longitude) {
            this.latitude = latitude; this.longitude = longitude;
            zoomScale=1; zoom = 14; centerX = MapProjection.x(longitude); centerY = MapProjection.y(latitude);
            updateSelection(); viewportChanged();
        }
        void retry() { tiles.clearFailures(); viewportChanged(); }
        void setRoute(List<double[]> points) {
            List<double[]> copy=new ArrayList<>(points.size());
            for(double[] point:points)copy.add(new double[]{point[0],point[1]});
            route=copy;invalidate();
        }
        protected void onSizeChanged(int w, int h, int oldw, int oldh) { viewportChanged(); }

        private void viewportChanged() {
            if (closed || getWidth() <= 0 || getHeight() <= 0) return;
            if (zoom == 0 && zoomScale<=1) { centerX = .5; centerY = .5; }
            int count = 1 << zoom;
            double pixels = displayedTileSize();
            double originX = centerX * world() - getWidth() / 2.0;
            double originY = centerY * world() - getHeight() / 2.0;
            int left = zoom == 0 ? 0 : (int)Math.floor(originX / pixels), top = (int)Math.floor(originY / pixels);
            int right = zoom == 0 ? 0 : (int)Math.floor((originX + getWidth() - 1) / pixels), bottom = (int)Math.floor((originY + getHeight() - 1) / pixels);
            List<MapTileSource.Tile> placed = new ArrayList<>();
            for (int y = Math.max(0, top); y <= Math.min(count - 1, bottom); y++) {
                for (int x = left; x <= right && placed.size() < 32; x++) {
                    placed.add(new MapTileSource.Tile(zoom, Math.floorMod(x, count), y, (float)(x * pixels - originX), (float)(y * pixels - originY)));
                }
            }
            tiles.setViewport(placed);
            invalidate();
        }

        protected void onDraw(Canvas canvas) {
            canvas.drawColor(Color.rgb(225, 231, 234));
            paint.setStyle(Paint.Style.FILL);
            for (MapTileSource.Tile tile : tiles.tiles()) {
                Bitmap bitmap = tiles.bitmap(tile.key);
                float pixels = (float)displayedTileSize();
                RectF bounds = new RectF(tile.left, tile.top, tile.left + pixels, tile.top + pixels);
                if (bitmap != null) canvas.drawBitmap(bitmap, null, bounds, paint);
                else if(!drawParent(canvas,tile,bounds)) {
                    paint.setColor(Color.rgb(202, 211, 216)); paint.setStyle(Paint.Style.STROKE); canvas.drawRect(bounds, paint); paint.setStyle(Paint.Style.FILL);
                }
            }
            poiLayer.draw(canvas);
            drawRoute(canvas,false);
            float markerX = (float)(getWidth() / 2.0 + MapProjection.deltaX(MapProjection.x(longitude), centerX) * world());
            float markerY = (float)(getHeight() / 2.0 + (MapProjection.y(latitude) - centerY) * world());
            float radius = Math.max(8, tileSize / 28f);
            paint.setColor(Color.WHITE); canvas.drawCircle(markerX, markerY, radius + 3, paint);
            paint.setColor(Color.rgb(197, 35, 52)); canvas.drawCircle(markerX, markerY, radius, paint);
            paint.setColor(Color.WHITE); canvas.drawCircle(markerX, markerY, radius / 3, paint);
            drawRoute(canvas,true);
            drawCurrentLocation(canvas);
        }

        /** Independent of the destination: selecting or searching never changes this position. */
        private void drawCurrentLocation(Canvas canvas) {
            double lat=Control.engine.latitude,lon=Control.engine.longitude;
            if(!Double.isFinite(lat)||!Double.isFinite(lon)||Math.abs(lat)>90||Math.abs(lon)>180)return;
            float x=projectX(lon),y=projectY(lat),radius=dp(getContext(),5);
            if(x < -radius*3 || x > getWidth()+radius*3 || y < -radius*3 || y > getHeight()+radius*3)return;
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(0x403493fa);canvas.drawCircle(x,y,radius*2.5f,paint);
            paint.setColor(Color.WHITE);canvas.drawCircle(x,y,radius+dp(getContext(),2),paint);
            paint.setColor(0xff1674e8);canvas.drawCircle(x,y,radius,paint);
        }

        protected void onAttachedToWindow() {
            super.onAttachedToWindow();
            if(!closed){removeCallbacks(locationRefresh);post(locationRefresh);}
        }

        private boolean drawParent(Canvas canvas,MapTileSource.Tile tile,RectF bounds){
            for(int z=tile.zoom-1;z>=Math.max(0,tile.zoom-5);z--){int factor=1<<(tile.zoom-z);Bitmap parent=tiles.bitmap(z+"-"+(tile.x/factor)+"-"+(tile.y/factor));if(parent==null)continue;
                int size=256/factor,x=(tile.x%factor)*size,y=(tile.y%factor)*size;
                canvas.drawBitmap(parent,new Rect(x,y,x+size,y+size),bounds,paint);return true;
            }return false;
        }
        private float projectX(double lon){return (float)(getWidth()/2.0+MapProjection.deltaX(MapProjection.x(lon),centerX)*world());}
        private float projectY(double lat){return (float)(getHeight()/2.0+(MapProjection.y(lat)-centerY)*world());}

        /** Each segment takes the shortest horizontal wrap, splitting at the dateline when needed. */
        private void drawRoute(Canvas canvas,boolean markers) {
            if(route.isEmpty() || getWidth()<=0 || getHeight()<=0)return;
            double pixels=world();float radius=Math.max(11,tileSize/20f);
            int saved=canvas.save();
            if(zoom==0)canvas.clipRect((float)((getWidth()-pixels)/2),(float)((getHeight()-pixels)/2),(float)((getWidth()+pixels)/2),(float)((getHeight()+pixels)/2));
            paint.setStyle(Paint.Style.FILL);paint.setStrokeWidth(Math.max(2,tileSize/90f));
            for(int i=0;i<route.size();i++) {
                double[] point=route.get(i);
                double pointX=MapProjection.x(point[1]);
                double x=getWidth()/2.0+MapProjection.deltaX(pointX,centerX)*pixels;
                double y=getHeight()/2.0+(MapProjection.y(point[0])-centerY)*pixels;
                if(!markers && i>0) {
                    double[] previous=route.get(i-1);
                    double previousX=MapProjection.x(previous[1]);
                    double start=getWidth()/2.0+MapProjection.deltaX(previousX,centerX)*pixels;
                    double end=start+MapProjection.deltaX(pointX,previousX)*pixels;
                    double startY=getHeight()/2.0+(MapProjection.y(previous[0])-centerY)*pixels;
                    int first=(int)Math.ceil(-Math.max(start,end)/pixels),last=(int)Math.floor((getWidth()-Math.min(start,end))/pixels);
                    paint.setColor(Color.rgb(25,103,190));
                    for(int repeat=first;repeat<=last && repeat<first+32;repeat++)canvas.drawLine((float)(start+repeat*pixels),(float)startY,(float)(end+repeat*pixels),(float)y,paint);
                }
                if(markers) {
                    int first=(int)Math.ceil((-radius-x)/pixels),last=(int)Math.floor((getWidth()+radius-x)/pixels);
                    for(int repeat=first;repeat<=last && repeat<first+32;repeat++) {
                        float shownX=(float)(x+repeat*pixels),shownY=(float)y;
                        if(shownY < -radius || shownY > getHeight()+radius)continue;
                        paint.setColor(Color.WHITE);canvas.drawCircle(shownX,shownY,radius+2,paint);
                        paint.setColor(Color.rgb(25,103,190));canvas.drawCircle(shownX,shownY,radius,paint);
                        paint.setTextSize(radius*1.1f);paint.setTextAlign(Paint.Align.CENTER);paint.setColor(Color.WHITE);
                        Paint.FontMetrics font=paint.getFontMetrics();canvas.drawText(Integer.toString(i+1),shownX,shownY-(font.ascent+font.descent)/2,paint);
                    }
                }
            }
            canvas.restoreToCount(saved);
        }

        public boolean onTouchEvent(MotionEvent event) {
            pinch.onTouchEvent(event);
            switch (event.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    getParent().requestDisallowInterceptTouchEvent(true);
                    downX = previousX = event.getX(); downY = previousY = event.getY(); dragged = false; multiTouch = false; return true;
                case MotionEvent.ACTION_POINTER_DOWN: multiTouch = true; return true;
                case MotionEvent.ACTION_MOVE:
                    if (multiTouch) return true;
                    float x = event.getX(), y = event.getY();
                    if (Math.hypot(x - downX, y - downY) > slop) dragged = true;
                    if (dragged && (zoom != 0 || zoomScale>1)) {
                        centerX = MapProjection.offsetX(centerX, previousX - x, world());
                        centerY = MapProjection.offsetY(centerY, previousY - y, world());
                        viewportChanged();
                    }
                    previousX = x; previousY = y; return true;
                case MotionEvent.ACTION_UP:
                    boolean insideWorld = zoom != 0 || (Math.abs(event.getX() - getWidth() / 2.0) <= world() / 2 && Math.abs(event.getY() - getHeight() / 2.0) <= world() / 2);
                    if (!dragged && !multiTouch) {
                        // A tap on a visible marker opens read-only details and never moves the destination.
                        List<JSONObject> hits = poiLayer.hasLayers() ? poiLayer.hits(event.getX(), event.getY()) : Collections.<JSONObject>emptyList();
                        if (!hits.isEmpty()) { performClick(); poiLayer.openDetails(hits); getParent().requestDisallowInterceptTouchEvent(false); return true; }
                        if (insideWorld) {
                            longitude = MapProjection.longitude(MapProjection.offsetX(centerX, event.getX() - getWidth() / 2.0, world()));
                            latitude = MapProjection.latitude(MapProjection.offsetY(centerY, event.getY() - getHeight() / 2.0, world()));
                            updateSelection(); invalidate(); performClick();
                        }
                    }
                    getParent().requestDisallowInterceptTouchEvent(false); return true;
                case MotionEvent.ACTION_CANCEL: getParent().requestDisallowInterceptTouchEvent(false); return true;
                default: return true;
            }
        }
        public boolean performClick() { super.performClick(); return true; }

        void close() {
            removeCallbacks(locationRefresh);
            if (closed) return;
            closed = true;
            poiLayer.close();
            tiles.close();
        }
        protected void onDetachedFromWindow() { close(); super.onDetachedFromWindow(); }
    }
}
