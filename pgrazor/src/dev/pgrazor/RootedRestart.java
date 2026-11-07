package dev.pgrazor;

import android.app.Activity;
import android.content.Intent;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;

/** Cold restart through the installer, whose foreground activity survives the game process. */
final class RootedRestart {
    private RootedRestart() {}

    static boolean restart(Activity activity, boolean switchSession) {
        if (activity == null) return false;
        try {
            // startActivityForResult gives the helper an OS-authenticated calling package. The
            // helper neither reads account data nor receives root commands, tokens or process IDs.
            activity.startActivityForResult(new Intent().setClassName(
                    "dev.pgrazor.root", "dev.pgrazor.root.RestartActivity"), 0x9A70);
            Control.event("rooted-restart", "Installer will reopen game; switchSession=" + switchSession);
            new Handler(Looper.getMainLooper()).postDelayed(() -> {
                Process.killProcess(Process.myPid());
                System.exit(0);
            }, 150L);
            return true;
        } catch (Throwable error) {
            // A module-only install can lack the helper. The queued switch remains durable;
            // the existing caller tells the user to close and reopen the game manually.
            Control.event("rooted-restart-unavailable", error.toString());
            return false;
        }
    }
}
