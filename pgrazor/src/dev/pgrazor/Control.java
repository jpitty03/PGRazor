package dev.pgrazor;

import android.app.Activity;
import android.app.Application;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.StrictMode;
import android.os.SystemClock;
import android.util.AtomicFile;
import android.util.Log;
import java.io.*;
import java.text.SimpleDateFormat;
import java.util.*;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

/** Module interface shared by the standalone preview and an instrumented host. */
public final class Control {
    private static Application hostApplication;
    public static boolean eventBoxesAllowed() {
        Application app=hostApplication;
        return app!=null && hostLive() && active() && !catchScreenVisible()
            && dev.pgrazor.events.EventSettings.boxCollectorEnabled(app);
    }
    public static String accountCommand(){return AccountProfiles.command();}
    public static String[] accountCommandSnapshot(){return AccountProfiles.commandSnapshot();}
    public static boolean accountManagementAllowed(){return foreground && backendReady && connected();}
    public static void accountIdentity(String id,String name){AccountProfiles.identity(id,name);}
    public static void setAccountsReady(boolean ready){AccountProfiles.support(ready);}
    public static void accountSessionEnded(){AccountProfiles.sessionEnded();}
    public static void accountLogoutComplete(){AccountProfiles.logoutComplete();}
    public static void accountLogoutFailed(){AccountProfiles.logoutFailed();}
    public static void accountSignInResetComplete(){AccountProfiles.signInResetComplete();}
    public static void accountSessionSaved(){AccountProfiles.sessionSaved();}
    static Activity accountActivity(){return pad==null?null:pad.activity;}
    static void quiesceAccounts() throws IOException {
        gameRoutes.stop("Account switch"); gameRoutesPlanning=false; gameRoutes.setPlanning(false); autoWalkPlanning=false;
        vgp.suspend();
        mapTapEpoch++; invalidateQuickBallOutcome();
        engine.stop(); saveState();
        if(stateWriteFailed)throw new IOException("Location save did not finish");
    }
    static void flushAccountWrites() throws IOException { flushSettings(2500); Cooldown.flush(); }
    public static boolean adventureSyncDismissAllowed() {
        Application app=hostApplication;
        return app!=null && hostLive() && desiredEnabled
            && app.checkSelfPermission("android.permission.ACCESS_FINE_LOCATION")!=0
            && app.checkSelfPermission("android.permission.ACCESS_COARSE_LOCATION")!=0;
    }
    private static volatile boolean mapPoiOpen,autoWalkPlanning;
    private static volatile String mapPoiSnapshot="";
    private static volatile long mapPoiAt;
    public static void setMapPoiOpen(boolean open){mapPoiOpen=open;if(!open&&!autoWalkPlanning){mapPoiSnapshot="";mapPoiAt=0;}}
    public static void setAutoWalkPlanning(boolean open){autoWalkPlanning=open;if(!open&&!mapPoiOpen){mapPoiSnapshot="";mapPoiAt=0;}}
    public static boolean mapPoiAllowed(){return (mapPoiOpen||autoWalkPlanning)&&hostLive();}
    public static void setMapPoiSnapshot(String value){if((mapPoiOpen||autoWalkPlanning)&&value!=null&&value.length()<160000){mapPoiSnapshot=value;mapPoiAt=SystemClock.elapsedRealtime();}}
    public static String mapPoiSnapshot(){return SystemClock.elapsedRealtime()-mapPoiAt<10000?mapPoiSnapshot:"";}
    public static final MotionEngine engine = new MotionEngine();
    private static final GameRoutesController gameRoutes = new GameRoutesController(
        engine, SystemClock::elapsedRealtime, System::currentTimeMillis);
    private static volatile boolean gameRoutesPlanning;
    private static String gameRoutesLastStatus="";
    private static volatile String gameRoutesReadiness="Waiting for Route support";
    public static void setGameRoutesPlanning(boolean open) { gameRoutesPlanning=open; gameRoutes.setPlanning(open); }
    public static boolean gameRoutesReady() { return gameRoutes.ready(); }
    public static boolean gameRoutesRunning() { return gameRoutes.running(); }
    public static String gameRoutesStatus() { return gameRoutes.ready()?gameRoutes.status():gameRoutesReadiness; }
    public static String gameRoutesSnapshot() { return gameRoutes.snapshot(); }
    public static String gameRoutesLastReward() { return gameRoutes.lastReward(); }
    public static String takeGameRoutesRewardNotice() { return gameRoutes.takeRewardNotice(); }
    public static String gameRoutesDetails() { return gameRoutes.details(); }
    public static String gameRoutesCommand() { return gameRoutes.command(); }
    public static boolean gameRoutesAllowed() { return hostLive() && desiredEnabled && (gameRoutesPlanning || gameRoutes.running()); }
    public static void setGameRoutesReady(boolean value,String reason) {
        gameRoutesReadiness=reason==null||reason.isEmpty()?"Waiting for Route support":reason;
        gameRoutes.setReady(value);
    }
    public static void setGameRoutesSnapshot(String value) { gameRoutes.acceptSnapshot(value); }
    public static void gameRoutesAction(String value) { gameRoutes.acceptAction(value); }
    public static void startGameRoutes(int count) {
        if(!hostLive()||!canEnable())throw new IllegalArgumentException("Location adapter is not ready");
        gameRoutes.start(count); saveState();
    }
    public static void stopGameRoutes() { gameRoutes.stop("Stopped"); saveState(); }
    public static void tickGameRoutes() {
        gameRoutes.tick(hostLive()&&canEnable());
        String status=gameRoutes.status();
        if(!status.equals(gameRoutesLastStatus)){gameRoutesLastStatus=status;event("game-routes",status);}
    }
    public static volatile String connection = "PREVIEW - game not connected";
    public static volatile long latitudeReads, longitudeReads;
    private static PrintWriter log;
    private static File logFile;
    private static Pad pad;
    private static boolean registered;
    private static volatile long leaseDeadline;
    private static volatile boolean instrumented;
    private static volatile boolean leaseExpired;
    private static volatile boolean preview = true, backendReady;
    private static volatile boolean desiredEnabled = true;
    private static volatile double[] realLocation;
    private static double realLatitude=Double.NaN,realLongitude=Double.NaN;
    private static volatile boolean foreground;
    private static volatile boolean cameraDiagnosticsConfigured;
    private static volatile boolean cameraDiagnosticRunning;
    private static volatile String cameraDiagnosticStatus = "";
    private static final AtomicBoolean cameraSampleRequest = new AtomicBoolean();
    private static volatile boolean interactionDiagnosticsConfigured, interactionDiagnosticRunning;
    private static volatile String interactionDiagnosticStatus="";
    private static final AtomicBoolean interactionTraceRequest=new AtomicBoolean(), interactionStopRequest=new AtomicBoolean();
    private static final Object quickSummaryLock=new Object();
    private static volatile boolean quickSummaryConfigured, quickSummaryReady, quickSummaryEnabled;
    private static volatile boolean quickSummaryAutoDismiss;
    private static volatile boolean autoTransferEnabled, autoTransferReady;
    private static volatile int transferProtect=63;
    private static volatile String transferStatus="Waiting for transfer support";
    // Custom keep list (canonical, sorted/de-duplicated positive dex IDs) plus the persistent Keep All
    // overlay. Keep All holds every catch regardless of the other rules; the keep list protects the
    // listed species. Both persist; Keep All active state is a normal saved preference (unlike VGP).
    private static volatile String transferKeepSpecies="";
    private static volatile boolean transferKeepAll, transferOverlayEnabled;
    private static volatile String quickSummaryStatus="";
    private static final Object quickBallLock=new Object();
    private static volatile boolean quickBallConfigured, quickBallReady, quickBallEnabled;
    private static volatile String quickBallStatus="";
    private static volatile boolean throwReady, throwCurve, throwAr, throwArReady;
    // Independent of throw rating, curveball and Quick Catch. Defaults off.
    private static volatile boolean throwAlwaysHit, throwAlwaysHitReady;
    private static volatile int throwGrade;
    // Optional IV/level overlays rendered by the ball-animation agent. Catch screen
    // defaults ON (prior behavior); owned Pokemon details defaults OFF. ivOwnedReady
    // is runtime only: whether the agent resolved the owned-details metadata.
    private static volatile boolean ivCatchEnabled=true, ivOwnedEnabled, ivOwnedReady;
    private static volatile int mapTapMode, mapTapEpoch;
    private static final MapTapSequence mapTapSequence=new MapTapSequence();
    private static volatile boolean mapTapReady;
    private static volatile boolean spawnBooster,spawnReady;
    private static volatile long spawnRangeAt;
    private static volatile double spawnOriginal,spawnEffective;
    public static void setSpawnRange(double original,double effective){if(!spawnBooster||!spawnAllowed()||!Double.isFinite(original)||!Double.isFinite(effective)||original<=0||effective<original)return;spawnOriginal=original;spawnEffective=effective;spawnRangeAt=SystemClock.elapsedRealtime();}
    public static String spawnStatus(){
        if(!spawnBooster)return "Off";
        if(!spawnAllowed())return "Waiting for map support";
        if(spawnRangeAt==0||SystemClock.elapsedRealtime()-spawnRangeAt>5000)return "Waiting for a range check; move a few steps on the map";
        return String.format(java.util.Locale.US,"Range check: %.0f to %.0f m (local)",spawnOriginal,spawnEffective);
    }
    public static boolean spawnBooster(){return spawnBooster;}
    public static void setSpawnBooster(boolean enabled){spawnRangeAt=0;spawnBooster=enabled;saveSettings();}
    public static void setSpawnReady(boolean ready){spawnReady=ready;if(!ready)spawnRangeAt=0;}
    public static boolean spawnAllowed(){return spawnReady&&quickSummaryReady()&&hostLive();}
    private static volatile int giftOptions;
    private static volatile boolean giftsReady;
    private static volatile String giftStatus="Waiting for gift support";
    public static int giftOptions(){return giftOptions;}
    public static void setGiftOptions(int value){if(value<0||value>7)return;giftOptions=value;saveSettings();}
    public static void setGiftsReady(boolean ready,String status){giftsReady=ready;giftStatus=status;}
    public static void setGiftStatus(String status){giftStatus=status;}
    public static String giftStatus(){return giftStatus;}
    public static boolean giftsAllowed(){return giftsReady&&quickSummaryReady()&&hostLive();}
    private static volatile boolean vgpEnabled,vgpReady,vgpOverlay;
    /** Persistent safety preference; when armed a live cooldown keeps VGP off. */
    private static volatile boolean vgpStopOnCooldown;
    private static volatile int vgpOptions=3;
    /**
     * Runtime-only VGP state (guard reason, status text, on-screen target), the enable/cooldown
     * guard decision and the result notification live in {@link VgpController}. The persistent
     * overlay/stop-on-cooldown/option preferences and the {@code vgpEnabled}/{@code vgpReady} flags
     * stay here and feed the controller through the accessors below, so the runtime state is owned
     * in one place only. These flags remain at the bridge boundary with the other readiness gates.
     */
    private static final VgpController vgp = new VgpController(
        () -> vgpEnabled, value -> vgpEnabled=value,
        () -> vgpOverlay, value -> vgpOverlay=value,
        () -> vgpStopOnCooldown, value -> vgpStopOnCooldown=value,
        () -> foreground, () -> vgpOptions,
        () -> Cooldown.remaining(), Control::cooldownAction,
        Control::event, Control::saveSettings);
    public static boolean vgpEnabled(){guardVgpCooldown();return vgpOverlay&&vgpEnabled;}
    public static boolean vgpOverlay(){return vgpOverlay;}
    public static void setVgpOverlay(boolean value){vgp.setOverlay(value);}
    public static boolean vgpOverlayVisible(){return vgpOverlay&&hostLive();}
    public static int vgpOptions(){return vgpOptions;}
    public static boolean vgpStopOnCooldown(){return vgpStopOnCooldown;}
    /**
     * VGP active state is runtime only (never persisted). Turning it on while the cooldown
     * guard is armed and a cooldown is live is refused with a visible reason; turning it off
     * clears any guard reason.
     */
    public static void setVgpEnabled(boolean value){vgp.setEnabled(value);}
    /**
     * Arms or disarms the optional cooldown safety. Arming it while VGP is already active and a
     * cooldown is live switches VGP off immediately; disarming clears any lingering guard reason.
     */
    public static void setVgpStopOnCooldown(boolean value){vgp.setStopOnCooldown(value);}
    public static void setVgpOptions(int value){vgpOptions=value&7;saveSettings();}
    public static void setVgpReady(boolean ready,String status){vgpReady=ready;vgp.setStatus(status);}
    public static void setVgpStatus(String status){vgp.setStatus(status);}
    /** A guard reason (cooldown pause/refusal) is shown in place of a generic native status. */
    public static String vgpStatus(){return vgp.status();}
    /**
     * When the guard is armed, a live cooldown trips VGP off before the next request and it stays
     * off (no auto-resume). Driven by the vgpEnabled()/vgpAllowed() heartbeat; the Cooldown read
     * happens with no lock held and logs only on the single off transition.
     */
    private static void guardVgpCooldown(){vgp.guardCooldown();}
    public static void setVgpTarget(boolean visible,double x,double y,boolean spin){vgp.setTarget(visible,x,y,spin);}
    public static boolean vgpMapVisible(){long age=SystemClock.elapsedRealtime()-buddyMapSample;return buddyMapSample>0&&age>=0&&age<1500;}
    public static boolean vgpTargetVisible(){return vgp.targetVisible(vgpAllowed(),vgpMapVisible());}
    public static float vgpTargetX(){return vgp.targetX();}
    public static float vgpTargetY(){return vgp.targetY();}
    public static boolean vgpTargetSpin(){return vgp.targetSpin();}

    public static boolean vgpAllowed(){if(!locationKnown())return false;guardVgpCooldown();return vgpEnabled&&vgpReady&&itemAccessAllowed();}
    public static void vgpResult(String action,boolean success){vgp.result(action,success);}
    private static volatile boolean autoIncubate, incubateLongest, incubatorReady;
    private static volatile String incubatorStatus="Waiting for incubation support";
    public static boolean autoIncubate(){return autoIncubate;}
    public static boolean incubateLongest(){return incubateLongest;}
    public static void setAutoIncubate(boolean v){autoIncubate=v;saveSettings();}
    public static void setIncubateLongest(boolean v){incubateLongest=v;saveSettings();}
    public static void setIncubatorReady(boolean ready,String status){incubatorReady=ready;incubatorStatus=status;}
    public static void setIncubatorStatus(String status){incubatorStatus=status;}
    public static String incubatorStatus(){return incubatorStatus;}
    public static boolean incubatorAvailable(){return incubatorReady&&itemAccessAllowed();}
    public static boolean incubatorAllowed(){long age=SystemClock.elapsedRealtime()-buddyMapSample;return autoIncubate&&incubatorAvailable()&&buddyMapSample>0&&age>=0&&age<1500;}
    private static volatile int buddyBerry;
    private static volatile boolean buddyReady;
    private static volatile long buddyMapSample;
    private static volatile String buddyStatus="Waiting for buddy support";
    public static int buddyBerry(){return buddyBerry;}
    public static void setBuddyBerry(int id){if(id!=0&&id!=701&&id!=703&&id!=705&&id!=706&&id!=707)return;buddyBerry=id;saveSettings();}
    public static void setBuddyReady(boolean ready,String status){buddyReady=ready;buddyStatus=status;}
    public static void setBuddyStatus(String status){buddyStatus=status;}
    public static String buddyStatus(){return buddyStatus;}
    public static boolean buddyAvailable(){return buddyReady&&itemAccessAllowed();}
    public static boolean buddySamplingRequested(){return (buddyBerry!=0||vgpEnabled||autoIncubate)&&hostLive();}
    public static boolean buddyAllowed(){long age=SystemClock.elapsedRealtime()-buddyMapSample;return buddyAvailable()&&buddyMapSample>0&&age>=0&&age<1500;}
    private static volatile boolean encounterBerryActive,encounterBerryBusy;
    private static volatile long encounterBerrySeen;
    public static void setEncounterBerry(boolean active,boolean busy){encounterBerryActive=active;encounterBerryBusy=busy;encounterBerrySeen=SystemClock.elapsedRealtime();}
    public static boolean catchScreenVisible(){long age=SystemClock.elapsedRealtime()-encounterBerrySeen;return encounterBerryActive&&age>=0&&age<2000&&foreground&&quickSummaryReady();}
    public static boolean encounterBerryVisible(){return encounterBerryEnabled&&catchScreenVisible();}
    public static boolean encounterBerryBusy(){return encounterBerryBusy;}
    private static volatile boolean cooldownOverlay;
    private static volatile String cooldownEncounter="";
    public static boolean cooldownOverlay(){return cooldownOverlay;}
    public static void setCooldownOverlay(boolean value){cooldownOverlay=value;saveSettings();}
    public static void setCooldownEncounter(String value){cooldownEncounter=value==null?"":value.toLowerCase(java.util.Locale.US);}
    public static void cooldownAction(String action){
        if(!foreground||!backendReady||!connected())return;
        if(action.equals("Ball throw")||action.equals("Ball drop")||action.equals("Berry feeding")){
            boolean wild=cooldownEncounter.contains("wild"),raid=cooldownEncounter.contains("raid");
            if(!wild&&!(raid&&!action.equals("Ball throw")))return;
        }
        Cooldown.record(action);
    }
    private static volatile String berryPriority="";
    private static volatile boolean encounterBerryEnabled;
    private static volatile boolean berryReady;
    /** Effective choice the native feeder reads: 0 (no feeding) whenever the overlay master is off. */
    public static int autoBerry(){return encounterBerryEnabled?autoBerrySelected():0;}
    /** Raw user selection shown in the overlay; retained while the master is off so it can be re-enabled. */
    public static int autoBerrySelected(){int[] ids=BerryPriority.parse(berryPriority);for(int id:ids)if(berryCount(id)>0)return id;return ids.length==0?0:ids[0];}
    public static String berryPriority(){return berryPriority;}
    public static String effectiveBerryPriority(){return encounterBerryEnabled?berryPriority:"";}
    public static boolean encounterBerryEnabled(){return encounterBerryEnabled;}
    public static void setEncounterBerryEnabled(boolean enabled){encounterBerryEnabled=enabled;saveSettings();event("encounter-berry-overlay",enabled?"ON":"OFF; feeding stopped, choice retained");}
    public static void setAutoBerry(int id){if(id!=0&&!BerryPriority.validId(id))return;berryPriority=id==0?"":String.valueOf(id);saveSettings();}
    public static void selectBerryPriority(int id){if(id!=0&&!BerryPriority.validId(id))return;berryPriority=BerryPriority.select(berryPriority,id);saveSettings();}
    public static void setBerryReady(boolean ready){berryReady=ready;}
    public static boolean berriesAllowed(){return berryReady&&itemAccessAllowed();}
    public static int berryCount(int id){for(String row:itemCatalog.split("\n")){String[] b=row.split("\\|",-1);try{if(b.length==3&&Integer.parseInt(b[0])==id)return Integer.parseInt(b[2]);}catch(NumberFormatException ignored){}}return 0;}
    private static volatile int battleOptions;
    private static volatile boolean battlesReady;
    private static volatile String battleStatus="Waiting for battle support";
    public static int battleOptions(){return battleOptions;}
    public static String battleStatus(){return battleStatus;}
    public static void setBattleOptions(int value){if(value<0||value>1023)return;battleOptions=value;saveSettings();event("battle-options",String.valueOf(value));}
    public static void setBattlesReady(boolean ready,String status){battlesReady=ready;battleStatus=status;}
    public static void setBattleStatus(String status){battleStatus=status;}
    public static boolean battlesAllowed(){return battlesReady&&quickSummaryReady()&&hostLive();}
    private static volatile int rocketOptions;
    private static volatile boolean rocketReady;
    private static volatile String rocketStatus="Waiting for battle support";
    public static int rocketOptions(){return rocketOptions;}
    public static String rocketStatus(){return rocketStatus;}
    public static void setRocketOptions(int value){if(value<0||value>7)return;rocketOptions=value;saveSettings();event("rocket-options",String.valueOf(value));}
    public static void setRocketReady(boolean ready,String status){rocketReady=ready;rocketStatus=status;}
    public static void setRocketStatus(String status){rocketStatus=status;}
    public static boolean rocketAllowed(){return rocketReady&&quickSummaryReady()&&hostLive();}
    private static volatile boolean itemTransferEnabled,itemTransferReady;
    private static volatile String itemRules="",itemLimits="",itemCatalog="",itemStatus="Waiting for item bag";
    public static boolean itemTransferEnabled(){return itemTransferEnabled;}
    public static String itemRules(){return itemRules;}
    public static String itemLimits(){return itemLimits;}
    public static String itemCatalog(){return itemCatalog;}
    public static String itemStatus(){return itemStatus;}
    public static boolean itemAccessAllowed(){return itemTransferReady&&quickSummaryReady()&&hostLive();}
    private static volatile long itemCatalogUntil,berryCatalogUntil;
    public static void requestItemCatalog(){itemCatalogUntil=SystemClock.elapsedRealtime()+5000;}
    public static void requestBerryCatalog(){berryCatalogUntil=SystemClock.elapsedRealtime()+5000;}
    /** 0: idle, 1: five berry counts only, 2: explicit inventory settings catalog. */
    public static int itemCatalogMode(){
        if(!hostLive())return 0;
        long now=SystemClock.elapsedRealtime();
        if(now<itemCatalogUntil)return 2;
        return now<berryCatalogUntil||encounterBerryVisible()?1:0;
    }
    public static void setItemReady(boolean ready,String status){itemTransferReady=ready;itemStatus=status;}
    public static void setItemStatus(String status){itemStatus=status==null?"":status;}
    public static void setItemCatalog(String value){
        if(value==null||value.length()>100000)return;
        TreeMap<Integer,String> rows=new TreeMap<>();
        for(String row:itemCatalog.split("\n")){String[] bits=row.split("\\|",-1);try{if(bits.length==3)rows.put(Integer.parseInt(bits[0]),bits[0]+"|"+bits[1]+"|0");}catch(NumberFormatException ignored){}}
        for(String row:value.split("\n")){String[] bits=row.split("\\|",-1);try{if(bits.length==3&&Integer.parseInt(bits[0])>0&&Integer.parseInt(bits[2])>=0)rows.put(Integer.parseInt(bits[0]),row);}catch(NumberFormatException ignored){}}
        itemCatalog=String.join("\n",rows.values());itemStatus="Ready: "+rows.size()+" recyclable item types";
    }
    private static boolean validItemRules(String value){
        if(value==null||value.length()>30000)return false;HashSet<Integer> seen=new HashSet<>();
        for(String row:value.split(";")){if(row.isEmpty())continue;if(!row.matches("[0-9]{1,7}:[0-9]{1,6}"))return false;String[] b=row.split(":");int id=Integer.parseInt(b[0]),cap=Integer.parseInt(b[1]);if(id<=0||cap>100000||!seen.add(id))return false;}return true;
    }
    public static void saveItemRules(boolean enabled,String rules,String limits){
        if(!validItemRules(rules)||!validItemRules(limits))throw new IllegalArgumentException("Invalid item limit");
        itemRules=rules;itemLimits=limits;itemTransferEnabled=enabled;saveSettings();event("item-rules",enabled?"Enabled":"Disabled");
    }
    private static volatile boolean quickSpinEnabled, quickSpinReady;
    private static volatile String quickSpinStatus="Waiting for game";
    public static boolean quickSpinEnabled() { return quickSpinEnabled; }
    public static String quickSpinStatus() { return quickSpinStatus; }
    public static void setQuickSpinEnabled(boolean value) { quickSpinEnabled=value; saveSettings(); event("quick-spin-setting",value?"ON":"OFF"); }
    public static void setQuickSpinReady(boolean value,String status) { quickSpinReady=value; quickSpinStatus=status; }
    public static boolean quickSpinAllowed() { return quickSpinEnabled && quickSpinReady && quickSummaryReady() && hostLive(); }
    private static volatile boolean cameraSteeringConfigured;
    private static final CameraHeadingState cameraHeading = new CameraHeadingState();
    private static final Handler main = new Handler(Looper.getMainLooper());
    private static final CatchOutcomeController catchOutcome = new CatchOutcomeController(
        main, () -> pad==null ? null : pad.activity,
        Control::quickBallAllowed, Control::autoTransferAllowed,
        detail -> event("quick-ball-toast",detail));
    private static final Object stateLock = new Object();
    private static final Object logLock = new Object();
    private static AtomicFile stateFile, settingsFile;
    private static String lastSaved;
    private static long lastCheckpoint;
    private static boolean stateWriteFailed;
    private static final AtomicBoolean checkpointPending = new AtomicBoolean();
    private static final ExecutorService stateWriter = Executors.newSingleThreadExecutor(task -> {
        Thread thread = new Thread(task, "pgrazor-state-writer"); thread.setDaemon(true); return thread;
    });
    // One daemon worker per burst: SettingsWriter only schedules a drain when idle, so at most one
    // drain writes at a time, and it exits once the queue empties. Nothing permanent blocks JVM exit.
    private static final Executor settingsIo = task -> {
        Thread thread = new Thread(task, "pgrazor-settings-writer"); thread.setDaemon(true); thread.start();
    };
    private static SettingsWriter settingsWriter;
    // Explicit export/import block until durable; a shorter bound keeps a background flush from stalling.
    private static final long SETTINGS_FLUSH_MS = 5000, SETTINGS_BACKGROUND_FLUSH_MS = 1000;
    public static void embedded(String status) {
        setGameRoutesReady(false,"Waiting for game");
        engine.pause(); preview = false; backendReady = false; connection = status;
        setQuickSummaryReady(false,"Waiting for catch summary support");
        setQuickBallReady(false,"Waiting for ball animation support");
        invalidateCameraHeading("Adapter initializing");
        cameraSampleRequest.set(false);
        cameraDiagnosticRunning = false;
        saveState();
    }
    public static boolean preview() { return preview; }
    /** Saved user choices survive readiness and lifecycle changes. */
    public static void configureQuickSummary(boolean configured) {
        synchronized(quickSummaryLock) {
            quickSummaryConfigured=configured; quickSummaryReady=false; autoTransferReady=false;
            quickSummaryStatus=configured ? "Checking catch summary support" : "";
        }
    }
    public static boolean quickSummaryConfigured() { return quickSummaryConfigured; }
    public static void configureQuickSummaryAutoDismiss(boolean enabled) { quickSummaryAutoDismiss=enabled; }
    public static boolean quickSummaryAutoDismiss() { return quickSummaryAutoDismiss; }
    public static boolean quickSummaryReady() { return quickSummaryConfigured && quickSummaryReady; }
    public static boolean quickSummaryEnabled() { return quickSummaryEnabled; }
    public static String quickSummaryStatus() { return quickSummaryStatus; }
    public static void setQuickSummaryReady(boolean ready,String status) {
        boolean disabled;
        synchronized(quickSummaryLock) {
            quickSummaryReady=quickSummaryConfigured && ready;
            if(!quickSummaryReady)autoTransferReady=false;
            disabled=quickSummaryEnabled && !quickSummaryReady;

            String text=status==null || status.length()==0 ? (quickSummaryReady ? "Ready" : "Unavailable") : status;
            quickSummaryStatus=quickSummaryConfigured ? text.substring(0,Math.min(text.length(),320)) : "";
        }
        if(disabled)event("quick-summary-setting","Suspended; saved choice retained");
    }
    public static void setQuickSummaryEnabled(boolean enabled) {
        boolean changed,actual;
        synchronized(quickSummaryLock) {
            actual=enabled;
            changed=quickSummaryEnabled!=actual; quickSummaryEnabled=actual;
        }
        // One Quick Catch choice controls dismissal and the 20x animation.
        setQuickBallEnabled(actual);
        if(changed)event("quick-summary-setting",actual ? (quickSummaryAutoDismiss ? "ON; summary and details auto-dismiss; new-species screens remain" : "ON; shorter post-catch wait; summary stays open") : "OFF");
    }
    public static boolean quickSummaryAllowed() {
        return quickSummaryEnabled && quickSummaryReady && hostLive();
    }
    public static boolean autoTransferEnabled(){return autoTransferEnabled;}
    public static boolean autoTransferReady(){return quickSummaryReady()&&autoTransferReady;}
    public static int transferProtect(){return transferProtect;}
    public static String transferStatus(){return transferStatus;}
    public static void setAutoTransferEnabled(boolean enabled){invalidateQuickBallOutcome();autoTransferEnabled=enabled;saveSettings();event("auto-transfer-setting",enabled?"ON; new catches below90percent only":"OFF");}
    public static void setTransferProtect(int mask){transferProtect=mask>=0&&mask<=63?mask:63;saveSettings();event("auto-transfer-protection",String.valueOf(transferProtect));}
    public static void setAutoTransferReady(boolean ready,String status){autoTransferReady=ready&&quickSummaryReady();transferStatus=status==null?"":status;}
    public static void transferResult(String status){transferStatus=status==null?"":status;}
    public static boolean autoTransferAllowed(){return autoTransferEnabled&&!transferKeepAll&&autoTransferReady()&&hostLive();}
    /** Canonical keep list: sorted, de-duplicated positive dex IDs (1..2000), comma-separated; empty when none. */
    public static String transferKeepSpecies(){return transferKeepSpecies;}
    public static void setTransferKeepSpecies(String value){
        boolean[] damaged={false};
        String canonical=normalizeKeepSpecies(value,damaged);
        if(damaged[0]){event("transfer-keep-list","Invalid selection ignored; existing keep list preserved");return;}
        if(canonical.equals(transferKeepSpecies))return;
        transferKeepSpecies=canonical;saveSettings();
        event("transfer-keep-species",(canonical.isEmpty()?0:canonical.split(",").length)+" species kept");
    }
    /** Keep All holds every catch regardless of the other rules; persisted and toggled by the overlay. */
    public static boolean transferKeepAll(){return transferKeepAll;}
    public static void setTransferKeepAll(boolean value){
        if(value==transferKeepAll)return;
        // Keep All never enables the auto-transfer master; turning it off only resumes the saved rules.
        transferKeepAll=value;invalidateQuickBallOutcome();saveSettings();
        event("transfer-keep-all",value?"ON; auto transfer held":"OFF; transfer rules resume; master unchanged");
    }
    public static boolean transferOverlayEnabled(){return transferOverlayEnabled;}
    public static void setTransferOverlayEnabled(boolean value){
        if(value==transferOverlayEnabled)return;
        transferOverlayEnabled=value;saveSettings();event("transfer-overlay",value?"ON":"OFF");
    }
    public static boolean transferOverlayVisible(){return transferOverlayEnabled&&autoTransferEnabled&&catchScreenVisible()&&backendReady&&connected();}
    /**
     * Canonical keep-species string from arbitrary input: positive dex IDs 1..2000, de-duplicated and
     * sorted, bounded to 2000 entries and 10000 characters. Invalid or out-of-range tokens are dropped;
     * when {@code damaged} is supplied, {@code damaged[0]} is set true if any token was rejected or a
     * bound was exceeded, so a loader can fail safe rather than trust a partially valid list.
     */
    private static String normalizeKeepSpecies(String raw,boolean[] damaged){
        boolean bad=false;
        if(raw==null){bad=true;raw="";}
        if(raw.length()>10000){bad=true;raw=raw.substring(0,10000);}
        java.util.TreeSet<Integer> ids=new java.util.TreeSet<>();
        for(String token:raw.isEmpty()?new String[0]:raw.split(",",-1)){
            if(!token.matches("[1-9][0-9]{0,3}")){bad=true;continue;}
            int id;
            try{id=Integer.parseInt(token);}catch(NumberFormatException invalid){bad=true;continue;}
            if(id<1||id>2000){bad=true;continue;}
            if(ids.size()>=2000){bad=true;break;}
            if(!ids.add(id))bad=true;
        }
        StringBuilder out=new StringBuilder();
        for(int id:ids){if(out.length()>0)out.append(',');out.append(id);}
        if(damaged!=null)damaged[0]=bad;
        return out.toString();
    }
    // IV/level overlay preferences. The stored choice is surfaced to the UI and the
    // agent; the agent itself fails closed when owned-details metadata is absent.
    public static boolean ivCatchEnabled(){return ivCatchEnabled;}
    public static boolean ivOwnedEnabled(){return ivOwnedEnabled;}
    public static boolean ivOwnedReady(){return ivOwnedReady;}
    public static boolean ivDisplayAllowed(){return quickBallReady&&hostLive();}
    public static void setIvCatchEnabled(boolean enabled){ivCatchEnabled=enabled;saveSettings();event("iv-catch-overlay",enabled?"ON":"OFF");}
    public static void setIvOwnedEnabled(boolean enabled){ivOwnedEnabled=enabled;saveSettings();event("iv-owned-overlay",enabled?"ON":"OFF");}
    public static void setIvOwnedReady(boolean ready){ivOwnedReady=ready;}
    public static String ivDisplayStatus(){return (quickBallConfigured&&ivOwnedEnabled&&!ivOwnedReady)?"Details overlay unsupported on this app version":"";}
    public static void configureQuickBall(boolean configured) {
        synchronized(quickBallLock) {
            quickBallConfigured=configured; quickBallReady=false; ivOwnedReady=false; setThrowReady(false);setMapTapReady(false);
            quickBallStatus=configured ? "Checking successful ball animation support" : "";
        }
        invalidateQuickBallOutcome();
    }
    public static boolean quickBallConfigured() { return quickBallConfigured; }
    public static boolean quickBallReady() { return quickBallConfigured && quickBallReady; }
    public static boolean quickBallEnabled() { return quickBallEnabled; }
    public static String quickBallStatus() { return quickBallStatus; }
    public static void setQuickBallReady(boolean ready,String status) {
        boolean disabled;
        synchronized(quickBallLock) {
            quickBallReady=quickBallConfigured && ready;
            disabled=quickBallEnabled && !quickBallReady;
            if(!quickBallReady){setThrowReady(false);setMapTapReady(false);}
            String text=status==null || status.length()==0 ? (quickBallReady ? "Ready" : "Unavailable") : status;
            quickBallStatus=quickBallConfigured ? text.substring(0,Math.min(text.length(),320)) : "";
        }
        if(!ready)invalidateQuickBallOutcome();
        if(disabled)event("quick-ball-setting","Suspended; saved choice retained");
    }
    public static void setQuickBallEnabled(boolean enabled) {
        boolean changed,actual;
        synchronized(quickBallLock) {
            actual=enabled;
            changed=quickBallEnabled!=actual; quickBallEnabled=actual;
        }
        if(!actual)invalidateQuickBallOutcome();
        saveSettings();
        if(changed)event("quick-ball-setting",actual ? "ON; experimental 20x catch and break-free ball animation; summary screens remain" : "OFF");
    }
    public static boolean quickBallAllowed() {
        return quickBallEnabled && quickBallReady && hostLive();
    }
    public static boolean mapTapReady() { return quickBallReady() && mapTapReady; }
    public static void setMapTapReady(boolean ready) { mapTapReady=ready; if(!ready)mapTapEpoch++; }
    public static int mapTapMode() { return mapTapMode; }
    public static int mapTapEpoch() { return mapTapEpoch; }
    public static void setMapTapMode(int mode) { mapTapEpoch++; mapTapMode=mode>=0 && mode<=3 ? mode : 0; saveSettings(); event("map-tap-mode",""+mapTapMode); }
    public static boolean mapTapAllowed() { return desiredEnabled && mapTapMode!=0 && mapTapReady() && hostLive(); }
    public static void applyMapTap(double latitude,double longitude,int mode,int epoch,double sampledAt) {
        double age=SystemClock.elapsedRealtime()-sampledAt;
        if(!Double.isFinite(age)||age<0||age>750)return;
        if(!Double.isFinite(latitude)||!Double.isFinite(longitude)||Math.abs(latitude)>90||Math.abs(longitude)>180)return;
        final long received=SystemClock.elapsedRealtime();
        main.post(() -> {
            if(!mapTapAllowed()||mode!=mapTapMode||epoch!=mapTapEpoch||SystemClock.elapsedRealtime()-received>750)return;
            if(engine.activeRouteToken()!=0){
                mapTapEpoch++; // discard queued taps and any pre-dialog double-tap pairing
                if(pad!=null)pad.confirmStopRoute();
                return; // the accidental tap never becomes a walk or teleport
            }
            boolean wantsTeleport=mode==2 || (mode==3 && mapTapSequence.doubleTap(latitude,longitude,sampledAt,epoch,android.view.ViewConfiguration.getDoubleTapTimeout()));
            if(wantsTeleport){if(pad!=null)pad.confirmMapTeleport(latitude,longitude,mode,epoch);return;}
            if(mode==1||mode==3)walkTo(latitude,longitude);else return;
            event("map-tap-applied",String.format(Locale.US,"mode=%d lat=%.7f lon=%.7f",mode,latitude,longitude));
        });
    }
    public static boolean throwReady() { return quickBallReady() && throwReady; }
    public static void setThrowReady(boolean ready) { throwReady=ready; }
    public static int throwGrade() { return throwGrade; }
    public static boolean throwCurve() { return throwCurve; }
    public static boolean throwAr() { return throwAr; }
    public static boolean throwArReady() { return throwArReady && throwReady(); }
    public static void setThrowArReady(boolean value) { throwArReady=value; }
    public static void setThrowAr(boolean value) { throwAr=value; saveSettings(); }

    public static void setThrowGrade(int grade) { throwGrade=grade>=0 && grade<=3 ? grade : 0; saveSettings(); event("throw-grade",""+throwGrade); }
    public static void setThrowCurve(boolean enabled) { throwCurve=enabled; saveSettings(); event("throw-curve",""+throwCurve); }
    public static boolean throwAlwaysHitReady() { return throwReady() && throwAlwaysHitReady; }
    public static void setThrowAlwaysHitReady(boolean ready) { throwAlwaysHitReady=ready; }
    public static boolean throwAlwaysHit() { return throwAlwaysHit; }
    public static void setThrowAlwaysHit(boolean enabled) { throwAlwaysHit=enabled; saveSettings(); event("throw-always-hit",""+throwAlwaysHit); }
    public static boolean throwAllowed() { return throwReady() && hostLive(); }
    /** Visible text for a natively confirmed outcome; anything else is rejected. */
    static String quickBallOutcomeText(String outcome) { return CatchOutcomeController.text(outcome); }
    /**
     * Queues one short post-catch toast; the queue, its cancellation token and the main-thread
     * display gate are owned by {@link CatchOutcomeController}. A newer outcome or any OFF,
     * readiness, background or connection change discards queued work via {@link #invalidateQuickBallOutcome()}.
     */
    public static boolean quickBallOutcome(String outcome) { return catchOutcome.request(outcome); }
    private static void invalidateQuickBallOutcome() { catchOutcome.invalidate(); }
    public static void configureInteractionDiagnostics(boolean configured) {
        interactionDiagnosticsConfigured=configured; interactionDiagnosticRunning=false;
        interactionTraceRequest.set(false); interactionStopRequest.set(false);
        interactionDiagnosticStatus=configured ? "Catch/spin diagnostics: checking metadata shortly" : "";
    }
    public static boolean interactionDiagnosticsConfigured() { return interactionDiagnosticsConfigured; }
    public static boolean interactionDiagnosticsReady() { return interactionDiagnosticsConfigured && hostLive(); }
    public static boolean interactionDiagnosticRunning() { return interactionDiagnosticRunning; }
    public static boolean interactionTracePending() { return interactionTraceRequest.get(); }
    public static String interactionDiagnosticStatus() { return interactionDiagnosticStatus; }
    public static void interactionDiagnostic(String text) {
        if (!interactionDiagnosticsConfigured) return;
        interactionDiagnosticStatus=text==null ? "" : text.substring(0,Math.min(text.length(),320));
    }
    public static void setInteractionDiagnosticRunning(boolean running) { interactionDiagnosticRunning=running && interactionDiagnosticsConfigured; }
    public static boolean requestInteractionTrace() {
        if (!interactionDiagnosticsReady() || interactionDiagnosticRunning || interactionTraceRequest.get()) return false;
        engine.stop(); saveState(); interactionStopRequest.set(false); interactionTraceRequest.set(true);
        interactionDiagnosticStatus="Capture requested; waiting for observer";
        event("interaction-trace-requested","Stationary capture for60s; fasterSummary="+quickSummaryEnabled());
        return true;
    }
    public static boolean takeInteractionTraceRequest() { return interactionTraceRequest.getAndSet(false) && interactionDiagnosticsReady() && !interactionDiagnosticRunning; }
    public static void requestInteractionStop() { interactionTraceRequest.set(false); interactionStopRequest.set(true); }
    public static boolean takeInteractionStopRequest() { return interactionStopRequest.getAndSet(false) || (interactionDiagnosticRunning && !interactionDiagnosticsReady()); }
    public static boolean desiredEnabled() { return desiredEnabled; }
    public static synchronized void setLocationSpoofing(boolean enabled) {
        gameRoutes.stop("Location mode changed");
        desiredEnabled=enabled; if(!enabled){realLocation=null;realLatitude=Double.NaN;realLongitude=Double.NaN;} mapTapEpoch++; autoWalkPlanning=false;
        engine.pause(); invalidateCameraHeading("Location mode changed");
        restoreRuntime("location-setting"); saveSettings(); saveState();
        event("location-spoofing",enabled?"ON":"OFF; using device location");
    }
    public static synchronized void observeRealCoordinate(boolean latitude,double value) {
        if(desiredEnabled||!Double.isFinite(value)||Math.abs(value)>(latitude?90:180))return;
        if(latitude)realLatitude=value;else realLongitude=value;
        if(Double.isFinite(realLatitude)&&Double.isFinite(realLongitude))
            realLocation=new double[]{realLatitude,realLongitude};
    }
    public static boolean locationKnown(){return desiredEnabled||realLocation!=null;}
    public static double currentLatitude(){double[] fix=realLocation;return desiredEnabled?engine.latitude:fix==null?Double.NaN:fix[0];}
    public static double currentLongitude(){double[] fix=realLocation;return desiredEnabled?engine.longitude:fix==null?Double.NaN:fix[1];}
    public static void configureCameraSteering(boolean configured) {
        synchronized (cameraHeading) {
            cameraSteeringConfigured=configured;
            cameraHeading.invalidate(); stopManualMotion();
        }
    }
    public static boolean cameraSteeringConfigured() { return cameraSteeringConfigured; }
    /** Camera-relative is the only joystick mode; unavailable headings stop input. */
    public static boolean cameraRelative() { return true; }
    public static boolean cameraSteeringRequested() {
        return cameraSteeringConfigured && hostLive();
    }
    public static void updateCameraHeading(double yawDegrees, int cameraId, boolean mapVisible, long ageMillis) {
        synchronized (cameraHeading) {
            long now=SystemClock.elapsedRealtime();
            long sampledAt=ageMillis>=0 && ageMillis<=now ? now-ageMillis : -1;
            updateCameraHeadingLocked(yawDegrees,cameraId,mapVisible,sampledAt,now);
        }
    }
    /** Native CLOCK_BOOTTIME timestamp, rounded down; includes bridge and heading-lock delay. */
    public static void updateCameraHeadingAt(double yawDegrees, int cameraId, boolean mapVisible, long sampledAtMillis) {
        synchronized (cameraHeading) {
            updateCameraHeadingLocked(yawDegrees,cameraId,mapVisible,sampledAtMillis,SystemClock.elapsedRealtime());
        }
    }
    private static void updateCameraHeadingLocked(double yawDegrees,int cameraId,boolean mapVisible,long sampledAtMillis,long now) {
        buddyMapSample=mapVisible&&sampledAtMillis>0&&sampledAtMillis<=now&&now-sampledAtMillis<1500?sampledAtMillis:0;
        // Expiry is checked before replacement, even if no UI tick observed the gap.
        CameraHeadingState.Sample previous=cameraHeading.sample(now);
        boolean accepted=cameraSteeringRequested() && cameraHeading.updateAt(yawDegrees,cameraId,mapVisible,sampledAtMillis,now);
        if (!accepted) cameraHeading.invalidate();
        CameraHeadingState.Sample current=cameraHeading.sample(now);
        if (cameraRelative() && (!previous.valid || !current.valid || previous.generation!=current.generation)) stopManualMotion();
    }
    public static void invalidateCameraHeading(String reason) {
        buddyMapSample=0;
        synchronized (cameraHeading) {
            cameraHeading.invalidate();
            if (cameraRelative()) stopManualMotion();
        }
    }
    private static void stopManualMotion() { engine.stopManualMotion(); }
    static long beginCameraGesture() {
        synchronized (cameraHeading) {
            return cameraSteeringRequested() ? cameraHeading.beginGesture(SystemClock.elapsedRealtime()) : -1;
        }
    }
    /** Heading validation and input assignment share a lock with asynchronous invalidation. */
    static boolean applyCameraInput(long gesture, double right, double up, long now) {
        synchronized (cameraHeading) {
            CameraSteering.Direction direction=cameraHeading.direction(gesture,right,up,now);
            if (!cameraSteeringRequested() || !direction.valid) { stopManualMotion(); return false; }
            engine.input(direction.east,direction.north); return true;
        }
    }
    static String cameraSteeringStatus() {
        synchronized (cameraHeading) {
            CameraHeadingState.Sample sample=cameraHeading.sample(SystemClock.elapsedRealtime());
            if (!cameraSteeringRequested() || !sample.valid) {
                if (cameraRelative()) stopManualMotion();
                return "Waiting for camera; release and drag again";
            }
            return String.format(Locale.US,"Camera heading %.0f\u00b0; up follows the view",sample.heading);
        }
    }
    public static void configureCameraDiagnostics(boolean configured) {
        cameraDiagnosticsConfigured = configured;
        cameraSampleRequest.set(false);
        cameraDiagnosticRunning = false;
        cameraDiagnosticStatus = configured ? "Camera diagnostic ready" : "";
    }
    public static boolean cameraDiagnosticsConfigured() { return cameraDiagnosticsConfigured; }
    public static boolean cameraDiagnosticsReady() { return cameraDiagnosticsConfigured && hostLive(); }
    public static boolean cameraSamplePending() { return cameraSampleRequest.get() || cameraDiagnosticRunning; }
    public static boolean cameraDiagnosticRunning() { return cameraDiagnosticRunning; }
    public static void setCameraDiagnosticRunning(boolean running) { cameraDiagnosticRunning = running && cameraDiagnosticsConfigured; }
    public static String cameraDiagnosticStatus() { return cameraDiagnosticStatus; }
    public static boolean requestCameraSample() {
        if (!cameraDiagnosticsReady() || cameraSamplePending()) return false;
        if (engine.walking) event("walk-cancelled", "Camera diagnostic requested");
        engine.stop(); saveState();
        cameraDiagnosticStatus = "Camera sample requested (10s)";
        cameraSampleRequest.set(true);
        event("camera-sample-requested", "Stationary sample; desiredEnabled=" + desiredEnabled + "; location override preference unchanged");
        return true;
    }
    public static boolean takeCameraSampleRequest() {
        boolean requested = cameraSampleRequest.getAndSet(false);
        return requested && cameraDiagnosticsReady() && !cameraDiagnosticRunning;
    }
    public static void cameraDiagnostic(String text) {
        if (!cameraDiagnosticsConfigured) return;
        if (text == null || text.length() == 0) text = "Camera diagnostic ready";
        cameraDiagnosticStatus = text.length() > 320 ? text.substring(0, 320) : text;
    }
    public static boolean canEnable() { return desiredEnabled && connected() && (preview || backendReady); }
    public static void connect() {
        engine.pause();
        invalidateCameraHeading("Adapter reconnecting");
        preview = false; backendReady = true;
        instrumented = true;
        connection = "Java location adapter - game acceptance unverified";
        renewLease();
        main.post(() -> restoreRuntime("backend-ready"));
    }
    public static void renewLease() {
        leaseDeadline = android.os.SystemClock.elapsedRealtime() + 8000;
        if(leaseExpired) {
            leaseExpired=false; connection="Java location adapter - game acceptance unverified";
            main.post(() -> { restoreRuntime("heartbeat-restored"); event("heartbeat-restored","Saved preference restored stationary; desiredEnabled="+desiredEnabled); });
        }
    }
    public static void checkLease() {
        if(!connected() && !leaseExpired) { leaseExpired=true; cameraSampleRequest.set(false); invalidateQuickBallOutcome(); invalidateCameraHeading("Connection lost"); engine.pause(); saveState(); connection="Adapter heartbeat late - suspended"; event("heartbeat-expired","Runtime suspended; saved enabled preference unchanged"); }
    }
    public static boolean connected() { return !instrumented || android.os.SystemClock.elapsedRealtime() <= leaseDeadline; }
    /** Shared host gate: foreground, with a ready backend whose connection lease is still live. */
    private static boolean hostLive() { return foreground && backendReady && connected() && AccountProfiles.allowed(); }
    public static synchronized void begin(Application app) {
        if (registered) return;
        registered = true;
        hostApplication=app;
        vgp.setApplication(app);
        try {
            File dir = new File(app.getFilesDir(), "pgrazor-logs");
            if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("Cannot create log folder");
            logFile = new File(dir, new SimpleDateFormat("yyyyMMdd-HHmmss-SSS", Locale.US).format(new Date()) + ".txt");
            log = new PrintWriter(new FileOutputStream(logFile), true);
        } catch (IOException error) { Log.e("PGRazor", "File logging unavailable", error); }
        // Recover recent run tails after a crash without changing or deleting private originals.
        new Thread(() -> recoverRunLogs(app),"pgrazor-log-recovery").start();
        event("session-start", connection);
        AccountProfiles.init(app);
        loadState(app);
        loadSettings(app);
        Cooldown.init(app);
        app.registerActivityLifecycleCallbacks(new Application.ActivityLifecycleCallbacks() {
            public void onActivityResumed(Activity activity) { GoogleBrowserAuth.onHostResumed(activity); attach(activity); }
            public void onActivityPaused(Activity activity) {
                GoogleBrowserAuth.onHostPaused(activity);
                if (pad != null && pad.activity == activity) { pad.close(); pad = null; }
            }
            public void onActivityDestroyed(Activity activity) {
                if (pad != null && pad.activity == activity) { pad.close(); pad = null; }
            }
            public void onActivityCreated(Activity a, Bundle b) {}
            public void onActivityStarted(Activity a) {}
            public void onActivityStopped(Activity a) {}
            public void onActivitySaveInstanceState(Activity a, Bundle b) {}
        });
    }
    public static void attach(Activity activity) {
        if (activity instanceof LogActivity || activity instanceof GoogleAuthActivity) return;
        if (activity.isFinishing() || activity.isDestroyed()) return;
        if (pad != null && pad.activity == activity) return;
        if (pad != null) pad.close();
        foreground = true;
        restoreRuntime("foreground");
        try { pad = new Pad(activity); event("panel-ready", activity.getClass().getName()); }
        catch (RuntimeException error) { suspendForBackground(); event("panel-error", error.toString()); }
    }
    public static void disconnect() {
        engine.pause(); backendReady = false;
        setQuickSummaryReady(false,"Catch summary support disconnected");
        setQuickBallReady(false,"Ball animation support disconnected");
        invalidateCameraHeading("Adapter disconnected");
        cameraSampleRequest.set(false);
        cameraDiagnosticRunning = false;
        if (cameraDiagnosticsConfigured) cameraDiagnosticStatus = "Camera diagnostic unavailable: adapter disconnected";
        saveState();
        connection = "DISCONNECTED - location override off";
        event("disconnected", "Runtime suspended; saved enabled preference unchanged");
    }

    public static void teleport(double latitude, double longitude) {
        gameRoutes.stop("Destination changed");
        engine.position(latitude, longitude); engine.stop();
        engine.enabled = foreground && canEnable();
        saveState();
    }
    public static void walkTo(double latitude, double longitude) {
        gameRoutes.stop("Manual walking started");
        engine.walkTo(latitude, longitude);
        if (!foreground || !canEnable()) engine.pause();
        saveState();
    }
    public static void walkRoute(double[][] waypoints) { walkRoute(waypoints, false); }
    /** As {@link #walkRoute(double[][])}, but a looping route repeats from its first stop; same lifecycle/save/readiness. */
    public static void walkRoute(double[][] waypoints, boolean loop) {
        gameRoutes.stop("Another route started");
        engine.walkRoute(waypoints, loop);
        if (!foreground || !canEnable()) engine.pause();
        saveState();
    }
    public static void startAutoWalk(double[][] waypoints,boolean loop) {
        if(!foreground||!canEnable())throw new IllegalArgumentException("Location adapter is not ready");
        gameRoutes.stop("Autowalk started");
        engine.walkRoute(waypoints,loop);saveState();
        event("autowalk-started","stops="+waypoints.length+" loop="+loop+" speed="+engine.speedKmh);
    }
    public static double cycleWalkSpeed() {
        double[] presets={0,9.3,16,30};double next=0;
        synchronized(engine){for(int i=0;i<presets.length;i++)if(Math.abs(engine.speedKmh-presets[i])<0.001){next=presets[(i+1)%presets.length];break;}engine.speed(next);}
        saveState();return next;
    }
    public static void suspendForBackground() {
        gameRoutes.stop("App left foreground; check the active in-game Route before restarting");
        gameRoutesPlanning=false;gameRoutes.setPlanning(false);
        requestInteractionStop();
        foreground = false; autoWalkPlanning=false;mapPoiOpen=false;mapPoiSnapshot="";mapPoiAt=0; mapTapEpoch++; invalidateQuickBallOutcome(); cameraSampleRequest.set(false); invalidateCameraHeading("App backgrounded");
        // Losing the foreground switches VGP off, clears any target, and never auto-resumes.
        vgp.suspend();
        engine.pause(); saveState();
        // Best effort: give any queued settings a bounded window to reach disk before a possible kill.
        // Writes already run off-thread, so this only waits briefly; failures are logged, never thrown.
        try { flushSettings(SETTINGS_BACKGROUND_FLUSH_MS); }
        catch (IOException pending) { event("settings-save-error",pending.toString()); }
    }
    private static void restoreRuntime(String reason) {
        if (!registered) return;
        LocationState.capture(engine, desiredEnabled).restoreStationary(engine, canEnable(), foreground);
        event("state-activation", reason + "; desiredEnabled=" + desiredEnabled + "; runtimeEnabled=" + engine.enabled + "; stationary");
    }

    /** A moving checkpoint is queued at most once per second; explicit controls call saveState directly. */
    public static void checkpoint(long now) {
        if (now - lastCheckpoint < 1000) return;
        lastCheckpoint = now;
        if (!checkpointPending.compareAndSet(false, true)) return;
        stateWriter.execute(() -> { try { saveState(); } finally { checkpointPending.set(false); } });
    }
    public static void saveState() {
        synchronized (stateLock) {
            if (stateFile == null) return;
            FileOutputStream out = null;
            StrictMode.ThreadPolicy policy = StrictMode.allowThreadDiskWrites();
            try {
                String encoded = LocationState.capture(engine, desiredEnabled).encode();
                if (encoded.equals(lastSaved)) return;
                out = stateFile.startWrite();
                out.write(encoded.getBytes(StandardCharsets.UTF_8));
                stateFile.finishWrite(out); out = null;
                lastSaved = encoded;
                if (stateWriteFailed) { stateWriteFailed = false; event("state-save-restored", "Location checkpoints are being saved again"); }
            } catch (IOException | RuntimeException error) {
                if (out != null) stateFile.failWrite(out);
                if (!stateWriteFailed) { stateWriteFailed = true; event("state-save-error", error.toString()); }
            } finally { StrictMode.setThreadPolicy(policy); }
        }
    }
    private static void loadState(Application app) {
        synchronized (stateLock) {
            stateFile = new AtomicFile(AccountProfiles.file(app, "pgrazor-location-state-v1.txt"));
            LocationState state = LocationState.defaults();
            String source = "defaults";
            StrictMode.ThreadPolicy policy = StrictMode.allowThreadDiskReads();
            try (FileInputStream in = stateFile.openRead()) {
                ByteArrayOutputStream bytes = new ByteArrayOutputStream();
                byte[] buffer = new byte[LocationState.MAX_RECORD_LENGTH + 1];
                int read;
                while ((read = in.read(buffer)) != -1) {
                    bytes.write(buffer, 0, read);
                    if (bytes.size() > LocationState.MAX_RECORD_LENGTH) throw new IOException("Oversized location state");
                }
                state = LocationState.decode(new String(bytes.toByteArray(), StandardCharsets.UTF_8));
                lastSaved = state.encode(); source = "saved";
            } catch (FileNotFoundException missing) {
                // First install has an enabled preference, but runtime remains gated by readiness.
            } catch (IOException | RuntimeException error) {
                event("state-load-error", "Using defaults: " + error);
            } finally { StrictMode.setThreadPolicy(policy); }
            // The spoofing preference is loaded from settings; coordinates restore stationary.
            state.restoreStationary(engine, false, false);
            event("state-restored", String.format(Locale.US,"source=%s lat=%.7f lon=%.7f speedKmh=%.2f bearing=%.2f desiredEnabled=%s; awaiting ready foreground; movement stopped",source,state.latitude,state.longitude,state.speedKmh,state.bearing,desiredEnabled));
        }
        saveState();
    }
    private static boolean darkTheme=true, joystickVisible=true;
    private static volatile int uiScalePercent=100;
    public static int uiScalePercent(){return uiScalePercent;}
    public static void setUiScalePercent(int value){if(value!=25&&value!=50&&value!=75&&value!=100)return;uiScalePercent=value;saveSettings();}
    public static boolean darkTheme(){return darkTheme;}
    public static boolean joystickVisible(){return joystickVisible;}
    public static void setDarkTheme(boolean value){darkTheme=value;saveSettings();}
    public static void setJoystickVisible(boolean value){joystickVisible=value;saveSettings();}
    private static void loadSettings(Application app) {
        synchronized(stateLock) {
            if(settingsFile==null) {
                // Create the destination and its writer once so the sink can close over a stable
                // target; the file path never changes within a process.
                settingsFile=new AtomicFile(AccountProfiles.file(app,"pgrazor-settings-v1.properties"));
                final AtomicFile target=settingsFile;
                settingsWriter=new SettingsWriter(settingsIo,snapshot->writeSettings(target,snapshot),
                                                  error->event("settings-save-error",error.toString()));
            }
            // A reload must reflect every queued write for this file, never a half-finished one.
            try { settingsWriter.flush(SETTINGS_FLUSH_MS); }
            catch(IOException pending) { event("settings-load-error","Pending save not flushed: "+pending); return; }
            StrictMode.ThreadPolicy policy=StrictMode.allowThreadDiskReads();
            try(FileInputStream in=settingsFile.openRead()) {
                Properties p=new Properties(); p.load(in);
                desiredEnabled=Boolean.parseBoolean(p.getProperty("locationSpoofing","true"));
                darkTheme=Boolean.parseBoolean(p.getProperty("darkTheme","true"));
                try{uiScalePercent=Integer.parseInt(p.getProperty("uiScalePercent","100"));}catch(NumberFormatException invalid){uiScalePercent=100;}
                if(uiScalePercent!=25&&uiScalePercent!=50&&uiScalePercent!=75&&uiScalePercent!=100)uiScalePercent=100;
                joystickVisible=Boolean.parseBoolean(p.getProperty("joystickVisible","true"));
                cooldownOverlay=Boolean.parseBoolean(p.getProperty("cooldownOverlay","false"));
                quickSummaryEnabled=Boolean.parseBoolean(p.getProperty("autoDismiss","false"));
                spawnBooster=Boolean.parseBoolean(p.getProperty("spawnBooster","false"));
                try{giftOptions=Integer.parseInt(p.getProperty("giftOptions","0"));}catch(NumberFormatException ignored){giftOptions=0;}
                if(giftOptions<0||giftOptions>7)giftOptions=0;
                // VGP active state is runtime only: always start OFF, ignoring any legacy persisted vgpEnabled.
                autoIncubate=Boolean.parseBoolean(p.getProperty("autoIncubate","false"));
                incubateLongest=Boolean.parseBoolean(p.getProperty("incubateLongest","false"));
                vgpEnabled=false;vgp.clearTarget();
                vgpOverlay=Boolean.parseBoolean(p.getProperty("vgpOverlay","false"));
                vgpStopOnCooldown=Boolean.parseBoolean(p.getProperty("vgpStopOnCooldown","false"));
                try{vgpOptions=Integer.parseInt(p.getProperty("vgpOptions","3"))&7;}catch(NumberFormatException ignored){vgpOptions=3;}
                try{buddyBerry=Integer.parseInt(p.getProperty("buddyBerry","0"));}catch(NumberFormatException ignored){buddyBerry=0;}
                if(buddyBerry!=701&&buddyBerry!=703&&buddyBerry!=705&&buddyBerry!=706&&buddyBerry!=707)buddyBerry=0;
                int legacyBerry;try{legacyBerry=Integer.parseInt(p.getProperty("autoBerry","0"));}catch(NumberFormatException ignored){legacyBerry=0;}
                if(!BerryPriority.validId(legacyBerry))legacyBerry=0;
                berryPriority=p.getProperty("berryPriority",legacyBerry==0?"":String.valueOf(legacyBerry));
                try{BerryPriority.parse(berryPriority);}catch(IllegalArgumentException invalid){berryPriority="";}
                // Migration: installs that had an active berry keep the overlay enabled; otherwise it defaults off.
                encounterBerryEnabled=Boolean.parseBoolean(p.getProperty("encounterBerry",String.valueOf(!berryPriority.isEmpty())));
                try{battleOptions=Integer.parseInt(p.getProperty("battleOptions","0"));}catch(NumberFormatException ignored){battleOptions=0;}
                if(battleOptions<0||battleOptions>1023)battleOptions=0;
                try{rocketOptions=Integer.parseInt(p.getProperty("rocketOptions","0"));}catch(NumberFormatException ignored){rocketOptions=0;}
                if(rocketOptions<0||rocketOptions>7)rocketOptions=0;
                itemTransferEnabled=Boolean.parseBoolean(p.getProperty("itemTransfer","false"));
                itemRules=p.getProperty("itemRules","");itemLimits=p.getProperty("itemLimits","");itemCatalog=p.getProperty("itemCatalog","");
                if(!validItemRules(itemRules)||!validItemRules(itemLimits)){itemTransferEnabled=false;itemRules="";itemLimits="";}
                quickSpinEnabled=Boolean.parseBoolean(p.getProperty("quickSpin","false"));
                autoTransferEnabled=Boolean.parseBoolean(p.getProperty("autoTransfer","false"));
                try { transferProtect=Integer.parseInt(p.getProperty("transferProtect","63")); if(transferProtect<0||transferProtect>63)transferProtect=63; } catch(NumberFormatException e){transferProtect=63;}
                transferKeepAll=Boolean.parseBoolean(p.getProperty("transferKeepAll","false"));
                transferOverlayEnabled=Boolean.parseBoolean(p.getProperty("transferOverlay","false"));
                boolean[] keepDamaged=new boolean[1];
                transferKeepSpecies=normalizeKeepSpecies(p.getProperty("transferKeepSpecies",""),keepDamaged);
                // Fail safe: a corrupt keep list must not let auto transfer drop a species the user meant
                // to keep. Disable the master even with Keep All on, so toggling it off stays safe.
                if(keepDamaged[0])autoTransferEnabled=false;
                ivCatchEnabled=Boolean.parseBoolean(p.getProperty("ivCatch","true"));
                ivOwnedEnabled=Boolean.parseBoolean(p.getProperty("ivOwned","false"));
                // Migrate the former independent animation preference to Quick Catch.
                quickBallEnabled=quickSummaryEnabled;
                throwCurve=Boolean.parseBoolean(p.getProperty("curve","false"));
                throwAlwaysHit=Boolean.parseBoolean(p.getProperty("throwAlwaysHit","false"));
                throwAr=Boolean.parseBoolean(p.getProperty("throwAr","false"));
                throwGrade=boundedSetting(p,"grade",3); mapTapMode=boundedSetting(p,"mapTap",3);
                event("settings-restored","dismiss="+quickSummaryEnabled+" fastBall="+quickBallEnabled+" grade="+throwGrade+" curve="+throwCurve+" mapTap="+mapTapMode+" cameraRelative=true");
            } catch(FileNotFoundException missing) {
                // Defaults apply only until the user first saves a choice.
            } catch(IOException | RuntimeException error) { event("settings-load-error",error.toString()); }
            finally { StrictMode.setThreadPolicy(policy); }
        }
    }
    private static int boundedSetting(Properties p,String key,int max) {
        try { int n=Integer.parseInt(p.getProperty(key,"0")); return n>=0&&n<=max?n:0; }
        catch(NumberFormatException invalid) { return 0; }
    }
    static Properties exportSettings() throws IOException {
        synchronized(stateLock) {
            if(settingsFile==null)throw new IOException("Settings are not ready");
            // Persist the live snapshot and wait for it so the read reflects current choices; a failed
            // write surfaces synchronously here rather than silently returning stale settings.
            settingsWriter.submit(settingsSnapshot());
            settingsWriter.flush(SETTINGS_FLUSH_MS);
            Properties p=new Properties();
            try(FileInputStream in=settingsFile.openRead()){p.load(in);}
            return p;
        }
    }
    static void importSettings(Application app,Properties incoming,double speed) throws IOException {
        synchronized(stateLock) {
            // exportSettings drained every queued write; holding stateLock blocks new submissions, so the
            // atomic replacement below cannot race the background writer for this destination.
            Properties merged=exportSettings();merged.putAll(incoming);
            if(incoming.containsKey("autoBerry")&&!incoming.containsKey("berryPriority"))merged.remove("berryPriority");
            FileOutputStream out=null;
            try{out=settingsFile.startWrite();merged.store(out,"PGRazor settings");settingsFile.finishWrite(out);out=null;}
            finally{if(out!=null)settingsFile.failWrite(out);}
            loadSettings(app); // Restores preferences and explicitly leaves VGP off.
            engine.stop();engine.speed(speed);mapTapEpoch++;restoreRuntime("settings-import");saveState(); // Keep current coordinates; never resume a route.
        }
    }
    // Capture current preferences under stateLock; serialization and AtomicFile IO run off the main thread.
    private static void saveSettings() {
        synchronized(stateLock) {
            if(settingsFile==null)return;
            settingsWriter.submit(settingsSnapshot());
        }
    }
    /** Snapshot of every persisted preference; must be built while holding stateLock. */
    private static Properties settingsSnapshot() {
        Properties p=new Properties();
        p.setProperty("locationSpoofing",String.valueOf(desiredEnabled));
        p.setProperty("darkTheme",String.valueOf(darkTheme));p.setProperty("uiScalePercent",String.valueOf(uiScalePercent));
        p.setProperty("joystickVisible",String.valueOf(joystickVisible));
        p.setProperty("spawnBooster",String.valueOf(spawnBooster));
        p.setProperty("giftOptions",String.valueOf(giftOptions));
        p.setProperty("vgpOverlay",String.valueOf(vgpOverlay));
        p.setProperty("vgpStopOnCooldown",String.valueOf(vgpStopOnCooldown));
        p.setProperty("vgpOptions",String.valueOf(vgpOptions));
        p.setProperty("buddyBerry",String.valueOf(buddyBerry));
        p.setProperty("autoIncubate",String.valueOf(autoIncubate));
        p.setProperty("incubateLongest",String.valueOf(incubateLongest));
        p.setProperty("cooldownOverlay",String.valueOf(cooldownOverlay));p.setProperty("berryPriority",berryPriority);
        p.setProperty("encounterBerry",String.valueOf(encounterBerryEnabled));
        p.setProperty("battleOptions",String.valueOf(battleOptions));
        p.setProperty("rocketOptions",String.valueOf(rocketOptions));
        p.setProperty("itemTransfer",String.valueOf(itemTransferEnabled));p.setProperty("itemRules",itemRules);p.setProperty("itemLimits",itemLimits);p.setProperty("itemCatalog",itemCatalog);
        p.setProperty("quickSpin",String.valueOf(quickSpinEnabled));
        p.setProperty("autoTransfer",String.valueOf(autoTransferEnabled)); p.setProperty("transferProtect",String.valueOf(transferProtect));
        p.setProperty("transferKeepAll",String.valueOf(transferKeepAll)); p.setProperty("transferOverlay",String.valueOf(transferOverlayEnabled)); p.setProperty("transferKeepSpecies",transferKeepSpecies);
        p.setProperty("ivCatch",String.valueOf(ivCatchEnabled)); p.setProperty("ivOwned",String.valueOf(ivOwnedEnabled));
        p.setProperty("autoDismiss",String.valueOf(quickSummaryEnabled)); p.setProperty("fastBall",String.valueOf(quickBallEnabled));
        p.setProperty("throwAr",String.valueOf(throwAr)); p.setProperty("curve",String.valueOf(throwCurve)); p.setProperty("grade",String.valueOf(throwGrade));
        p.setProperty("throwAlwaysHit",String.valueOf(throwAlwaysHit));
        p.setProperty("mapTap",String.valueOf(mapTapMode));
        return p;
    }
    /** Writer sink: serializes to the captured AtomicFile off-thread, rolling back on failure. Never takes stateLock. */
    private static void writeSettings(AtomicFile file,Properties snapshot) throws IOException {
        FileOutputStream out=null;
        try { out=file.startWrite(); snapshot.store(out,"PGRazor settings"); file.finishWrite(out); out=null; }
        finally { if(out!=null)file.failWrite(out); }
    }
    /** Blocks until queued settings writes are durable; IO failures reach the caller synchronously. */
    static void flushSettings(long timeoutMillis) throws IOException {
        SettingsWriter writer;
        synchronized(stateLock) { writer=settingsWriter; }
        // Flush off stateLock: the writer never needs it, so a timed wait must not pin it either.
        if(writer!=null) writer.flush(timeoutMillis);
    }
    public static void event(String kind, String detail) {
        if("event-boxes".equals(kind)) {
            boolean available=false;
            try { available=new org.json.JSONObject(detail).optBoolean("available",false); }
            catch(Exception ignored) {}
            dev.pgrazor.events.EventSettings.publishBoxCollectorCapability(available);
        }
        synchronized (logLock) {
            String line = new SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss.SSSXXX", Locale.US).format(new Date()) + " [" + kind + "] " + detail;
            Log.i("PGRazor", line);
            if (log != null) log.println(line);
        }
    }
    private static void recoverRunLogs(Application app) {
        try {
            File[] files=new File(app.getFilesDir(),"pgrazor-logs").listFiles((dir,name)->name.endsWith(".txt"));
            File destination=app.getExternalFilesDir("pgrazor-diagnostics");
            if(files==null||destination==null)return;
            if(!destination.isDirectory()&&!destination.mkdirs())return;
            java.util.Arrays.sort(files,(a,b)->b.getName().compareTo(a.getName()));
            int count=0;
            for(File source:files){
                if(source.equals(logFile))continue;
                if(count++>=6)break;
                try(RandomAccessFile in=new RandomAccessFile(source,"r");FileOutputStream out=new FileOutputStream(new File(destination,source.getName()))){
                    long remaining=Math.min(in.length(),524288L);in.seek(in.length()-remaining);
                    byte[] buffer=new byte[8192];
                    while(remaining>0){int n=in.read(buffer,0,(int)Math.min(buffer.length,remaining));if(n<0)break;out.write(buffer,0,n);remaining-=n;}
                }
            }
            event("log-recovery","Recent run tails saved to app external diagnostics directory");
        }catch(IOException|RuntimeException error){event("log-recovery-error",error.toString());}
    }
    public static File currentLog() { return logFile; }
    // The staged profile's stationary fix is needed before login/map identity is available.
    public static boolean active() { return desiredEnabled && foreground && backendReady && connected() && engine.enabled; }
    public static double latitude() { latitudeReads++; return currentLatitude(); }
    public static double longitude() { longitudeReads++; return currentLongitude(); }
    public static float speed() { return (float)engine.metersPerSecond(); }
    public static float bearing() { return (float)engine.bearing; }
}
