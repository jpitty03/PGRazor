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
        load.setAccessible(true); load.invoke(null,app);
        Control.setBattleOptions(111); Control.setBattleOptions(981);
        if(!"981".equals(Control.exportSettings().getProperty("battleOptions")))
            throw new AssertionError("Export missed latest queued choice");
        Control.setAutoBerry(705); Control.setBattleOptions(2);
        java.util.Properties incoming=new java.util.Properties();
        incoming.setProperty("idleWalking","false"); incoming.setProperty("battleOptions","5"); incoming.setProperty("autoBerry","701");
        Control.importSettings(app,incoming,9.3); Control.flushSettings(5000);
        if(Control.idleWalking())throw new AssertionError("Idle walking import did not apply false");
        if(Control.battleOptions()!=5 || Control.autoBerry()!=701 || Control.vgpEnabled())
            throw new AssertionError("Import ordering, legacy berry migration or VGP OFF changed");
        if(!"5".equals(Control.exportSettings().getProperty("battleOptions")))
            throw new AssertionError("A queued snapshot overwrote imported choices");
        Control.setDarkTheme(false); Control.flushSettings(5000);
        Field writer=Control.class.getDeclaredField("settingsWriter"); writer.setAccessible(true);
        Object original=writer.get(null);
        writer.set(null,new SettingsWriter(Runnable::run,p->{throw new IOException("disk unavailable");},e->{}));
        try {
            Control.setDarkTheme(true);
            load.invoke(null,app);
            if(!Control.darkTheme())throw new AssertionError("Failed save allowed stale settings reload");
        } finally { writer.set(null,original); }
        System.out.println("PASS: export/import ordering, migration and failed-save reload isolation");
    }
}
