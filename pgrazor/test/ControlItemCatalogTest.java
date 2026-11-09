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
        int token=Control.requestRegressionItemCatalog();check(token>=3&&Control.itemCatalogMode()==token,"Regression demand carries identity");
        Control.setItemCatalog("1|Ball|7");check(Control.regressionCatalogCompleted()!=token,"Ordinary update cannot acknowledge regression read");
        Control.setItemCatalog("#regression:"+(token+1)+"\n1|Ball|8");check(Control.regressionCatalogCompleted()!=token,"Foreign acknowledgement rejected");
        Control.setItemCatalog("#regression:"+token+"\n1|Ball|9");check(Control.regressionCatalogCompleted()==token,"Matching full read acknowledged");
        check(Control.regressionCatalog().contains("1|Ball|9"),"Completed counts captured");
        Control.setItemCatalog("1|Ball|10");check(Control.regressionCatalog().contains("1|Ball|9"),"Later UI updates cannot change completed read");
        int next=Control.requestRegressionItemCatalog();check(next>token,"Read identity never reused");
        Control.setItemCatalog("#regression:"+token+"\n1|Ball|11");check(Control.regressionCatalogCompleted()!=next,"Delayed previous response rejected");
        android.os.SystemClock.now+=5001;Control.renewLease();check(Control.itemCatalogMode()==0,"Regression demand expires");
        Control.setItemCatalog("#regression:"+next+"\n1|Ball|12");check(Control.regressionCatalogCompleted()!=next,"Expired read cannot succeed");
        System.out.println("PASS: Control demand-driven inventory and berry sampling");
    }
}
