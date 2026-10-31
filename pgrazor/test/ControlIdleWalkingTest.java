package dev.pgrazor;

/** Exercises the actual Control gating used by Pad, with Android clock/lifecycle shells. */
public final class ControlIdleWalkingTest {
    static void check(boolean ok,String message){if(!ok)throw new AssertionError(message);}
    static void flag(String name,boolean value)throws Exception{
        java.lang.reflect.Field f=Control.class.getDeclaredField(name);f.setAccessible(true);f.setBoolean(null,value);
    }
    static void idle(){for(int i=0;i<40;i++)Control.stepMotion(.1,false);}
    static void stopped(){check(!Control.engine.idleWalkingActive()&&Control.engine.metersPerSecond()==0,"Idle motion not stopped");}
    public static void main(String[] args)throws Exception{
        check(!Control.idleWalking(),"Must default off");
        flag("foreground",true);Control.connect();Control.engine.enabled=true;Control.engine.position(0,0);
        idle();stopped();Control.setIdleWalking(true);idle();check(Control.engine.idleWalkingActive(),"Ready idle motion starts");
        for(String gate:new String[]{"foreground","backendReady","desiredEnabled"}){
            flag(gate,false);Control.stepMotion(.1,false);stopped();
            double lat=Control.engine.latitude,lon=Control.engine.longitude;idle();check(lat==Control.engine.latitude&&lon==Control.engine.longitude,"Closed gate moved position");
            flag(gate,true);idle();check(Control.engine.idleWalkingActive(),"Gate recovery failed: "+gate);
        }
        for(String gate:new String[]{"autoWalkPlanning","gameRoutesPlanning"}){
            flag(gate,true);Control.stepMotion(.1,false);stopped();flag(gate,false);idle();
            check(Control.engine.idleWalkingActive(),"Planning recovery failed");
        }
        Control.stepMotion(.1,true);stopped();for(int i=0;i<50;i++)Control.stepMotion(.1,true);stopped();
        idle();check(Control.engine.idleWalkingActive(),"Joystick release did not resume");
        android.os.SystemClock.now+=9000;Control.stepMotion(.1,false);stopped();Control.renewLease();idle();
        check(Control.engine.idleWalkingActive(),"Lease recovery failed");
        Control.suspendForBackground();idle();stopped();check(Control.idleWalking(),"Background lost preference");
        Control.setIdleWalking(false);check(!Control.idleWalking(),"Disable preference failed");
        System.out.println("PASS idle walking Control gates: foreground, readiness, location mode, planning, joystick, lease and background");
    }
}
