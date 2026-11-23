package dev.pgrazor;

/** Exercise production Control with native-timestamped screen-transition sequences. */
public final class ControlJoystickVisibilityTest {
    static void check(boolean ok,String message){if(!ok)throw new AssertionError(message);}
    static void flag(String name,boolean value)throws Exception{
        java.lang.reflect.Field f=Control.class.getDeclaredField(name);f.setAccessible(true);f.setBoolean(null,value);
    }
    static long now=1000;
    static void at(long value){now=value;android.os.SystemClock.now=now;Control.renewLease();}
    static void map(boolean visible){Control.updateCameraHeadingAt(30,7,visible,now);}
    static void menu(boolean clear){Control.event("joystick-menu",now+"|"+(clear?"1":"0"));}
    static void both(){map(true);menu(true);}
    static void settle(){
        both();long start=now;
        for(int i=1;i<=6;i++){at(start+i*100);both();check(!Control.joystickOnMap(),"Wait through transient map frames");}
        at(start+650);both();check(Control.joystickOnMap(),"Stable map restores joystick");
    }
    public static void main(String[] args)throws Exception{
        check(Control.joystickOnMap(),"Preview works without native signals");
        flag("foreground",true);Control.connect();at(1000);
        check(!Control.joystickOnMap(),"Connecting waits for map");
        both();check(!Control.joystickOnMap(),"First map sample cannot show joystick");
        at(1700);check(!Control.joystickOnMap(),"Elapsed time alone cannot qualify cached samples");
        both();check(Control.joystickOnMap(),"Fresh continuous map evidence shows joystick");
        for(int i=0;i<30;i++){at(now+100);both();check(Control.joystickOnMap(),"Valid heartbeats never reset visibility");}
        menu(false);check(!Control.joystickOnMap(),"Main menu hides immediately while map renders");
        check(Control.joystickVisible(),"Menu preserves saved toggle");
        // A submenu closes the Pokeball menu before its camera becomes hidden.
        at(now+100);both();check(!Control.joystickOnMap(),"Menu-to-submenu gap stays hidden");
        at(now+300);both();check(!Control.joystickOnMap(),"Transient map confirmation still stays hidden");
        map(false);check(!Control.joystickOnMap(),"Submenu fully open stays hidden");
        // Hidden/visible signals between UI polls must also restart the settling interval.
        at(now+100);both();at(now+400);both();map(false);
        at(now+100);settle();
        map(false);check(!Control.joystickOnMap(),"Profile hides immediately");
        at(now+100);settle();
        Control.setJoystickVisible(false);menu(false);at(now+100);both();at(now+700);both();
        check(!Control.joystickOnMap(),"Menu return never enables a disabled preference");
        Control.setJoystickVisible(true);check(Control.joystickOnMap(),"Enabled preference on settled map works");
        at(now+1500);check(!Control.joystickOnMap(),"Expired map/menu signals hide joystick");
        both();check(!Control.joystickOnMap(),"Recovery from expired samples must settle again");
        settle();
        Control.event("joystick-menu","bad|1");check(!Control.joystickOnMap(),"Malformed menu fails closed");
        Control.event("joystick-menu",(now+1)+"|1");check(!Control.joystickOnMap(),"Future menu fails closed");
        at(now+100);settle();
        Control.updateCameraHeadingAt(30,7,true,now+1);check(!Control.joystickOnMap(),"Future map fails closed");
        at(now+100);settle();
        Control.invalidateCameraHeading("test camera lost");check(!Control.joystickOnMap(),"Camera loss hides immediately");
        at(now+100);settle();
        flag("foreground",false);check(!Control.joystickOnMap(),"Background hides joystick");
        flag("foreground",true);flag("backendReady",false);check(!Control.joystickOnMap(),"Unavailable host hides joystick");
        check(Control.joystickVisible(),"Transient visibility never changes preference");
        System.out.println("PASS joystick: immediate hide, stable restore, submenu gaps, heartbeat continuity, freshness and lifecycle");
    }
}
