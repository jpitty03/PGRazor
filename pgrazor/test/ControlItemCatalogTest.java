package dev.pgrazor;
public final class ControlItemCatalogTest {
    static void check(boolean value,String message){if(!value)throw new AssertionError(message);}
    static void flag(String name,boolean value)throws Exception{java.lang.reflect.Field f=Control.class.getDeclaredField(name);f.setAccessible(true);f.setBoolean(null,value);}
    public static void main(String[] args)throws Exception{
        Control.configureQuickSummary(true);Control.setQuickSummaryReady(true,"Ready");
        flag("foreground",true);Control.connect();Control.setItemReady(true,"Ready");
        check(Control.itemAccessAllowed(),"Shared item readiness remains available with auto-discard off");
        check(!Control.itemTransferEnabled()&&Control.itemCatalogMode()==0,"Idle disabled transfer requests no bag scan");
        Control.requestItemCatalog();check(Control.itemCatalogMode()==2,"Opening inventory settings explicitly requests catalog");
        android.os.SystemClock.now+=5001;Control.renewLease();check(Control.itemCatalogMode()==0,"Closed settings demand expires");
        Control.requestBerryCatalog();check(Control.itemCatalogMode()==1,"Buddy controls request only berry quantities");
        android.os.SystemClock.now+=5001;Control.renewLease();check(Control.itemCatalogMode()==0,"Berry UI demand expires");
        Control.setEncounterBerryEnabled(true);Control.setEncounterBerry(true,false);
        check(Control.itemCatalogMode()==1,"Visible catch overlay requests only berry quantities");
        Control.setEncounterBerry(false,false);check(Control.itemCatalogMode()==0,"Leaving encounter stops berry sampling");
        Control.requestItemCatalog();flag("foreground",false);check(Control.itemCatalogMode()==0,"Background cannot request a catalog");
        flag("foreground",true);check(Control.itemAccessAllowed(),"Shared availability was not tied to auto-discard");
        System.out.println("PASS: Control demand-driven inventory and berry sampling");
    }
}
