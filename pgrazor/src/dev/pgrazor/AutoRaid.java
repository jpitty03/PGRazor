package dev.pgrazor;

import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import java.util.Properties;
import org.json.JSONObject;

/** Explicit raid sessions. A restart/account change never resumes spending passes. */
public final class AutoRaid {
    private AutoRaid() {}
    private static final Object LOCK=new Object();
    private static final Handler MAIN=new Handler(Looper.getMainLooper());
    private static AutoRaidPreferences preferences=new AutoRaidPreferences(), session;
    private static long token=System.currentTimeMillis(), lastEvent, route, travelSerial, motionGeneration;
    private static boolean running,stopping;
    private static String message="Not running";
    private static int completed;
    public static AutoRaidPreferences preferences(){synchronized(LOCK){return preferences.copy();}}
    public static void configure(AutoRaidPreferences value){
        AutoRaidPreferences copy=value.copy();copy.validate();
        synchronized(LOCK){if(running||stopping)throw new IllegalStateException("Stop Auto-Raid before changing its settings.");preferences=copy;}
        Control.saveSettings();
    }
    public static void read(Properties p){synchronized(LOCK){preferences=new AutoRaidPreferences();preferences.read(p);running=stopping=false;session=null;token++;route=travelSerial=0;message="Not running";}}
    public static void write(Properties p){synchronized(LOCK){preferences.write(p);}}
    public static boolean running(){synchronized(LOCK){return running||stopping;}}
    public static String status(){synchronized(LOCK){return message+(session==null?"":" · "+completed+"/"+session.maxRaids+" raids");}}
    public static void start(AutoRaidPreferences value){
        if(!Control.automationReady())throw new IllegalStateException("Wait for the game and location to be ready.");
        if((Control.battleOptions()&1)==0)throw new IllegalStateException("Enable Raid auto quick attack in battle settings first.");
        if(OneShotAutomation.enabled()||GiftBatchSettings.running()||BatchEvolutionSettings.enabled()||ShundoSniper.active()||GymAutomationSettings.running())
            throw new IllegalStateException("Stop the current automation before starting Auto-Raid.");
        if(Control.engine.walking||Control.gameRoutesRunning()||Control.automationPlanning())throw new IllegalStateException("Stop the current walk or Route first.");
        configure(value);
        synchronized(LOCK){session=preferences.copy();token++;completed=0;travelSerial=route=0;motionGeneration=Control.engine.routeGeneration();lastEvent=SystemClock.elapsedRealtime();running=true;stopping=false;message="Checking raid support";}
        Control.event("auto-raid-session","Started; Premium passes "+(value.premiumPass?"allowed":"disabled"));
    }
    public static void stop(){synchronized(LOCK){if(!running&&!stopping)return;running=false;stopping=true;message="Stopping; waiting for the current raid action";stopMovement();}}
    private static void stopMovement(){if(route!=0){Control.engine.stopRoute(route);route=0;}}
    public static void reset(){synchronized(LOCK){stopMovement();running=stopping=false;token++;session=null;travelSerial=0;message="Not running";}}
    /** Called by the main movement loop. Manual movement revokes this session's travel. */
    public static void tick(boolean manual){synchronized(LOCK){
        if(!running&&!stopping)return;
        if(running&&(manual||Control.engine.routeGeneration()!=motionGeneration)){stopMovement();running=false;stopping=true;message="Stopping: movement changed";}
        if(SystemClock.elapsedRealtime()-lastEvent>45000){stopMovement();running=stopping=false;token++;message="Stopped: raid controller stopped responding; check the game before restarting.";}
    }}
    public static String config(){
        if(!running())return null;
        // Cooldown consults location/state; never acquire its lock while holding LOCK.
        long cooldown=Math.max(0,Math.min(7200000L,Cooldown.remaining()));
        synchronized(LOCK){
        if((!running&&!stopping)||session==null)return null;
        AutoRaidPreferences p=session;
        return "R1|"+token+"|"+(running?1:0)+"|"+p.tierMask+"|"+p.maxRaids+"|"+p.minPlayers+"|"+p.waitSeconds+"|"+(p.walk?1:0)+"|"+(p.freePass?1:0)+"|"+(p.premiumPass?1:0)+"|"+(p.stopOnShiny?1:0)+"|"+p.bosses+"|"+cooldown;
    }}
    public static void publish(String detail){
        try{
            JSONObject event=new JSONObject(detail);
            long received=event.getLong("token");
            synchronized(LOCK){
                if(received!=token||(!running&&!stopping))return;
                String phase=event.optString("phase","");
                if(phase.isEmpty())throw new IllegalArgumentException("Missing raid phase");
                long serial=0;double lat=0,lon=0;
                if("travel".equals(phase)){
                    serial=event.getLong("travel");lat=event.getDouble("latitude");lon=event.getDouble("longitude");
                    if(serial<=0||!Double.isFinite(lat)||!Double.isFinite(lon)||Math.abs(lat)>90||Math.abs(lon)>180)
                        throw new IllegalArgumentException("Invalid raid travel");
                }
                lastEvent=SystemClock.elapsedRealtime();
                message=event.optString("message",message);completed=Math.min(session.maxRaids,Math.max(completed,event.optInt("completed",completed)));
                if("done".equals(phase)||"stopped".equals(phase)||"error".equals(phase)){stopMovement();running=stopping=false;return;}
                if("travel".equals(phase)&&running){
                    if(serial<=travelSerial)return;travelSerial=serial;
                    final long sequence=serial;final double latitude=lat,longitude=lon;
                    MAIN.post(()->Control.autoRaidTravel(received,sequence,latitude,longitude));
                }
            }
        }catch(Exception invalid){Control.event("auto-raid-diagnostic","Invalid raid controller event: "+invalid.getClass().getSimpleName()+": "+invalid.getMessage());}
    }
    // Control holds its state lock first, matching preference reload/export lock order.
    static void travel(long received,long serial,double lat,double lon){synchronized(LOCK){
        if(received!=token||!running||serial!=travelSerial)return;
        if(!Control.automationReady()){stop();message="Stopping: game unavailable before travel";return;}
        try{
            if(session.walk){Control.walkRoute(new double[][]{{lat,lon}});route=Control.engine.activeRouteToken();}
            else {Control.teleport(lat,lon);route=0;}
            motionGeneration=Control.engine.routeGeneration();
        }catch(IllegalArgumentException unavailable){stop();message="Stopping: "+unavailable.getMessage();}
    }}
}
