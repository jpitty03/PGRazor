package dev.pgrazor;
public final class ControlManualTransferTest {
    static void check(boolean value,String why){if(!value)throw new AssertionError(why);}
    static void set(String name,Object value)throws Exception{java.lang.reflect.Field f=Control.class.getDeclaredField(name);f.setAccessible(true);f.set(null,value);}
    public static void main(String[] args)throws Exception{
        android.app.Application app=new android.app.Application(){public java.io.File getFilesDir(){return new java.io.File(args[0]);}};
        java.lang.reflect.Method load=Control.class.getDeclaredMethod("loadSettings",android.app.Application.class);load.setAccessible(true);load.invoke(null,app);
        if(args[1].equals("load")){check(Control.manualTransferFree(),"manual choice restored");return;}
        check(!Control.manualTransferFree()&&!Control.manualTransferAllowed(),"Off by default");
        Control.setManualTransferFree(true);check(!Control.manualTransferAllowed(),"choice alone cannot authorize gameplay");
        Control.configureQuickSummary(true);Control.setQuickSummaryReady(true,"Ready");set("foreground",true);Control.connect();
        check(Control.manualTransferAllowed(),"ready foreground lease permits manual UI extension");
        Control.setEggSettingsOpen(true);check(!Control.manualTransferAllowed(),"no manual warning automation behind settings");
        Control.setEggSettingsOpen(false);check(Control.manualTransferAllowed(),"closing settings resumes manual gate");
        set("foreground",false);check(!Control.manualTransferAllowed(),"background cannot authorize");set("foreground",true);
        android.os.SystemClock.now+=9000;check(!Control.manualTransferAllowed(),"connection expiry");Control.renewLease();
        check(Control.manualTransferAllowed(),"renewed lease");
        Control.manualTransferUpdate(false,"Unavailable contract");check(!Control.manualTransferReady()&&Control.manualTransferStatus().contains("Unavailable"),"metadata status visible");
        Control.manualTransferUpdate(true,"Ready");check(Control.manualTransferReady(),"readiness published");
        Control.setManualTransferFree(false);check(!Control.manualTransferAllowed()&&Control.manualTransferStatus().equals("Off"),"Off preserved");
        check(!Control.autoTransferEnabled(),"manual switch never enables auto transfer");
        Control.setManualTransferFree(true);Control.flushSettings(5000);
        check(Control.exportSettings().getProperty("manualTransferFree").equals("true"),"manual choice included in backup");
        System.out.println("PASS manual Control: defaults, saved choice, readiness, lifecycle/lease and independent auto-transfer policy");
    }
}
