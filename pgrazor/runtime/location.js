// Game-facing GPS adapter. Creation is inert: delivery installation must run on the main thread.
// Dependencies are supplied by the bootstrap; this module owns its hooks and retained manager.
globalThis.PGRazorLocation = Object.freeze({create({Java, control, isDisposed, bootLog}) {
    if(globalThis.PGRazorNativeCorePath){
        if(!globalThis.PGRazorCompiledLocation)throw Error('compiled-location-adapter-missing');
        const adapter=globalThis.PGRazorCompiledLocation.create({Java,control,isDisposed,bootLog});
        if(!adapter)throw Error('compiled-location-adapter-unavailable');
        return adapter;
    }
    const permissionHooks = [];
    let locationDelivery = null;
    function installLocationPermissionGates() {
        const installed = [];
        let reported = false;
        function report() {
            if (!reported) { reported = true; control.event('location-permission', 'Using synthetic location; Android permission unchanged'); }
        }
        function hook(method, implementation) { installed.push(method); method.implementation = implementation; }
        try {
            // Only the game's foreground-location gates accept our synthetic provider.
            // Android Context/PackageManager checks and actual OS grants remain untouched.
            function syntheticPermission(permission) {
                const name = String(permission);
                return !isDisposed() && control.desiredEnabled() &&
                (name === 'android.permission.ACCESS_FINE_LOCATION' || name === 'android.permission.ACCESS_COARSE_LOCATION');
            }
            const unityPermission = Java.use('com.unity3d.player.UnityPermissions')
                .hasUserAuthorizedPermission.overload('android.content.Context', 'java.lang.String');
            hook(unityPermission, function (context, permission) {
                if (syntheticPermission(permission)) { report(); return true; }
                return unityPermission.call(this, context, permission);
            });
            const pluginPermission = Java.use('com.unity3d.plugin.UnityAndroidPermissions')
                .IsPermissionGranted.overload('android.app.Activity', 'java.lang.String');
            hook(pluginPermission, function (activity, permission) {
                if (activity !== null && syntheticPermission(permission)) { report(); return true; }
                return pluginPermission.call(this, activity, permission);
            });
            // Current game builds also query Niantic's foreground permission service.
            const Permissions = Java.use('com.nianticlabs.platform.niaadvsynccore.permission.Permissions');
            const PermissionStatus = Java.use('com.nianticlabs.platform.niaadvsynccore.permission.PermissionStatus');
            // Early startup (notably MuMu) can expose null enum fields before
            // their class initializer has run. Invoke the enum API first.
            Permissions.values();
            PermissionStatus.values();
            const locationFlag = String(Permissions.LOCATION.value.getValue());
            const preciseFlag = String(Permissions.PRECISE_LOCATION.value.getValue());
            const permissionStatus = Java.use('com.nianticlabs.platform.niaadvsyncpermissions.internal.InternalPermissionManager')
                .permissionStatus.overload('long');
            hook(permissionStatus, function (permission) {
                if (!isDisposed() && control.desiredEnabled()) {
                    const flag = String(permission);
                    if (flag === locationFlag) { report(); return PermissionStatus.GRANTED_IN_USE.value; }
                    if (flag === preciseFlag) { report(); return PermissionStatus.GRANTED_ALWAYS.value; }
                }
                return permissionStatus.call(this, permission);
            });
            permissionHooks.push(...installed);
        } catch (error) {
            for (const method of installed) method.implementation = null;
            control.event('location-permission-error', String(error));
        }
    }
    // Use the game's own negative/acknowledge actions; never grant a permission or enable sync.
    function installAdventureSyncDismissal() {
        for (const [name, action] of [
            ['InitialDialog', 'onClickCancel'],
            ['ManualPermissionDialog', 'onClickCancel'],
            ['FinalDialog', 'onClickClose']
        ]) {
            try {
                const klass = Java.use('com.nianticlabs.background.configuration.' + name);
                const create = klass.onCreate.overload('android.os.Bundle');
                const dismiss = klass[action].overload('android.view.View');
                create.implementation = function (state) {
                    create.call(this, state);
                    try {
                        if (isDisposed() || !control.desiredEnabled() || this.isFinishing()) return;
                        // These OS checks are intentionally NOT the game's synthetic permission gates.
                        if (this.checkSelfPermission('android.permission.ACCESS_FINE_LOCATION') === 0 ||
                            this.checkSelfPermission('android.permission.ACCESS_COARSE_LOCATION') === 0) return;
                        dismiss.call(this, null);
                        control.event('adventure-sync-dismissed', name + ': app location permission denied');
                    } catch (error) { control.event('adventure-sync-dismiss-error', String(error)); }
                };
                permissionHooks.push(create);
            } catch (error) { control.event('adventure-sync-hook-unavailable', name + ': ' + error); }
        }
    }
    function installLocationDelivery() {
        control.event('location-delivery', 'Initializing game location adapter on main thread');
        const Manager = Java.use('com.nianticlabs.nia.location.NianticLocationManager');
        const Service = Java.use('com.nianticlabs.nia.contextservice.ContextService');
        const Location = Java.use('android.location.Location');
        const System = Java.use('java.lang.System');
        const Clock = Java.use('android.os.SystemClock');
        const handler = Service.getServiceHandler();
        const notify = Manager.nativeLocationUpdate.overload('java.lang.String', 'android.location.Location', 'int', 'android.content.Context');
        const installed = [];
        let manager = null, paused = false, stopped = false, runnable = null, first = true;
        const providers = new Map();let nextProvider=1,Listener;
        function restoreProviders() {
            for(const record of providers.values()) {
                try { record.provider.setListener(record.original); }
                catch(error) { bootLog('location-listener-restore: '+error); }
                record.provider.$dispose();record.original.$dispose();record.proxy.$dispose();
            }
            providers.clear();
        }
        function attachProviders(current) {
            const iterator=current.providers.value.values().iterator();
            while(iterator.hasNext()) {
                const object=iterator.next();
                if(Array.from(providers.values()).some(r=>r.provider.$isSameObject(object)))continue;
                const provider=Java.cast(object,Java.use(String(object.getClass().getName())));
                const original=provider.providerListener.value;if(original===null)continue;
                const id=nextProvider++,proxy=Listener.$new();proxy.key.value=id;
                const record={provider:Java.retain(provider),original:Java.retain(original),proxy,
                    owner:current,name:String(provider.getProviderName())};
                providers.set(id,record);
                try { provider.setListener(proxy); }
                catch(error){providers.delete(id);record.provider.$dispose();record.original.$dispose();proxy.$dispose();throw error;}
            }
        }
        function forward(listener,method,args) {
            const record=providers.get(listener.key.value);if(!record)return;
            if(method!=='onProviderSatellites'&&enabled(record.owner)&&deliver(record.owner,record.name))return;
            record.original[method].apply(record.original,args);
        }
        function same(object) { return manager !== null && manager.$isSameObject(object); }
        function release() {
            if (runnable !== null) handler.removeCallbacks(runnable);
            restoreProviders();
            const old = manager; manager = null;
            if (old !== null) old.$dispose();
        }
        function enabled(object) {
            return !stopped && !isDisposed() && !paused && same(object) && control.active();
        }
        function capture(object) {
            if (stopped || isDisposed()) return;
            if (!same(object)) { release(); manager = Java.retain(object); first = true; }
            paused = false;
            handler.removeCallbacks(runnable);
            handler.post(runnable);
        }
        function deliver(current, provider) {
            let sent = false;
            // Use identical freshness/accuracy for both periodic and physical-provider callbacks.
            // Otherwise an old real fix can replace the synthetic fix between timer ticks.
            Java.synchronized(current.callbackLock.value, () => {
                if (!enabled(current) || String(current.nativeClassPointer.value) === '0') return;
                const fix = Location.$new(provider);
                try {
                    fix.setLatitude(control.latitude()); fix.setLongitude(control.longitude());
                    fix.setSpeed(control.speed()); fix.setBearing(control.bearing());
                    fix.setAltitude(10); fix.setAccuracy(5);
                    fix.setTime(System.currentTimeMillis());
                    fix.setElapsedRealtimeNanos(Clock.elapsedRealtimeNanos());
                    notify.call(current, provider, fix, 3, current.getContext());
                    sent = true;
                    if (first) { first = false; control.event('location-delivery', 'Synthetic location updates active'); }
                } finally { fix.$dispose(); }
            });
            return sent;
        }
        function tick() {
            if (stopped || isDisposed() || manager === null || paused) return;
            const current = manager;
            try {
                attachProviders(current);
                if (enabled(current)) deliver(current, 'fused');
            } catch (error) {
                if (first) { first = false; bootLog('location-delivery-error: ' + error); }
            } finally {
                if (!stopped && !isDisposed() && manager === current && !paused) handler.postDelayed(runnable, 1000);
            }
        }
        function hook(method, implementation) {
            installed.push(method); method.implementation = implementation;
        }
        function stop() {
            stopped = true;
            release();
            for (const method of installed) method.implementation = null;
            installed.length = 0;
        }
        try {
            // Use a normal provider-listener object; never replace nativeLocationUpdate.
            // ART can crash resolving calls to that replaced native method on provider threads.
            Listener=Java.registerClass({name:'dev.pgrazor.LocationProviderListener',
                implements:[Java.use('com.nianticlabs.nia.location.NianticLocationProvider$ProviderListener')],
                fields:{key:'int'},methods:{
                    onProviderStatus:function(status){forward(this,'onProviderStatus',[status]);},
                    onProviderLocation:function(fix){forward(this,'onProviderLocation',[fix]);},
                    onProviderSatellites:function(time,satellites){forward(this,'onProviderSatellites',[time,satellites]);}
                }});
            const Runner = Java.registerClass({name: 'dev.pgrazor.LocationDeliveryTick',
                implements: [Java.use('java.lang.Runnable')], methods: {run: tick}});
            runnable = Runner.$new();
            // Hook the private JNI lifecycle dispatchers, not virtual onStart/onStop.
            // ART 17 can crash dispatching a replaced virtual lifecycle override.
            const start = Service.invokeOnStart.overload(), end = Service.invokeOnStop.overload();
            const pause = Service.invokeOnPause.overload(), resume = Service.invokeOnResume.overload();
            function locationManager(object) { return Manager.class.isInstance(object); }
            hook(start, function () {
                const result = start.call(this);
                if (locationManager(this)) capture(Java.cast(this, Manager));
                return result;
            });
            hook(end, function () { if (same(this)) release(); return end.call(this); });
            hook(pause, function () {
                if (same(this)) { paused = true; handler.removeCallbacks(runnable); }
                return pause.call(this);
            });
            hook(resume, function () { const result = resume.call(this); if (locationManager(this)) capture(Java.cast(this, Manager)); return result; });
            locationDelivery = {stop};
            control.event('location-delivery', 'Game location adapter installed; OS permission unchanged');
        } catch (error) {
            stop();
            throw error;
        }
    }
    function stop() {
        if (locationDelivery !== null) { locationDelivery.stop(); locationDelivery = null; }
        for (const method of permissionHooks) method.implementation = null;
        permissionHooks.length = 0;
    }
    return {installPermissions() { installLocationPermissionGates(); installAdventureSyncDismissal(); }, installDelivery: installLocationDelivery, stop};
}});
