package dev.pgrazor;
import android.app.*;
import android.content.*;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.os.*;
import android.util.AtomicFile;
import android.view.*;
import android.widget.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.*;
import java.util.concurrent.*;
public final class Cooldown {
    private static final CooldownState state=new CooldownState();
    private static final ExecutorService io=Executors.newSingleThreadExecutor();
    private static final ExecutorService geo=Executors.newSingleThreadExecutor();
    private static final Handler main=new Handler(Looper.getMainLooper());
    private static final Map<String,String> countries=new ConcurrentHashMap<>();
    private static AtomicFile file; private static long nextId; private static Context context;
    private static volatile IOException writeFailure;
    public static synchronized void init(Context app){context=app.getApplicationContext();file=new AtomicFile(AccountProfiles.file(app,"cooldown-history.txt"));try{state.decode(new String(file.readFully(),StandardCharsets.UTF_8));for(CooldownState.Entry e:state.rows)nextId=Math.max(nextId,e.id);}catch(IOException ignored){}
        // PGRazorX: read the offline borders in the background at startup, so the history's flags are ready on open.
        geo.execute(Cooldown::loadBorders);}
    private static CountryLookup loadBorders(){CountryLookup table=borders;if(table!=null)return table;
        try(InputStream in=context.getAssets().open("pgrazor/country-borders.dat")){table=CountryLookup.read(in);borders=table;}catch(Exception ignored){}return table;}
    static void flush() throws IOException {
        try {io.submit(()->{}).get(2500,TimeUnit.MILLISECONDS);if(writeFailure!=null)throw writeFailure;}
        catch(Exception error){throw new IOException("Cooldown save did not finish",error);}
    }
    private static void save(){final String data=state.encode();final AtomicFile f=file;if(f==null)return;io.execute(()->{FileOutputStream out=null;try{out=f.startWrite();out.write(data.getBytes(StandardCharsets.UTF_8));f.finishWrite(out);writeFailure=null;}catch(IOException e){writeFailure=e;if(out!=null)f.failWrite(out);Control.event("cooldown-save-error",e.toString());}});}
    public static synchronized void record(String action){double lat=Control.currentLatitude(),lon=Control.currentLongitude();if(!CooldownState.valid(lat,lon))return;long now=System.currentTimeMillis();if(!state.rows.isEmpty()){CooldownState.Entry last=state.rows.get(0);if(action.equals("Gym battle")&&last.action.equals(action)&&CooldownState.distance(lat,lon,last.lat,last.lon)<0.01&&now-last.time<60000){state.rows.set(0,new CooldownState.Entry(last.id,now,lat,lon,action));if(now/5000!=last.time/5000)save();return;}if(last.action.equals(action)&&now-last.time<1500&&CooldownState.distance(lat,lon,last.lat,last.lon)<0.01)return;}state.add(new CooldownState.Entry(++nextId,now,lat,lon,action));save();}
    public static synchronized long remaining(){if(!Control.locationKnown())return 0;return state.remaining(System.currentTimeMillis(),Control.currentLatitude(),Control.currentLongitude());}
    /** Read-only destination estimate: never records teleporting as a cooldown action. */
    public static synchronized long remainingAt(double lat,double lon){return state.remaining(System.currentTimeMillis(),lat,lon);}
    public static synchronized boolean hasHistory(){return !state.rows.isEmpty();}
    public static synchronized String label(){if(state.rows.isEmpty())return "Est. 0:00:00";if(!Control.locationKnown())return "Waiting for GPS";long seconds=(remaining()+999)/1000;return String.format(Locale.US,"Est. %d:%02d:%02d",seconds/3600,(seconds/60)%60,seconds%60);}
    private static synchronized ArrayList<CooldownState.Entry> entries(){return new ArrayList<>(state.rows);}
    private static synchronized void remove(long id){state.rows.removeIf(e->e.id==id);save();}
    private static Button button(Context c,String text){Button b=new Button(c);b.setText(text);b.setAllCaps(false);b.setTextSize(12);return b;}
    /** PGRazorX: country flag from the bundled offline borders (Geocoder needed Google services and the network). */
    private static volatile CountryLookup borders;
    private static void country(TextView label,CooldownState.Entry e){String key=String.format(Locale.US,"%.2f,%.2f",e.lat,e.lon);String cached=countries.get(key);if(cached!=null){label.setText(cached);return;}label.setText("");
        geo.execute(()->{String value="";try{CountryLookup table=loadBorders();if(table!=null)value=table.flag(e.lat,e.lon);}catch(Exception ignored){}
            if(value.isEmpty())value="\uD83C\uDF0A";countries.put(key,value);final String result=value;main.post(()->{if(label.getTag()==e)label.setText(result);});});}
    private static final String HOW_IT_WORKS="Estimated from recorded actions and current location. Teleporting and walking do not reset it. Automatic: wild encounter throws, encounter berries, spins and gym attacks. Use Add action for gym placement/feeding, accessories or an action performed outside this app. Quest catches are excluded. Deleting history changes only this estimate.";
    /**
     * PGRazorX: game-style page (same look as the world map): big title, the estimate in a white card with
     * "How it works", the history in a card that fills the page and scrolls, and the game action bar
     * (round close in the centre, Add action on the right).
     */
    public static void show(Activity a){
        final Context ui=MapTheme.wrap(a); // full-screen, matching the PGRazor dark/light theme
        LinearLayout root=new LinearLayout(ui);root.setOrientation(LinearLayout.VERTICAL);root.setPadding(MapTheme.dp(ui,12),MapTheme.dp(ui,4),MapTheme.dp(ui,12),MapTheme.dp(ui,4));
        // Estimate card.
        LinearLayout top=new LinearLayout(ui);top.setOrientation(LinearLayout.VERTICAL);top.setPadding(MapTheme.dp(ui,16),MapTheme.dp(ui,8),MapTheme.dp(ui,16),MapTheme.dp(ui,2));
        TextView timer=new TextView(ui);timer.setGravity(Gravity.CENTER);timer.setText(label());top.addView(timer);
        TextView caption=new TextView(ui);caption.setGravity(Gravity.CENTER);caption.setText("Estimated from recorded actions and your current location.");caption.setPadding(0,MapTheme.dp(ui,2),0,MapTheme.dp(ui,6));top.addView(caption);
        Button how=SettingsUi.button(ui,"How it works");top.addView(how,new LinearLayout.LayoutParams(-1,-2));
        root.addView(top,new LinearLayout.LayoutParams(-1,-2));
        // HISTORY header with paging.
        final int[] page={0};
        LinearLayout header=new LinearLayout(ui);header.setGravity(Gravity.CENTER_VERTICAL);header.setPadding(MapTheme.dp(ui,8),MapTheme.dp(ui,14),MapTheme.dp(ui,2),MapTheme.dp(ui,8));
        TextView title=new TextView(ui);title.setText("History");header.addView(title,new LinearLayout.LayoutParams(0,-2,1));
        RoundIcon previous=new RoundIcon(ui,RoundIcon.PREVIOUS,"Previous page",true),next=new RoundIcon(ui,RoundIcon.NEXT,"Next page",true);
        TextView pageLabel=new TextView(ui);pageLabel.setGravity(Gravity.CENTER);pageLabel.setPadding(MapTheme.dp(ui,10),0,MapTheme.dp(ui,10),0);
        int small=MapTheme.dp(ui,34);header.addView(previous,new LinearLayout.LayoutParams(small,small));header.addView(pageLabel,new LinearLayout.LayoutParams(-2,-2));header.addView(next,new LinearLayout.LayoutParams(small,small));
        root.addView(header);
        // History card: fills the page down to the action bar and scrolls inside.
        LinearLayout card=new LinearLayout(ui);card.setOrientation(LinearLayout.VERTICAL);card.setPadding(MapTheme.dp(ui,14),MapTheme.dp(ui,2),MapTheme.dp(ui,10),MapTheme.dp(ui,2));
        // PGRazorX: a recycled list: only the rows on screen are built (each row has three round buttons, so building
        // a whole page of them up front is what made the page slow to open).
        final ListView scroll=new ListView(ui);scroll.setVerticalScrollBarEnabled(false);scroll.setVerticalFadingEdgeEnabled(true);scroll.setFadingEdgeLength(MapTheme.dp(ui,24));
        scroll.setSelector(new android.graphics.drawable.ColorDrawable(0));scroll.setCacheColorHint(0);
        final TextView empty=new TextView(ui);empty.setText("No recorded actions yet");empty.setGravity(Gravity.CENTER);empty.setPadding(0,MapTheme.dp(ui,24),0,MapTheme.dp(ui,24));
        card.addView(empty,new LinearLayout.LayoutParams(-1,-2));
        card.addView(scroll,new LinearLayout.LayoutParams(-1,-1));
        root.addView(card,new LinearLayout.LayoutParams(-1,0,1));
        final Runnable[] repaint={null};
        final SimpleDateFormat stamp=new SimpleDateFormat("MM-dd HH:mm:ss",Locale.US);
        final ArrayList<CooldownState.Entry> shown=new ArrayList<>();
        final BaseAdapter adapter=new BaseAdapter(){
            public int getCount(){return shown.size();}
            public Object getItem(int position){return shown.get(position);}
            public long getItemId(int position){return shown.get(position).id;}
            public View getView(int position,View convert,ViewGroup parent){
                LinearLayout row=(LinearLayout)convert;
                if(row==null){row=new LinearLayout(ui);row.setGravity(Gravity.CENTER_VERTICAL);row.setPadding(0,MapTheme.dp(ui,9),0,MapTheme.dp(ui,9));
                    TextView flag=new TextView(ui);flag.setTextSize(20);flag.setSingleLine(true);flag.setGravity(Gravity.CENTER_VERTICAL);row.addView(flag,new LinearLayout.LayoutParams(MapTheme.dp(ui,34),MapTheme.dp(ui,32)));
                    LinearLayout info=new LinearLayout(ui);info.setOrientation(LinearLayout.VERTICAL);
                    TextView when=new TextView(ui);when.setTextSize(14);when.setTypeface(MapTheme.bold());when.setTextColor(MapTheme.ink());info.addView(when);
                    TextView what=new TextView(ui);what.setTextSize(12);what.setTextColor(MapTheme.muted());info.addView(what);
                    row.addView(info,new LinearLayout.LayoutParams(0,-2,1));
                    RoundIcon copy=new RoundIcon(ui,RoundIcon.COPY,"Copy coordinates",false),map=new RoundIcon(ui,RoundIcon.MAP,"Open on the map",false),del=new RoundIcon(ui,RoundIcon.DELETE,"Delete this action",false);
                    for(RoundIcon b:new RoundIcon[]{copy,map,del}){LinearLayout.LayoutParams lp=new LinearLayout.LayoutParams(small,small);lp.leftMargin=MapTheme.dp(ui,6);row.addView(b,lp);}}
                final CooldownState.Entry e=shown.get(position);
                TextView flag=(TextView)row.getChildAt(0);LinearLayout info=(LinearLayout)row.getChildAt(1);
                flag.setTag(e);country(flag,e);
                ((TextView)info.getChildAt(0)).setText(stamp.format(new Date(e.time)));
                ((TextView)info.getChildAt(1)).setText(e.action+String.format(Locale.US,"\n%.5f, %.5f",e.lat,e.lon));
                row.getChildAt(2).setOnClickListener(v->{((ClipboardManager)a.getSystemService(Context.CLIPBOARD_SERVICE)).setPrimaryClip(ClipData.newPlainText("Coordinates",String.format(Locale.US,"%.7f, %.7f",e.lat,e.lon)));Toast.makeText(a,"Coordinates copied",Toast.LENGTH_SHORT).show();});
                row.getChildAt(3).setOnClickListener(v->WorldMapDialog.show(a,e.lat,e.lon,(lat,lon)->mapApply(a,lat,lon,false),(lat,lon)->mapApply(a,lat,lon,true)));
                row.getChildAt(4).setOnClickListener(v->{remove(e.id);repaint[0].run();});
                return row;
            }};
        scroll.setDivider(new android.graphics.drawable.ColorDrawable(MapTheme.dlgHair()));scroll.setDividerHeight(Math.max(1,MapTheme.dp(ui,1)));
        scroll.setAdapter(adapter);
        Runnable render=()->{ArrayList<CooldownState.Entry> rows=entries();int pages=Math.max(1,(rows.size()+39)/40);page[0]=Math.min(page[0],pages-1);
            previous.setEnabled(page[0]>0);next.setEnabled(page[0]+1<pages);previous.setAlpha(page[0]>0?1f:0.4f);next.setAlpha(page[0]+1<pages?1f:0.4f);pageLabel.setText((page[0]+1)+" / "+pages);
            shown.clear();shown.addAll(rows.subList(Math.min(rows.size(),page[0]*40),Math.min(rows.size(),(page[0]+1)*40)));
            empty.setVisibility(rows.isEmpty()?View.VISIBLE:View.GONE);empty.setTextColor(MapTheme.muted());
            adapter.notifyDataSetChanged();};
        repaint[0]=render;
        previous.setOnClickListener(v->{if(page[0]>0){page[0]--;repaint[0].run();scroll.setSelection(0);}});
        next.setOnClickListener(v->{page[0]++;repaint[0].run();scroll.setSelection(0);});
        how.setOnClickListener(v->{
            TextView text=new TextView(ui);text.setText(HOW_IT_WORKS);text.setPadding(MapTheme.dp(ui,20),MapTheme.dp(ui,4),MapTheme.dp(ui,20),MapTheme.dp(ui,8));text.setLineSpacing(MapTheme.dp(ui,2),1f);
            ScrollView sv=new ScrollView(ui);sv.addView(text);
            AlertDialog help=new AlertDialog.Builder(ui).setTitle("How it works").setView(sv).setNegativeButton("Close",null).create();
            help.setOnShowListener(d->{MapTheme.style(sv);text.setTextColor(MapTheme.ink());MapTheme.styleButtons(help);MapTheme.gameFooter(help,null);});
            help.show();
        });
        AlertDialog dialog=new AlertDialog.Builder(ui).setTitle("Cooldown history").setView(root).setNegativeButton("Close",null).setPositiveButton("Add action",null).create();
        final Handler clock=new Handler(Looper.getMainLooper());
        final Runnable tick=new Runnable(){public void run(){if(!dialog.isShowing())return;String now=label();if(!now.contentEquals(timer.getText()))timer.setText(now);clock.postDelayed(this,1000);}};
        dialog.setOnShowListener(d->{
            MapTheme.fullScreen(dialog);MapTheme.style(root);MapTheme.styleButtons(dialog);MapTheme.pageTitle(dialog);
            for(View c:new View[]{top,card}){c.setBackground(MapTheme.cardBackground(ui));MapTheme.elevate(c,2.5f);}
            timer.setTextSize(30);timer.setTypeface(MapTheme.bold());timer.setTextColor(MapTheme.ink());
            caption.setTextSize(12.5f);caption.setTextColor(MapTheme.muted());
            SettingsUi.styleTree(ui,how);MapTheme.sectionHeader(title);pageLabel.setTextColor(MapTheme.ink());pageLabel.setTypeface(MapTheme.bold());
            render.run();
            MapTheme.gameFooter(dialog,root);
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v->showAddAction(ui,render));
            clock.post(tick);
        });
        dialog.setOnDismissListener(d->clock.removeCallbacksAndMessages(null));
        DialogLifecycle.bind(a,dialog,null);
        dialog.show();
    }
    /** "Record action here": one game pill per action, closed with the round close button. */
    private static void showAddAction(Context ui,Runnable render){
        LinearLayout choiceLayout=new LinearLayout(ui);choiceLayout.setOrientation(LinearLayout.VERTICAL);
        int pad=MapTheme.dp(ui,12);choiceLayout.setPadding(pad,pad,pad,pad);
        ScrollView choiceScroll=new ScrollView(ui);LinearLayout choiceList=new LinearLayout(ui);choiceList.setOrientation(LinearLayout.VERTICAL);
        choiceScroll.addView(choiceList,new ScrollView.LayoutParams(-1,-2));choiceLayout.addView(choiceScroll,new LinearLayout.LayoutParams(-1,-2));
        AlertDialog choiceDialog=new AlertDialog.Builder(ui).setTitle("Record action here").setView(choiceLayout).setNegativeButton("Cancel",null).create();
        String[] actions={"Catch / ball throw","Berry feeding","PokémonStop spin","Gym placement / feeding","Gym battle","Accessory action"};
        for(String act:actions){Button btn=new Button(ui);btn.setText(act);btn.setAllCaps(false);
            btn.setOnClickListener(iv->{record(act);render.run();choiceDialog.dismiss();});
            LinearLayout.LayoutParams lp=new LinearLayout.LayoutParams(-1,MapTheme.dp(ui,44));lp.bottomMargin=MapTheme.dp(ui,6);choiceList.addView(btn,lp);}
        choiceDialog.setOnShowListener(d->{MapTheme.style(choiceLayout);MapTheme.styleGameButtons(choiceLayout);MapTheme.styleButtons(choiceDialog);MapTheme.gameFooter(choiceDialog,null);});
        choiceDialog.show();
    }
    /** PGRazorX: small round white button with a drawn teal (or red) icon, like the map's floating buttons. */
    private static final class RoundIcon extends View {
        static final int COPY=0,MAP=1,DELETE=2,PREVIOUS=3,NEXT=4;
        private final int icon;private final boolean raised;private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
        /** raised: a shadowed button (paging); list rows use flat ones with a hairline ring, which scroll much lighter. */
        RoundIcon(Context c,int icon,String description,boolean raised){super(c);this.icon=icon;this.raised=raised;setContentDescription(description);setClickable(true);setFocusable(true);
            if(raised){setOutlineProvider(new ViewOutlineProvider(){public void getOutline(View v,android.graphics.Outline o){o.setOval(0,0,v.getWidth(),v.getHeight());}});
            setElevation(MapTheme.dp(c,2));}
            setForeground(new android.graphics.drawable.RippleDrawable(android.content.res.ColorStateList.valueOf(0x2236b89e),null,new android.graphics.drawable.ShapeDrawable(new android.graphics.drawable.shapes.OvalShape())));}
        @Override protected void onDraw(Canvas c){super.onDraw(c);float cx=getWidth()/2f,cy=getHeight()/2f,r=Math.min(cx,cy),u=r/17f;
            paint.setStyle(Paint.Style.FILL);paint.setColor(MapTheme.card());c.drawCircle(cx,cy,r,paint);
            if(!raised){paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(Math.max(1f,u));paint.setColor(MapTheme.outline());c.drawCircle(cx,cy,r-u,paint);}
            paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(1.8f*u);paint.setStrokeCap(Paint.Cap.ROUND);paint.setStrokeJoin(Paint.Join.ROUND);
            paint.setColor(icon==DELETE?0xffd9534f:MapTheme.accent());
            switch(icon){
                case COPY:c.drawRoundRect(cx-5*u,cy-3*u,cx+3*u,cy+6*u,1.5f*u,1.5f*u,paint);c.drawRoundRect(cx-2*u,cy-6*u,cx+6*u,cy+3*u,1.5f*u,1.5f*u,paint);break;
                case MAP:{Path p=new Path();p.moveTo(cx-7*u,cy-5*u);p.lineTo(cx-2.5f*u,cy-7*u);p.lineTo(cx+2.5f*u,cy-5*u);p.lineTo(cx+7*u,cy-7*u);p.lineTo(cx+7*u,cy+5*u);p.lineTo(cx+2.5f*u,cy+7*u);p.lineTo(cx-2.5f*u,cy+5*u);p.lineTo(cx-7*u,cy+7*u);p.close();c.drawPath(p,paint);c.drawLine(cx-2.5f*u,cy-7*u,cx-2.5f*u,cy+5*u,paint);c.drawLine(cx+2.5f*u,cy-5*u,cx+2.5f*u,cy+7*u,paint);break;}
                case DELETE:c.drawLine(cx-5*u,cy-5*u,cx+5*u,cy+5*u,paint);c.drawLine(cx-5*u,cy+5*u,cx+5*u,cy-5*u,paint);break;
                case PREVIOUS:c.drawLine(cx+2*u,cy-5*u,cx-3*u,cy,paint);c.drawLine(cx-3*u,cy,cx+2*u,cy+5*u,paint);break;
                default:c.drawLine(cx-2*u,cy-5*u,cx+3*u,cy,paint);c.drawLine(cx+3*u,cy,cx-2*u,cy+5*u,paint);break;
            }}
    }
    /** Applies a destination chosen from the history map, validating adapter readiness first (never a dummy no-op). */
    private static boolean mapApply(Activity a,double lat,double lon,boolean walk){
        if(!Control.canEnable()){Toast.makeText(a,"Location adapter is not ready",Toast.LENGTH_SHORT).show();return false;}
        try{
            if(walk)Control.walkTo(lat,lon);else Control.teleport(lat,lon);
            Control.event(walk?"cooldown-map-walk":"cooldown-map-teleport",String.format(Locale.US,"lat=%.7f lon=%.7f",lat,lon));
            return true;
        }catch(IllegalArgumentException error){Toast.makeText(a,"Select valid coordinates",Toast.LENGTH_SHORT).show();return false;}
    }
}
