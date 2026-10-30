package dev.pgrazor.events;

import android.content.Context;

public final class EventSettingsTest {
    public static void main(String[] args) {
        Context context = new Context();
        check(!EventSettings.boxCollectorRequested(context), "default preference off");
        check(!EventSettings.boxCollectorAvailable(), "capability defaults unavailable");
        check(!EventSettings.setBoxCollectorRequested(context, true), "cannot enable unverified collector");
        check(!EventSettings.boxCollectorRequested(context), "refusal does not save intent");

        EventSettings.publishBoxCollectorCapability(true);
        check(EventSettings.setBoxCollectorRequested(context, true), "verified enable persists");
        check(EventSettings.boxCollectorEnabled(context), "effective enabled");
        check(EventSettings.boxCollectorRequested(new Context()), "preference survives context recreation");

        EventSettings.publishBoxCollectorCapability(false);
        check(!EventSettings.boxCollectorEnabled(context), "capability revocation immediately gates runtime");
        check(EventSettings.boxCollectorRequested(context), "saved intent retained separately");
        check(EventSettings.setBoxCollectorRequested(context, false), "can disable while unavailable");
        EventSettings.publishBoxCollectorCapability(true);
        check(!EventSettings.boxCollectorEnabled(context), "revived capability does not override disabled intent");

        Context.failWrites = true;
        check(!EventSettings.setBoxCollectorRequested(context, true), "persistence failure is reported");
        check(!EventSettings.boxCollectorEnabled(context), "failed save does not enable");
        Context.failWrites = false;

        check(EventSettings.setBoxCollectorRequested(context, true), "enable persists after write recovery");
        check(EventSettings.boxCollectorEnabled(context), "recovered enable is effective");

        System.out.println("PASS EventSettings capability and preference gates");
    }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
}
