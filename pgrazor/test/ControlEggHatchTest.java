package dev.pgrazor;
/** Drives the actual command/expiry policy; Android touch dispatch is tested on device. */
public final class ControlEggHatchTest {
    static void check(boolean value,String text){if(!value)throw new AssertionError(text);}
    static void set(String name,Object value)throws Exception{
        java.lang.reflect.Field f=Control.class.getDeclaredField(name);f.setAccessible(true);f.set(null,value);
    }
    static void drain(){android.os.Handler.drain();}
    public static void main(String[] args)throws Exception{
        android.app.Application app=new android.app.Application(){public java.io.File getFilesDir(){return new java.io.File(args[0]);}};
        java.lang.reflect.Method load=Control.class.getDeclaredMethod("loadSettings",android.app.Application.class);load.setAccessible(true);load.invoke(null,app);
        if(args[1].equals("load")){check(Control.eggSkip()&&!Control.eggKeepSpecial(),"hatch settings survive restart");return;}
        check(!Control.eggSkip()&&Control.eggKeepSpecial()&&!Control.eggObserveOnly(),"normal defaults: Skip off, special protection on");
        Control.configureQuickSummary(true);Control.setQuickSummaryReady(true,"Ready");
        set("foreground",true);Control.connect();android.os.Handler.queue.clear();
        set("pad",new Pad(new android.app.Activity()));Control.setEggSkip(true);
        check(Control.eggHatchAllowed(),"independent hatch option, ready foreground");
        long now=android.os.SystemClock.now;
        Control.setEggObserveOnly(true);Control.eggHatchUpdate(true,true,true,now+200,"Observe");drain();check(Pad.eggTaps==0,"observe-only refuses synthetic touch");
        Control.setEggObserveOnly(false);Control.eggHatchUpdate(true,true,true,now+200,"Queued");Control.setEggObserveOnly(true);drain();check(Pad.eggTaps==0,"entering observation cancels queued touch");
        Control.setEggObserveOnly(false);
        Control.eggHatchUpdate(true,true,true,now+200,"Opening");drain();check(Pad.eggTaps==1,"fresh native tap dispatched");
        Control.eggHatchUpdate(true,true,true,now+200,"Opening");Control.setEggSkip(false);drain();check(Pad.eggTaps==1,"Off cancels queued touch");
        Control.setEggSkip(true);Control.eggHatchUpdate(true,true,true,now+200,"Opening");
        Control.eggHatchUpdate(true,false,false,0,"Pokemon screen");drain();check(Pad.eggTaps==1,"summary revokes queued touch");
        Control.eggHatchUpdate(true,true,true,now+200,"Opening");android.os.SystemClock.now=now+201;drain();check(Pad.eggTaps==1,"expired queued touch discarded");
        now=android.os.SystemClock.now;Control.eggHatchUpdate(true,true,true,now+200,"Opening");set("foreground",false);drain();check(Pad.eggTaps==1,"background blocks touch");
        set("foreground",true);Control.eggHatchUpdate(true,true,true,now+1000,"Bad deadline");drain();check(Pad.eggTaps==1,"unbounded future deadline discarded");
        now=android.os.SystemClock.now;
        Control.eggHatchUpdate(true,true,true,now+200,"Opening");
        Control.setEggKeepSpecial(false);drain();
        check(Pad.eggTaps==1,"changing keep-special cancels an already queued tap");
        check(!Control.eggHatchAllowed(),"changing keep-special immediately suspends native actions");
        android.os.SystemClock.now+=1001;check(Control.eggHatchAllowed(),"configuration pause expires");
        Control.setEggSettingsOpen(true);check(!Control.eggHatchAllowed(),"settings panel revokes native close and taps");
        android.os.SystemClock.now+=5000;check(!Control.eggHatchAllowed(),"settings stay protected beyond the pause");
        Control.setEggSettingsOpen(false);check(!Control.eggHatchAllowed(),"closing settings waits for a stable game scene");
        android.os.SystemClock.now+=1001;check(Control.eggHatchAllowed(),"ready after settings settle");
        Control.flushSettings(5000);
        check(Control.exportSettings().getProperty("eggSkip").equals("true"),"Skip hatch persisted");
        System.out.println("PASS hatch Control: defaults, leases, expiry, Off, background, summary cancellation and saved options");
    }
}
