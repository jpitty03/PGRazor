package dev.pgrazor;

import android.app.Activity;

/** Small compiled boundary for verified DEX call-site/body rewrites.
 *
 * <p>Loading this class creates no helpers. Each family resolves independently on
 * first use; an unavailable delivery/dialog family never disables sign-in or
 * permission decisions. There are no game-class compile dependencies here.
 *
 * <p>DEX caller obligations: afterStart/afterResume run ONLY after the original
 * private ContextService dispatcher returns normally; beforeStop/beforePause run
 * BEFORE the original, which must still execute if helper work fails. Preserve
 * original receiver, arguments, return and thrown exception. Delivery creation
 * must occur on Android main through prepareDelivery() in BootstrapProvider
 * before Unity Activity/service startup, as required by GameLocationDelivery; this boundary
 * does not reorder lifecycle calls by posting them. Sign-in delegates its own
 * main-looper queue and never falls back to GoogleSignInClient after an error.
 * The DEX sign-in wrapper must check browserSignInEnabled() BEFORE dispatch and
 * execute its original body when false. Bootstrap configures the packaging option;
 * the flag defaults false. Changing it never constructs or reopens a helper.
 *
 * <p>locationPermission is an override predicate, not an OS permission grant:
 * false means execute original. The plugin gate must additionally reject a null
 * Activity BEFORE consulting this predicate. permissionStatus null means execute
 * original; nonnull needs a game-owned check-cast at the verified DEX call site.
 * Dialog calls run after original onCreate and remain optional/isolated.
 *
 * <p>shutdown is terminal for this process, not a pause/stop operation: browser
 * sign-in must survive backgrounding. Already initialized sign-in still accepts
 * late calls through its closed helper so consumers receive Cancelled on main.
 * A late first-ever sign-in fails explicitly, with no browser and no fabricated
 * response. New factories cannot publish after shutdown/generation changes.
 * This class installs no hooks and is NOT wired into production by itself.
 */
public final class GameBridgeDispatch {
    private GameBridgeDispatch() {}
    private static final Object LOCK=new Object();
    private static boolean closed;
    private static boolean browserSignIn;
    private static long generation;
    private static int logged;
    private static final class Cell<T> {T value;Thread creator;}
    private interface Factory<T> {T create();}
    private interface Closer<T> {void close(T value);}
    private static final Cell<GameGoogleSignIn> GOOGLE=new Cell<>();
    private static final Cell<GameLocationDelivery> DELIVERY=new Cell<>();
    private static final Cell<GameLocationPermissions> PERMISSIONS=new Cell<>();

    private static <T> T lazy(Cell<T> cell,Factory<T> factory,Closer<T> closer) {
        final long epoch;
        synchronized(LOCK){
            while(cell.creator!=null&&!closed){
                // Reflection/static initialization can synchronously reenter this
                // boundary. Do not deadlock or publish a half-created helper.
                if(cell.creator==Thread.currentThread())return null;
                try{LOCK.wait();}catch(InterruptedException interrupted){
                    Thread.currentThread().interrupt();throw new IllegalStateException("Game bridge initialization interrupted");
                }
            }
            if(closed)return null;
            if(cell.value!=null)return cell.value;
            epoch=generation;cell.creator=Thread.currentThread();
        }
        T created;
        try{created=factory.create();}
        catch(RuntimeException|Error failure){
            synchronized(LOCK){cell.creator=null;LOCK.notifyAll();}
            throw failure;
        }
        boolean accepted;
        synchronized(LOCK){
            accepted=!closed&&generation==epoch;
            if(accepted)cell.value=created;
            cell.creator=null;LOCK.notifyAll();
        }
        if(!accepted){try{closer.close(created);}catch(Throwable ignored){}return null;}
        return created;
    }
    private static GameLocationPermissions permissions(){
        return lazy(PERMISSIONS,GameLocationPermissions::create,GameLocationPermissions::shutdown);
    }
    private static GameLocationDelivery delivery(){
        return lazy(DELIVERY,GameLocationDelivery::create,GameLocationDelivery::shutdown);
    }
    private static GameLocationDelivery existingDelivery(){synchronized(LOCK){return closed?null:DELIVERY.value;}}
    private static void once(int bit,String phase){
        synchronized(LOCK){if((logged&bit)!=0)return;logged|=bit;}
        try{Control.event("game-bridge",phase);}catch(Throwable ignored){}
    }
    /** Synchronous bootstrap preparation on main; propagate factory failure so
     * bootstrap can report it without postponing initialization to service thread.
     * Idempotent after successful creation; never captures a manager or starts ticks.
     */
    public static void prepareDelivery(){
        if(delivery()==null)throw new IllegalStateException("Game bridge delivery preparation unavailable");
    }
    /** Gate future DEX browser dispatch only; pending auth retains its helper lifecycle. */
    public static void configureBrowserSignIn(boolean enabled){
        synchronized(LOCK){browserSignIn=!closed&&enabled;}
    }
    public static boolean browserSignInEnabled(){synchronized(LOCK){return !closed&&browserSignIn;}}
    public static void signIn(Activity activity,Object consumer){
        GameGoogleSignIn helper;
        synchronized(LOCK){helper=GOOGLE.value;}
        if(helper==null)helper=lazy(GOOGLE,GameGoogleSignIn::create,GameGoogleSignIn::shutdown);
        if(helper==null)throw new IllegalStateException("Game bridge sign-in unavailable");
        // Preserve the helper's explicit dispatch/factory failures. No original
        // Google client fallback and no credential/exception logging here.
        boolean stopping;synchronized(LOCK){stopping=closed;}
        if(stopping)helper.shutdown(); // late dispatch mirrors inactive JS hook: cancel, never begin
        helper.signIn(activity,consumer);
    }
    public static boolean locationPermission(String permission){
        try{GameLocationPermissions helper=permissions();
            boolean override=helper!=null&&helper.locationPermission(permission);
            if(override)once(1,"Using synthetic location; Android permission unchanged");return override;
        }catch(Throwable failure){once(2,"Location permission helper unavailable");return false;}
    }
    public static Object permissionStatus(long flag){
        try{GameLocationPermissions helper=permissions();Object value=helper==null?null:helper.permissionStatus(flag);
            if(value!=null)once(1,"Using synthetic location; Android permission unchanged");return value;
        }catch(Throwable failure){once(2,"Location permission helper unavailable");return null;}
    }
    public static void dismissDialog(Object dialog,String action){
        try{GameLocationPermissions helper=permissions();if(helper!=null)helper.dismissAdventureSync(dialog,action);}
        catch(Throwable failure){once(4,"Adventure Sync dialog helper unavailable");}
    }
    public static void afterStart(Object owner){
        try{GameLocationDelivery helper=delivery();if(helper!=null)helper.start(owner);}
        catch(Throwable failure){once(8,"Location delivery helper unavailable");}
    }
    public static void afterResume(Object owner){
        try{GameLocationDelivery helper=delivery();if(helper!=null)helper.resume(owner);}
        catch(Throwable failure){once(8,"Location delivery helper unavailable");}
    }
    public static void beforeStop(Object owner){
        try{GameLocationDelivery helper=existingDelivery();if(helper!=null)helper.stop(owner);}
        catch(Throwable failure){once(8,"Location delivery helper unavailable");}
    }
    public static void beforePause(Object owner){
        try{GameLocationDelivery helper=existingDelivery();if(helper!=null)helper.pause(owner);}
        catch(Throwable failure){once(8,"Location delivery helper unavailable");}
    }
    public static void shutdown(){
        final GameGoogleSignIn google;final GameLocationDelivery delivery;final GameLocationPermissions permissions;
        synchronized(LOCK){
            if(closed)return;closed=true;browserSignIn=false;generation++;
            google=GOOGLE.value;delivery=DELIVERY.value;permissions=PERMISSIONS.value;
            DELIVERY.value=null;PERMISSIONS.value=null;LOCK.notifyAll();
        }
        // Helpers own their own runtime/main-thread shutdown. Never hold LOCK
        // across provider restoration, handler work or a game-consumer callback.
        // Close authentication before provider restoration can synchronously
        // dispatch another sign-in through a game callback.
        try{if(google!=null)google.shutdown();}catch(Throwable failure){once(64,"Sign-in shutdown failed");}
        try{if(permissions!=null)permissions.shutdown();}catch(Throwable failure){once(32,"Permission shutdown failed");}
        try{if(delivery!=null)delivery.shutdown();}catch(Throwable failure){once(16,"Delivery shutdown failed");}
    }
}
