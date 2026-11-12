package dev.pgrazor;

import android.app.Activity;
import android.app.Application;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Handler;
import android.os.Looper;
import android.os.StrictMode;
import java.io.File;
import java.io.IOException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** A process owns one immutable storage namespace. Switching always starts a new process. */
final class AccountProfiles {
    private static final Handler main = new Handler(Looper.getMainLooper());
    private static final ExecutorService io = Executors.newSingleThreadExecutor(r -> new Thread(r,"pgrazor-accounts"));
    private static AccountProfileStore store;
    private static final java.util.Set<SharedPreferences> openPreferences=java.util.Collections.newSetFromMap(new java.util.concurrent.ConcurrentHashMap<SharedPreferences,Boolean>());
    private static String startupId = "default";
    private static volatile boolean initialized, verified, available, switching, restarting, deleting;
    // Whether the game has reported account-switch support yet, and whether the "storage missing"
    // reason has already been logged this episode, so prerequisite diagnostics stay bounded.
    private static volatile boolean supportKnown, storageMissingNoted;
    private static volatile long generation;
    private static volatile String message = "Waiting for game account";
    private static final AccountCommandState commands = new AccountCommandState();
    private static String observed = "";

    /**
     * Bounded account-lifecycle diagnostics. Routed through the shared event log so a user report
     * stuck on "Waiting for game account" shows which prerequisite is missing and which transition
     * or command last ran. Carries no account id, trainer name, label or secret, and swallows any
     * sink failure so a logging problem can never alter account handling. Kinds are kept few and
     * stable ("account-storage" / "account-support" / "account-state" / "account-command") so the
     * last line of each can be retained across log rotation.
     */
    private static void diag(String kind, String detail) { try { Control.event(kind, detail); } catch (Throwable ignored) {} }

    static synchronized void init(Application app) {
        if(initialized)return;
        initialized=true;
        // Startup must know the namespace before any feature reads its settings.
        StrictMode.ThreadPolicy policy=StrictMode.allowThreadDiskWrites();
        try { store=new AccountProfileStore(app.getFilesDir()); startupId=store.selected().id; store.cancelPending(); diag("account-storage","opened"); }
        catch(Exception error) { message="Account data could not be opened. Existing data has been kept."; diag("account-storage","unavailable; existing data kept"); }
        finally { StrictMode.setThreadPolicy(policy); }
    }
    static boolean allowed(){return !initialized || verified && !switching && !restarting;}
    static String command(){return commands.current();}
    static String[] commandSnapshot(){return commands.snapshot();}
    static String status(){return message;}
    static String currentId(){return startupId;}
    static boolean matchesAutomationIdentity(String id){return verified&&!switching&&!restarting&&id!=null&&id.equals(observed);}
    static AccountProfileStore store(){return store;}
    static boolean canSwitch(){return store!=null&&available&&verified&&!switching&&!restarting&&!deleting;}
    static boolean canDelete(String id){return canSwitch()&&!startupId.equals(id)&&store.get(id)!=null;}
    static boolean canResetSignIn(){return store!=null&&available&&!verified&&!switching&&!restarting&&!deleting;}
    static void resetSignIn(){
        if(!canResetSignIn())return;
        switching=true;commands.issue("reset-login:"+java.util.UUID.randomUUID());message="Restoring sign-in choices...";
        final String request=commands.current();diag("account-command","issued "+AccountCommandState.kind(request));
        main.postDelayed(()->{if(request.equals(commands.current())&&!restarting){diag("account-command","timed out "+AccountCommandState.kind(request));logoutFailed();}},20000);
    }
    static void signInResetComplete(){main.post(()->{
        if(!commands.current().startsWith("reset-login:"))return;
        commands.clear();message="Reopening date-of-birth screen";diag("account-command","sign-in reset; reopening");
        io.execute(()->{try{flush();main.post(AccountProfiles::restart);}catch(IOException error){main.post(()->fail("Close and reopen the game to finish resetting sign-in."));}});
    });}
    static File file(Context context,String name){
        if(store==null)return new File(context.getFilesDir(),name);
        try{return store.dataFile(startupId,name);}catch(IOException error){throw new IllegalStateException("Cannot open account storage",error);}
    }
    static SharedPreferences preferences(Context context,String scope){
        SharedPreferences prefs=context.getSharedPreferences(store==null?scope:store.preferenceName(startupId,scope),Context.MODE_PRIVATE);
        openPreferences.add(prefs);return prefs;
    }
    private static void flush() throws IOException {
        Control.flushAccountWrites();
        for(SharedPreferences prefs:openPreferences)if(!prefs.edit().commit())throw new IOException("Preferences save did not finish");
    }
    static void support(boolean value){
        boolean changed=!supportKnown||available!=value;supportKnown=true;
        available=value;if(!value)message="Account switching is unavailable on this game version";
        if(changed)diag("account-support",value?"switching supported":"switching unavailable on this game version");
    }
    static void identity(String id,String trainer) {
        if(id==null||id.isEmpty()||id.length()>256||trainer==null||trainer.isEmpty()||trainer.length()>128)return;
        main.post(()->{
            if(store==null){if(!storageMissingNoted){storageMissingNoted=true;diag("account-storage","identity ignored: storage unavailable");}return;}
            if(switching||restarting||deleting||id.equals(observed))return;
            storageMissingNoted=false;
            verified=false; observed=id;
            final long epoch=generation;
            io.execute(()->{
                try {
                    AccountProfileStore.Profile found=store.observeIdentity(id,trainer);
                    if(found.label.equals("Default")||found.label.equals("New account")){
                        store.rename(found.id,trainer);found=store.get(found.id);
                    }
                    final AccountProfileStore.Profile actual=found;
                    main.post(()->{
                        if(epoch!=generation)return;
                        if(actual.id.equals(startupId)){verified=true;message=actual.label;diag("account-state","active account verified");return;}
                        switching=true;message="Restoring " + actual.label;diag("account-state","restoring a saved account");
                        try{Control.quiesceAccounts();}catch(IOException error){fail("Could not save outgoing location. Reopen the game.");return;}
                        io.execute(()->{
                            try{flush();store.selectVerified(actual.id);main.post(AccountProfiles::restart);}
                            catch(Exception error){main.post(()->fail("Could not restore account. Reopen the game."));}
                        });
                    });
                }catch(Exception error){main.post(()->{observed="";verified=false;message="Could not save this account. Automation is paused.";diag("account-state","identity save failed; automation paused");});}
            });
        });
    }
    static void sessionEnded(){
        verified=false;generation++;
        main.post(()->{observed="";try{Control.quiesceAccounts();}catch(IOException ignored){}message="Signed out; choose Google or PTC to continue";diag("account-state","session ended; sign-in required");});
    }
    static void logoutComplete(){
        main.post(()->{
            if(store==null||restarting||!switching)return;
            try{Control.quiesceAccounts();}catch(IOException error){logoutFailed();return;}
            io.execute(()->{
                try{
                    if(store.pendingId().isEmpty()){logoutFailed();return;}
                    flush();store.activatePending();
                    main.post(()->{commands.clear();message="Opening sign-in";diag("account-command","logout complete; opening sign-in");restart();});
                }catch(Exception error){logoutFailed();}
            });
        });
    }
    static void sessionSaved(){main.post(()->{
        if(!switching||restarting||!commands.current().startsWith("save-session:"))return;
        commands.clear();message="Restoring saved session...";diag("account-command","saved session captured; restoring");
        io.execute(()->{try{flush();main.post(()->restart(true));}catch(IOException error){logoutFailed();}});
    });}
    static void switchTo(Activity activity,String id) {
        if(!canSwitch())return;
        switching=true;verified=false;generation++;diag("account-command","switch requested");
        try{Control.quiesceAccounts();}catch(IOException error){fail("Could not save outgoing location. Reopen the game.");return;}
        message="Saving account...";
        io.execute(()->{
            try{
                flush();store.prepareSwitch(id);
                main.post(()->{
                    commands.issue("save-session:"+java.util.UUID.randomUUID());message="Saving sign-in...";
                    final String request=commands.current();diag("account-command","issued "+AccountCommandState.kind(request));
                    main.postDelayed(()->{if(request.equals(commands.current())&&!restarting){diag("account-command","timed out "+AccountCommandState.kind(request));logoutFailed();}},20000);
                });
            }catch(Exception error){logoutFailed();}
        });
    }
    static void logoutFailed(){main.post(()->{
        commands.clear(); observed="";
        io.execute(()->{
            try{if(store!=null)store.cancelPending();main.post(()->{
                switching=false;message="Sign-out did not finish. Return to the map to retry, or sign in if prompted.";diag("account-command","sign-out did not finish");
            });}catch(IOException error){main.post(()->fail("Could not cancel switching. Reopen the game."));}
        });
    });}
    static void add(Activity activity,String label){
        if(!canSwitch())return;
        io.execute(()->{
            try{AccountProfileStore.Profile profile=store.add(label);main.post(()->switchTo(activity,profile.id));}
            catch(Exception error){main.post(()->{message="Could not add account";diag("account-command","add account failed");});}
        });
    }
    static void rename(String id,String label,Runnable updated){io.execute(()->{
        try{store.rename(id,label);main.post(updated);}
        catch(Exception error){main.post(()->{message="Could not rename account";diag("account-command","rename account failed");});}
    });}
    static void delete(Activity activity,String id,Runnable removed){
        if(!canDelete(id))return;
        deleting=true;message="Deleting saved account...";
        Context context=activity.getApplicationContext();
        io.execute(()->{
            try{AccountSessions.deleteProfile(context,store,id);main.post(()->{deleting=false;message=store.selected().label;diag("account-command","account removed");removed.run();});}
            catch(Exception error){main.post(()->{deleting=false;message="Could not finish deleting the account. Please retry.";diag("account-command","account delete failed");});}
        });
    }
    private static void fail(String value){verified=false;message=value;diag("account-state","blocked: "+value);}
    private static void restart() {
        restart(false);
    }
    private static void restart(boolean switchSession) {
        Activity activity=Control.accountActivity();
        if(activity==null||activity.isFinishing()){fail("Account saved. Close and reopen the game to finish switching.");return;}
        restarting=true;diag("account-command","restarting app to apply account change");
        try {
            activity.startActivity(new Intent(activity,AccountRestartActivity.class)
                .putExtra("sourcePid",android.os.Process.myPid()).putExtra("switchSession",switchSession).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        }catch(RuntimeException error){restarting=false;fail("Account saved. Close and reopen the game to finish switching.");}
    }
}
