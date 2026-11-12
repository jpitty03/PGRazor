package dev.pgrazor;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.List;

/**
 * Private, isolated helper that performs a cold-process restart during an account switch.
 *
 * <p>The main app durably selects the target account and flushes its settings, then launches this
 * activity explicitly with the int extra {@code sourcePid}. It is declared {@code exported="false"}
 * on a private {@code :pgrazor_restart} process with no intent-filter, so it has no external entry
 * point. Running in its own process lets it outlive and deliberately terminate the old main process,
 * tearing down any stale native session or pending callback that could otherwise write into a
 * different account's files; simply finishing an activity in the old process would not reliably do
 * that.
 *
 * <p>It never trusts an arbitrary pid. It kills {@code sourcePid} only when that pid is still a live
 * process of this same app UID whose process name equals the package (i.e. the main process), and
 * only once: a recreation re-uses a saved flag so a recycled pid that now belongs to a fresh
 * replacement main process is never killed. After a short delay it starts the package's launcher and
 * finishes; if that fails it stays up and offers a manual "Open game" button, so the helper process
 * stays alive until the game is successfully reopened.
 *
 * <p>Session switching runs after the old process has stopped. It never initializes Control or
 * the native runtime in this helper process. Compatible with Android API 28+.
 */
public final class AccountRestartActivity extends Activity {
    /** Int extra: pid of the main process that asked for the restart. */
    public static final String EXTRA_SOURCE_PID = "sourcePid";
    private static final String STATE_HANDLED = "pgrazor_restart_handled";
    private static final long LAUNCH_DELAY_MS = 400L;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private boolean handled;
    private boolean launched;
    private boolean processing;
    private boolean sessionApplied;
    private TextView status;

    /**
     * Routes an account-switch restart. The patched APK starts this activity in its private
     * {@code :pgrazor_restart} process; PGRazor Root has no second process or manifest entry, so it
     * hands off to {@link RootedRestart}, which cold-restarts the game so the pending switch is
     * applied before Unity starts. Returns {@code false} when no restart could be initiated, so the
     * caller can fall back to asking the user to reopen the game.
     */
    public static boolean launch(Activity activity, boolean switchSession) {
        if (activity == null) return false;
        if (PgAssets.rooted()) return RootedRestart.restart(activity, switchSession);
        try {
            activity.startActivity(new Intent(activity, AccountRestartActivity.class)
                    .putExtra(EXTRA_SOURCE_PID, Process.myPid())
                    .putExtra("switchSession", switchSession)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            return true;
        } catch (RuntimeException error) {
            return false;
        }
    }

    @Override protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        handled = savedInstanceState != null && savedInstanceState.getBoolean(STATE_HANDLED, false);
        sessionApplied = savedInstanceState != null && savedInstanceState.getBoolean("sessionApplied",false);
        setContentView(buildView());
        // Act on the original caller's pid only, and only once. A recreated instance must never kill a
        // replacement main process that may have inherited a recycled pid.
        if (!handled) {
            killSourceProcess(getIntent().getIntExtra(EXTRA_SOURCE_PID, 0));
            handled = true;
        }
        handler.postDelayed(this::launchGame, LAUNCH_DELAY_MS);
    }

    private LinearLayout buildView() {
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setGravity(Gravity.CENTER);
        layout.setBackgroundColor(0xFF101014);
        layout.setPadding(48, 48, 48, 48);
        status = new TextView(this);
        status.setText("Switching account…");
        status.setTextColor(Color.WHITE);
        status.setTextSize(20);
        status.setGravity(Gravity.CENTER);
        layout.addView(status, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        return layout;
    }

    private void killSourceProcess(int sourcePid) {
        int myPid = Process.myPid();
        int myUid = Process.myUid();
        if (sourcePid <= 0 || sourcePid == myPid) return;
        ActivityManager am = (ActivityManager) getSystemService(ACTIVITY_SERVICE);
        if (am == null) return;
        List<ActivityManager.RunningAppProcessInfo> running = am.getRunningAppProcesses();
        if (running == null) return;
        String pkg = getPackageName();
        for (ActivityManager.RunningAppProcessInfo info : running) {
            // Never trust the pid alone: require the same app UID and the main process name (== package),
            // so this can only terminate the old main process, never itself or another private process.
            if (info.pid == sourcePid && info.uid == myUid && pkg.equals(info.processName)) {
                Process.killProcess(sourcePid);
                return;
            }
        }
    }

    private void launchGame() {
        if (launched || processing) return;
        try {
            // A failed/denied kill must never reuse the outgoing process and its frozen namespace.
            ActivityManager am = (ActivityManager) getSystemService(ACTIVITY_SERVICE);
            List<ActivityManager.RunningAppProcessInfo> running = am == null ? null : am.getRunningAppProcesses();
            if (running == null) throw new IllegalStateException("Cannot verify old process stopped");
            int sourcePid = getIntent().getIntExtra(EXTRA_SOURCE_PID, 0);
            if (sourcePid <= 0 || sourcePid == Process.myPid()) throw new IllegalStateException("Invalid source process");
            for (ActivityManager.RunningAppProcessInfo info : running) {
                if (info.pid == sourcePid && info.uid == Process.myUid() && getPackageName().equals(info.processName))
                    throw new IllegalStateException("Old process is still running");
            }
            if(getIntent().getBooleanExtra("switchSession",false)&&!sessionApplied){
                processing=true;
                new Thread(()->{
                    try{AccountSessions.switchPending(getApplicationContext());handler.post(()->{
                        processing=false;sessionApplied=true;if(!isDestroyed())launchGame();
                    });}
                    catch(Exception error){handler.post(()->{processing=false;if(!isDestroyed())showManualLaunch();});}
                },"pgrazor-session-switch").start();
                return;
            }
            Intent launch = getPackageManager().getLaunchIntentForPackage(getPackageName());
            if (launch == null) throw new IllegalStateException("No launcher activity for this package.");
            launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
            startActivity(launch);
            launched = true;
            finish();
        } catch (Exception failure) {
            showManualLaunch();
        }
    }

    private void showManualLaunch() {
        status.setText("Couldn't finish switching accounts.\nYour saved sessions have been kept. Tap Retry to continue.");
        LinearLayout layout = (LinearLayout) status.getParent();
        if (layout != null && layout.getChildCount() < 2) {
            Button open = new Button(this);
            open.setText("Retry");
            open.setOnClickListener(v -> launchGame());
            layout.addView(open, new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            if(getIntent().getBooleanExtra("switchSession",false)){
                Button back=new Button(this);back.setText("Return to previous account");
                back.setOnClickListener(v->{
                    if(processing)return;processing=true;
                    new Thread(()->{
                        try{AccountSessions.cancelSwitch(getApplicationContext());handler.post(()->{processing=false;sessionApplied=true;if(!isDestroyed())launchGame();});}
                        catch(Exception error){handler.post(()->{processing=false;if(!isDestroyed())status.setText("Couldn't restore the previous account. Saved sessions have been kept.");});}
                    },"pgrazor-session-return").start();
                });layout.addView(back);
            }
        }
    }

    @Override protected void onSaveInstanceState(Bundle outState) {
        super.onSaveInstanceState(outState);
        outState.putBoolean(STATE_HANDLED, handled);
        outState.putBoolean("sessionApplied",sessionApplied);
    }

    @Override protected void onDestroy() {
        handler.removeCallbacksAndMessages(null);
        super.onDestroy();
    }
}
