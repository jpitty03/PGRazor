package dev.pgrazor;

/** Two separate JVM processes verify saved choices survive a cold start. */
public final class ControlSettingsTest {
    public static void main(String[] args) throws Exception {
        android.app.Application app=new android.app.Application() {
            public java.io.File getFilesDir(){return new java.io.File(args[1]);}
        };
        java.lang.reflect.Method load=Control.class.getDeclaredMethod("loadSettings",android.app.Application.class);
        load.setAccessible(true); load.invoke(null,app);
        Control.configureQuickBall(true); Control.configureQuickSummary(true); Control.configureCameraSteering(true);
        if(args[0].equals("save")) {
            Control.setGiftOptions(7);
            Control.setSpawnBooster(true);
            Control.setAutoBerry(705); Control.setEncounterBerryEnabled(true); Control.setBuddyBerry(703);
            Control.setBattleOptions(111); Control.setBattleOptions(981); // Rapid change: the newest value must win after coalescing.
            Control.setRocketOptions(5);
            Control.setItemCatalog("1|Poke Ball|150\n101|Potion|4");
            Control.saveItemRules(true,"1:100;101:0;","1:100;101:0;2:250;");
            try{Control.saveItemRules(true,"1:-1;","");throw new AssertionError("Negative cap accepted");}catch(IllegalArgumentException expected){}
            Control.setQuickSpinEnabled(true); Control.setAutoTransferEnabled(true); Control.setTransferProtect(13);
            Control.setQuickBallEnabled(true); Control.setQuickSummaryEnabled(true);
            Control.setIvCatchEnabled(false); Control.setIvOwnedEnabled(true);
            Control.setShinyScannerEnabled(true);
            Control.setThrowGrade(3); Control.setThrowCurve(true); Control.setMapTapMode(2);
            Control.setQuickBallReady(false,"Loading"); Control.setQuickSummaryReady(false,"Loading");
        }
        if(!Control.itemTransferEnabled()||!Control.itemRules().equals("1:100;101:0;")||!Control.itemLimits().equals("1:100;101:0;2:250;")||!Control.itemCatalog().contains("Poke Ball"))throw new AssertionError("Item limits not persisted");
        if(!Control.spawnBooster()||Control.spawnAllowed())throw new AssertionError("Spawn persistence/readiness");
        if(Control.giftOptions()!=7||Control.giftsAllowed())throw new AssertionError("Gift persistence/readiness");
        if(Control.buddyBerry()!=703||Control.buddyAllowed())throw new AssertionError("Buddy preference/readiness");
        if(!Control.encounterBerryEnabled()||!Control.berryPriority().equals("705")||Control.autoBerry()!=705)
            throw new AssertionError("Berry choice/overlay not persisted");
        if(Control.battleOptions()!=981||Control.battlesAllowed())throw new AssertionError("Other battle persistence/readiness");
        if(Control.rocketOptions()!=5||Control.rocketAllowed())throw new AssertionError("Rocket persistence/readiness");
        if(Control.itemAccessAllowed())throw new AssertionError("Item settings bypassed readiness");
        if(!Control.quickSpinEnabled()||!Control.autoTransferEnabled()||Control.transferProtect()!=13||!Control.quickBallEnabled()||!Control.quickSummaryEnabled()||Control.throwGrade()!=3||
           !Control.throwCurve()||Control.mapTapMode()!=2||!Control.cameraRelative())
            throw new AssertionError("Saved settings lost across readiness/configuration/process restart");
        if(Control.quickSpinAllowed()||Control.autoTransferAllowed()||Control.quickBallAllowed()||Control.quickSummaryAllowed()||Control.mapTapAllowed()||Control.throwAllowed())
            throw new AssertionError("Saved preferences bypassed runtime readiness");
        if(Control.ivCatchEnabled()||!Control.ivOwnedEnabled())throw new AssertionError("IV display preferences lost across process restart");
        if(!Control.shinyScannerEnabled()||Control.shinyScannerAllowed())throw new AssertionError("Scanner preference must persist without granting runtime permission");
        // Pending off-thread writes must reach disk before this process exits, or the restart cannot see them.
        Control.flushSettings(5000);
        if(args[0].equals("save")) {
            // Read the file directly: this proves the daemon writer drained the newest coalesced snapshot.
            java.util.Properties saved=new java.util.Properties();
            try(java.io.FileInputStream in=new java.io.FileInputStream(new java.io.File(args[1],"pgrazor-settings-v1.properties"))){saved.load(in);}
            if(!"981".equals(saved.getProperty("battleOptions"))||!"705".equals(saved.getProperty("berryPriority"))||!"5".equals(saved.getProperty("rocketOptions")))
                throw new AssertionError("Queued settings were not flushed to disk before exit");
        }
        System.out.println("PASS: settings "+args[0]);
    }
}
