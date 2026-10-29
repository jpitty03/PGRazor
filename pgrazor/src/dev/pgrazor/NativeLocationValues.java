package dev.pgrazor;

import android.location.Location;

/** Versioned, compiled DEX call boundary; ART hooks separately cover JNI callers. */
public final class NativeLocationValues {
    public static final int VERSION = 1;
    private NativeLocationValues() {}
    public static double getLatitude(Location value) throws Throwable {
        return NativeLocationGetters.read(0, value).doubleValue();
    }
    public static double getLongitude(Location value) throws Throwable {
        return NativeLocationGetters.read(1, value).doubleValue();
    }
    public static float getSpeed(Location value) throws Throwable {
        return NativeLocationGetters.read(2, value).floatValue();
    }
    public static float getBearing(Location value) throws Throwable {
        return NativeLocationGetters.read(3, value).floatValue();
    }
}
