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
import android.widget.Switch;
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
        layout.setPadding(pad, dp(activity, 4), pad, dp(activity, 4));
        // PGRazorX: game-style page. Layer pills, the map in a white rounded card with round floating
        // buttons, an info card (selection, hint, saved places, route builder) and the game action bar.
        TextView hint = label(ui, "Blue dot: you. Red pin: destination. Drag to pan; pinch to zoom.", 12);
        TextView selected = label(ui, "", 15);
        Button hotspots = button(ui, "Hotspots");layout.addView(hotspots,new LinearLayout.LayoutParams(-1,dp(activity,48)));
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
        layers.setPadding(0, dp(activity, 2), 0, dp(activity, 8));
        SharedPreferences preferences=AccountProfiles.preferences(activity,"pgrazor-map");
        String[] names={"Stops","Gyms","Raids"};
        final List<Runnable> pills=new ArrayList<>();
        for(int i=0;i<names.length;i++){
            final int bit=1<<i;final String key=names[i];
            Button toggle=new Button(ui);toggle.setText(names[i]);toggle.setTextSize(13.5f);
            final boolean[] on={preferences.getBoolean(key,false)};if(on[0])map.setPoiLayer(bit,true);
            toggle.setContentDescription("Show "+names[i]+" on the map");
            Runnable look=()->MapTheme.gamePill(toggle,on[0]);pills.add(look);
            toggle.setOnClickListener(v->{on[0]=!on[0];map.setPoiLayer(bit,on[0]);preferences.edit().putBoolean(key,on[0]).apply();map.refreshPois();look.run();});
            LinearLayout.LayoutParams lp=new LinearLayout.LayoutParams(0,dp(activity,38),1);lp.leftMargin=lp.rightMargin=dp(activity,3);
            layers.addView(toggle,lp);
        }
        layout.addView(layers);
        // The map card: white rim, rounded corners, floating round buttons and the attribution.
        android.widget.FrameLayout mapCard=new android.widget.FrameLayout(ui);
        mapCard.setPadding(dp(activity,3),dp(activity,3),dp(activity,3),dp(activity,3));
        mapCard.setBackground(MapTheme.shape(ui,0xffffffff,0,16));
        mapCard.setElevation(dp(activity,3));
        mapCard.setOutlineProvider(new android.view.ViewOutlineProvider(){public void getOutline(View v,android.graphics.Outline o){o.setRoundRect(0,0,v.getWidth(),v.getHeight(),dp(activity,16));}});
        mapCard.setClipToOutline(true);
        mapCard.addView(map,new android.widget.FrameLayout.LayoutParams(-1,-1));
        LinearLayout fabs=new LinearLayout(ui);fabs.setOrientation(LinearLayout.VERTICAL);
        Fab globe=new Fab(ui,Fab.GLOBE,"Show world map"),center=new Fab(ui,Fab.TARGET,"Center selected destination"),
            current=new Fab(ui,Fab.LOCATE,"Center map on your current in-game location"),retry=new Fab(ui,Fab.RETRY,"Retry map tiles");
        for(Fab f:new Fab[]{globe,center,current,retry}){LinearLayout.LayoutParams fl=new LinearLayout.LayoutParams(dp(activity,42),dp(activity,42));fl.bottomMargin=dp(activity,8);fabs.addView(f,fl);}
        retry.setVisibility(View.GONE);
        globe.setOnClickListener(v -> map.worldView());
        center.setOnClickListener(v -> map.centerSelection());
        current.setOnClickListener(v -> map.centerCurrentLocation());
        retry.setOnClickListener(v -> map.retry());
        android.widget.FrameLayout.LayoutParams fp=new android.widget.FrameLayout.LayoutParams(-2,-2,Gravity.TOP|Gravity.END);fp.topMargin=fp.rightMargin=dp(activity,10);
        mapCard.addView(fabs,fp);
        TextView attribution = label(ui, "", 10);
        attribution.setText(Html.fromHtml("© <a href=\"https://www.openstreetmap.org/copyright\">OpenStreetMap contributors</a>", Html.FROM_HTML_MODE_LEGACY));
        attribution.setMovementMethod(LinkMovementMethod.getInstance());
        attribution.setPadding(dp(activity,5),dp(activity,1),dp(activity,5),dp(activity,1));
        attribution.setBackground(MapTheme.shape(ui,0xb3ffffff,0,4));
        android.widget.FrameLayout.LayoutParams ap=new android.widget.FrameLayout.LayoutParams(-2,-2,Gravity.BOTTOM|Gravity.END);ap.rightMargin=ap.bottomMargin=dp(activity,6);
        mapCard.addView(attribution,ap);
        // PGRazorX: the running Autowalk/GPX route (drawn on the map) gets a progress card with a stop button.
        WalkCard walkCard=new WalkCard(ui,map);
        android.widget.FrameLayout.LayoutParams wl=new android.widget.FrameLayout.LayoutParams(-1,-2,Gravity.BOTTOM);
        wl.leftMargin=wl.rightMargin=dp(activity,8);wl.bottomMargin=dp(activity,24);
        mapCard.addView(walkCard.view,wl);map.setWalkCard(walkCard);
        layout.map = mapCard;
        // The map fills the space left by the controls (see ResponsiveLayout); this is only a baseline height.
        layout.addView(mapCard, new LinearLayout.LayoutParams(-1, dp(activity, 240)));
        // Retry only matters when tiles failed: show it while the status asks for it.
        status.addTextChangedListener(new android.text.TextWatcher(){
            public void beforeTextChanged(CharSequence t,int a,int b,int c){}
            public void onTextChanged(CharSequence t,int a,int b,int c){}
            public void afterTextChanged(android.text.Editable e){retry.setVisibility(e.toString().contains("etry")?View.VISIBLE:View.GONE);}
        });
        // Route tools (route mode only): Add stop / Undo / Stops / GPX, Loop and Clear route.
        LinearLayout actions = new LinearLayout(ui);
        actions.setPadding(0, dp(activity, 8), 0, 0);
        Button add = button(ui, "Add stop"), undo = button(ui, "Undo"), stops = button(ui, "Stops");
        Button gpx = route == null ? null : button(ui, "GPX");
        if (gpx != null) { gpx.setContentDescription("Save, import, or export a GPX route"); gpx.setVisibility(View.GONE); }
        Button[] row = gpx == null ? new Button[] {add, undo, stops} : new Button[] {add, undo, stops, gpx};
        for (Button b : row) { LinearLayout.LayoutParams al=new LinearLayout.LayoutParams(0, dp(activity, 40), 1); al.leftMargin=al.rightMargin=dp(activity,3); actions.addView(b, al); }
        actions.setVisibility(View.GONE);
        layout.addView(actions);
        Switch loop = new Switch(ui); loop.setText("Loop route"); loop.setTextSize(13);
        loop.setContentDescription("Repeat the route from the first stop after the last"); loop.setVisibility(View.GONE);
        LinearLayout routeTools = new LinearLayout(ui);
        routeTools.setGravity(Gravity.CENTER_VERTICAL);
        routeTools.addView(loop,new LinearLayout.LayoutParams(0,dp(activity,44),1));
        Button clearRoute=button(ui,"Clear route");
        clearRoute.setContentDescription("Clear all stops from the route draft");
        clearRoute.setVisibility(View.GONE);
        routeTools.addView(clearRoute,new LinearLayout.LayoutParams(dp(activity,112),dp(activity,40)));
        routeTools.setVisibility(View.GONE);
        layout.addView(routeTools);
        // Info card: selection, hint and loaded locations, then Saved places and the route builder.
        LinearLayout info = new LinearLayout(ui);
        info.setOrientation(LinearLayout.VERTICAL);
        info.setPadding(dp(activity, 16), dp(activity, 10), dp(activity, 16), dp(activity, 4));
        LinearLayout.LayoutParams il = new LinearLayout.LayoutParams(-1, -2); il.topMargin = dp(activity, 10);
        layout.addView(info, il);
        selected.setPadding(0, 0, 0, dp(activity, 2));
        info.addView(selected);
        // PGRazorX: tap the selected coordinates to copy them to the clipboard.
        selected.setOnClickListener(v -> { ((android.content.ClipboardManager)activity.getSystemService(Context.CLIPBOARD_SERVICE)).setPrimaryClip(android.content.ClipData.newPlainText("Coordinates", String.format(Locale.US, "%.6f, %.6f", map.latitude, map.longitude))); Toast.makeText(activity, "Coordinates copied", Toast.LENGTH_SHORT).show(); });
        info.addView(hint);
        TextView coverage=label(ui,"Nearby locations loaded by the game",12);info.addView(coverage);map.setPoiStatus(coverage);
        info.addView(status);
        Button saved = SettingsUi.button(ui, "Saved places");
        info.addView(saved, new LinearLayout.LayoutParams(-1, -2));
        Button routeMode = SettingsUi.button(ui, "Route builder"); routeMode.setContentDescription("Open route builder");
        routeMode.setVisibility(route == null ? View.GONE : View.VISIBLE);
        info.addView(routeMode, new LinearLayout.LayoutParams(-1, -2));
        map.refreshPois();
        PlaceUi places = new PlaceUi(activity, ui, map, saved);
        saved.setOnClickListener(v -> places.openLibrary());
        RouteUi routes = route == null ? null : new RouteUi(activity,ui,map,routeMode,hint,add,undo,stops,gpx,clearRoute,loop,route,walk != null);
        if(routes!=null){routes.reopen=()->WorldMapDialog.show(activity,Control.engine.latitude,Control.engine.longitude,listener,walk,route);routes.normalTools=fabs;routes.routeRows=new View[]{actions,routeTools};}
        AlertDialog.Builder builder = new AlertDialog.Builder(ui).setTitle("World map").setView(layout)
            .setNegativeButton("Cancel", null).setPositiveButton("Teleport", null);
        if (walk != null) builder.setNeutralButton("Walk here", null);
        AlertDialog dialog = builder.create();
        map.poiLayer.setDestinationActions(walk == null ? null : (lat, lon) -> {
            if (!walk.onDestination(lat, lon)) return false;
            dialog.dismiss(); return true;
        }, (latitudePoi, longitudePoi) -> {
            if (cooldownDialog[0] != null && cooldownDialog[0].isShowing()) return false;
            cooldownDialog[0] = TeleportDialog.confirm(activity, latitudePoi, longitudePoi, (lat, lon) -> {
                if (!listener.onDestination(lat, lon)) return false;
                dialog.dismiss(); return true;
            });
            return false; // Keep details visible if the cooldown confirmation is cancelled.
        });
        DialogLifecycle.bind(activity, dialog, () -> { if(hotspotsDialog[0]!=null)hotspotsDialog[0].dismiss(); if(cooldownDialog[0]!=null)cooldownDialog[0].dismiss(); if(searchDialog[0]!=null)searchDialog[0].dismiss(); if(routes!=null)routes.close(); places.close(); map.close(); });
        dialog.setOnShowListener(d -> {
            MapTheme.fullScreen(dialog); MapTheme.style(layout);styleMapButtons(layout); MapTheme.styleButtons(dialog); MapTheme.pageTitle(dialog);
            // Game look re-applied over the generic styling: pills, info card rows, muted captions.
            for(Runnable look:pills)look.run();
            info.setBackground(MapTheme.cardBackground(ui)); MapTheme.elevate(info, 2.5f);
            SettingsUi.styleTree(ui, saved); SettingsUi.styleTree(ui, routeMode);
            selected.setTypeface(MapTheme.bold()); selected.setTextColor(MapTheme.ink());
            for(TextView t:new TextView[]{hint,coverage,status}){t.setTextColor(MapTheme.muted());}
            attribution.setTextColor(MapTheme.muted()); attribution.setLinkTextColor(MapTheme.value());
            for(Button b:row) MapTheme.gameSelected(b,false);
            MapTheme.gameFooter(dialog, layout);
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
    private static Button button(Context c, String text) {
        Button b=SettingsUi.button(c,text);SettingsUi.styleTree(c,b);return b;
    }
    private static void styleMapButtons(View root){
        if(root instanceof SettingsUi.RowButton)SettingsUi.styleTree(root.getContext(),root);
        if(root instanceof android.view.ViewGroup){android.view.ViewGroup group=(android.view.ViewGroup)root;
            for(int i=0;i<group.getChildCount();i++)styleMapButtons(group.getChildAt(i));}
    }

    /** PGRazorX: round white floating map button with a drawn teal icon (world, selected, my location, retry). */
    private static final class Fab extends View {
        static final int GLOBE=0, TARGET=1, LOCATE=2, RETRY=3;
        private final int icon;
        private final android.graphics.Paint paint=new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
        Fab(Context c,int icon,String description){
            super(c);this.icon=icon;setContentDescription(description);setClickable(true);setFocusable(true);
            setOutlineProvider(new android.view.ViewOutlineProvider(){public void getOutline(View v,android.graphics.Outline o){o.setOval(0,0,v.getWidth(),v.getHeight());}});
            setElevation(dp(c,3));
            setForeground(new android.graphics.drawable.RippleDrawable(android.content.res.ColorStateList.valueOf(0x2236b89e),null,new android.graphics.drawable.ShapeDrawable(new android.graphics.drawable.shapes.OvalShape())));
        }
        @Override protected void onDraw(android.graphics.Canvas c){
            super.onDraw(c);float cx=getWidth()/2f,cy=getHeight()/2f,r=Math.min(cx,cy),u=r/21f;
            paint.setStyle(android.graphics.Paint.Style.FILL);paint.setColor(MapTheme.card());c.drawCircle(cx,cy,r,paint);
            paint.setStyle(android.graphics.Paint.Style.STROKE);paint.setStrokeWidth(2f*u);paint.setStrokeCap(android.graphics.Paint.Cap.ROUND);paint.setColor(MapTheme.accent());
            switch(icon){
                case GLOBE:
                    c.drawCircle(cx,cy,9*u,paint);c.drawOval(cx-4*u,cy-9*u,cx+4*u,cy+9*u,paint);c.drawLine(cx-9*u,cy,cx+9*u,cy,paint);break;
                case TARGET:
                    c.drawCircle(cx,cy,8.5f*u,paint);paint.setStyle(android.graphics.Paint.Style.FILL);c.drawCircle(cx,cy,3.5f*u,paint);break;
                case LOCATE:
                    c.drawCircle(cx,cy,6.5f*u,paint);c.drawLine(cx,cy-11*u,cx,cy-7*u,paint);c.drawLine(cx,cy+7*u,cx,cy+11*u,paint);
                    c.drawLine(cx-11*u,cy,cx-7*u,cy,paint);c.drawLine(cx+7*u,cy,cx+11*u,cy,paint);paint.setStyle(android.graphics.Paint.Style.FILL);c.drawCircle(cx,cy,2.2f*u,paint);break;
                default:
                    c.drawArc(cx-8*u,cy-8*u,cx+8*u,cy+8*u,-60,280,false,paint);paint.setStyle(android.graphics.Paint.Style.FILL);
                    android.graphics.Path h=new android.graphics.Path();float ax=cx+4f*u,ay=cy-6.9f*u;h.moveTo(ax+4.5f*u,ay-0.5f*u);h.lineTo(ax-1f*u,ay-4.5f*u);h.lineTo(ax-0.5f*u,ay+3.5f*u);h.close();c.drawPath(h,paint);break;
            }
        }
    }

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
        final Switch loop;
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
        /** Normal-mode floating map buttons and the route-only rows, swapped when the builder opens. */
        View normalTools; View[] routeRows;
        RouteUi(Activity activity,Context ui,MapView map,Button mode,TextView hint,Button add,Button undo,Button stops,Button gpx,Button clearRoute,Switch loop,RouteListener listener,boolean hasWalk) {
            this.activity=activity;this.ui=ui;this.map=map;this.mode=mode;this.hint=hint;this.loop=loop;
            this.add=add;this.undo=undo;this.stops=stops;this.gpx=gpx;this.clearRoute=clearRoute;this.listener=listener;this.hasWalk=hasWalk;
            clearRoute.setOnClickListener(v -> clearDraft());
            mode.setOnClickListener(v -> {editing=!editing;if(editing)maybeLoadPending();refresh();});
            add.setOnClickListener(v -> {if(editing)addStop();});
            undo.setOnClickListener(v -> {if(editing)undo();});
            stops.setOnClickListener(v -> {if(editing)showStops();});
            gpx.setOnClickListener(v -> {if(editing)GpxRouteDialog.menu(activity,this::sourceRoute,this::onGpxChosen);});
        }
        void attach(AlertDialog parent) {this.parent=parent;if(pendingRoute!=null){editing=true;maybeLoadPending();}refresh();}
        void refresh() {
            if(draft.isEmpty())imported=false;
            boolean big=draft.size()>LIMIT;
            mode.setText(editing ? "Back to map" : "Route builder");
            if(normalTools!=null)normalTools.setVisibility(editing ? View.GONE : View.VISIBLE);
            if(routeRows!=null)for(View r:routeRows)r.setVisibility(editing ? View.VISIBLE : View.GONE);
            mode.setContentDescription(editing ? "Leave route builder without starting" : "Open route builder");
            hint.setText(!editing ? "Blue dot: you. Red pin: destination. Tap to select; drag to pan."
                : big ? "GPX route: "+draft.size()+" stops. Tap Start to walk it in order."
                : "Route: "+draft.size()+"/"+LIMIT+" stops. Tap map, then Add stop.");
            add.setText("Add stop"); add.setContentDescription("Add selected destination as next route stop");
            undo.setText("Undo"); undo.setContentDescription("Undo last route stop");
            stops.setText("Stops ("+draft.size()+")"); stops.setContentDescription("View ordered route stops and clear route");
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
                if(walk!=null && hasWalk)walk.setVisibility(editing ? View.INVISIBLE : View.VISIBLE); // keeps the close button centred
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
            if(draft.isEmpty()) {
                AlertDialog.Builder builder=new AlertDialog.Builder(ui).setTitle("Route stops ("+draft.size()+(draft.size()>LIMIT?"":"/"+LIMIT)+")")
                    .setMessage("Select a destination on the map and tap Add stop. Add at least two stops before starting.")
                    .setNegativeButton("Close",null).setPositiveButton("Clear route",null);
                AlertDialog dialog=builder.create();dialogs.add(dialog);
                dialog.setOnDismissListener(d -> dialogs.remove(dialog));
                dialog.setOnShowListener(d -> {
                    Button clear=dialog.getButton(AlertDialog.BUTTON_POSITIVE);clear.setEnabled(false);
                    MapTheme.styleButtons(dialog);
                });
                dialog.show();
            } else {
                String[] rows=new String[draft.size()];
                for(int i=0;i<rows.length;i++)rows[i]=String.format(Locale.US,"%d.  %.6f, %.6f",i+1,draft.get(i)[0],draft.get(i)[1]);
                AlertDialog dialog=MapTheme.showListDialog(ui, "Route stops ("+draft.size()+(draft.size()>LIMIT?"":"/"+LIMIT)+")", rows, (d,index) -> {double[] stop=draft.get(index);map.select(stop[0],stop[1]);});
                dialogs.add(dialog);
                dialog.setOnDismissListener(d -> dialogs.remove(dialog));
                dialog.show();
            }
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
            View custom = dialog.findViewById(android.R.id.custom); if (custom != null) MapTheme.style(custom);
            MapTheme.styleButtons(dialog);
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
            if (snapshot.entries.isEmpty()) {
                AlertDialog.Builder builder = new AlertDialog.Builder(ui).setTitle("Saved places (" + snapshot.entries.size() + "/50)")
                    .setNegativeButton("Close", null).setPositiveButton("Save selected", null);
                builder.setMessage(snapshot.damaged ? "Saved places could not be read. You can save new destinations here." : "No saved places yet. Save the selected map destination to find it again later.");
                AlertDialog dlg = builder.create();
                dlg.setOnShowListener(d -> {
                    MapTheme.styleButtons(dlg);
                    dlg.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> showSave(dlg));
                });
                show(dlg);
            } else {
                String[] names = new String[snapshot.entries.size()];
                for (int i = 0; i < names.length; i++) {
                    SavedPlaces.Entry entry = snapshot.entries.get(i);
                    names[i] = String.format(Locale.US, "%s\n%.6f, %.6f", entry.name, entry.latitude, entry.longitude);
                }
                AlertDialog dlg = MapTheme.showListDialog(ui, "Saved places (" + snapshot.entries.size() + "/50)", names, (dialog, which) -> showPlace(snapshot.entries.get(which))); dlg.setButton(AlertDialog.BUTTON_POSITIVE, "Save selected", (android.content.DialogInterface.OnClickListener) null); dlg.setOnShowListener(d -> { MapTheme.styleButtons(dlg); dlg.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> showSave(dlg)); }); // PGRazorX: save from the list too
                show(dlg);
            }
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

    /** PGRazorX: progress of the running Autowalk/GPX route over the map: lap badge, stop count, distance and Stop. */
    private static final class WalkCard {
        static final int TEAL = Color.rgb(30, 140, 140);
        final LinearLayout view;
        final TextView title, badge, main, sub;
        final Bar bar;
        WalkCard(Context ui, MapView map) {
            view = new LinearLayout(ui); view.setOrientation(LinearLayout.VERTICAL);
            int p = dp(ui, 14); view.setPadding(p, dp(ui, 10), p, dp(ui, 12));
            view.setBackground(MapTheme.shape(ui, MapTheme.card(), 0, 16)); view.setElevation(dp(ui, 4));
            LinearLayout head = new LinearLayout(ui); head.setGravity(Gravity.CENTER_VERTICAL);
            title = label(ui, "AUTOWALK", 11); title.setTypeface(MapTheme.bold()); title.setLetterSpacing(.06f); title.setTextColor(MapTheme.muted());
            badge = label(ui, "", 10); badge.setTypeface(MapTheme.bold()); badge.setTextColor(Color.WHITE); badge.setLetterSpacing(.04f);
            badge.setPadding(dp(ui, 8), dp(ui, 2), dp(ui, 8), dp(ui, 2)); badge.setBackground(MapTheme.shape(ui, TEAL, 0, 10));
            head.addView(title);
            LinearLayout.LayoutParams bl = new LinearLayout.LayoutParams(-2, -2); bl.leftMargin = dp(ui, 8); head.addView(badge, bl);
            LinearLayout body = new LinearLayout(ui); body.setGravity(Gravity.CENTER_VERTICAL);
            LinearLayout texts = new LinearLayout(ui); texts.setOrientation(LinearLayout.VERTICAL);
            main = label(ui, "", 17); main.setTypeface(MapTheme.bold()); main.setTextColor(MapTheme.ink());
            sub = label(ui, "", 12); sub.setTextColor(MapTheme.muted());
            texts.addView(main); texts.addView(sub);
            body.addView(texts, new LinearLayout.LayoutParams(0, -2, 1));
            View stop = new View(ui) {
                final Paint pen = new Paint(Paint.ANTI_ALIAS_FLAG);
                @Override protected void onDraw(Canvas c) {
                    float r = Math.min(getWidth(), getHeight()) / 2f, cx = getWidth() / 2f, cy = getHeight() / 2f, h = r * .36f;
                    pen.setColor(TEAL); c.drawCircle(cx, cy, r, pen);
                    pen.setColor(Color.WHITE); c.drawRoundRect(cx - h, cy - h, cx + h, cy + h, r * .1f, r * .1f, pen);
                }
            };
            stop.setContentDescription("Stop the route");
            stop.setOnClickListener(v -> {
                long token = Control.engine.activeRouteToken();
                if (token != 0 && Control.engine.stopRoute(token)) { Control.saveState(); Control.event("route-stopped", "Stopped from the world map"); }
                map.pollRoute();
            });
            LinearLayout.LayoutParams sl = new LinearLayout.LayoutParams(dp(ui, 42), dp(ui, 42)); sl.leftMargin = dp(ui, 10);
            body.addView(stop, sl);
            bar = new Bar(ui);
            view.addView(head);
            LinearLayout.LayoutParams ml = new LinearLayout.LayoutParams(-1, -2); ml.topMargin = dp(ui, 4); view.addView(body, ml);
            LinearLayout.LayoutParams pl = new LinearLayout.LayoutParams(-1, dp(ui, 5)); pl.topMargin = dp(ui, 8); view.addView(bar, pl);
            view.setVisibility(View.GONE);
        }
        void update(MotionEngine.RouteView route, int stops) {
            if (route == null || stops == 0) { view.setVisibility(View.GONE); return; }
            view.setVisibility(View.VISIBLE);
            badge.setVisibility(route.looping ? View.VISIBLE : View.GONE);
            badge.setText("\u21bb LOOP \u00b7 LAP " + route.lap);
            main.setText(String.format(Locale.US, "Stop %d of %d", route.next + 1, stops));
            double speed = Control.engine.speedKmh;
            String distance = route.remainingMeters >= 1000 ? String.format(Locale.US, "%.1f km", route.remainingMeters / 1000) : Math.round(route.remainingMeters) + " m";
            String pace = speed == 0 ? "paused (0 km/h)" : String.format(Locale.US, "%s km/h", speed == Math.rint(speed) ? String.valueOf((long)speed) : String.valueOf(speed));
            if (route.looping) sub.setText(distance + " to close the lap \u00b7 " + pace);
            else {
                String eta = speed > 0 ? " \u00b7 ~" + Math.max(1, Math.round(route.remainingMeters / (speed / 3.6) / 60)) + " min" : "";
                sub.setText(distance + " left \u00b7 " + pace + eta);
            }
            bar.set(route.lapMeters > 0 ? 1 - route.remainingMeters / route.lapMeters : (double)route.next / stops);
        }
        /** Thin rounded progress track in the game's teal. */
        static final class Bar extends View {
            final Paint pen = new Paint(Paint.ANTI_ALIAS_FLAG);
            double value;
            Bar(Context c) { super(c); }
            void set(double v) { v = Math.max(0, Math.min(1, Double.isFinite(v) ? v : 0)); if (v != value) { value = v; invalidate(); } }
            @Override protected void onDraw(Canvas c) {
                float h = getHeight(), w = getWidth();
                pen.setColor(MapTheme.dark() ? 0xff3e5350 : 0xffe4e9e8); c.drawRoundRect(0, 0, w, h, h / 2, h / 2, pen);
                pen.setColor(Color.rgb(54, 184, 158)); c.drawRoundRect(0, 0, (float)(w * value), h, h / 2, h / 2, pen);
            }
        }
    }

    /** PGRazorX: small map that shows a route before it starts (Autowalk dialog); it ignores touch. */
    static final class RoutePreview {
        final android.widget.FrameLayout view;
        private final MapView map;
        RoutePreview(Activity activity) {
            Context ui = MapTheme.wrap(activity);
            map = new MapView(activity, Control.engine.latitude, Control.engine.longitude, new TextView(activity), new TextView(activity), true, false);
            view = framed(activity, ui, map);
        }
        void show(double[][] points, boolean loop) { map.showPlanned(points, loop); }
        void close() { map.close(); }
    }

    /** White rounded map card with the OpenStreetMap attribution, as in the world map. */
    private static android.widget.FrameLayout framed(Activity activity, Context ui, MapView map) {
        android.widget.FrameLayout card = new android.widget.FrameLayout(ui);
        card.setPadding(dp(activity, 3), dp(activity, 3), dp(activity, 3), dp(activity, 3));
        card.setBackground(MapTheme.shape(ui, 0xffffffff, 0, 16));
        card.setElevation(dp(activity, 2));
        card.setOutlineProvider(new android.view.ViewOutlineProvider() { public void getOutline(View v, android.graphics.Outline o) { o.setRoundRect(0, 0, v.getWidth(), v.getHeight(), dp(activity, 16)); } });
        card.setClipToOutline(true);
        card.addView(map, new android.widget.FrameLayout.LayoutParams(-1, -1));
        TextView attribution = label(ui, "", 9);
        attribution.setText(Html.fromHtml("\u00a9 <a href=\"https://www.openstreetmap.org/copyright\">OpenStreetMap contributors</a>", Html.FROM_HTML_MODE_LEGACY));
        attribution.setMovementMethod(LinkMovementMethod.getInstance());
        attribution.setPadding(dp(activity, 5), dp(activity, 1), dp(activity, 5), dp(activity, 1));
        attribution.setBackground(MapTheme.shape(ui, 0xb3ffffff, 0, 4));
        attribution.setTextColor(MapTheme.muted()); attribution.setLinkTextColor(MapTheme.value());
        android.widget.FrameLayout.LayoutParams ap = new android.widget.FrameLayout.LayoutParams(-2, -2, Gravity.BOTTOM | Gravity.END);
        ap.rightMargin = ap.bottomMargin = dp(activity, 6);
        card.addView(attribution, ap);
        return card;
    }

    /** PGRazorX: full-screen look at a route before starting it (pan and pinch only; nothing is selected). */
    static AlertDialog showRoute(Activity activity, double[][] points, boolean loop, String title) {
        if (activity.isFinishing() || activity.isDestroyed() || points == null || points.length == 0) return null;
        Context ui = MapTheme.wrap(activity);
        ResponsiveLayout layout = new ResponsiveLayout(activity);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(dp(activity, 12), dp(activity, 4), dp(activity, 12), dp(activity, 4));
        MapView map = new MapView(activity, points[0][0], points[0][1], new TextView(activity), new TextView(activity), true, true);
        android.widget.FrameLayout card = framed(activity, ui, map);
        layout.map = card;
        layout.addView(card, new LinearLayout.LayoutParams(-1, dp(activity, 240)));
        TextView caption = label(ui, (loop ? "Loop route: after the last stop it returns to stop 1 and repeats. " : "") + "Drag to pan, pinch to zoom.", 12);
        caption.setPadding(dp(activity, 4), dp(activity, 8), dp(activity, 4), 0);
        layout.addView(caption);
        map.showPlanned(points, loop);
        AlertDialog dialog = new AlertDialog.Builder(ui).setTitle(title).setView(layout).setNegativeButton("Close", null).create();
        DialogLifecycle.bind(activity, dialog, map::close);
        dialog.setOnShowListener(d -> {
            MapTheme.fullScreen(dialog); MapTheme.style(layout);styleMapButtons(layout); MapTheme.styleButtons(dialog); MapTheme.pageTitle(dialog);
            caption.setTextColor(MapTheme.muted());
            MapTheme.gameFooter(dialog, layout);
        });
        dialog.show();
        return dialog;
    }

    private static final class MapView extends View {
        private final TextView selectionLabel;
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
        private final int tileSize, slop;
        private final MapTileSource tiles;
        private final MapPoiLayer poiLayer;
        private List<double[]> route = Collections.emptyList();
        private volatile boolean closed;
        /** PGRazorX: preview maps show a planned route only (no selection, no live route); static ones ignore touch. */
        private final boolean preview, touchable;
        private double[][] planned;
        private boolean plannedLoop, fitPending;
        private MotionEngine.RouteView active;
        private double[][] activePoints;
        private long activeToken;
        private WalkCard walkCard;
        private final Handler ticker = new Handler(Looper.getMainLooper());
        private final Runnable tick = this::pollRoute;
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
            this(activity, latitude, longitude, selection, status, false, true);
        }
        MapView(Activity activity, double latitude, double longitude, TextView selection, TextView status, boolean preview, boolean touchable) {
            super(activity);
            this.preview = preview; this.touchable = touchable;
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
            }, preview ? new MapPoiLayer.Snapshots() {
                public void setOpen(boolean open) {}
                public String read() { return "{\"points\":[]}"; }
            } : new MapPoiLayer.Snapshots() {
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
            selectionLabel.setText(String.format(Locale.US, "Selected: %.6f, %.6f  \uD83D\uDCCB", latitude, longitude));
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
        protected void onSizeChanged(int w, int h, int oldw, int oldh) { if (fitPending) fitPlanned(); viewportChanged(); }

        void setWalkCard(WalkCard card) { walkCard = card; }
        /** Follows the engine's running planned route twice a second; the route itself is copied only when it changes. */
        void pollRoute() {
            if (closed) return;
            MotionEngine.RouteView view = Control.engine.routeView(activeToken);
            if (view == null) { active = null; activePoints = null; activeToken = 0; }
            else {
                if (view.points != null) activePoints = view.points;
                if (activePoints == null) { activeToken = 0; view = null; } // fetch the copy on the next tick
                else { activeToken = view.token; active = view; }
            }
            if (walkCard != null) walkCard.update(view, activePoints == null ? 0 : activePoints.length);
            invalidate();
            ticker.removeCallbacks(tick);
            ticker.postDelayed(tick, 500);
        }
        /** Shows a route that has not started (Autowalk preview) and fits the map to it. */
        void showPlanned(double[][] points, boolean loop) {
            if (points != null && planned != null && loop == plannedLoop && java.util.Arrays.deepEquals(points, planned)) return;
            if (points == null || points.length == 0) { planned = null; invalidate(); return; }
            // Refit only for a different route, not when a nearby plan merely follows the moving avatar.
            boolean refit = planned == null || planned.length != points.length;
            planned = new double[points.length][];
            for (int i = 0; i < points.length; i++) planned[i] = points[i].clone();
            plannedLoop = loop; fitPending |= refit;
            if (fitPending && getWidth() > 0 && getHeight() > 0) { fitPlanned(); viewportChanged(); }
            invalidate();
        }
        private void fitPlanned() {
            if (planned == null || getWidth() <= 0 || getHeight() <= 0) return;
            fitPending = false;
            double minX = 1, maxX = 0, minY = 1, maxY = 0;
            for (double[] p : planned) {
                double x = MapProjection.x(p[1]), y = MapProjection.y(p[0]);
                minX = Math.min(minX, x); maxX = Math.max(maxX, x); minY = Math.min(minY, y); maxY = Math.max(maxY, y);
            }
            double dx = Math.max(maxX - minX, 1e-7), dy = Math.max(maxY - minY, 1e-7);
            double scale = Math.min(getWidth() * 0.78 / dx, getHeight() * 0.72 / dy);
            zoom = Math.max(2, Math.min(17, (int)Math.floor(Math.log(scale / tileSize) / Math.log(2))));
            zoomScale = 1; centerX = (minX + maxX) / 2; centerY = (minY + maxY) / 2;
        }

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
            // PGRazorX: the route builder's draft wins; otherwise a planned preview or the running route.
            if (route.isEmpty()) {
                if (planned != null) drawWalk(canvas, planned, 0, plannedLoop, false);
                else if (active != null && activePoints != null) drawWalk(canvas, activePoints, active.next, active.looping, true);
            }
            drawRoute(canvas,false);
            if (preview) return;
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
            if(!closed){removeCallbacks(locationRefresh);post(locationRefresh);if(!preview){ticker.removeCallbacks(tick);ticker.post(tick);}}
        }

        private static final int WALK_BLUE = Color.rgb(25, 103, 190), WALK_DONE = Color.rgb(154, 167, 168),
            WALK_NEXT = Color.rgb(255, 138, 61), WALK_LOOP = Color.rgb(30, 140, 140);
        /**
         * PGRazorX: an Autowalk/GPX route. Live routes show walked legs grey and dashed, pending legs blue with
         * direction arrows, the next stop orange and the avatar; a loop adds the dotted way back to stop 1,
         * which wears a dashed start/finish ring. Long GPX routes skip the per-point markers.
         */
        private void drawWalk(Canvas canvas, double[][] points, int next, boolean loop, boolean live) {
            int n = points.length;
            if (n == 0 || getWidth() <= 0 || getHeight() <= 0) return;
            float[] xs = new float[n], ys = new float[n];
            for (int i = 0; i < n; i++) { xs[i] = projectX(points[i][1]); ys[i] = projectY(points[i][0]); }
            float avatarX = projectX(Control.engine.longitude), avatarY = projectY(Control.engine.latitude);
            float stroke = Math.max(4, tileSize / 64f), radius = Math.max(11, tileSize / 20f);
            next = Math.max(0, Math.min(n - 1, next));
            paint.setStyle(Paint.Style.STROKE); paint.setStrokeCap(Paint.Cap.ROUND); paint.setStrokeJoin(Paint.Join.ROUND); paint.setStrokeWidth(stroke);
            android.graphics.Path path = new android.graphics.Path();
            if (live && next > 0) {
                path.moveTo(xs[0], ys[0]);
                for (int i = 1; i < next; i++) path.lineTo(xs[i], ys[i]);
                path.lineTo(avatarX, avatarY);
                paint.setColor(WALK_DONE); paint.setPathEffect(new android.graphics.DashPathEffect(new float[]{stroke * 1.8f, stroke * 1.5f}, 0));
                canvas.drawPath(path, paint);
            }
            path.reset();
            if (live) { path.moveTo(avatarX, avatarY); for (int i = next; i < n; i++) path.lineTo(xs[i], ys[i]); }
            else { path.moveTo(xs[0], ys[0]); for (int i = 1; i < n; i++) path.lineTo(xs[i], ys[i]); }
            paint.setColor(WALK_BLUE); paint.setPathEffect(null); canvas.drawPath(path, paint);
            if (loop && n > 1) {
                paint.setPathEffect(new android.graphics.DashPathEffect(new float[]{stroke * 0.2f, stroke * 2f}, 0));
                canvas.drawLine(xs[n - 1], ys[n - 1], xs[0], ys[0], paint);
                paint.setPathEffect(null);
            }
            // Direction arrows on the pending legs long enough to hold one.
            paint.setStyle(Paint.Style.FILL); paint.setColor(Color.WHITE);
            int arrows = 0;
            for (int i = live ? next : 1; i < n && arrows < 60; i++) {
                float ax = i == next && live ? avatarX : xs[i - 1], ay = i == next && live ? avatarY : ys[i - 1];
                if (!live && i == 0) continue;
                float dx = xs[i] - ax, dy = ys[i] - ay, length = (float)Math.hypot(dx, dy);
                if (length < radius * 6) continue;
                float mx = ax + dx / 2, my = ay + dy / 2;
                if (mx < 0 || my < 0 || mx > getWidth() || my > getHeight()) continue;
                float ux = dx / length, uy = dy / length, size = stroke * 1.15f;
                android.graphics.Path head = new android.graphics.Path();
                head.moveTo(mx + ux * size, my + uy * size);
                head.lineTo(mx - ux * size - uy * size, my - uy * size + ux * size);
                head.lineTo(mx - ux * size + uy * size, my - uy * size - ux * size);
                head.close(); canvas.drawPath(head, paint); arrows++;
            }
            boolean all = n <= 80;
            paint.setTextAlign(Paint.Align.CENTER); paint.setTextSize(radius * 1.05f); paint.setTypeface(MapTheme.bold());
            Paint.FontMetrics font = paint.getFontMetrics();
            for (int i = n - 1; i >= 0; i--) {
                boolean isNext = live && i == next, done = live && i < next, start = i == 0;
                if (!all && !isNext && !start) continue;
                float x = xs[i], y = ys[i];
                if (x < -radius * 2 || y < -radius * 2 || x > getWidth() + radius * 2 || y > getHeight() + radius * 2) continue;
                int fill = isNext ? WALK_NEXT : done ? WALK_DONE : loop && start ? WALK_LOOP : WALK_BLUE;
                if (isNext) { paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(stroke * 0.8f); paint.setColor(WALK_NEXT); canvas.drawCircle(x, y, radius * 1.55f, paint); }
                if (loop && start) {
                    paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(stroke * 0.75f); paint.setColor(WALK_LOOP);
                    paint.setPathEffect(new android.graphics.DashPathEffect(new float[]{radius * 1.1f, radius * 0.35f}, 0));
                    canvas.drawCircle(x, y, radius * (isNext ? 2.05f : 1.6f), paint); paint.setPathEffect(null);
                }
                paint.setStyle(Paint.Style.FILL);
                paint.setColor(Color.WHITE); canvas.drawCircle(x, y, radius + 2, paint);
                paint.setColor(fill); canvas.drawCircle(x, y, radius, paint);
                paint.setColor(Color.WHITE);
                canvas.drawText(done ? "\u2713" : Integer.toString(i + 1), x, y - (font.ascent + font.descent) / 2, paint);
            }
            if (live) {
                float r = radius * 0.75f;
                paint.setColor(Color.WHITE); canvas.drawCircle(avatarX, avatarY, r + 3, paint);
                paint.setColor(WALK_LOOP); canvas.drawCircle(avatarX, avatarY, r, paint);
                paint.setColor(Color.WHITE); canvas.drawCircle(avatarX, avatarY, r / 3, paint);
            }
            paint.setTypeface(null);
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
            if (!touchable) return false;
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
                    if (!dragged && !multiTouch && !preview) {
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
            ticker.removeCallbacks(tick);
            poiLayer.close();
            tiles.close();
        }
        protected void onDetachedFromWindow() { close(); super.onDetachedFromWindow(); }
    }
}
