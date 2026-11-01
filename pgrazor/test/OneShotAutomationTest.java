package dev.pgrazor;

import android.os.SystemClock;

public final class OneShotAutomationTest {
    private static void require(boolean value,String message){if(!value)throw new AssertionError(message);}
    private static void rejects(Class<? extends RuntimeException> type,Runnable action){
        try{action.run();}catch(RuntimeException e){if(type.isInstance(e))return;throw e;}
        throw new AssertionError("Expected "+type.getSimpleName());
    }
    private static void clean(){
        OneShotAutomation.reset();Control.engine=new MotionEngine();Control.ready=true;
        Control.gameRoute=Control.planning=false;Control.vgp=true;Control.saves=Control.walks=0;
        BatchEvolutionSettings.active=GymAutomationSettings.active=SmartCatchSettings.active=false;
        SystemClock.now=1000;
    }
    private static String token(){return OneShotAutomation.config().split("\\|")[1];}
    private static void event(String kind,String id,String phase,String message){
        OneShotAutomation.publish(kind,"{\"token\":\""+id+"\",\"phase\":\""+phase+"\",\"message\":\""+message+"\"}");
    }
    public static void main(String[] args){
        clean();
        for(double h:new double[]{Double.NaN,Double.POSITIVE_INFINITY,-1,360})
            rejects(IllegalArgumentException.class,()->OneShotAutomation.startIncense(h));
        require(Control.walks==0&&!OneShotAutomation.enabled(),"invalid headings must not mutate movement");
        Control.ready=false;rejects(IllegalStateException.class,()->OneShotAutomation.startIncense(0));
        rejects(IllegalStateException.class,()->OneShotAutomation.startRemoteFeed(0,0,false,0));
        Control.ready=true;BatchEvolutionSettings.active=true;
        rejects(IllegalStateException.class,()->OneShotAutomation.startIncense(0));
        rejects(IllegalStateException.class,()->OneShotAutomation.startRemoteFeed(0,0,false,0));
        BatchEvolutionSettings.active=false;Control.gameRoute=true;
        rejects(IllegalStateException.class,()->OneShotAutomation.startIncense(0));
        Control.gameRoute=false;Control.planning=true;
        rejects(IllegalStateException.class,()->OneShotAutomation.startIncense(0));Control.planning=false;
        Control.engine.walkTo(38,-122);rejects(IllegalStateException.class,()->OneShotAutomation.startIncense(0));

        clean();Control.engine.speed(7);OneShotAutomation.startIncense(90);
        require(OneShotAutomation.incenseRunning()&&Control.engine.walking&&Control.engine.speedKmh==10.5,"session starts owned walk");
        require(Math.abs(Control.engine.routeProgress().remainingMeters-2625)<0.01,"route is 2625m");
        rejects(IllegalStateException.class,()->OneShotAutomation.startRemoteFeed(0,0,false,0));
        rejects(IllegalStateException.class,()->OneShotAutomation.startIncense(0));
        SystemClock.now+=899999;OneShotAutomation.tick(false);require(OneShotAutomation.incenseRunning(),"not early");
        SystemClock.now++;OneShotAutomation.tick(false);
        require(!OneShotAutomation.incenseRunning()&&!Control.engine.walking&&Control.engine.speedKmh==7,"timeout stops owned walk and restores speed");
        require(OneShotAutomation.idleBlocked(),"completion holds idle walking");
        OneShotAutomation.tick(true);require(!OneShotAutomation.idleBlocked(),"manual movement releases idle hold");

        clean();OneShotAutomation.startIncense(180);Control.engine.walkRoute(new double[][]{{38,-122}});
        long replacement=Control.engine.activeRouteToken();Control.engine.speed(8);OneShotAutomation.tick(false);
        require(!OneShotAutomation.incenseRunning()&&Control.engine.activeRouteToken()==replacement&&Control.engine.walking,"replacement route survives");
        require(Control.engine.speedKmh==8&&!OneShotAutomation.idleBlocked(),"replacement speed survives");
        clean();OneShotAutomation.startIncense(0);Control.engine.speed(9);OneShotAutomation.tick(false);
        require(!Control.engine.walking&&Control.engine.speedKmh==9,"changed speed is not overwritten");

        clean();OneShotAutomation.startIncense(0);String first=token();OneShotAutomation.stopIncense();
        OneShotAutomation.startIncense(0);String second=token();
        event("incense-bird",first,"","old");require(OneShotAutomation.incenseRunning()&&Control.vgp,"stale bird ignored");
        event("incense-bird",second,"","bird");
        require(!OneShotAutomation.incenseRunning()&&!Control.vgp&&!Control.engine.walking&&OneShotAutomation.idleBlocked(),"bird stops route and VGP with idle hold");
        require(android.widget.Toast.count>0,"bird notification requested");
        event("incense-open",second,"opened","Opening requested");require(OneShotAutomation.incenseStatus().equals("Opening requested"),"open handoff status");
        OneShotAutomation.reset();require(!OneShotAutomation.idleBlocked(),"reset clears hold");
        clean();OneShotAutomation.startIncense(0);Control.ready=false;OneShotAutomation.tick(false);
        require(!OneShotAutomation.incenseRunning()&&!Control.engine.walking&&OneShotAutomation.idleBlocked(),"readiness loss stops own route");
        clean();OneShotAutomation.startIncense(0);event("incense-session",token(),"error","Detector unavailable");
        require(!OneShotAutomation.incenseRunning()&&!Control.vgp&&OneShotAutomation.idleBlocked(),"observer error stops safely");

        clean();
        for(int bad:new int[]{-1,1001}){
            rejects(IllegalArgumentException.class,()->OneShotAutomation.startRemoteFeed(bad,0,false,0));
            rejects(IllegalArgumentException.class,()->OneShotAutomation.startRemoteFeed(0,bad,false,0));
            rejects(IllegalArgumentException.class,()->OneShotAutomation.startRemoteFeed(0,0,false,bad));
        }
        GymAutomationSettings.active=true;rejects(IllegalStateException.class,()->OneShotAutomation.startRemoteFeed(0,0,false,0));GymAutomationSettings.active=false;
        SmartCatchSettings.active=true;rejects(IllegalStateException.class,()->OneShotAutomation.startRemoteFeed(0,0,false,0));SmartCatchSettings.active=false;
        OneShotAutomation.startRemoteFeed(0,1000,true,1000);String remote=token();
        require(OneShotAutomation.config().equals("1|"+remote+"|1|0|1000|1|1000|0"),"remote config preserves limits and choice");
        rejects(IllegalStateException.class,()->OneShotAutomation.startIncense(0));
        require(!Control.engine.walking&&!OneShotAutomation.idleBlocked(),"remote feeding does not own movement");
        event("remote-gym",Long.toString(Long.parseLong(remote)-1),"done","stale");require(OneShotAutomation.remoteFeedRunning(),"stale remote completion ignored");
        OneShotAutomation.stopRemoteFeed();require(!OneShotAutomation.remoteFeedRunning()&&OneShotAutomation.config().split("\\|")[2].equals("0"),"cancel revokes remote config");
        OneShotAutomation.startRemoteFeed(0,0,false,0);SystemClock.now+=1800000;OneShotAutomation.tick(false);require(!OneShotAutomation.remoteFeedRunning(),"remote deadline");
        OneShotAutomation.startRemoteFeed(0,0,false,0);Control.ready=false;OneShotAutomation.tick(false);require(!OneShotAutomation.remoteFeedRunning(),"remote readiness loss");
        Control.ready=true;OneShotAutomation.startRemoteFeed(0,0,false,0);event("remote-gym",token(),"done","Complete");
        require(!OneShotAutomation.remoteFeedRunning()&&OneShotAutomation.remoteFeedStatus().equals("Complete"),"remote completion");
        OneShotAutomation.reset();require(!OneShotAutomation.enabled(),"final reset");
        System.out.println("PASS OneShotAutomation: lifecycle, route ownership, token gates, remote bounds, idle hold");
    }
}
