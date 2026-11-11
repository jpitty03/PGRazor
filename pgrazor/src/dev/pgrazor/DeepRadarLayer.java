package dev.pgrazor;

import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;
import org.json.JSONObject;
import java.io.InputStream;
import java.io.ByteArrayOutputStream;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Set;

/** Passive game-payload layer, independent of POI rendering and movement policy. */
final class DeepRadarLayer {
    private final Context context,ui;
    private final MapPoiLayer.Projector projector;
    private final Runnable invalidate;
    private final Handler main=new Handler(Looper.getMainLooper());
    private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG|Paint.FILTER_BITMAP_FLAG);
    private final SharedPreferences prefs;
    private final List<AlertDialog> dialogs=new ArrayList<>();
    private final LinkedHashMap<Integer,Bitmap> icons=new LinkedHashMap<Integer,Bitmap>(64,.75f,true){protected boolean removeEldestEntry(java.util.Map.Entry<Integer,Bitmap> e){return size()>96;}};
    private JSONObject names;
    private boolean closed,enabled,unregistered;
    private Set<Integer> species=Collections.emptySet();
    private TextView status;
    private WorldMapDialog.DestinationListener walk,teleport;
    private final Runnable refreshTask=this::refresh;
    DeepRadarLayer(android.app.Activity c,MapPoiLayer.Projector p,Runnable redraw){context=c;ui=MapTheme.wrap(c);projector=p;invalidate=redraw;prefs=AccountProfiles.preferences(c,"pgrazor-map");
        unregistered=prefs.getBoolean("DeepRadarUnregistered",false);try{species=DeepRadarState.speciesFilter(prefs.getString("DeepRadarSpecies",""));}catch(Exception ignored){}
    }
    void attach(CheckBox toggle,TextView label){status=label;enabled=prefs.getBoolean("DeepRadar",false);toggle.setChecked(enabled);toggle.setOnCheckedChangeListener((b,on)->{enabled=on;prefs.edit().putBoolean("DeepRadar",on).apply();refresh();});label.setOnClickListener(v->list());label.setContentDescription("Deep Radar spawn list and filters");refresh();}
    void setDestinationActions(WorldMapDialog.DestinationListener walk,WorldMapDialog.DestinationListener teleport){this.walk=walk;this.teleport=teleport;}
    private List<DeepRadarState.Row> rows(){DeepRadarState.Snapshot s=Control.deepRadarSnapshot();List<DeepRadarState.Row> result=new ArrayList<>();if(s!=null&&enabled&&!closed){long now=SystemClock.elapsedRealtime();for(DeepRadarState.Row r:s.rows)if(DeepRadarState.visible(r,now,species,unregistered))result.add(r);}return result;}
    private void refresh(){if(closed)return;Control.setDeepRadarOpen(enabled);if(status!=null){status.setVisibility(enabled?android.view.View.VISIBLE:android.view.View.GONE);DeepRadarState.Snapshot s=Control.deepRadarSnapshot();
        String text=s==null||s.status.equals("waiting")?"Deep Radar: waiting for a game map response":s.status.equals("unsupported")?"Deep Radar unavailable: "+s.message:s.status.equals("stale")?"Deep Radar: waiting for fresh map data":"Deep Radar: "+rows().size()+" spawns · tap for list / filters";
        if(s!=null&&!s.message.isEmpty()&&!s.status.equals("unsupported"))text+=" · "+s.message;status.setText(text);}
        invalidate.run();main.removeCallbacks(refreshTask);main.postDelayed(refreshTask,1000);
    }
    void draw(Canvas canvas){if(!enabled||closed)return;Set<String> cells=new HashSet<>();List<DeepRadarState.Row> rows=rows();
        for(DeepRadarState.Row r:rows)if(r.kind.equals("cell")&&cells.add(r.cell)){Path path=new Path();float min=Float.POSITIVE_INFINITY,max=Float.NEGATIVE_INFINITY;boolean polar=false;
            for(int i=0;i<r.cornerCount();i++){float x=projector.screenX(r.cornerLon(i)),y=projector.screenY(r.cornerLat(i));polar|=Math.abs(r.cornerLat(i))>MapProjection.MAX_LATITUDE;min=Math.min(min,x);max=Math.max(max,x);if(i==0)path.moveTo(x,y);else path.lineTo(x,y);}path.close();
            if(!polar&&max-min<Math.max(projector.width()*2,dp(100))){paint.setStyle(Paint.Style.FILL);paint.setColor(0x33515fee);canvas.drawPath(path,paint);paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(dp(1));paint.setColor(0xaa6477ff);canvas.drawPath(path,paint);}}
        paint.setStyle(Paint.Style.FILL);
        for(DeepRadarState.Row r:rows){if(!r.located())continue;float x=projector.screenX(r.lon),y=projector.screenY(r.lat),radius=dp(15);if(x< -radius||x>projector.width()+radius||y< -radius||y>projector.height()+radius)continue;
            paint.setColor(r.kind.equals("exact")?0xee198a60:r.kind.equals("fort")?0xee256fb0:0xee6653b7);canvas.drawCircle(x,y,radius,paint);Bitmap icon=icon(r.species);
            if(icon!=null){paint.setColor(Color.WHITE);canvas.drawBitmap(icon,null,new RectF(x-radius,y-radius,x+radius,y+radius),paint);}else{paint.setColor(Color.WHITE);paint.setTextSize(dp(10));paint.setTextAlign(Paint.Align.CENTER);canvas.drawText(Integer.toString(r.species),x,y+dp(3),paint);}
            if(r.kind.equals("cell")){paint.setColor(Color.WHITE);paint.setTextSize(dp(12));canvas.drawText("?",x+radius,y-radius/2,paint);}}
    }
    boolean tap(float x,float y){List<DeepRadarState.Row> hits=new ArrayList<>();for(DeepRadarState.Row r:rows())if(r.located()&&Math.hypot(projector.screenX(r.lon)-x,projector.screenY(r.lat)-y)<dp(25))hits.add(r);if(hits.isEmpty())return false;choose(hits);return true;}
    private String name(DeepRadarState.Row r){if(names==null){names=new JSONObject();try(InputStream in=context.getAssets().open("pgrazor/map-icons/pokemon-names.json")){ByteArrayOutputStream out=new ByteArrayOutputStream();byte[] b=new byte[8192];int n;while((n=in.read(b))!=-1&&out.size()<250000)out.write(b,0,n);names=new JSONObject(out.toString("UTF-8"));}catch(Exception ignored){}}
        return names.optString(Integer.toString(r.species),"#"+r.species);}
    private Bitmap icon(int id){if(icons.containsKey(id))return icons.get(id);Bitmap result=null;try(InputStream in=context.getAssets().open("pgrazor/feed-icons/pokemon/"+id+".png")){result=BitmapFactory.decodeStream(in);}catch(Exception ignored){}icons.put(id,result);return result;}
    private void list(){if(!enabled||closed)return;List<DeepRadarState.Row> all=rows();all.sort(Comparator.comparingInt(r->r.species));String[] labels=new String[all.size()+1];labels[0]="Filters…";for(int i=0;i<all.size();i++)labels[i+1]=name(all.get(i))+" · "+all.get(i).locationLabel();
        show(new AlertDialog.Builder(ui).setTitle("Deep Radar · "+all.size()+" spawns").setItems(labels,(d,which)->{if(which==0)filters();else details(all.get(which-1));}).setNegativeButton("Close",null).create());}
    private void choose(List<DeepRadarState.Row> hits){if(hits.size()==1){details(hits.get(0));return;}String[] labels=new String[hits.size()];for(int i=0;i<labels.length;i++)labels[i]=name(hits.get(i))+" · "+hits.get(i).locationLabel();show(new AlertDialog.Builder(ui).setTitle("Spawns at this location").setItems(labels,(d,i)->details(hits.get(i))).setNegativeButton("Close",null).create());}
    private DeepRadarState.Row current(String id){for(DeepRadarState.Row r:rows())if(r.encounter.equals(id))return r;return null;}
    private void details(DeepRadarState.Row initial){String extra=initial.kind.equals("cell")?"\nThe spawn is somewhere in this cell. Pocket narrowing is not yet verified.":initial.kind.equals("fort")?"\nThis pin is the linked PokéStop, not the spawn's exact coordinates.":"";
        AlertDialog.Builder b=new AlertDialog.Builder(ui).setTitle(name(initial)).setMessage(initial.locationLabel()+(initial.located()?String.format(java.util.Locale.US,"\n%.6f, %.6f",initial.lat,initial.lon):"")+extra+"\nRegistration: "+(initial.uncaught<0?"unknown":initial.uncaught==1?"not caught":"caught")).setNegativeButton("Close",null);
        if(initial.located()&&walk!=null)b.setNeutralButton(initial.kind.equals("cell")?"Walk to area":"Walk",null);
        if(initial.located()&&teleport!=null)b.setPositiveButton("Teleport",null);
        AlertDialog d=b.create();d.setOnShowListener(v->{MapTheme.styleButtons(d);if(initial.located()&&walk!=null)d.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener(w->move(initial.encounter,walk,d));if(initial.located()&&teleport!=null)d.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(w->move(initial.encounter,teleport,d));});show(d);}
    private void move(String id,WorldMapDialog.DestinationListener action,AlertDialog d){DeepRadarState.Row r=current(id);if(r==null||!r.located()){Toast.makeText(context,"Spawn is no longer fresh",Toast.LENGTH_SHORT).show();d.dismiss();return;}if(action.onDestination(r.lat,r.lon))d.dismiss();}
    private void filters(){LinearLayout box=new LinearLayout(ui);box.setOrientation(LinearLayout.VERTICAL);int pad=(int)dp(16);box.setPadding(pad,pad,pad,pad);TextView hint=new TextView(ui);hint.setText("Pokédex numbers, separated by commas. Leave blank for all species.");box.addView(hint);EditText input=new EditText(ui);input.setSingleLine(false);input.setText(prefs.getString("DeepRadarSpecies",""));box.addView(input);CheckBox unknown=new CheckBox(ui);unknown.setText("Not caught only (excludes unknown registration)");unknown.setChecked(unregistered);box.addView(unknown);
        AlertDialog d=new AlertDialog.Builder(ui).setTitle("Deep Radar filters").setView(box).setNegativeButton("Cancel",null).setPositiveButton("Apply",null).create();d.setOnShowListener(v->{MapTheme.style(box);MapTheme.styleButtons(d);d.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(w->{try{String text=input.getText().toString().trim();species=DeepRadarState.speciesFilter(text);unregistered=unknown.isChecked();prefs.edit().putString("DeepRadarSpecies",text).putBoolean("DeepRadarUnregistered",unregistered).apply();d.dismiss();refresh();}catch(IllegalArgumentException e){input.setError(e.getMessage());}});});show(d);}
    private void show(AlertDialog d){if(closed)return;dialogs.add(d);d.setOnDismissListener(v->dialogs.remove(d));d.show();MapTheme.styleButtons(d);}
    private float dp(float n){return n*context.getResources().getDisplayMetrics().density;}
    void close(){if(closed)return;closed=true;Control.setDeepRadarOpen(false);main.removeCallbacks(refreshTask);for(AlertDialog d:new ArrayList<>(dialogs))d.dismiss();dialogs.clear();icons.clear();}
}
