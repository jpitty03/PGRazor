package dev.pgrazor;

public final class RouteStartTest {
    private static void require(boolean ok, String what) { if (!ok) throw new AssertionError(what); }
    private static void refused(Runnable r, String what) {
        try { r.run(); } catch (IllegalArgumentException expected) { return; }
        throw new AssertionError(what);
    }

    public static void main(String[] args) {
        // Roughly 111 m between stops, going north along a meridian.
        double[][] route = {{37.0, -122.0}, {37.001, -122.0}, {37.002, -122.0}, {37.003, -122.0}};
        double m = RouteStart.meters(37.0, -122.0, 37.001, -122.0);
        require(Math.abs(m - 111.2) < 0.5, "haversine meters: " + m);
        require(RouteStart.meters(37.0, -122.0, 37.0, -122.0) == 0, "same point");

        require(RouteStart.closest(route, 37.0021, -122.0001) == 2, "closest stop");
        require(RouteStart.closest(route, 36.9, -122.0) == 0, "before the start");
        require(RouteStart.closest(route, 37.0005, -122.0) == 0, "tie goes to the earlier stop");
        refused(() -> RouteStart.closest(new double[0][], 37, -122), "empty route refused");

        double[][] rest = RouteStart.from(route, 2, false);
        require(rest.length == 2 && rest[0][0] == 37.002 && rest[1][0] == 37.003, "one-way route continues from the stop");
        double[][] last = RouteStart.from(route, 3, false);
        require(last.length == 1 && last[0][0] == 37.003, "last stop alone");
        double[][] lap = RouteStart.from(route, 2, true);
        require(lap.length == 4 && lap[0][0] == 37.002 && lap[1][0] == 37.003 && lap[2][0] == 37.0 && lap[3][0] == 37.001,
            "loop keeps every stop, starting at the chosen one");
        lap[0][0] = 0;
        require(route[2][0] == 37.002, "result is a copy");
        require(RouteStart.from(route, 0, false).length == 4, "from the beginning");
        refused(() -> RouteStart.from(route, 4, false), "out of range refused");
        refused(() -> RouteStart.from(route, -1, true), "negative refused");

        require(RouteStart.distance(85.4).equals("85 m") && RouteStart.distance(1234).equals("1.2 km"), "distance text");
        System.out.println("RouteStartTest passed");
    }
}
