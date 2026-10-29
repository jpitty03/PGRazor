// Compiled game-location adapter for the selected native pilot. Creation is inert; the
// permission policy and the whole provider/delivery path live in the compiled Java helpers
// (dev.pgrazor.GameLocationPermissions, dev.pgrazor.GameLocationDelivery). This module only
// installs the game-facing hooks and bridges them to those helpers. It never constructs a
// location fix, never replaces a native method, and never touches a real OS permission grant.
globalThis.PGRazorCompiledLocation = Object.freeze({create({Java, control, isDisposed, bootLog}) {
    const permissionHooks = [];
    let permissionsHelper = null, permissionsInstalled = false;
    let delivery = null, deliveryHooks = [], deliveryInstalled = false;
    let stopped = false, reported = false;
    function inactive() { return stopped || isDisposed(); }
    function log(message) { try { bootLog(message); } catch (_) {} }
    function restore(hooks) {
        for (const method of hooks) { try { method.implementation = null; } catch (error) { log('location-hook-restore: ' + error); } }
    }
    function report() {
        if (!reported) { reported = true; control.event('location-permission', 'Using synthetic location; Android permission unchanged'); }
    }
    function installPermissions() {
        if (inactive() || permissionsInstalled) return;
        let helper;
        try { helper = Java.use('dev.pgrazor.GameLocationPermissions').create(); }
        catch (error) { control.event('location-permission-error', String(error)); return; }
        // The permission gates are a single required group: if any hook cannot be installed,
        // roll the whole group back and shut the helper down so nothing is partially overridden.
        const installed = [];
        function hook(method, implementation) { installed.push(method); method.implementation = implementation; }
        try {
            const PermissionStatus = Java.use('com.nianticlabs.platform.niaadvsynccore.permission.PermissionStatus');
            // Only the game's foreground-location gates accept our synthetic answer. Android
            // Context/PackageManager checks and the real OS grants stay untouched.
            const unityPermission = Java.use('com.unity3d.player.UnityPermissions')
                .hasUserAuthorizedPermission.overload('android.content.Context', 'java.lang.String');
            hook(unityPermission, function (context, permission) {
                if (!inactive() && helper.locationPermission(permission)) { report(); return true; }
                return unityPermission.call(this, context, permission);
            });
            const pluginPermission = Java.use('com.unity3d.plugin.UnityAndroidPermissions')
                .IsPermissionGranted.overload('android.app.Activity', 'java.lang.String');
            hook(pluginPermission, function (activity, permission) {
                // A null Activity is never overridden: defer entirely to the original.
                if (activity !== null && !inactive() && helper.locationPermission(permission)) { report(); return true; }
                return pluginPermission.call(this, activity, permission);
            });
            const permissionStatus = Java.use('com.nianticlabs.platform.niaadvsyncpermissions.internal.InternalPermissionManager')
                .permissionStatus.overload('long');
            hook(permissionStatus, function (permission) {
                if (!inactive()) {
                    // The 64-bit flag is forwarded to Java as-is; it is never narrowed to a JS number.
                    const override = helper.permissionStatus(permission);
                    if (override !== null) { report(); return Java.cast(override, PermissionStatus); }
                }
                return permissionStatus.call(this, permission);
            });
            permissionHooks.push(...installed);
        } catch (error) {
            restore(installed);
            try { helper.shutdown(); } catch (_) {}
            control.event('location-permission-error', String(error));
            return;
        }
        permissionsHelper = helper;
        permissionsInstalled = true;
        installDialogDismissal(helper);
    }
    // Each Adventure Sync dialog hook is optional and isolated: a missing dialog class is logged
    // only, exactly as the legacy adapter behaves. The negative/acknowledge action is chosen
    // here and validated by the helper, which also performs the OS-grant and lifecycle checks.
    function installDialogDismissal(helper) {
        for (const [name, action] of [
            ['InitialDialog', 'onClickCancel'],
            ['ManualPermissionDialog', 'onClickCancel'],
            ['FinalDialog', 'onClickClose']
        ]) {
            try {
                const klass = Java.use('com.nianticlabs.background.configuration.' + name);
                const create = klass.onCreate.overload('android.os.Bundle');
                create.implementation = function (state) {
                    create.call(this, state);
                    try { if (!inactive()) helper.dismissAdventureSync(this, action); }
                    catch (error) { control.event('adventure-sync-dismiss-error', String(error)); }
                };
                permissionHooks.push(create);
            } catch (error) { control.event('adventure-sync-hook-unavailable', name + ': ' + error); }
        }
    }
    function installDelivery() {
        if (inactive() || deliveryInstalled) return;
        control.event('location-delivery', 'Initializing compiled game location adapter on main thread');
        // The selected pilot never silently falls back to the legacy adapter: a missing or
        // failing Java delivery factory propagates to the host.
        const instance = Java.use('dev.pgrazor.GameLocationDelivery').create();
        const installed = [];
        function hook(method, implementation) { installed.push(method); method.implementation = implementation; }
        try {
            const Service = Java.use('com.nianticlabs.nia.contextservice.ContextService');
            // Hook the private JNI lifecycle dispatchers, not virtual onStart/onStop: ART can
            // crash dispatching a replaced virtual lifecycle override.
            const start = Service.invokeOnStart.overload(), end = Service.invokeOnStop.overload();
            const pause = Service.invokeOnPause.overload(), resume = Service.invokeOnResume.overload();
            // Start/resume: the game runs first, then the helper captures the service. A disposed
            // adapter still runs the original but never hands the service to the helper.
            hook(start, function () {
                const result = start.call(this);
                if (!inactive()) { try { instance.start(this); } catch (error) { log('location-delivery-start: ' + error); } }
                return result;
            });
            hook(resume, function () {
                const result = resume.call(this);
                if (!inactive()) { try { instance.resume(this); } catch (error) { log('location-delivery-resume: ' + error); } }
                return result;
            });
            // Stop/pause: the helper runs first so it can release before teardown, but a helper
            // fault must never skip the game's own lifecycle, so the original always runs.
            hook(end, function () {
                if (!inactive()) { try { instance.stop(this); } catch (error) { log('location-delivery-stop: ' + error); } }
                return end.call(this);
            });
            hook(pause, function () {
                if (!inactive()) { try { instance.pause(this); } catch (error) { log('location-delivery-pause: ' + error); } }
                return pause.call(this);
            });
            delivery = instance;
            deliveryHooks = installed;
            deliveryInstalled = true;
            control.event('location-delivery', 'Compiled game location adapter installed; OS permission unchanged');
        } catch (error) {
            restore(installed);
            try { instance.shutdown(); } catch (_) {}
            throw error;
        }
    }
    function stop() {
        stopped = true;
        if (delivery !== null) { try { delivery.shutdown(); } catch (_) {} delivery = null; }
        restore(deliveryHooks);
        deliveryHooks = [];
        deliveryInstalled = false;
        if (permissionsHelper !== null) { try { permissionsHelper.shutdown(); } catch (_) {} permissionsHelper = null; }
        restore(permissionHooks);
        permissionHooks.length = 0;
        permissionsInstalled = false;
    }
    return {installPermissions, installDelivery, stop};
}});
