package dev.pgrazor;

import android.os.SystemClock;
import org.json.JSONObject;

/** Explicit, session-only tasks. No saved enabled flag and no automatic restart. */
public final class OneShotAutomation {
    private OneShotAutomation() {}
    private static long token=System.currentTimeMillis(),route,deadline,remoteDeadline;
    private static double previousSpeed;
    private static boolean incense,remote,hold;
    private static int nanab,razz,pinapReserve;
    private static boolean pinap;
    private static String incenseMessage="Activate Daily Adventure Incense in game, then start a session.",remoteMessage="Ready";
    public static synchronized boolean incenseRunning(){return incense;}
    public static synchronized boolean remoteFeedRunning(){return remote;}
    public static synchronized boolean idleBlocked(){return incense||hold;}
    public static synchronized boolean enabled(){return incense||remote;}
    public static synchronized boolean overlayVisible(){return incense||remote||hold;}
    public static synchronized String overlayStatus(){
        if(incense){
            long seconds=Math.max(0,(deadline-SystemClock.elapsedRealtime())/1000);
            long meters=Math.round(Math.max(0,2625-Control.engine.routeProgress().remainingMeters));
            return "Incense "+(seconds/60)+":"+String.format(java.util.Locale.US,"%02d",seconds%60)+" · "+meters+" m";
        }
        return remote?remoteMessage:incenseMessage;
    }
    public static synchronized String incenseStatus(){
        if(!incense)return incenseMessage;
        long seconds=Math.max(0,(deadline-SystemClock.elapsedRealtime())/1000);
        return incenseMessage+" · "+(seconds/60)+":"+String.format(java.util.Locale.US,"%02d",seconds%60)+" remaining";
    }
    public static synchronized String remoteFeedStatus(){return remoteMessage;}
    private static void ready(){
        if(!Control.automationReady())throw new IllegalStateException("Wait for the game and location to be ready.");
        if(incense||remote||BatchEvolutionSettings.enabled()||GiftBatchSettings.running())throw new IllegalStateException("Finish or stop the current automation first.");
    }
    public static synchronized void startIncense(double heading){
        if(!Double.isFinite(heading)||heading<0||heading>=360)throw new IllegalArgumentException("Heading must be 0–359 degrees.");
        ready();
        if(Control.engine.walking||Control.gameRoutesRunning()||Control.automationPlanning())throw new IllegalStateException("Stop your current walk or Route first.");
        double lat=Math.toRadians(Control.currentLatitude()),lon=Math.toRadians(Control.currentLongitude());
        double b=Math.toRadians(heading),d=2625.0/6371008.8;
        double target=Math.asin(Math.sin(lat)*Math.cos(d)+Math.cos(lat)*Math.sin(d)*Math.cos(b));
        double dest=lon+Math.atan2(Math.sin(b)*Math.sin(d)*Math.cos(lat),Math.cos(d)-Math.sin(lat)*Math.sin(target));
        previousSpeed=Control.engine.speedKmh;
        Control.walkRoute(new double[][]{{Math.toDegrees(target),(Math.toDegrees(dest)+540)%360-180}});
        Control.engine.speed(10.5);route=Control.engine.activeRouteToken();
        token++;hold=false;incense=true;deadline=SystemClock.elapsedRealtime()+900000;
        incenseMessage="Walking; starting bird observer";
    }
    private static void finishIncense(String message,boolean birdHold){
        if(incense&&Control.engine.routeGeneration()==route){
            Control.engine.stopRoute(route);
            if(Control.engine.speedKmh==10.5)Control.engine.speed(previousSpeed);
        }
        incense=false;hold=birdHold;incenseMessage=message;Control.saveState();
    }
    public static synchronized void stopIncense(){finishIncense("Session stopped",false);}
    public static synchronized void startRemoteFeed(int n,int r,boolean p,int pr){
        if(n<0||r<0||pr<0||n>1000||r>1000||pr>1000)throw new IllegalArgumentException("Reserves must be 0–1000.");
        ready();
        if(GymAutomationSettings.running()||SmartCatchSettings.gymSpinActive())throw new IllegalStateException("Stop Gym automation and VGP+ gym spinning before starting this batch.");
        token++;nanab=n;razz=r;pinap=p;pinapReserve=pr;remote=true;remoteDeadline=SystemClock.elapsedRealtime()+1800000;
        remoteMessage="Finding your deployed defenders";
    }
    public static synchronized void stopRemoteFeed(){remote=false;remoteMessage="Stopping; any submitted feed will finish once.";}
    public static synchronized String config(){
        tick(false);
        return "1|"+token+"|"+(remote?1:0)+"|"+nanab+"|"+razz+"|"+(pinap?1:0)+"|"+pinapReserve+"|"+(incense?1:0);
    }
    public static synchronized void tick(boolean manual){
        if(incense){
            if(!Control.automationReady())finishIncense("Session stopped: game or location unavailable",true);
            else if(manual||Control.engine.routeGeneration()!=route)finishIncense("Session stopped: movement changed",false);
            else if(SystemClock.elapsedRealtime()>=deadline||!Control.engine.walking)finishIncense("Session complete",true);
            else if(Control.engine.speedKmh!=10.5)finishIncense("Session stopped: speed changed",false);
        }else if(manual)hold=false;
        if(remote&&(!Control.automationReady()||SystemClock.elapsedRealtime()>=remoteDeadline))stopRemoteFeed();
    }
    public static synchronized void reset(){if(incense)finishIncense("Session stopped",false);remote=false;hold=false;token++;remoteMessage="Ready";}
    public static synchronized void publish(String kind,String detail){
        try{
            JSONObject data=new JSONObject(detail);
            if(!Long.toString(token).equals(data.optString("token")))return;
            if("remote-gym".equals(kind)){
                remoteMessage=data.optString("message","Waiting");
                String phase=data.optString("phase");
                if("done".equals(phase)||"stopped".equals(phase)||"error".equals(phase))remote=false;
            }else if("incense-open".equals(kind)){
                incenseMessage=data.optString("message","Encounter opening requested");
            }else if(incense&&"incense-bird".equals(kind)){
                Control.setVgpEnabled(false);
                finishIncense("Galarian bird found! Walking and VGP stopped. Handle the encounter manually.",true);
                notifyBird();
            }else if(incense&&"incense-session".equals(kind)){
                if("error".equals(data.optString("phase"))){Control.setVgpEnabled(false);finishIncense(data.optString("message","Observer unavailable"),true);}
                else incenseMessage="Walking; watching for Galarian birds";
            }
        }catch(Exception ignored){}
    }
    private static void notifyBird(){
        new android.os.Handler(android.os.Looper.getMainLooper()).post(()->{
            android.app.Activity activity=Control.accountActivity();
            if(activity==null||activity.isFinishing())return;
            android.widget.Toast.makeText(activity,"Galarian bird found — walking and VGP stopped",android.widget.Toast.LENGTH_LONG).show();
            try{
                final android.media.ToneGenerator tone=new android.media.ToneGenerator(android.media.AudioManager.STREAM_NOTIFICATION,90);
                tone.startTone(android.media.ToneGenerator.TONE_PROP_BEEP2,1000);
                new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(tone::release,1200);
                android.os.Vibrator vibrator=(android.os.Vibrator)activity.getSystemService(android.content.Context.VIBRATOR_SERVICE);
                if(vibrator!=null&&vibrator.hasVibrator())vibrator.vibrate(700);
            }catch(RuntimeException ignored){}
        });
    }
}
