package dev.pgrazor;

/** Monotonic heading freshness and gesture invalidation, independent of Android. */
public final class CameraHeadingState {
    /** The game can read its camera only about twice a second, so allow a few missed readings. */
    public static final long MAX_AGE_MILLIS = 1500;
    private boolean valid, hasCamera;
    private int cameraId;
    private double heading;
    private long sampledAt, generation;

    public static final class Sample {
        public final boolean valid;
        public final double heading;
        public final long ageMillis, generation;
        private Sample(boolean valid, double heading, long ageMillis, long generation) {
            this.valid=valid; this.heading=heading; this.ageMillis=ageMillis; this.generation=generation;
        }
    }
    public synchronized void invalidate() { valid=false; generation++; }
    /** sampledAt and now share Android CLOCK_BOOTTIME; delivery delay is never renewed away. */
    public synchronized boolean updateAt(double degrees, int camera, boolean mapVisible, long sampledAtMillis, long now) {
        long age=sampledAtMillis>=0 && sampledAtMillis<=now ? now-sampledAtMillis : -1;
        return update(degrees,camera,mapVisible,age,now);
    }
    public synchronized boolean update(double degrees, int camera, boolean mapVisible, long ageMillis, long now) {
        sample(now); // A fresh update must not bridge an expired held gesture.
        if (!mapVisible || !Double.isFinite(degrees) || ageMillis<0 || ageMillis>MAX_AGE_MILLIS
                || now<0 || ageMillis>now) { invalidate(); return false; }
        long timestamp=now-ageMillis;
        if (valid && timestamp<sampledAt) { invalidate(); return false; }
        if (hasCamera && cameraId!=camera) invalidate();
        cameraId=camera; hasCamera=true; sampledAt=timestamp;
        heading=((degrees%360)+360)%360; valid=true;
        return true;
    }
    public synchronized Sample sample(long now) {
        long age=now>=sampledAt ? now-sampledAt : -1;
        if (valid && (age<0 || age>MAX_AGE_MILLIS)) invalidate();
        return new Sample(valid,heading,age,generation);
    }
    /** A negative token cannot become valid when a heading later arrives. */
    public synchronized long beginGesture(long now) {
        Sample sample=sample(now); return sample.valid ? sample.generation : -1;
    }
    public synchronized CameraSteering.Direction direction(long gesture, double right, double up, long now) {
        Sample sample=sample(now);
        return CameraSteering.rotate(right,up,sample.heading,
            sample.valid && gesture>=0 && gesture==sample.generation ? sample.ageMillis : -1, MAX_AGE_MILLIS);
    }
}
