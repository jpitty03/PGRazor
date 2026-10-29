package dev.pgrazor;

import android.location.Location;
import com.nianticlabs.nia.contextservice.ContextService;
import com.nianticlabs.nia.location.NianticLocationManager;
import com.nianticlabs.nia.location.NianticLocationProvider;

/**
 * Drives the real {@link GameLocationDelivery} against fake game classes whose names and reflection
 * signatures match the runtime adapter (runtime/location.js). No Android device or game binary is
 * involved; the fakes assert their own invariants (status 3, non-null context, callbackLock held) so a
 * successful delivery proves the production reflection path, and the scenarios below assert the
 * observable behaviour (freshness, suppression vs forwarding, restore, cleanup) without matching any
 * implementation string.
 */
public final class GameLocationDeliveryTest {
    static int checks;

    public static void main(String[] args) {
        installFreshness();
        nativePointerZero();
        disabledAndSatelliteForwarding();
        eligibleSuppressesOriginal();
        pauseResumeStop();
        shutdownIrreversible();
        replacementManager();
        newProviders();
        listenerInstallFailure();
        retainedStaleCallback();
        distinctIdentityEqualProviders();
        reentrantStopDuringSetListener();
        boundedFaultDoesNotDropOriginal();
        System.out.println("PASS: GameLocationDelivery (" + checks + " checks)");
    }

    // --- Scenarios --------------------------------------------------------------------------------

    static void installFreshness() {
        GameLocationDelivery delivery = begin();
        Recorder rec = new Recorder();
        NianticLocationProvider provider = new NianticLocationProvider("fused", rec);
        NianticLocationManager manager = managerWith(provider);
        Control.lat = 1.5; Control.lon = 2.5; Control.spd = 3f; Control.brg = 4f;

        delivery.start(manager);
        tick();
        check(manager.delivered.size() == 1, "initial periodic fix delivered");
        Location first = manager.delivered.get(0);
        check(first.latitude == 1.5 && first.longitude == 2.5, "fix uses current coords");
        check(first.speed == 3f && first.bearing == 4f, "fix uses current speed/bearing");
        check(first.altitude == 10 && first.accuracy == 5, "fix altitude 10 / accuracy 5");
        check(first.elapsedRealtimeNanos == android.os.SystemClock.NANOS, "elapsedRealtimeNanos from SystemClock");
        check(first.time > 0, "fix time from System.currentTimeMillis");
        check(manager.deliveredProvider.get(0).equals("fused"), "periodic fix uses provider name fused");
        check(ContextService.handler.pending.size() == 1, "one timer rescheduled");

        Control.lat = 9.9; Control.spd = 7f;
        tick();
        check(manager.delivered.size() == 2, "periodic fix repeats");
        check(manager.delivered.get(1).latitude == 9.9 && manager.delivered.get(1).speed == 7f, "fresh fix each tick");
        check(manager.delivered.get(0).latitude == 1.5, "earlier fix not mutated");
        check(ContextService.handler.pending.size() == 1, "no duplicate callback timer");
        delivery.shutdown();
    }

    static void nativePointerZero() {
        GameLocationDelivery delivery = begin();
        Recorder rec = new Recorder();
        NianticLocationProvider provider = new NianticLocationProvider("gps", rec);
        NianticLocationManager manager = managerWith(provider);
        manager.nativeClassPointer = 0;

        delivery.start(manager);
        tick();
        check(manager.delivered.isEmpty(), "zero native pointer suppresses periodic delivery");

        NianticLocationProvider.ProviderListener proxy = provider.getListener();
        proxy.onProviderLocation(new Location("gps"));
        check(manager.delivered.isEmpty(), "zero native pointer suppresses callback delivery");
        check(rec.location == 1, "original forwarded when native pointer zero");
        delivery.shutdown();
    }

    static void disabledAndSatelliteForwarding() {
        GameLocationDelivery delivery = begin();
        Recorder rec = new Recorder();
        NianticLocationProvider provider = new NianticLocationProvider("gps", rec);
        NianticLocationManager manager = managerWith(provider);
        delivery.start(manager);
        tick();
        manager.delivered.clear();
        manager.deliveredProvider.clear();
        NianticLocationProvider.ProviderListener proxy = provider.getListener();

        Control.active = false;
        proxy.onProviderLocation(new Location("gps"));
        proxy.onProviderStatus(1);
        check(manager.delivered.isEmpty(), "inactive delivers nothing");
        check(rec.location == 1 && rec.status == 1, "inactive forwards original status/location");

        Control.active = true;
        proxy.onProviderSatellites(5L, 9);
        check(manager.delivered.isEmpty(), "satellites never synthesize a fix");
        check(rec.satellites == 1, "satellites always forward original");
        delivery.shutdown();
    }

    static void eligibleSuppressesOriginal() {
        GameLocationDelivery delivery = begin();
        Recorder rec = new Recorder();
        NianticLocationProvider provider = new NianticLocationProvider("network", rec);
        NianticLocationManager manager = managerWith(provider);
        delivery.start(manager);
        tick();
        manager.delivered.clear();
        manager.deliveredProvider.clear();

        NianticLocationProvider.ProviderListener proxy = provider.getListener();
        check(proxy != rec, "provider wrapped with our proxy");
        proxy.onProviderLocation(new Location("real"));
        check(manager.delivered.size() == 1, "eligible callback delivers synthetic fix");
        check(manager.deliveredProvider.get(0).equals("network"), "synthetic fix uses provider name");
        check(rec.location == 0, "successful synthetic delivery suppresses original");
        delivery.shutdown();
    }

    static void pauseResumeStop() {
        GameLocationDelivery delivery = begin();
        Recorder rec = new Recorder();
        NianticLocationProvider provider = new NianticLocationProvider("gps", rec);
        NianticLocationManager manager = managerWith(provider);
        delivery.start(manager);
        tick();
        check(ContextService.handler.pending.size() == 1, "running leaves a timer");

        NianticLocationManager other = managerWith(new NianticLocationProvider("x", new Recorder()));
        delivery.pause(other);
        check(ContextService.handler.pending.size() == 1, "pause for a non-current manager is ignored");
        delivery.pause(manager);
        check(ContextService.handler.pending.isEmpty(), "pause cancels the timer");

        delivery.resume(manager);
        check(ContextService.handler.pending.size() == 1, "resume schedules exactly once");
        tick();
        check(ContextService.handler.pending.size() == 1, "resume does not duplicate the timer");

        delivery.stop(other);
        check(provider.getListener() != rec, "stop for a non-current manager restores nothing");
        delivery.stop(manager);
        check(provider.getListener() == rec, "stop restores the current manager's provider");
        delivery.shutdown();
    }

    static void shutdownIrreversible() {
        GameLocationDelivery delivery = begin();
        Recorder rec = new Recorder();
        NianticLocationProvider provider = new NianticLocationProvider("gps", rec);
        NianticLocationManager manager = managerWith(provider);
        delivery.start(manager);
        tick();
        delivery.shutdown();
        check(provider.getListener() == rec, "shutdown restores providers");
        ContextService.handler.pending.clear();

        delivery.shutdown(); // idempotent
        delivery.start(manager);
        check(ContextService.handler.pending.isEmpty(), "start after shutdown does nothing");
        manager.delivered.clear();
        manager.deliveredProvider.clear();
        tick();
        check(manager.delivered.isEmpty(), "no delivery after shutdown");
    }

    static void replacementManager() {
        GameLocationDelivery delivery = begin();
        Recorder recA = new Recorder();
        NianticLocationProvider providerA = new NianticLocationProvider("a", recA);
        NianticLocationManager managerA = managerWith(providerA);
        delivery.start(managerA);
        tick();
        int deliveredToA = managerA.delivered.size();

        Recorder recB = new Recorder();
        NianticLocationProvider providerB = new NianticLocationProvider("b", recB);
        NianticLocationManager managerB = managerWith(providerB);
        delivery.start(managerB);
        check(providerA.getListener() == recA, "switching restores the old manager's provider");
        tick();
        check(providerB.getListener() != recB, "switching wraps the new manager's provider");
        check(managerB.delivered.size() >= 1, "new manager receives synthetic fixes");
        check(managerA.delivered.size() == deliveredToA, "old manager no longer receives fixes");
        delivery.shutdown();
    }

    static void newProviders() {
        GameLocationDelivery delivery = begin();
        Recorder rec1 = new Recorder();
        NianticLocationProvider provider1 = new NianticLocationProvider("p1", rec1);
        NianticLocationManager manager = managerWith(provider1);
        delivery.start(manager);
        tick();
        check(provider1.setCount == 1, "first provider wrapped once");

        Recorder rec2 = new Recorder();
        NianticLocationProvider provider2 = new NianticLocationProvider("p2", rec2);
        manager.providers.put("p2", provider2);
        tick();
        check(provider1.setCount == 1, "existing provider not re-wrapped");
        check(provider2.getListener() != rec2, "newly appearing provider wrapped");
        delivery.shutdown();
    }

    static void listenerInstallFailure() {
        GameLocationDelivery delivery = begin();
        Recorder rec = new Recorder();
        NianticLocationProvider provider = new NianticLocationProvider("gps", rec);
        provider.failSetListener = true;
        NianticLocationManager manager = managerWith(provider);
        delivery.start(manager);
        tick();
        check(provider.getListener() == rec, "failed install leaves the original listener");
        check(manager.delivered.isEmpty(), "failed install delivers nothing");
        check(ContextService.handler.pending.size() == 1, "failed install still reschedules the timer");
        delivery.shutdown();
    }

    static void retainedStaleCallback() {
        GameLocationDelivery delivery = begin();
        Recorder rec = new Recorder();
        NianticLocationProvider provider = new NianticLocationProvider("gps", rec);
        NianticLocationManager manager = managerWith(provider);
        delivery.start(manager);
        tick();
        NianticLocationProvider.ProviderListener stale = provider.getListener();
        delivery.stop(manager);
        manager.delivered.clear();
        manager.deliveredProvider.clear();

        stale.onProviderLocation(new Location("gps"));
        check(manager.delivered.isEmpty(), "stale proxy delivers nothing after stop");
        check(rec.location == 1, "stale proxy forwards the original callback");
        delivery.shutdown();
    }

    static void distinctIdentityEqualProviders() {
        GameLocationDelivery delivery = begin();
        Recorder rec1 = new Recorder();
        Recorder rec2 = new Recorder();
        NianticLocationProvider provider1 = new NianticLocationProvider("dup", rec1);
        NianticLocationProvider provider2 = new NianticLocationProvider("dup", rec2);
        check(provider1.equals(provider2) && provider1 != provider2, "providers equal but distinct");
        NianticLocationManager manager = new NianticLocationManager();
        manager.providers.put("a", provider1);
        manager.providers.put("b", provider2);
        delivery.start(manager);
        tick();
        check(provider1.getListener() != rec1 && provider2.getListener() != rec2, "both equal providers wrapped");
        check(provider1.setCount == 1 && provider2.setCount == 1, "each wrapped exactly once");
        delivery.shutdown();
    }

    static void reentrantStopDuringSetListener() {
        GameLocationDelivery delivery = begin();
        Recorder rec = new Recorder();
        NianticLocationProvider provider = new NianticLocationProvider("gps", rec);
        NianticLocationManager manager = managerWith(provider);
        provider.onSetListener = () -> delivery.stop(manager);
        delivery.start(manager);
        tick();
        check(provider.getListener() == rec, "reentrant stop restores the provider");
        check(manager.delivered.isEmpty(), "reentrant stop delivers nothing");
        check(ContextService.handler.pending.isEmpty(), "reentrant stop publishes no stale timer");
        delivery.shutdown();
    }

    static void boundedFaultDoesNotDropOriginal() {
        GameLocationDelivery delivery = begin();
        Recorder rec = new Recorder();
        NianticLocationProvider provider = new NianticLocationProvider("gps", rec);
        NianticLocationManager manager = managerWith(provider);
        manager.failNative = true;
        delivery.start(manager);
        tick(); // first delivery fault (logged once)
        int events = Control.events.size();
        NianticLocationProvider.ProviderListener proxy = provider.getListener();
        proxy.onProviderLocation(new Location("gps"));
        proxy.onProviderLocation(new Location("gps"));
        proxy.onProviderLocation(new Location("gps"));
        check(rec.location == 3, "delivery fault forwards the original callback");
        check(Control.events.size() == events, "fault logging is bounded");
        delivery.shutdown();
    }

    // --- Helpers ----------------------------------------------------------------------------------

    static GameLocationDelivery begin() {
        ContextService.handler.pending.clear();
        Control.active = true;
        Control.lat = 1.0; Control.lon = 1.0; Control.spd = 0f; Control.brg = 0f;
        Control.events.clear();
        GameLocationDelivery delivery = GameLocationDelivery.create();
        Control.events.clear();
        return delivery;
    }

    static NianticLocationManager managerWith(NianticLocationProvider provider) {
        NianticLocationManager manager = new NianticLocationManager();
        manager.providers.put(provider.getProviderName(), provider);
        return manager;
    }

    static void tick() {
        ContextService.handler.runNext();
    }

    static void check(boolean condition, String what) {
        checks++;
        if (!condition) throw new AssertionError("FAIL: " + what);
    }

    static final class Recorder implements NianticLocationProvider.ProviderListener {
        int status, location, satellites;
        public void onProviderStatus(int value) { status++; }
        public void onProviderLocation(Location fix) { location++; }
        public void onProviderSatellites(long time, int count) { satellites++; }
    }
}
