package dev.pgrazor;

/** Coordinate integration independent of Android and of any game. */
public final class MotionEngine {
    /** Imported GPX routes may contain many more points than nearby POI plans. */
    public static final int MAX_ROUTE_STOPS = 5000;
    private static final double EARTH_RADIUS_METERS = 6371008.8;
    private static final double SAME_POINT_RADIANS = 1e-14;
    public volatile double latitude = 37.7749, longitude = -122.4194;
    public volatile double speedKmh = 5.0, bearing;
    public volatile boolean enabled;
    public volatile boolean walking;
    public volatile double targetLatitude, targetLongitude;
    private volatile double east, north;
    private double[][] route = new double[0][];
    private double[] remainingLegMeters = new double[0];
    private int visitedStops;
    /** When set, the chosen route repeats from its first stop after the last; cleared by any cancellation. */
    private boolean looping;
    private boolean plannedRoute;
    private long routeGeneration;
    /** Nonzero only for an active planned route, including a one-stop GPX/Autowalk. */
    public synchronized long activeRouteToken() {return walking&&plannedRoute?routeGeneration:0;}
    /** Retains ownership after arrival; changes on cancellation or replacement. */
    public synchronized long routeGeneration() { return routeGeneration; }
    public synchronized boolean stopRoute(long token) {
        if(token==0||token!=activeRouteToken())return false;
        stop();return true;
    }

    /** One coherent progress snapshot, without exposing the mutable route. */
    public static final class RouteProgress {
        public final int total, visited;
        public final boolean active;
        public final double remainingMeters;
        RouteProgress(int total, int visited, boolean active, double remainingMeters) {
            this.total=total; this.visited=visited; this.active=active; this.remainingMeters=remainingMeters;
        }
    }
    public synchronized RouteProgress routeProgress() {
        // Only the current leg changes each tick; GPX routes can have thousands of points.
        double remaining=0;
        if(visitedStops<route.length)remaining=remainingLegMeters[visitedStops]
            +angle(unit(latitude,longitude),unit(route[visitedStops][0],route[visitedStops][1]))*EARTH_RADIUS_METERS;
        return new RouteProgress(route.length,visitedStops,walking,remaining);
    }
    private static void validatePosition(double lat, double lon) {
        if (!Double.isFinite(lat) || !Double.isFinite(lon) || Math.abs(lat) > 90 || Math.abs(lon) > 180)
            throw new IllegalArgumentException("Latitude must be -90 to 90; longitude -180 to 180.");
    }
    public synchronized void position(double lat, double lon) {
        validatePosition(lat, lon);
        latitude = lat; longitude = lon;
    }
    public synchronized void speed(double value) {
        // Exactly 0 pauses travel in place; otherwise the usual 0.5..30 km/h range applies.
        if (!Double.isFinite(value) || (value != 0 && (value < 0.5 || value > 30)))
            throw new IllegalArgumentException("Speed must be 0 (paused) or between 0.5 and 30 km/h.");
        speedKmh = value;
    }
    /** Explicitly start a shortest-path walk; callers must check adapter readiness. */
    public synchronized void walkTo(double lat, double lon) {
        walkRoute(new double[][]{{lat,lon}});plannedRoute=false;
    }
    /** Validate/copy the entire route before replacing any current movement; a one-shot route. */
    public synchronized void walkRoute(double[][] waypoints) { walkRoute(waypoints, false); }
    /** As {@link #walkRoute(double[][])}, but a looping route repeats from its first stop after the last. */
    public synchronized void walkRoute(double[][] waypoints, boolean loop) {
        if (waypoints==null || waypoints.length==0 || waypoints.length>MAX_ROUTE_STOPS)
            throw new IllegalArgumentException("Choose between 1 and " + MAX_ROUTE_STOPS + " route stops.");
        double[][] copy=new double[waypoints.length][2];
        for (int i=0; i<waypoints.length; i++) {
            if (waypoints[i]==null || waypoints[i].length!=2)
                throw new IllegalArgumentException("Each route stop needs latitude and longitude.");
            double lat=waypoints[i][0], lon=waypoints[i][1];
            validatePosition(lat,lon); copy[i][0]=lat; copy[i][1]=lon;
        }
        // Mutation only after full validation, so an invalid route leaves any active walk untouched.
        double[] legs=new double[copy.length];
        for(int i=copy.length-2;i>=0;i--)legs[i]=legs[i+1]
            +angle(unit(copy[i][0],copy[i][1]),unit(copy[i+1][0],copy[i+1][1]))*EARTH_RADIUS_METERS;
        route=copy; remainingLegMeters=legs; visitedStops=0; looping=loop;plannedRoute=true;routeGeneration++;
        targetLatitude=route[0][0]; targetLongitude=route[0][1];
        east = north = 0;
        enabled = true;
        walking = true;
        drainCoincident();
    }
    /** Settle stops already at the current position; bounded to one lap so a fully coincident loop can't spin. */
    private void drainCoincident() {
        for (int guard=route.length; guard>0 && walking
                && angle(unit(latitude,longitude),unit(targetLatitude,targetLongitude))<=SAME_POINT_RADIANS; guard--)
            arrive();
    }
    public synchronized void input(double x, double y) {
        clearRoute();
        if (!Double.isFinite(x) || !Double.isFinite(y)) { east = north = 0; return; }
        double length = Math.max(1, Math.hypot(x, y));
        east = x / length; north = y / length;
        if (Math.hypot(east, north) > 0.05)
            bearing = (Math.toDegrees(Math.atan2(east, north)) + 360) % 360;
    }
    private void clearRoute() { plannedRoute=false;routeGeneration++;walking=false; route=new double[0][]; remainingLegMeters=new double[0]; visitedStops=0; looping=false; }
    public synchronized void stop() { clearRoute(); east = north = 0; }
    /** Camera invalidation stops manual velocity without cancelling a walk or its arrival ownership. */
    public synchronized void stopManualMotion() { east = north = 0; }
    public synchronized void pause() { enabled = false; stop(); }
    public synchronized double metersPerSecond() {
        return enabled ? speedKmh / 3.6 * (walking ? 1 : Math.hypot(east, north)) : 0;
    }
    public synchronized void step(double seconds) {
        if (!enabled || !Double.isFinite(seconds) || seconds <= 0) return;
        // Avoid jumps after a stalled UI frame or suspended app.
        double distance = metersPerSecond() * Math.min(seconds, 0.25) / EARTH_RADIUS_METERS;
        if (walking) {
            // Carry this frame's unused distance across stops, never a new budget
            // per leg. The bounded route also bounds duplicate-point processing. A
            // looping route repeats within the same distance budget; zero-distance
            // (coincident) arrivals are capped to one lap. A hard iteration limit also
            // bounds tiny nonzero loops; unused travel is dropped rather than freezing a frame.
            int coincidentArrivals=0,iterations=0;
            while (walking && iterations++<200 && (distance>0 || angle(unit(latitude,longitude),unit(targetLatitude,targetLongitude))<=SAME_POINT_RADIANS)) {
                double before=distance;
                distance=stepWalk(distance);
                if (before-distance>SAME_POINT_RADIANS) coincidentArrivals=0;
                else if (++coincidentArrivals>route.length) break;
            }
            return;
        }
        if (distance == 0) return;
        double lat = Math.toRadians(latitude), lon = Math.toRadians(longitude), direction = Math.toRadians(bearing);
        double nextLat = Math.asin(Math.max(-1, Math.min(1,
            Math.sin(lat) * Math.cos(distance) + Math.cos(lat) * Math.sin(distance) * Math.cos(direction))));
        double nextLon = lon + Math.atan2(Math.sin(direction) * Math.sin(distance) * Math.cos(lat),
            Math.cos(distance) - Math.sin(lat) * Math.sin(nextLat));
        latitude = Math.toDegrees(nextLat);
        longitude = ((Math.toDegrees(nextLon) + 540) % 360) - 180;
    }

    private void arrive() {
        latitude = targetLatitude; longitude = targetLongitude;
        visitedStops++;
        if (visitedStops>=route.length) visitedStops = looping ? 0 : route.length;
        walking = looping ? route.length>0 : visitedStops<route.length;
        if (walking) { targetLatitude=route[visitedStops][0]; targetLongitude=route[visitedStops][1]; }
        east = north = 0;
    }

    private double stepWalk(double distance) {
        double[] here = unit(latitude, longitude), target = unit(targetLatitude, targetLongitude);
        double remaining = angle(here, target);
        if (remaining <= SAME_POINT_RADIANS) { arrive(); return Math.max(0,distance-remaining); }
        if (distance <= 0) return 0;
        double dot = dot(here, target);
        double[] tangent = {target[0]-dot*here[0], target[1]-dot*here[1], target[2]-dot*here[2]};
        double length = norm(tangent);
        if (dot < 0 && length < 1e-12) {
            // Antipodes have multiple shortest paths. Select one deterministically
            // without dividing by a vanishing cross product, including at poles.
            double[] axis = Math.abs(here[2]) < 0.9 ? new double[]{0,0,1} : new double[]{1,0,0};
            double projection = dot(here, axis);
            tangent = new double[]{axis[0]-projection*here[0], axis[1]-projection*here[1], axis[2]-projection*here[2]};
            length = norm(tangent);
        }
        for (int i=0; i<3; i++) tangent[i] /= length;
        double lat = Math.toRadians(latitude), lon = Math.toRadians(longitude);
        double headingEast = -Math.sin(lon)*tangent[0] + Math.cos(lon)*tangent[1];
        double headingNorth = -Math.sin(lat)*Math.cos(lon)*tangent[0] - Math.sin(lat)*Math.sin(lon)*tangent[1] + Math.cos(lat)*tangent[2];
        bearing = (Math.toDegrees(Math.atan2(headingEast, headingNorth))+360)%360;
        if (remaining <= distance) { arrive(); return Math.max(0,distance-remaining); }
        double cosine = Math.cos(distance), sine = Math.sin(distance);
        double x = here[0]*cosine+tangent[0]*sine;
        double y = here[1]*cosine+tangent[1]*sine;
        double z = here[2]*cosine+tangent[2]*sine;
        latitude = Math.toDegrees(Math.atan2(z, Math.hypot(x,y)));
        longitude = ((Math.toDegrees(Math.atan2(y,x))+540)%360)-180;
        return 0;
    }

    private static double[] unit(double latitude, double longitude) {
        double lat = Math.toRadians(latitude), lon = Math.toRadians(longitude), cosine = Math.cos(lat);
        return new double[]{cosine*Math.cos(lon), cosine*Math.sin(lon), Math.sin(lat)};
    }
    private static double dot(double[] a, double[] b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
    private static double norm(double[] v) { return Math.hypot(Math.hypot(v[0],v[1]),v[2]); }
    private static double angle(double[] a, double[] b) {
        double[] cross = {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]};
        return Math.atan2(norm(cross), dot(a,b));
    }
}
