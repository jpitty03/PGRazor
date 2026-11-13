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

        // --- Go Wild November 2026 Safari toggles -------------------------------
        // Both default off, are independent of each other, and are never capability gated: the
        // native owner validates Safari balls and falls back on its own.
        check(!EventSettings.wildVgpSafari(new Context()), "VGP+ safari defaults off");
        check(!EventSettings.wildAutoBallSafari(new Context()), "auto ball safari defaults off");

        // No capability is ever published for these, yet enabling must still succeed.
        check(EventSettings.setWildVgpSafari(context, true), "VGP+ safari enables without any capability");
        check(EventSettings.wildVgpSafari(context), "VGP+ safari persists");
        check(!EventSettings.wildAutoBallSafari(context), "enabling VGP+ safari leaves auto ball safari off");
        check(EventSettings.wildVgpSafari(new Context()), "VGP+ safari survives context recreation");

        check(EventSettings.setWildAutoBallSafari(context, true), "auto ball safari enables independently");
        check(EventSettings.wildAutoBallSafari(context), "auto ball safari persists");
        check(EventSettings.wildVgpSafari(context), "auto ball safari change does not disturb VGP+ safari");

        check(EventSettings.setWildVgpSafari(context, false), "VGP+ safari disables independently");
        check(!EventSettings.wildVgpSafari(context), "VGP+ safari off after disable");
        check(EventSettings.wildAutoBallSafari(context), "disabling VGP+ safari leaves auto ball safari on");

        // Separate persistence: a Go Wild write must not disturb the Halloween collector intent.
        check(EventSettings.boxCollectorRequested(context), "Halloween collector intent survives Go Wild writes");

        // Rollback: a failed commit reports failure and reverts to the previously stored value.
        Context.failWrites = true;
        check(!EventSettings.setWildAutoBallSafari(context, false), "failed Go Wild save is reported");
        Context.failWrites = false;
        check(EventSettings.wildAutoBallSafari(context), "failed Go Wild save rolls back to the prior value");
        check(!EventSettings.wildVgpSafari(context), "failed Go Wild save does not touch the other toggle");

        check(EventSettings.setWildAutoBallSafari(context, false), "auto ball safari disables after write recovery");
        check(!EventSettings.wildAutoBallSafari(context), "recovered disable is effective");

        System.out.println("PASS EventSettings capability and preference gates");
    }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
}
