package dev.pgrazor.root;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.widget.TextView;

/** Foreground handoff for a game-requested cold restart. Performs no root or account operations. */
public final class RestartActivity extends Activity {
    private final Handler handler = new Handler(Looper.getMainLooper());
    private boolean scheduled;
    private final Runnable reopen = () -> {
        if (isFinishing() || isDestroyed()) return;
        Intent launch = getPackageManager().getLaunchIntentForPackage(InstallerConfig.GAME_PACKAGE);
        if (launch != null) {
            launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
            try { startActivity(launch); finish(); return; } catch (RuntimeException ignored) {}
        }
        TextView text = new TextView(this);
        text.setPadding(32, 64, 32, 32);
        text.setText("Open Pokémon GO to finish switching accounts.");
        setContentView(text);
    };

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        // An explicit component is exported for the official game's startActivityForResult call.
        // Reject launcher/adb/other-app calls; intent extras cannot impersonate the calling package.
        if (!InstallerConfig.GAME_PACKAGE.equals(getCallingPackage())) { finish(); return; }
        TextView text = new TextView(this);
        text.setTextSize(20);
        text.setPadding(32, 64, 32, 32);
        text.setText("Restarting Pokémon GO…");
        setContentView(text);
    }

    @Override protected void onResume() {
        super.onResume();
        if (!isFinishing() && !scheduled) {
            scheduled = true;
            // The caller exits after 150 ms. Stay visibly foreground before launching the new game.
            handler.postDelayed(reopen, 1500L);
        }
    }

    @Override protected void onDestroy() { handler.removeCallbacks(reopen); super.onDestroy(); }
}
