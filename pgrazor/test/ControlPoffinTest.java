package dev.pgrazor;
public final class ControlPoffinTest {
    static void check(boolean b,String message){if(!b)throw new AssertionError(message);}
    public static void main(String[] args)throws Exception{
        android.app.Application app=new android.app.Application(){public java.io.File getFilesDir(){return new java.io.File(args[0]);}};
        java.lang.reflect.Method load=Control.class.getDeclaredMethod("loadSettings",android.app.Application.class);load.setAccessible(true);load.invoke(null,app);
        if(args[1].equals("load")){check(Control.buddyBerry()==709,"Poffin restored after restart");return;}
        Control.setBuddyBerry(709);check(Control.buddyBerry()==709,"Poffin accepted for buddy");
        Control.setBuddyBerry(704);check(Control.buddyBerry()==709,"wrong Poffin ID rejected");
        Control.setAutoBerry(0);Control.selectBerryPriority(709);check(Control.berryPriority().isEmpty(),"Poffin excluded from capture");
        Control.setItemCatalog("709|Poffin|3\n708|Silver Pinap Berry|4");
        check(Control.berryCount(709)==3&&Control.berryCount(708)==4,"separate treat stocks");
        check(!Control.itemCatalog().contains("709|"),"Poffin excluded from recycling picker");
        Control.setItemCatalog("709|Poffin|100001");check(Control.berryCount(709)==0,"invalid quantity rejected");
        Control.setItemCatalog("709|Poffin|2");Control.setItemCatalog("");check(Control.berryCount(709)==0,"missing stock cleared");
        try{Control.saveItemRules(true,"709:0","");throw new AssertionError("Poffin recycling rule accepted");}catch(IllegalArgumentException expected){}
        Control.flushSettings(5000);check(Control.exportSettings().getProperty("buddyBerry").equals("709"),"Poffin saved");
        Control.setBuddyBerry(0);check(Control.buddyBerry()==0,"can switch Poffin off");
        Control.setBuddyBerry(709);Control.flushSettings(5000);
        System.out.println("PASS Poffin: stock separation, recycling/capture exclusion, preferences and OFF");
    }
}
