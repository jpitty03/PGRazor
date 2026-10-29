// Frida Gadget script-mode entry: runs wholly inside the packaged application.
// No listening socket, desktop controller, root command, or mock-flag hooks.
let bootParameters = {}, control = null, javaControl = null, timer = null;
let diagnosticTimer = null, diagnostic = null, disposed = false;
let diagnosticRunning = false, cameraReading = '', lastHeadingLog = 0;
let interactionTimer = null, interaction = null, interactionRunning = false;
const hooks = [];
let googleSignInPending = null, googleSignInTimer = null;
let compiledGoogleSignIn = null;
function finishGoogleSignIn(status, email, token) {
    const pending = googleSignInPending;
    if (!pending) return;
    googleSignInPending = null;
    if (googleSignInTimer !== null) { clearInterval(googleSignInTimer); googleSignInTimer = null; }
    Java.scheduleOnMainThread(() => {
        try {
            const Response = Java.use('com.nianticlabs.platform.authgooglesignin.SignInResponse');
            pending.consumer.invoke(Response.$new(status, email || null, token || null));
            control.event('google-browser', 'Completed: ' + status);
        } catch (_) { control.event('google-browser', 'Game sign-in callback failed'); }
        finally { pending.consumer.$dispose(); }
    });
}
function installGoogleBrowserLogin() {
    if(bootParameters.compiledJavaBridges){bootLog('google-signin-dex-owned');return;}
    if (bootParameters.googleBrowserLogin !== true) return;
    if (globalThis.PGRazorNativeCorePath) {
        if (!globalThis.PGRazorCompiledGoogleSignIn) throw Error('compiled-google-signin-adapter-missing');
        const adapter = PGRazorCompiledGoogleSignIn.create({Java, isDisposed: () => disposed, bootLog});
        if (!adapter) throw Error('compiled-google-signin-adapter-unavailable');
        compiledGoogleSignIn = adapter;
        adapter.install();
        control.event('google-browser', 'Compiled browser sign-in hook ready');
        return;
    }
    const Browser = Java.use('dev.pgrazor.GoogleBrowserAuth');
    const Manager = Java.use('com.nianticlabs.platform.authgooglesignin.SignInManager$Companion');
    const signIn = Manager.signIn.overload('android.app.Activity', 'com.google.android.gms.auth.api.signin.GoogleSignInClient', 'kotlin.jvm.functions.Function1');
    signIn.implementation = function (activity, client, consumer) {
        if (googleSignInPending) {
            Browser.cancel(googleSignInPending.id);
            finishGoogleSignIn('Cancelled', null, null);
        }
        const held = Java.retain(consumer);
        try {
            const id = String(Browser.begin(activity));
            googleSignInPending = {id, consumer: held};
            // Browser authentication deliberately survives the game's background pause.
            googleSignInTimer = setInterval(() => Java.performNow(() => {
                const pending = googleSignInPending;
                if (!pending) return;
                try {
                    const raw = Browser.takeResult(pending.id);
                    if (raw === null) return;
                    const result = JSON.parse(String(raw));
                    const status = ['Success', 'Cancelled', 'Fail'].includes(result.status) ? result.status : 'Fail';
                    finishGoogleSignIn(status, result.email, result.idToken);
                } catch (_) { Browser.cancel(pending.id); finishGoogleSignIn('Fail', null, null); }
            }), 250);
        } catch (_) {
            googleSignInPending = {id: '', consumer: held};
            finishGoogleSignIn('Fail', null, null);
        }
    };
    hooks.push(signIn);
    control.event('google-browser', 'Browser sign-in hook ready');
}
function bootLog(text) {
    const line = new Date().toISOString() + ' ' + text;
    console.log(line);
    try {
        if (bootParameters.startupLog) {
            const file = new File(bootParameters.startupLog, 'a');
            try { file.write(line + '\n'); file.flush(); } finally { file.close(); }
        }
    } catch (_) {}
}
let locationAdapter = null;
let translatedBridge = null, translatedTimer = null;
function stopCompiledGoogleSignIn() {
    const adapter = compiledGoogleSignIn;
    compiledGoogleSignIn = null;
    if (adapter !== null) {
        try { adapter.stop(); } catch (_) { bootLog('compiled-google-signin-stop-failed'); }
    }
}
function rollback() {
    stopCompiledGoogleSignIn();
    if (locationAdapter !== null) { locationAdapter.stop(); locationAdapter = null; }
    for (const method of hooks) method.implementation = null;
    hooks.length = 0;
}
function finishDiagnostic() {
    diagnosticRunning = false;
    if (!disposed && control && bootParameters.cameraSteering === true) {
        try { Java.performNow(() => control.invalidateCameraHeading('Camera reader stopped')); }
        catch (error) { bootLog('camera-status-error: ' + error); }
    }
    if (!disposed && control && bootParameters.cameraSample === true) {
        try { Java.performNow(() => control.setCameraDiagnosticRunning(false)); }
        catch (error) { bootLog('camera-status-error: ' + error); }
    }
}
function runDiagnostic() {
    const path = bootParameters.sceneProbePath || bootParameters.cameraProbePath || bootParameters.surveyPath;
    if (!path || disposed || diagnosticRunning) return;
    const kind = bootParameters.sceneProbePath ? 'scene-probe' : bootParameters.cameraProbePath ? 'camera-probe' : 'il2cpp-survey';
    diagnosticRunning = true;
    cameraReading = '';
        try {
            if (bootParameters.cameraSample === true) Java.performNow(() => {
                control.setCameraDiagnosticRunning(true);
                control.cameraDiagnostic('Checking camera thread...');
            });
            if (diagnostic === null)
                diagnostic = Script.evaluate('/pgrazor-' + kind + '.js', File.readAllText(path));
            const result = diagnostic.start({ enabled: true,
                observeRuntime: bootParameters.sceneObserve === true,
                sampleCamera: bootParameters.cameraSample === true || bootParameters.cameraSteering === true,
                continuous: bootParameters.cameraSteering === true,
                shouldSample() {
                    if (disposed || bootParameters.cameraSteering !== true) return false;
                    let requested = false;
                    Java.performNow(() => { requested = control.cameraSteeringRequested() || control.buddySamplingRequested(); });
                    return requested;
                }, onEvent(event) {
                if (disposed) return;
                Java.performNow(() => {
                    if (bootParameters.cameraSteering === true && event.kind === 'camera-sample') {
                        if (!Number.isFinite(event.yawDegrees) || !Number.isInteger(event.cameraId) ||
                            event.cameraId === 0 || !Number.isFinite(event.timeMs) || event.timeMs < 0) {
                            control.invalidateCameraHeading('Invalid camera reading');
                            return;
                        }
                        control.updateCameraHeadingAt(event.yawDegrees, event.cameraId, event.mapVisible === true,
                            Math.floor(event.timeMs));
                        if (event.timeMs - lastHeadingLog >= 1000) {
                            lastHeadingLog = event.timeMs;
                            control.event('camera-heading', JSON.stringify(event));
                        }
                    } else {
                        if (bootParameters.cameraSteering === true && event.kind === 'camera-unavailable')
                            control.invalidateCameraHeading('Camera unavailable');
                        control.event(kind, JSON.stringify(event));
                    }
                    if (bootParameters.cameraSample !== true) return;
                    if (event.kind === 'camera-sample') {
                        cameraReading = 'Camera yaw (world): ' + Number(event.yawDegrees).toFixed(1) +
                            '\u00b0\nCamera ' + event.cameraId + ' \u00b7 sample ' + event.sampleIndex;
                        control.cameraDiagnostic(cameraReading);
                    } else if (event.kind === 'finished') {
                        control.cameraDiagnostic((cameraReading ? cameraReading + '\n' : '') +
                            'Diagnostic ' + event.status + (event.reason ? ': ' + event.reason : '') +
                            (event.message ? ': ' + event.message : ''));
                    }
                });
            }});
            Promise.resolve(result).then(finishDiagnostic, error => {
                finishDiagnostic();
                bootLog(kind + '-error: ' + error);
                if (!disposed && bootParameters.cameraSample === true)
                    Java.performNow(() => control.cameraDiagnostic('Camera diagnostic error: ' + error));
            });
        } catch (error) {
            finishDiagnostic();
            bootLog(kind + '-error: ' + error);
            Java.performNow(() => {
                control.event(kind + '-error', String(error));
                if (bootParameters.cameraSample === true) control.cameraDiagnostic('Camera diagnostic error: ' + error);
            });
        }
}
function scheduleDiagnostic() {
    if (!bootParameters.cameraProbePath && !bootParameters.surveyPath && !bootParameters.sceneProbePath) return;
    const kind = bootParameters.sceneProbePath ? 'scene-probe' : bootParameters.cameraProbePath ? 'camera-probe' : 'il2cpp-survey';
    control.event(kind + '-scheduled', bootParameters.cameraSteering === true ?
        'Camera steering reader starts after a 1.5 second startup grace' : 'Opt-in diagnostic starts in 20 seconds');
    diagnosticTimer = setTimeout(() => {
        diagnosticTimer = null;
        runDiagnostic();
    }, bootParameters.cameraSteering === true ? 1500 : 20000);
}
function finishInteraction(result, observe) {
    interactionRunning = false;
    if (disposed || !control) return;
    Java.performNow(() => {
        control.setInteractionDiagnosticRunning(false);
        const status=result && result.status || 'complete';
        const detail=result && (result.reason || result.message);
        control.interactionDiagnostic(observe ? 'Capture '+status+'; see Run log'+(detail?': '+detail:'') :
            (status==='complete' ? 'Ready to record catch and spin callbacks' : 'Metadata '+status+(detail?': '+detail:'')));
    });
}
function runInteraction(observe) {
    if (disposed || interactionRunning || !bootParameters.interactionProbePath || bootParameters.interactionProbe !== true) return;
    try {
        let ready=false;
        Java.performNow(() => { ready=control.interactionDiagnosticsReady(); });
        if (!ready) return;
        interactionRunning=true;
        Java.performNow(() => {
            control.setInteractionDiagnosticRunning(true);
            control.interactionDiagnostic(observe ? 'Preparing catch/spin capture...' : 'Checking catch/spin metadata...');
        });
        if (interaction === null) interaction=Script.evaluate('/pgrazor-interaction-probe.js',File.readAllText(bootParameters.interactionProbePath));
        const result=interaction.start({enabled:true,observeRuntime:observe,readResults:observe,onEvent(event) {
            if (disposed) return;
            Java.performNow(() => {
                control.event('interaction-probe',JSON.stringify(event));
                if (event.kind==='observation-ready') control.interactionDiagnostic('Recording for 60s: catch normally, then spin a stop. Minimize this panel.');
            });
        }});
        Promise.resolve(result).then(value=>finishInteraction(value,observe),error=>{
            bootLog('interaction-probe-error: '+error);finishInteraction({status:'error',message:String(error)},observe);
        });
    } catch (error) {
        bootLog('interaction-probe-error: '+error);
        finishInteraction({status:'error',message:String(error)},observe);
    }
}
function stopInteraction() {
    if (interactionTimer!==null) { clearTimeout(interactionTimer);interactionTimer=null; }
    if (interaction===null || !interactionRunning) return;
    try { return interaction.stop(); }
    catch (error) { bootLog('interaction-stop-error: '+error);finishInteraction({status:'error',message:String(error)},true); }
}
function scheduleInteraction() {
    if (bootParameters.interactionProbe !== true || !bootParameters.interactionProbePath) return;
    control.event('interaction-probe-scheduled','Metadata check in 25 seconds; event capture requires an explicit button press');
    interactionTimer=setTimeout(()=>{interactionTimer=null;runInteraction(false);},25000);
}
function heartbeat() {
    Java.performNow(() => {
        control.renewLease();
        if (bootParameters.cameraSample === true && control.takeCameraSampleRequest()) {
            if (diagnosticTimer !== null) { clearTimeout(diagnosticTimer); diagnosticTimer = null; }
            setImmediate(runDiagnostic);
        }
        if (bootParameters.interactionProbe === true) {
            if (control.takeInteractionStopRequest()) stopInteraction();
            if (control.takeInteractionTraceRequest()) {
                if (interactionTimer!==null) {clearTimeout(interactionTimer);interactionTimer=null;}
                setImmediate(()=>runInteraction(true));
            }
        }
    });
}
function startGameplayTransport() {
        translatedBridge=bootParameters.compiledNativeTransport ? PGRazorNativeTransport.create() : PGRazorTranslated.client({io:PGRazorTranslated.files(bootParameters.mailboxPath),session:bootParameters.mailboxSession});
        control=translatedBridge.control;
        // Same-ABI Android retains Java lifecycle ownership. Gameplay readers,
        // pulses and events exclusively use the compiled snapshot/queue.
        if(javaControl){
            const lifecycle=javaControl;
            control.renewLease=()=>lifecycle.renewLease();
            control.disconnect=()=>lifecycle.disconnect();
        }
        let started=false,lastBeat=0;
        translatedTimer=setInterval(()=>{try {
            if(!translatedBridge.tick()) return;
            if(!started) {
                started=true;
                if(bootParameters.cameraSteering)control.configureCameraSteering(true);
                if(bootParameters.cameraSample)control.configureCameraDiagnostics(true);
                if(bootParameters.interactionProbe)control.configureInteractionDiagnostics(true);
                control.event('translated-ready','ARM game runtime connected to host Java controls');
                scheduleDiagnostic();scheduleInteraction();
            }
            if(Date.now()-lastBeat>=200){lastBeat=Date.now();heartbeat();}
        }catch(error){bootLog('translated-runtime-error: '+error);}},50);
}
function start() {
    if (disposed) return;
    if(bootParameters.translatedRole==='native') {
        PGRazorTranslated.installCodeValidation();
        // ARM has the game libraries; ART lives in the x86-64 host runtime.
        // Only Control messages cross this boundary, never Java object handles.
        globalThis.Java={performNow:fn=>fn(),scheduleOnMainThread:fn=>fn()};
        startGameplayTransport();
        return;
    }
    try {
        const source = File.readAllText(bootParameters.bridgePath);
        globalThis.Java = Script.evaluate('/pgrazor-java-bridge.js', source + '\nbridge;');
        bootLog('java-bridge-loaded');
        // init() has already returned. Waiting for the application class loader
        // here cannot hold the dynamic linker constructor open.
        Java.perform(() => {
            if (disposed) return;
            try {
                control = javaControl = Java.use('dev.pgrazor.Control');
                const locationControl=javaControl;
                if(!bootParameters.compiledJavaBridges){
                    locationAdapter = PGRazorLocation.create({Java, control, isDisposed: () => disposed, bootLog});
                    locationAdapter.installPermissions();
                }else bootLog('location-dex-owned');
                try { installGoogleBrowserLogin(); } catch (_) { control.event('google-browser', 'Browser sign-in unavailable'); }
                if(!bootParameters.compiledLocationGetters){
                const Location = Java.use('android.location.Location');
                // Observe only the value already returned; never call another hooked getter.
                for (const [name, getter] of [['getLatitude','latitude'],['getLongitude','longitude'],['getSpeed','speed'],['getBearing','bearing']]) {
                    const method = Location[name].overload();
                    method.implementation = function () {
                        if (locationControl.active()) return locationControl[getter]();
                        const value=method.call(this);
                        try{if(!locationControl.desiredEnabled()&&name==='getLatitude')
                            locationControl.observeRealCoordinate(true,value);
                        else if(!locationControl.desiredEnabled()&&name==='getLongitude')
                            locationControl.observeRealCoordinate(false,value);}catch(_){}
                        return value;
                    };
                    hooks.push(method);
                }
                }else bootLog('location-native-owned');
                control.connect();
                if(bootParameters.translatedRole==='host') {
                    if(bootParameters.compiledNativeTransport) {
                        control.event('translated-host-ready','Compiled JNI transport v1; Java owns cadence');
                    } else if(globalThis.PGRazorNativeCorePath) {
                        translatedBridge=Java.use('dev.pgrazor.RuntimeMailbox').create(
                            bootParameters.mailboxPath,bootParameters.mailboxSession);
                        translatedBridge.start();
                        control.event('translated-host-ready','Compiled Java mailbox v1');
                    } else {
                        translatedBridge=PGRazorTranslated.host({io:PGRazorTranslated.files(bootParameters.mailboxPath),control,session:bootParameters.mailboxSession});
                        translatedBridge.tick();
                        translatedTimer=setInterval(()=>Java.performNow(()=>translatedBridge.tick()),50);
                    }
                }
                // Game service class initialization can wait on the main thread.
                // Do not initialize it from the Gadget Java.perform worker during startup.
                Java.scheduleOnMainThread(() => {
                    if (disposed) return;
                    try { if(locationAdapter!==null)locationAdapter.installDelivery(); }
                    catch (error) {
                        control.event('location-delivery-error', String(error));
                        bootLog('location-delivery-unavailable: ' + error);
                    }
                });
                if (bootParameters.cameraSample === true) control.configureCameraDiagnostics(true);
                if (bootParameters.cameraSteering === true) control.configureCameraSteering(true);
                if (bootParameters.interactionProbe === true && bootParameters.interactionProbePath) control.configureInteractionDiagnostics(true);
                control.event('embedded-ready', (bootParameters.compiledLocationGetters ? 'Compiled ART Location getters initialized' : 'Frida Java Location getters installed')+'; saved preference restoration scheduled; see state-activation; game acceptance unverified');
                bootLog('embedded-ready');
                if(bootParameters.compiledNativeTransport&&bootParameters.translatedRole!=='host'){
                    startGameplayTransport();
                    return;
                }
                timer = setInterval(heartbeat, 2000);
                scheduleDiagnostic();
                scheduleInteraction();
            } catch (error) {
                rollback();
                if (control) { control.embedded('Embedded adapter failed - open Run log'); control.event('embedded-error',String(error)); }
                bootLog('embedded-error: ' + error);
            }
        });
    } catch (error) { bootLog('embedded-error: ' + error); }
}
rpc.exports = {
    init(stage, parameters) {
        bootParameters = parameters || {};
        globalThis.PGRazorNativeCorePath = bootParameters.nativeCorePath || '';
        globalThis.PGRazorNativeCoreTest = bootParameters.nativeCoreTest === true;
        globalThis.PGRazorNativeDiagnostic = bootParameters.nativeCoreTest === true ? bootLog : () => {};
        setImmediate(start);
    },
    dispose() {
        disposed = true;
        if(translatedTimer!==null){clearInterval(translatedTimer);translatedTimer=null;}
        if (compiledGoogleSignIn !== null && typeof Java !== 'undefined') Java.performNow(stopCompiledGoogleSignIn);
        if (googleSignInTimer !== null) { clearInterval(googleSignInTimer); googleSignInTimer = null; }
        if (googleSignInPending && typeof Java !== 'undefined') Java.performNow(() => {
            try { Java.use('dev.pgrazor.GoogleBrowserAuth').cancel(googleSignInPending.id); }
            finally { finishGoogleSignIn('Cancelled', null, null); }
        });
        if (diagnosticTimer !== null) clearTimeout(diagnosticTimer);
        const diagnosticStopped = diagnostic !== null ? diagnostic.stop() : undefined;
        const interactionStopped=stopInteraction();
        if (timer !== null) clearInterval(timer);
        if (typeof Java !== 'undefined') Java.performNow(() => {
            rollback();
            if (control) {
                if (bootParameters.cameraSample === true) control.setCameraDiagnosticRunning(false);
                if (bootParameters.cameraSteering === true) control.invalidateCameraHeading('Adapter disposed');
                if (bootParameters.interactionProbe === true) control.setInteractionDiagnosticRunning(false);
                Java.scheduleOnMainThread(() => control.disconnect());
            }
        });
        if(translatedBridge!==null){translatedBridge.stop();translatedBridge=null;}
        return Promise.all([diagnosticStopped,interactionStopped]);
    }
};
