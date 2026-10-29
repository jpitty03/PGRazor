// Compiled Google sign-in adapter for the selected native pilot. Creation is inert; the whole
// browser-authentication path — pending consumers, the poll timer, the JSON/token parsing and the
// game-facing SignInResponse callback — lives in the compiled Java helper
// (dev.pgrazor.GameGoogleSignIn). This module only installs the single game sign-in hook and
// bridges it to that helper. It never retains the consumer in JS, never registers a class, never
// runs a timer, never parses a token, and once installed it never falls back to the original
// GoogleSignInClient flow.
globalThis.PGRazorCompiledGoogleSignIn = Object.freeze({create({Java, isDisposed, bootLog}) {
    let helper = null, hook = null, installed = false, stopped = false;
    function inactive() { return stopped || isDisposed(); }
    // Errors are logged by constant phase only. A raw exception can carry a token, email or other
    // credential fragment, so it is never placed in the log line.
    function log(phase) { try { bootLog(phase); } catch (_) {} }
    function install() {
        // Inert until explicitly installed; installs at most once; never installs once the adapter
        // is stopped or the agent disposed.
        if (installed || inactive()) return;
        // The helper factory is the initial install step: a failure here propagates unchanged and
        // is never swallowed into a legacy fallback. Nothing is overridden yet, so there is nothing
        // to roll back.
        const instance = Java.use('dev.pgrazor.GameGoogleSignIn').create();
        let method = null;
        try {
            method = Java.use('com.nianticlabs.platform.authgooglesignin.SignInManager$Companion')
                .signIn.overload('android.app.Activity', 'com.google.android.gms.auth.api.signin.GoogleSignInClient', 'kotlin.jvm.functions.Function1');
            method.implementation = function (activity, client, consumer) {
                // The GoogleSignInClient is deliberately ignored: the helper owns the whole browser
                // flow and every pending consumer. A stopped or disposed adapter still forwards so
                // the captured consumer is cancelled on the main thread, but the already-dead helper
                // begins no browser. The original Google sign-in flow is never called.
                if (inactive()) { try { instance.shutdown(); } catch (_) {} }
                try { instance.signIn(activity, consumer); }
                catch (error) { log('google-signin-dispatch-error'); throw error; }
            };
            hook = method;
        } catch (error) {
            // Partial install (overload resolution or the implementation setter failed): roll back
            // whatever was overridden and shut the helper down before the failure propagates. The
            // selected pilot never silently reverts to the original Google sign-in flow.
            if (method !== null) { try { method.implementation = null; } catch (_) {} }
            try { instance.shutdown(); } catch (_) {}
            log('google-signin-install-error');
            throw error;
        }
        helper = instance;
        installed = true;
        log('google-signin-ready');
    }
    function stop() {
        // Mark stopped first so any in-flight or retained hook forwards to a dead helper instead of
        // beginning a browser. The helper is shut down even if unhooking later throws.
        stopped = true;
        if (helper !== null) { try { helper.shutdown(); } catch (_) {} helper = null; }
        if (hook !== null) { try { hook.implementation = null; } catch (_) { log('google-signin-stop-error'); } hook = null; }
        installed = false;
    }
    return {install, stop};
}});
