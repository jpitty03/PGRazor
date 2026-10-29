package dev.pgrazor;

import android.app.Application;
import android.content.*;
import android.database.Cursor;
import android.net.Uri;
import android.os.*;
import org.json.JSONObject;
import java.io.*;
import java.nio.charset.StandardCharsets;

/** APK-owned startup entry. Does not use adb, su, another process, or a server. */
public final class BootstrapProvider extends ContentProvider {
    private static NativeRuntimeTransport nativeTransport;
    public boolean onCreate() {
        Context context=getContext();
        if(context==null) return false;
        Context appContext=context.getApplicationContext();
        if(!(appContext instanceof Application)) {
            android.util.Log.e("PGRazor","bootstrap-error: Application context unavailable"); return false;
        }
        Application app=(Application)appContext;
        if(!Application.getProcessName().equals(context.getApplicationInfo().processName)) return true;
        // Finish an interrupted auth swap before Unity caches any credentials or preferences.
        boolean sessionRecovered;
        try { sessionRecovered=AccountSessions.recover(context); }
        catch(IOException failure) { throw new IllegalStateException("Saved account session could not be restored; game startup stopped",failure); }
        Control.embedded("Embedded controls - starting"); Control.begin(app);
        // DEX bridges receive service callbacks before the script loader is ready.
        // Resolve delivery on main now: ContextService's verified initializer creates
        // only its HandlerThread and handlers; it invokes no game-native method.
        try {
            JSONObject startupMode=new JSONObject(readAsset(context,"pgrazor/mode.json"));
            if(startupMode.optBoolean("compiledJavaBridges",false)) {
                GameBridgeDispatch.configureBrowserSignIn(startupMode.optBoolean("googleBrowserLogin",false));
                try { GameBridgeDispatch.prepareDelivery(); }
                catch(Throwable failure) { Control.event("location-delivery-error","Compiled bridge preparation failed: "+failure.getClass().getSimpleName()); }
                Control.event("compiled-java-bridges-ready","DEX authentication, permissions and service lifecycle dispatch enabled");
            }
        } catch(Exception failure) {
            Control.event("compiled-java-bridges-unavailable",failure.getClass().getSimpleName());
        }
        if(!sessionRecovered)Control.event("account-session-recovery","Interrupted session could not be restored; sign in again. Saved profiles preserved.");
        Control.event("bootstrap", "pid="+android.os.Process.myPid()+" uid="+android.os.Process.myUid()+" process="+Application.getProcessName());
        // Native initialization must not block ContentProvider startup or the main thread.
        new Thread(() -> load(context),"pgrazor-loader").start();
        return true;
    }
    private void load(Context context) {
        try {
            JSONObject mode = new JSONObject(readAsset(context,"pgrazor/mode.json"));
            boolean compiledHost=mode.optBoolean("compiledJavaBridges",false)
                &&mode.optBoolean("compiledLocationGetters",false)&&mode.optBoolean("compiledNativeTransport",false);
            boolean survey=mode.optBoolean("il2cppSurvey",false);
            boolean cameraProbe=mode.optBoolean("cameraProbe",false);
            boolean cameraSample=mode.optBoolean("cameraSample",false);
            boolean cameraSteering=mode.optBoolean("cameraSteering",false);
            boolean sceneProbe=mode.optBoolean("sceneProbe",false);
            boolean sceneObserve=mode.optBoolean("sceneObserve",false);
            boolean interactionProbe=mode.optBoolean("interactionProbe",false);
            boolean quickClose=mode.optBoolean("quickClose",false);
            if(quickClose&&!mode.optBoolean("quickSummary",false)) throw new IOException("Summary auto-dismiss requires the summary lifecycle");
            // Compiled-only production gameplay: no Gadget, no PGRazor scripts, no java-bridge.
            // Quick Catch (close), the ball animation and camera steering ship as the standard
            // native feature set; interaction diagnostics stay opt-in. Research survey, scene and
            // passive-camera modes are explicitly rejected here and keep their existing Gadget path.
            boolean compiledGameplay=mode.optBoolean("compiledGameplay",false);
            boolean researchRuntime=mode.optBoolean("researchRuntime",false);
            if(compiledGameplay&&researchRuntime)
                throw new IOException("Compiled gameplay and research runtime are mutually exclusive");
            if(!compiledGameplay&&mode.optBoolean("gadget",false)&&!researchRuntime)
                throw new IOException("Gadget requires explicit researchRuntime mode");
            if(researchRuntime&&(mode.optBoolean("quickSummary",false)||quickClose||mode.optBoolean("quickBall",false)
                    ||mode.optBoolean("compiledCatchWait",false)))
                throw new IOException("Research runtime does not support retired script gameplay features");
            if(compiledGameplay) {
                if(!mode.optBoolean("nativePilot",false)||!mode.optBoolean("compiledJavaBridges",false)
                    ||!mode.optBoolean("compiledLocationGetters",false)||!mode.optBoolean("compiledNativeTransport",false))
                    throw new IOException("Compiled gameplay requires the native pilot, compiled Java bridges, compiled Location getters and compiled native transport");
                if(mode.optBoolean("gadget",false)) throw new IOException("Compiled gameplay must not bundle the Gadget runtime");
                if(survey||sceneProbe||sceneObserve||cameraSample||(cameraProbe&&!cameraSteering))
                    throw new IOException("Compiled gameplay does not support research survey, scene or passive camera modes");
            }
            if(sceneObserve&&!sceneProbe) throw new IOException("Scene observation requires the scene probe");
            if(cameraSample&&!cameraProbe) throw new IOException("Camera sampling requires the camera probe");
            if(cameraSteering&&!cameraProbe) throw new IOException("Camera steering requires the camera probe");
            if(cameraSteering&&cameraSample) throw new IOException("Camera steering and sampling cannot run together");
            if(survey&&cameraProbe) throw new IOException("Camera probe and IL2CPP survey cannot run together");
            if(sceneProbe&&(survey||cameraProbe)) throw new IOException("Scene probe cannot run with other native diagnostics or camera steering");
            if(interactionProbe&&(survey||sceneProbe||cameraSample||(cameraProbe&&!cameraSteering))) throw new IOException("Interaction probe can run alone or with camera steering only");
            if(!compiledGameplay&&(survey||cameraProbe||sceneProbe||interactionProbe)&&!mode.optBoolean("gadget",false)) throw new IOException("Native diagnostics require embedded Gadget");
            if(!compiledGameplay&&!mode.optBoolean("gadget",false)) {
                Control.embedded("PANEL ONLY - location adapter not bundled");
                Control.event("bootstrap-ready","Panel-only build; coordinates do not reach the game"); return;
            }
            String abi=mode.optString("preferredAbi","");
            if(!android.os.Process.is64Bit()) throw new IOException("This prototype supports only 64-bit app processes");
            String nativeAbi=RuntimeAbi.nativeProcessAbi();
            Control.event("native-process-abi","native="+nativeAbi+" requested="+abi+" source=/proc/self/exe");
            org.json.JSONArray bundled=mode.optJSONArray("runtimeAbis");
            String[] available=new String[bundled==null?1:bundled.length()];
            if(bundled==null) available[0]=abi;
            else for(int i=0;i<available.length;i++) available[i]=bundled.getString(i);
            abi=RuntimeAbi.select(abi,nativeAbi,available);
            boolean translated="x86_64".equals(abi)&&java.util.Arrays.asList(available).contains("arm64-v8a");
            if(compiledGameplay) {
                if(!"arm64-v8a".equals(abi)&&!translated) throw new IOException("Compiled gameplay requires the ARM64 gameplay runtime");
                loadCompiledGameplay(context,abi,translated,cameraSteering,cameraSample,interactionProbe,quickClose);
                return;
            }
            if(!"arm64-v8a".equals(abi)&&!translated) {
                // Java GPS/UI use the host ABI. ARM Unity callbacks cannot be
                // installed into the host runtime across MuMu's translator.
                survey=cameraProbe=cameraSample=cameraSteering=false;
                sceneProbe=sceneObserve=interactionProbe=false;
                Control.event("native-features-unavailable","ARM gameplay hooks unavailable in translated process; location and Java controls remain active");
            }
            File directory=new File(context.getCodeCacheDir(),"pgrazor-"+android.os.Process.myPid()+"-"+System.currentTimeMillis());
            if(!directory.mkdirs()) throw new IOException("Cannot create private runtime directory");
            File library=compiledHost&&translated?null:copyAsset(context,"pgrazor/"+abi+"/libfrida-gadget.so",directory,"libfrida-gadget.so");
            File bridge=compiledHost?null:copyAsset(context,"pgrazor/java-bridge.js",directory,"java-bridge.js");
            File script=copyAsset(context,"pgrazor/embedded-agent.js",directory,"embedded-agent.js");
            JSONObject parameters=new JSONObject().put("googleBrowserLogin",mode.optBoolean("googleBrowserLogin",false)).put("startupLog",new File(directory,"gadget-startup.txt").getAbsolutePath());
            if(bridge!=null)parameters.put("bridgePath",bridge.getAbsolutePath());
            parameters.put("compiledJavaBridges",mode.optBoolean("compiledJavaBridges",false));
            if(mode.optBoolean("compiledLocationGetters",false)) {
                if(!mode.optBoolean("nativePilot",false)||!mode.optBoolean("compiledJavaBridges",false))
                    throw new IOException("Compiled Location requires native and DEX bridges");
                File art=copyAsset(context,"pgrazor/"+abi+"/libpgrazor-art.so",directory,"libpgrazor-art.so");
                System.load(art.getAbsolutePath());
                if(!NativeJavaHooks.ready()||!NativeLocationGetters.install())
                    throw new IOException("Compiled Location initialization failed");
                parameters.put("compiledLocationGetters",true);
                Control.event("compiled-location-ready","Compiled ART getters and DEX callers initialized");
            }
            if(mode.optBoolean("nativePilot",false)) {
                try {
                    File core=copyAsset(context,"pgrazor/"+abi+"/libpgrazor-core.so",directory,"libpgrazor-core.so");
                    System.load(core.getAbsolutePath());
                    if(NativeCore.apiVersion()!=1 || NativeCore.policySelfcheck()!=0)
                        throw new IOException("Native core contract validation failed");
                    File gameplayCore=translated ? copyAsset(context,"pgrazor/arm64-v8a/libpgrazor-core.so",directory,"libpgrazor-core-arm64.so") : core;
                    parameters.put("nativeCorePath",gameplayCore.getAbsolutePath());
                    parameters.put("nativeCoreTest",mode.optBoolean("nativeSelftest",false));
                    if(mode.optBoolean("compiledNativeTransport",false)) {
                        // Standard guest JNI and guest dlopen must share this exact absolute path.
                        // Load/register in the ARM namespace before creating the host daemon.
                        if(translated)System.load(gameplayCore.getAbsolutePath());
                        NativeRuntimeTransport transport=NativeRuntimeTransport.create(500,2048,1048576,1048576);
                        transport.tick();
                        if(transport.isStopped())throw new IOException("Native transport initialization failed");
                        nativeTransport=transport;
                        if(compiledHost)transport.ownLifecycle();
                        transport.start();
                        if(transport.isStopped())throw new IOException("Native transport scheduling failed");
                        parameters.put("compiledNativeTransport",true);
                        Control.event("native-transport-ready","Compiled ARM JNI settings/event exchange active");
                        NativeRuntimeBootstrap.start();
                    }
                    if(!translated)NativeRuntimeBootstrap.start();
                    Control.event("native-core-ready","Host JNI validated; abi="+abi+"; gameplay validation pending");
                } catch(Throwable error) {
                    if(nativeTransport!=null){nativeTransport.stop();nativeTransport=null;}
                    if(compiledHost)throw error;
                    // A pilot failure must not disable existing settings, auth or the runtime bridge.
                    Control.event("native-core-unavailable",error.getClass().getSimpleName());
                }
            }
            if(survey) {
                File diagnostic=copyAsset(context,"pgrazor/il2cpp-survey.js",directory,"il2cpp-survey.js");
                parameters.put("surveyPath",diagnostic.getAbsolutePath());
                Control.event("il2cpp-survey-enabled","Opt-in native ARM64 metadata diagnostic bundled");
            }
            if(cameraProbe) {
                File diagnostic=copyAsset(context,"pgrazor/camera-probe.js",directory,"camera-probe.js");
                parameters.put("cameraProbePath",diagnostic.getAbsolutePath());
                if(cameraSample) parameters.put("cameraSample",true);
                if(cameraSteering) parameters.put("cameraSteering",true);
                Control.event("camera-probe-enabled","Opt-in native ARM64 camera probe bundled; sampling="+cameraSample+" steering="+cameraSteering);
            }
            if(sceneProbe) {
                File diagnostic=copyAsset(context,"pgrazor/scene-probe.js",directory,"scene-probe.js");
                parameters.put("sceneProbePath",diagnostic.getAbsolutePath());
                if(sceneObserve) parameters.put("sceneObserve",true);
                Control.event("scene-probe-enabled","Opt-in native ARM64 scene diagnostic bundled; observeRuntime="+sceneObserve);
            }
            if(interactionProbe) {
                File diagnostic=copyAsset(context,"pgrazor/interaction-probe.js",directory,"interaction-probe.js");
                parameters.put("interactionProbePath",diagnostic.getAbsolutePath()).put("interactionProbe",true);
                Control.event("interaction-probe-enabled","Opt-in interaction metadata probe bundled; observation requires a user request");
            }
            if(compiledHost) {
                Control.configureCameraSteering(cameraSteering);
                Control.configureCameraDiagnostics(cameraSample);
                Control.configureInteractionDiagnostics(interactionProbe);
                Control.event("compiled-host-ready","Java lifecycle, Location and settings transport initialized");
                if(!translated)parameters.put("translatedRole","native");
            }
            File gameLibrary=null;
            if(translated) {
                if(!parameters.optBoolean("compiledNativeTransport",false)) {
                File mailbox=new File(directory,"mailbox");
                if(!mailbox.mkdirs()) throw new IOException("Cannot create runtime mailbox");
                for(String name:new String[]{"state.json","events.json"}) {
                    try(FileOutputStream out=new FileOutputStream(new File(mailbox,name))) {out.write("{}".getBytes(StandardCharsets.UTF_8));}
                }
                parameters.put("mailboxPath",mailbox.getAbsolutePath()).put("mailboxSession",directory.getName());
                }
                JSONObject gameParameters=new JSONObject(parameters.toString()).put("translatedRole","native")
                    .put("startupLog",new File(directory,"game-startup.txt").getAbsolutePath());
                gameLibrary=copyAsset(context,"pgrazor/arm64-v8a/libfrida-gadget.so",directory,"libpgrazor-game.so");
                JSONObject gameConfig=new JSONObject().put("runtime","qjs").put("interaction",new JSONObject()
                    .put("type","script").put("path",script.getAbsolutePath()).put("parameters",gameParameters).put("on_change","ignore"));
                try(FileOutputStream out=new FileOutputStream(new File(directory,"libpgrazor-game.config"))) {
                    out.write(gameConfig.toString().getBytes(StandardCharsets.UTF_8));
                }
                parameters.put("translatedRole","host");
                for(String key:new String[]{"surveyPath","cameraProbePath","sceneProbePath","interactionProbePath"}) parameters.remove(key);
                for(String key:new String[]{"cameraSample","cameraSteering","sceneObserve","interactionProbe"}) parameters.put(key,false);
            }
            JSONObject interaction=new JSONObject().put("type","script").put("path",script.getAbsolutePath()).put("parameters",parameters).put("on_change","ignore");
            JSONObject config=new JSONObject().put("interaction",interaction).put("runtime","qjs");
            try(FileOutputStream out=new FileOutputStream(new File(directory,"libfrida-gadget.config"))) {
                out.write(config.toString().getBytes(StandardCharsets.UTF_8));
            }
            if(!compiledHost)Control.embedded("Embedded location adapter - loading");
            if(library!=null)Control.event("gadget-loading","abi="+abi+" version="+mode.optString("version")+" library="+library.getAbsolutePath());
            final File startupLog=new File(directory,"gadget-startup.txt");
            new Handler(Looper.getMainLooper()).postDelayed(() -> {
                if(!Control.canEnable()) {
                    Control.event("gadget-not-ready","Location adapter did not become ready in 20 seconds");
                    try {
                        if(startupLog.isFile()) {
                            byte[] data = new byte[(int)Math.min(8192,startupLog.length())];
                            try(FileInputStream in=new FileInputStream(startupLog)) { int n=in.read(data); if(n>0)Control.event("gadget-detail",new String(data,0,n,StandardCharsets.UTF_8)); }
                        }
                    } catch(IOException error) { Control.event("gadget-detail-error",error.toString()); }
                }
            },20000);
            if(library!=null) {
                System.load(library.getAbsolutePath());
                Control.event("gadget-loaded","Native library loaded; awaiting gameplay readiness");
            }
            if(gameLibrary!=null) {
                final File translatedLibrary=gameLibrary;
                new Handler(Looper.getMainLooper()).postDelayed(() -> new Thread(() -> {
                    try {
                        System.load(translatedLibrary.getAbsolutePath());
                        Control.event("game-gadget-loaded","Translated ARM runtime loaded; awaiting native hooks");
                    } catch(Throwable error) { Control.event("game-gadget-error",error.toString()); }
                },"pgrazor-game-loader").start(),1500);
            }
        } catch(Throwable error) {
            if(nativeTransport!=null){nativeTransport.stop();nativeTransport=null;}
            Control.embedded("Embedded adapter failed - open Run log");
            Control.event("bootstrap-error",error.toString());
        }
    }
    /**
     * Final compiled-only gameplay startup: no Frida Gadget, no PGRazor script runtime and no
     * java-bridge are packaged or loaded. It loads the compiled ART getters and the host (and, under
     * MuMu translation, the guest ARM) native core exactly like the working compiled host, keeps the
     * feature-52 binary transport owning the Java lifecycle, configures the UI settings the compiled
     * gameplay hosts read through the transport snapshot, then arms the real Unity-render callback
     * (the compiled DEX bridge) and returns before any script/Gadget path. No background IL2CPP
     * thread_attach happens here; IL2CPP bootstrap runs on the rendered Unity thread.
     */
    private void loadCompiledGameplay(Context context,String abi,boolean translated,
            boolean cameraSteering,boolean cameraSample,boolean interactionProbe,boolean quickClose) throws Exception {
        File directory=new File(context.getCodeCacheDir(),"pgrazor-"+android.os.Process.myPid()+"-"+System.currentTimeMillis());
        if(!directory.mkdirs()) throw new IOException("Cannot create private runtime directory");
        // Compiled ART getters and DEX callers, in the same order as the compiled host.
        File art=copyAsset(context,"pgrazor/"+abi+"/libpgrazor-art.so",directory,"libpgrazor-art.so");
        System.load(art.getAbsolutePath());
        if(!NativeJavaHooks.ready()||!NativeLocationGetters.install())
            throw new IOException("Compiled Location initialization failed");
        Control.event("compiled-location-ready","Compiled ART getters and DEX callers initialized");
        // Host native core; under translation also load/register the guest ARM core in the ARM
        // namespace before the host transport daemon, sharing this exact absolute path.
        File core=copyAsset(context,"pgrazor/"+abi+"/libpgrazor-core.so",directory,"libpgrazor-core.so");
        System.load(core.getAbsolutePath());
        if(NativeCore.apiVersion()!=1||NativeCore.policySelfcheck()!=0)
            throw new IOException("Native core contract validation failed");
        File gameplayCore=translated?copyAsset(context,"pgrazor/arm64-v8a/libpgrazor-core.so",directory,"libpgrazor-core-arm64.so"):core;
        if(translated)System.load(gameplayCore.getAbsolutePath());
        // Feature 52 binary transport: typed settings/event exchange and the sole 50 ms cadence.
        NativeRuntimeTransport transport=NativeRuntimeTransport.create(500,2048,1048576,1048576);
        transport.tick();
        if(transport.isStopped())throw new IOException("Native transport initialization failed");
        nativeTransport=transport;
        transport.ownLifecycle();
        transport.start();
        if(transport.isStopped())throw new IOException("Native transport scheduling failed");
        Control.event("native-transport-ready","Compiled ARM JNI settings/event exchange active");
        // Settings the compiled gameplay hosts read through the transport snapshot. Quick Catch
        // auto-dismiss follows quick-close; sampling stays off; interaction diagnostics stay opt-in.
        Control.configureQuickSummary(true);
        Control.configureQuickBall(true);
        Control.configureQuickSummaryAutoDismiss(quickClose);
        Control.configureCameraSteering(cameraSteering);
        Control.configureCameraDiagnostics(cameraSample);
        Control.configureInteractionDiagnostics(interactionProbe);
        Control.event("compiled-host-ready","Java lifecycle, Location and settings transport initialized");
        // Attest exclusive gameplay ownership; the compiled DEX render bridge drives onRendered().
        NativeRuntimeBootstrap.startGameplay();
        Control.event("compiled-gameplay-ready","Compiled-only gameplay armed; no Frida or PGRazor scripts packaged; abi="+abi);
    }
    private static String readAsset(Context context,String name) throws IOException {
        try(InputStream in=context.getAssets().open(name); ByteArrayOutputStream out=new ByteArrayOutputStream()) {
            byte[] buffer=new byte[8192]; int count; while((count=in.read(buffer))!=-1)out.write(buffer,0,count);
            return new String(out.toByteArray(),StandardCharsets.UTF_8);
        }
    }
    private static File copyAsset(Context context,String asset,File directory,String name) throws IOException {
        File file=new File(directory,name);
        try(InputStream in=context.getAssets().open(asset); FileOutputStream out=new FileOutputStream(file)) {
            byte[] buffer=new byte[65536]; int count; while((count=in.read(buffer))!=-1)out.write(buffer,0,count);
        }
        if(!file.setReadOnly()) throw new IOException("Cannot mark runtime file read-only");
        return file;
    }
    public Cursor query(Uri u,String[] p,String s,String[] a,String order) { return null; }
    public String getType(Uri u) { return null; }
    public Uri insert(Uri u,ContentValues v) { throw new UnsupportedOperationException(); }
    public int delete(Uri u,String s,String[] a) { throw new UnsupportedOperationException(); }
    public int update(Uri u,ContentValues v,String s,String[] a) { throw new UnsupportedOperationException(); }
}
