package dev.pgrazor;

import java.io.IOException;
import java.lang.reflect.Field;
import java.lang.reflect.Method;

/** A failed pending save must never make reload replace live choices with an older disk snapshot. */
public final class ControlSettingsReloadTest {
    public static void main(String[] args) throws Exception {
        android.app.Application app=new android.app.Application() {
            public java.io.File getFilesDir(){return new java.io.File(args[0]);}
        };
        Method load=Control.class.getDeclaredMethod("loadSettings",android.app.Application.class);
        if(Control.gymRepeatEnabled())throw new AssertionError("Gym repeat should default off");
        load.setAccessible(true); load.invoke(null,app);
        Control.setGymRepeatEnabled(true);
        if(!"true".equals(Control.exportSettings().getProperty("gymRepeatEnabled")))
            throw new AssertionError("Export missed gym repeat toggle");
        Control.setBattleOptions(111); Control.setBattleOptions(981);
        if(!"981".equals(Control.exportSettings().getProperty("battleOptions")))
            throw new AssertionError("Export missed latest queued choice");
        Control.setAutoBerry(705); Control.setBattleOptions(2);
        java.util.Properties incoming=new java.util.Properties();
        incoming.setProperty("idleWalking","false"); incoming.setProperty("battleOptions","5"); incoming.setProperty("autoBerry","701"); incoming.setProperty("gymRepeatEnabled","false");
        Control.importSettings(app,incoming,9.3); Control.flushSettings(5000);
        if(Control.idleWalking())throw new AssertionError("Idle walking import did not apply false");
        if(Control.gymRepeatEnabled())throw new AssertionError("Gym repeat import did not apply false");
        if(Control.battleOptions()!=5 || Control.autoBerry()!=701 || Control.vgpEnabled())
            throw new AssertionError("Import ordering, legacy berry migration or VGP OFF changed");
        if(!"5".equals(Control.exportSettings().getProperty("battleOptions")))
            throw new AssertionError("A queued snapshot overwrote imported choices");
        // Evolve before transferring: a corrupt persisted mode must fail safe and disable the feature on reload.
        Control.setAutoTransferEnabled(true);
        Control.setEvolveTransferEnabled(true); Control.setEvolveTransferMode(1); Control.setEvolveTransferStages(2); Control.setEvolveTransferSpecies("1,4,25");
        if(!Control.evolveTransferEnabled()||Control.evolveTransferMode()!=1||Control.evolveTransferStages()!=2||!Control.evolveTransferSpecies().equals("1,4,25"))
            throw new AssertionError("Evolve-before-transfer choices not applied before import");
        java.util.Properties corruptEvolve=new java.util.Properties(); corruptEvolve.setProperty("evolveTransferMode","9");
        Control.importSettings(app,corruptEvolve,9.3); Control.flushSettings(5000);
        if(Control.autoTransferEnabled()||Control.evolveTransferEnabled()||Control.evolveTransferMode()!=0||Control.evolveTransferStages()!=2||!Control.evolveTransferSpecies().equals("1,4,25"))
            throw new AssertionError("Corrupt evolve-transfer mode did not fail safe to disabled on reload");
        Control.setDarkTheme(false); Control.flushSettings(5000);
        Field writer=Control.class.getDeclaredField("settingsWriter"); writer.setAccessible(true);
        Object original=writer.get(null);
        writer.set(null,new SettingsWriter(Runnable::run,p->{throw new IOException("disk unavailable");},e->{}));
        try {
            Control.setDarkTheme(true);
            load.invoke(null,app);
            if(!Control.darkTheme())throw new AssertionError("Failed save allowed stale settings reload");
        } finally { writer.set(null,original); }
        System.out.println("PASS: export/import ordering, migration, evolve-transfer corrupt-config fail-safe and failed-save reload isolation");
    }
}
