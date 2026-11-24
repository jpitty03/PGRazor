package dev.pgrazor;

import android.app.Activity;
import android.os.Handler;
import android.widget.Toast;

/** Runs real Control outcome-toast policy; Android shells are supplied by test_control_java.py. */
public final class ControlQuickBallOutcomeTest {
    private static void check(boolean value,String message) {
        if(!value)throw new AssertionError(message);
    }
    private static void set(String name,Object value) throws Exception {
        java.lang.reflect.Field field=Control.class.getDeclaredField(name);
        field.setAccessible(true); field.set(null,value);
    }
    private static boolean logged(String text) {
        return android.util.Log.lines.stream().anyMatch(line->line.contains("[quick-ball-toast] "+text));
    }
    private static Toast last() { return Toast.made.get(Toast.made.size()-1); }
    private static void enable() {
        Control.setQuickBallReady(true,"Ready"); Control.setQuickBallEnabled(true); Control.setCatchPreview(true); Handler.drain();
        check(Control.quickBallAllowed(),"Fixture restored to active");
    }
    /** Queues one outcome, applies a race, then drains the main thread. No toast may appear. */
    private static void raced(String outcome,Runnable race,String reason) {
        int before=Toast.made.size();
        check(Control.quickBallOutcome(outcome),"Active feature queues "+outcome);
        race.run(); Handler.drain();
        check(Toast.made.size()==before,"Queued toast discarded: "+reason);
        check(logged("suppressed outcome="+outcome+"; "+reason),"Suppression logged: "+reason);
    }
    public static void main(String[] args) throws Exception {
        Activity game=new Activity();
        check(!Control.quickBallOutcome("caught"),"Unconfigured feature never queues a toast");
        Control.configureQuickBall(true); Control.setQuickBallReady(true,"Ready");
        set("foreground",true); set("pad",new Pad(game)); Control.connect(); Handler.drain();
        check(Control.catchPreview(),"Catch preview defaults on");
        Control.setCatchPreview(false);
        check(!Control.quickBallOutcome("caught"),"Catch preview OFF never queues a toast");
        Handler.drain();
        check(Toast.made.isEmpty() && logged("suppressed outcome=caught; feature not active in foreground"),"No toast while OFF");
        Control.setCatchPreview(true);

        Control.engine.position(12.5,45.25); Control.engine.speed(7);
        boolean desired=Control.desiredEnabled();
        Control.setQuickBallEnabled(true); Handler.drain();
        check(Control.quickBallAllowed(),"Ready connected foreground feature is active");
        String[][] cases={{"caught","Caught"},{"broke-free","Not caught - broke free"},{"fled","Not caught - fled"}};
        for(String[] c:cases) {
            int before=Toast.made.size();
            check(Control.quickBallOutcome(c[0]),"Confirmed outcome queues: "+c[0]);
            check(Toast.made.size()==before,"Toast is created only by main-thread work");
            check(logged("received outcome="+c[0]+"; queued for display"),"Receipt logged before display");
            Handler.drain();
            check(Toast.made.size()==before+1 && last().shown && last().text.toString().equals(c[1]) && last().context==game,"Shows "+c[1]);
            check(logged("displayed outcome="+c[0]+"; Toast.show requested, user visibility unverified"),"Display request logged without claiming it was seen");
        }
        check(Toast.made.get(0).cancelled && Toast.made.get(1).cancelled && !last().cancelled,"A new toast replaces the previous one");

        for(String bad:new String[]{null,"","Caught","missed","error","item-replacement","unknown","caught "})
            check(!Control.quickBallOutcome(bad),"Rejects unsupported outcome: "+bad);
        Handler.drain();
        check(Toast.made.size()==3 && logged("suppressed; unsupported outcome"),"Unsupported outcomes never display");
        check(android.util.Log.lines.stream().noneMatch(line->line.contains("item-replacement")),"Unsupported host text is not echoed");

        int before=Toast.made.size();
        check(Control.quickBallOutcome("caught") && Control.quickBallOutcome("fled"),"Two outcomes queue");
        Handler.drain();
        check(Toast.made.size()==before+1 && last().text.toString().equals("Not caught - fled"),"Only the newest queued outcome displays");
        check(logged("suppressed outcome=caught; superseded or turned off"),"Superseded outcome logged");

        Toast visible=last();
        raced("caught",()->Control.setCatchPreview(false),"superseded or turned off");
        check(visible.cancelled,"Turning Catch preview OFF cancels a visible outcome toast");
        enable();
        raced("fled",()->{ Control.suspendForBackground(); try { set("foreground",true); } catch(Exception e) { throw new RuntimeException(e); } },
            "superseded or turned off");
        enable();
        raced("caught",()->{ try { set("foreground",false); } catch(Exception e) { throw new RuntimeException(e); } },
            "feature not active in foreground");
        set("foreground",true); enable();
        raced("broke-free",Control::disconnect,"superseded or turned off");
        Control.connect(); enable();
        raced("caught",()->Control.setQuickBallReady(false,"Cleanup pending"),"superseded or turned off");
        enable();
        raced("fled",()->android.os.SystemClock.now+=9000,"feature not active in foreground");
        Control.renewLease(); enable();
        raced("caught",()->{ android.os.SystemClock.now+=9000; Control.checkLease(); Control.renewLease(); },"superseded or turned off");
        enable();

        set("pad",null);
        raced("caught",()->{},"no resumed game screen");
        Activity closing=new Activity(); closing.finishing=true; set("pad",new Pad(closing));
        raced("fled",()->{},"no resumed game screen");
        set("pad",new Pad(game));

        Toast.fail=true;
        raced("caught",()->{},"toast error IllegalStateException");
        Toast.fail=false;
        check(Control.quickBallEnabled() && Control.quickBallAllowed(),"Toast failure does not disable the feature");
        Handler.accept=false;
        check(!Control.quickBallOutcome("caught") && logged("suppressed outcome=caught; main thread unavailable"),"Rejected main-thread post is reported");
        Handler.accept=true;
        before=Toast.made.size();
        check(Control.quickBallOutcome("caught"),"Recovers after failures"); Handler.drain();
        check(Toast.made.size()==before+1 && last().shown,"Toast displays again after isolated failures");

        Control.configureQuickSummary(true);Control.setQuickSummaryReady(true,"Ready");Control.setAutoTransferReady(true,"Ready");Control.setAutoTransferEnabled(true);
        Control.setQuickBallEnabled(false);Handler.drain();
        for(String[] c:new String[][]{{"transferred","Transferred"},{"transfer-error","Transfer Error"}}){
            check(Control.quickBallOutcome(c[0]),"Transfer toast independent of fast-ball toggle");Handler.drain();check(last().shown&&last().text.toString().equals(c[1]),"Transfer toast text");
        }
        raced("transferred",()->{Control.setAutoTransferEnabled(false);Control.setAutoTransferEnabled(true);},"superseded or turned off");
        // Catch preview is independent of Quick catch: with Quick catch OFF the result still shows.
        check(!Control.quickBallAllowed(),"Quick catch is off here");
        check(Control.quickBallOutcome("broke-free"),"Catch preview works without Quick catch");Handler.drain();
        check(last().shown&&last().text.toString().equals("Not caught - broke free"),"Preview toast text without Quick catch");
        Control.setQuickBallEnabled(true);Handler.drain();

        check(Control.engine.latitude==12.5 && Control.engine.longitude==45.25 && Control.engine.speedKmh==7,"Outcome handling preserves coordinates/speed");
        check(Control.desiredEnabled()==desired,"Outcome handling preserves saved location intent");
        Control.embedded("Restarting adapter");
        check(Control.quickBallEnabled() && !Control.quickBallOutcome("caught"),"Adapter initialization rejects outcomes while retaining choice");
        System.out.println("PASS: real Control ball outcome whitelist, main-thread display gate, OFF/background/disconnect/lease/readiness races, superseded/failed toasts, location preservation");
    }
}
