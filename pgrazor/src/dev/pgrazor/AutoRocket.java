package dev.pgrazor;

import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import java.util.Properties;
import org.json.JSONObject;

/**
 * Auto Rocket sessions: walk (or teleport) to invaded PokéStops, battle each Team GO Rocket grunt (and
 * leaders when chosen), catch the shadow Pokémon and move on. The native controller finds stops and
 * presses the game's own screens; this class owns the session, its settings and the movement. While a
 * session runs the Rocket auto attack, Auto throw and Skip cutscenes are on for the game, whatever
 * their own switches say, and back to those switches when it ends.
 */
public final class AutoRocket {
    private AutoRocket() {}
    private static final Object LOCK=new Object();
    private static final Handler MAIN=new Handler(Looper.getMainLooper());
    private static AutoRocketPreferences preferences=new AutoRocketPreferences(), session;
    private static long token=System.currentTimeMillis(), lastEvent, route, travelSerial, motionGeneration;
    private static boolean running,stopping;
    private static String message="Not running";
    private static int completed;
    public static AutoRocketPreferences preferences(){synchronized(LOCK){return preferences.copy();}}
    public static void configure(AutoRocketPreferences value){
        AutoRocketPreferences copy=value.copy();copy.validate();
        synchronized(LOCK){if(running||stopping)throw new IllegalStateException("Stop Auto Rocket before changing its settings.");preferences=copy;}
        Control.saveSettings();
    }
    public static void read(Properties p){synchronized(LOCK){preferences=new AutoRocketPreferences();preferences.read(p);running=stopping=false;session=null;token++;route=travelSerial=0;message="Not running";}}
    public static void write(Properties p){synchronized(LOCK){preferences.write(p);}}
    public static boolean running(){synchronized(LOCK){return running||stopping;}}
    public static String status(){synchronized(LOCK){return message+(session==null?"":" · "+completed+"/"+session.maxBattles+" battles");}}
    public static void start(AutoRocketPreferences value){
        if(!Control.automationReady())throw new IllegalStateException("Wait for the game and location to be ready.");
        if(AutoRaid.running()||OneShotAutomation.enabled()||GiftBatchSettings.running()||BatchEvolutionSettings.enabled()||ShundoSniper.active()||GymAutomationSettings.running())
            throw new IllegalStateException("Stop the current automation before starting Auto Rocket.");
        if(Control.engine.walking||Control.gameRoutesRunning()||Control.automationPlanning())throw new IllegalStateException("Stop the current walk or Route first.");
        configure(value);
        synchronized(LOCK){session=preferences.copy();token++;completed=0;travelSerial=route=0;motionGeneration=Control.engine.routeGeneration();lastEvent=SystemClock.elapsedRealtime();running=true;stopping=false;message="Looking for Team GO Rocket stops";}
        Control.event("auto-rocket-session","Started; "+AutoRocketPreferences.TARGET_LABELS[value.targets]+", "+(value.useTransferRules?"transfer rules":"keep all"));
    }
    public static void stop(){synchronized(LOCK){if(!running&&!stopping)return;running=false;stopping=true;message="Stopping after the current battle";stopMovement();}}
    private static void stopMovement(){if(route!=0){Control.engine.stopRoute(route);route=0;}}
    public static void reset(){synchronized(LOCK){stopMovement();running=stopping=false;token++;session=null;travelSerial=0;message="Not running";}}
    /** Called by the main movement loop. Manual movement ends the session's travel. */
    public static void tick(boolean manual){synchronized(LOCK){
        if(!running&&!stopping)return;
        if(running&&(manual||Control.engine.routeGeneration()!=motionGeneration)){stopMovement();running=false;stopping=true;message="Stopping: movement changed";}
        if(SystemClock.elapsedRealtime()-lastEvent>45000){stopMovement();running=stopping=false;token++;message="Stopped: Auto Rocket stopped responding; check the game before restarting.";}
    }}
    /** The slot-12 command while a session runs, else null. */
    public static String config(){
        if(!running())return null;
        // Cooldown consults location/state; never acquire its lock while holding LOCK.
        long cooldown=Math.max(0,Math.min(7200000L,Cooldown.remaining()));
        synchronized(LOCK){
            if((!running&&!stopping)||session==null)return null;
            AutoRocketPreferences p=session;
            return "T1|"+token+"|"+(running?1:0)+"|"+p.targets+"|"+p.maxBattles+"|"+(p.walk?1:0)+"|"+(p.useTransferRules?1:0)+"|"+cooldown+"|"+p.maxDistanceKm*1000;
        }
    }
    public static void publish(String detail){
        try{
            JSONObject event=new JSONObject(detail);
            long received=event.getLong("token");
            synchronized(LOCK){
                if(received!=token||(!running&&!stopping))return;
                String phase=event.optString("phase","");
                if(phase.isEmpty())throw new IllegalArgumentException("Missing phase");
                lastEvent=SystemClock.elapsedRealtime();
                if("diagnostic".equals(phase))return;
                long serial=0;double lat=0,lon=0;
                if("travel".equals(phase)){
                    serial=event.getLong("travel");lat=event.getDouble("latitude");lon=event.getDouble("longitude");
                    if(serial<=0||!Double.isFinite(lat)||!Double.isFinite(lon)||Math.abs(lat)>90||Math.abs(lon)>180)
                        throw new IllegalArgumentException("Invalid travel");
                }
                message=event.optString("message",message);
                completed=Math.min(session.maxBattles,Math.max(completed,event.optInt("completed",completed)));
                if("done".equals(phase)||"stopped".equals(phase)||"error".equals(phase)){stopMovement();running=stopping=false;return;}
                if("travel".equals(phase)&&running){
                    if(serial<=travelSerial)return;travelSerial=serial;
                    final long sequence=serial;final double latitude=lat,longitude=lon;
                    MAIN.post(()->Control.autoRocketTravel(received,sequence,latitude,longitude));
                }
            }
        }catch(Exception invalid){Control.event("auto-rocket-diagnostic","Invalid controller event: "+invalid.getClass().getSimpleName()+": "+invalid.getMessage());}
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
