import dev.pgrazor.MotionEngine;
public class MotionTest {
    static void check(boolean ok,String message) { if(!ok) throw new AssertionError(message); }
    static void near(double actual,double expected,double tolerance,String message) {
        check(Double.isFinite(actual)&&Math.abs(actual-expected)<=tolerance,message+": "+actual+" expected "+expected);
    }
    // Independent haversine measurement, rather than the engine's vector rotation.
    static double meters(double lat1,double lon1,double lat2,double lon2) {
        double a=Math.sin(Math.toRadians(lat2-lat1)/2), b=Math.sin(Math.toRadians(lon2-lon1)/2);
        double h=a*a+Math.cos(Math.toRadians(lat1))*Math.cos(Math.toRadians(lat2))*b*b;
        return 2*6371008.8*Math.asin(Math.sqrt(Math.min(1,Math.max(0,h))));
    }
    static MotionEngine at(double latitude,double longitude) {
        MotionEngine e=new MotionEngine(); e.position(latitude,longitude); e.speed(3.6); return e;
    }
    static void finish(MotionEngine e,int limit) {
        for(int i=0;i<limit&&e.walking;i++)e.step(.25);
        check(!e.walking,"Walk must arrive within expected time");
        check(e.latitude==e.targetLatitude&&e.longitude==e.targetLongitude,"Arrival uses exact target coordinates");
        check(e.enabled&&e.metersPerSecond()==0,"Arrival holds override without movement");
    }
    static void testWalking() {
        MotionEngine e=at(0,0); e.walkTo(.0001,0);
        check(e.enabled&&e.walking,"Explicit walk enables override");
        near(e.metersPerSecond(),1,1e-10,"Route uses configured speed");
        for(int i=0;i<40;i++)e.step(.25);
        near(meters(0,0,e.latitude,e.longitude),10,1e-5,"Route distance after 10 seconds");
        check(e.walking,"Does not arrive early"); finish(e,5);
        double lat=e.latitude,lon=e.longitude; e.step(.25);
        check(lat==e.latitude&&lon==e.longitude,"Arrival stays still");

        e=at(0,0);e.walkTo(0,.0000001); e.step(.25); finish(e,0);
        e=at(0,0);e.walkTo(0,1);e.step(600);
        near(meters(0,0,e.latitude,e.longitude),.25,1e-5,"Route clamps suspended interval");
        lat=e.latitude;lon=e.longitude;e.step(Double.NaN);e.step(-1);e.step(0);e.step(Double.POSITIVE_INFINITY);
        check(lat==e.latitude&&lon==e.longitude,"Invalid time never moves route");
        e.speed(7.2);e.step(.25);
        near(meters(lat,lon,e.latitude,e.longitude),.5,1e-5,"Speed change applies during route");

        e=at(1,2);e.walkTo(1,2);finish(e,0);
        e=at(0,180);e.walkTo(0,-180);finish(e,0);
        e=at(90,0);e.walkTo(90,120);finish(e,0);

        e=at(0,179.99999); e.walkTo(0,-179.99999); e.step(.25);
        check(e.longitude>179.99999,"Dateline route initially travels east");
        near(e.bearing,90,1e-8,"Dateline takes short eastward path"); finish(e,9);
        e=at(90,0);e.walkTo(89.9999,120); e.step(.25);
        check(e.latitude<90&&Double.isFinite(e.longitude),"Walk from pole remains finite");
        near(e.longitude,120,1e-5,"Walk from pole follows target meridian");finish(e,45);
        e=at(89.9999,0);e.walkTo(89.9999,180); e.step(.25);
        check(e.latitude>89.9999,"Shortest polar route starts north"); finish(e,90);

        e=at(0,0);e.walkTo(0,180);e.step(.25);
        check(e.latitude>0&&Double.isFinite(e.longitude),"Antipodal path is deterministic and finite");
        near(meters(0,0,e.latitude,e.longitude),.25,1e-5,"Antipodal step respects speed");
        e=at(90,0);e.walkTo(-90,0);e.step(.25);
        check(e.latitude<90&&Double.isFinite(e.longitude),"Antipodal poles remain finite");
        near(meters(90,0,e.latitude,e.longitude),.25,1e-5,"Antipodal pole step respects speed");
        e=at(0,0);e.walkTo(.0000001,179.9999999);e.step(.25);
        near(meters(0,0,e.latitude,e.longitude),.25,1e-5,"Near-antipodal step respects speed");
    }
    static void testInterruption() {
        MotionEngine manual=at(0,0); manual.enabled=true; manual.input(1,0); manual.step(.25);
        long manualGeneration=manual.routeGeneration(); double manualLon=manual.longitude;
        manual.stopManualMotion(); manual.step(.25);
        check(manual.metersPerSecond()==0 && manual.longitude==manualLon && manual.routeGeneration()==manualGeneration,
            "Camera stop clears manual velocity without fabricating a takeover");
        MotionEngine e=at(0,0);e.walkTo(1,0);e.step(.25);e.pause();
        double lat=e.latitude,lon=e.longitude;e.step(.25);
        check(!e.walking&&!e.enabled&&lat==e.latitude&&lon==e.longitude,"Pause cancels route and movement");
        e.enabled=true;e.step(.25);
        check(lat==e.latitude&&lon==e.longitude,"Re-enabling does not resume canceled route");
        e.walkTo(1,0);e.stop();e.step(.25);
        check(!e.walking&&e.enabled&&e.metersPerSecond()==0&&lat==e.latitude,"Stop cancels and holds position");
        e.walkTo(1,0);e.input(1,0);e.step(.25);
        check(!e.walking&&e.longitude>lon,"Manual east input takes over northward route");
        near(e.bearing,90,1e-10,"Manual heading replaces route heading");
        e.walkTo(1,0);e.input(0,0);check(!e.walking&&e.metersPerSecond()==0,"Manual release cancels route");
        e.walkTo(1,0);e.input(Double.NaN,1);check(!e.walking&&e.metersPerSecond()==0,"Invalid input stops route safely");
        e.walkTo(1,0);e.enabled=false;lat=e.latitude;lon=e.longitude;e.step(.25);
        check(lat==e.latitude&&lon==e.longitude&&e.metersPerSecond()==0,"Disabled route never moves");
    }
    static void testRouteValidation() {
        MotionEngine e=at(1,2);e.walkTo(3,4);e.step(.25);
        double lat=e.latitude,lon=e.longitude,bearing=e.bearing;
        double[][] invalid={{Double.NaN,0},{0,Double.NaN},{Double.POSITIVE_INFINITY,0},{0,Double.NEGATIVE_INFINITY},{90.01,0},{-90.01,0},{0,180.01},{0,-180.01}};
        for(double[] point:invalid) {
            boolean rejected=false;try{e.walkTo(point[0],point[1]);}catch(IllegalArgumentException expected){rejected=true;}
            check(rejected,"Invalid route rejected");
            check(e.walking&&e.enabled&&e.targetLatitude==3&&e.targetLongitude==4&&e.latitude==lat&&e.longitude==lon&&e.bearing==bearing&&e.speedKmh==3.6,"Invalid route leaves active state unchanged");
        }
        e.pause();boolean rejected=false;try{e.walkTo(91,0);}catch(IllegalArgumentException expected){rejected=true;}
        check(rejected&&!e.enabled&&!e.walking,"Invalid route cannot enable a paused override");
    }
    static void testMultipleStops() {
        double tenth=Math.toDegrees(.1/6371008.8);
        MotionEngine e=at(0,0);
        double[][] points={{tenth,0},{tenth,.00001},{.00001,.00001}};
        e.walkRoute(points);
        MotionEngine.RouteProgress initial=e.routeProgress();
        near(initial.remainingMeters,meters(0,0,tenth,0)+meters(tenth,0,tenth,.00001)+meters(tenth,.00001,.00001,.00001),1e-7,"Remaining distance follows legs, not direct finish");
        check(initial.total==3&&initial.visited==0&&initial.active,"Route begins at first stop");
        points[0][0]=70;points[1]=new double[]{80,80};
        e.step(.25);
        check(e.routeProgress().visited==1&&e.walking,"Reaches first stop then continues");
        near(e.latitude,tenth,1e-10,"Turns east at the first stop");
        near(meters(tenth,0,e.latitude,e.longitude),.15,1e-6,"Only unused frame distance is spent after the turn");
        near(e.routeProgress().remainingMeters,initial.remainingMeters-.25,1e-6,"Progress decreases by frame budget");
        finish(e,20);
        check(e.latitude==.00001&&e.longitude==.00001,"Caller mutations cannot rewrite queued destinations");
        check(e.routeProgress().visited==3&&e.routeProgress().remainingMeters==0,"Completion counts every stop");

        e=at(0,0);e.walkRoute(new double[][]{{0,0},{tenth,0},{tenth,0},{tenth,tenth}});e.step(.25);
        check(!e.walking&&e.routeProgress().visited==4,"Duplicates and multiple short legs finish in one bounded frame");
        near(e.longitude,tenth,0,"Exact last-stop arrival");
        near(e.bearing,90,1e-7,"Short final leg updates bearing even when completed within the frame");
        e=at(0,0);double quarter=Math.toDegrees(.25/6371008.8);
        e.walkRoute(new double[][]{{quarter,0},{quarter,0},{quarter,0}});e.step(.25);
        check(!e.walking&&e.routeProgress().visited==3,"Duplicate final stops settle in the same frame even with no distance budget left");
        double[][] duplicate=new double[50][2];e=at(0,0);e.walkRoute(duplicate);
        check(!e.walking&&e.routeProgress().visited==50&&e.enabled,"All duplicate stops finish immediately without looping forever");

        e=at(0,179.999999);e.walkRoute(new double[][]{{0,-179.999999},{.00001,-179.999999}});e.step(.25);
        check(e.routeProgress().visited==1&&e.latitude>0&&e.longitude==-179.999999,"Crosses dateline then turns north in same frame");
        e=at(0,0);e.walkRoute(new double[][]{{0,.00001},{.00001,.00001}});e.step(600);
        near(meters(0,0,e.latitude,e.longitude),.25,1e-6,"Multi-stop routes clamp long suspended intervals");
        e.pause();e.enabled=true;e.step(.25);
        check(e.routeProgress().total==0&&!e.walking&&e.metersPerSecond()==0,"Pause cancels queue without resume on enable");
        e.walkRoute(new double[][]{{1,0},{1,1}});e.input(1,0);
        check(e.routeProgress().total==0&&!e.walking,"Manual input cancels the entire queue");
        e.walkRoute(new double[][]{{1,0},{1,1}});e.stop();
        check(e.routeProgress().total==0&&!e.walking,"Stop clears queue");
        e.walkRoute(new double[][]{{1,0},{1,1}});e.walkTo(2,2);
        check(e.routeProgress().total==1&&e.targetLatitude==2,"Single Walk here replaces queue");
        double[][][] invalid={null,new double[0][],new double[MotionEngine.MAX_ROUTE_STOPS+1][2],{{1,1},null},{{1,1},{2}},{{1,1},{2,3,4}},{{1,1},{Double.NaN,0}},{{1,1},{91,0}}};
        for(double[][] route:invalid) {
            boolean rejected=false;try {e.walkRoute(route);}catch(IllegalArgumentException expected){rejected=true;}
            check(rejected&&e.walking&&e.targetLatitude==2&&e.routeProgress().total==1,"Invalid later stop cannot partially replace existing route");
        }
        e.pause();boolean rejected=false;try{e.walkRoute(new double[][]{{0,0},{0,181}});}catch(IllegalArgumentException expected){rejected=true;}
        check(rejected&&!e.enabled&&e.routeProgress().total==0,"Invalid route cannot enable override");
    }
    public static void main(String[] args) {
        MotionEngine e=new MotionEngine(); e.position(0,0); e.speed(3.6); e.enabled=true; e.input(0,1);
        for(int i=0;i<40;i++)e.step(.25);
        check(Math.abs(e.latitude-0.000089932)<1e-8,"10m north"); check(e.longitude==0,"North leaves longitude unchanged");
        e.stop(); double before=e.latitude; e.step(.25); check(e.latitude==before,"Release stops movement");
        e.position(0,179.999999); e.input(1,0); e.step(.25); check(e.longitude<0,"Dateline wraps");
        e.position(90,0); e.step(.25); check(Double.isFinite(e.latitude)&&Double.isFinite(e.longitude),"Pole remains finite");
        e.position(0,0); e.input(1,1); check(Math.abs(e.metersPerSecond()-1)<1e-8,"Diagonal speed capped");
        e.pause(); e.step(.25); check(e.latitude==0&&e.longitude==0,"Pause stops");
        boolean invalid=false; try{e.position(Double.NaN,0);}catch(IllegalArgumentException expected){invalid=true;} check(invalid,"Reject NaN");
        invalid=false; try{e.speed(-1);}catch(IllegalArgumentException expected){invalid=true;} check(invalid,"Reject negative speed");
        e.enabled=true; e.input(0,1); e.step(600); check(e.latitude<.000003,"No background time jump");
        testWalking();testInterruption();testRouteValidation();testMultipleStops();
        MotionEngine guard=new MotionEngine();guard.position(0,0);
        guard.walkTo(1,1);check(guard.activeRouteToken()==0,"Ordinary tap walk is not a planned route");
        guard.walkRoute(new double[][]{{1,1}},true);long token=guard.activeRouteToken();
        check(token!=0,"One-stop Autowalk is protected");guard.step(.25);
        check(guard.activeRouteToken()==token&&guard.walking,"Prompt token leaves movement running");
        guard.walkRoute(new double[][]{{2,2},{3,3}});
        check(!guard.stopRoute(token)&&guard.walking,"Stale dialog cannot stop a replacement route");
        check(guard.stopRoute(guard.activeRouteToken())&&!guard.walking,"Confirmation stops current route");

        MotionEngine imported=new MotionEngine();imported.position(0,0);
        double[][] longRoute=new double[300][2];
        for(int i=0;i<longRoute.length;i++) {longRoute[i][0]=1;longRoute[i][1]=i/1000.0;}
        imported.walkRoute(longRoute,true);
        check(imported.routeProgress().total==300 && imported.targetLatitude==1,"GPX route above nearby limit keeps all points");
        longRoute[0][0]=70;
        check(imported.targetLatitude==1,"Imported route is copied");
        imported.stop();check(imported.routeProgress().total==0,"Long route cancels normally");
        System.out.println("PASS: joystick and walk regressions; multi-stop frame budget, turns, duplicates, ownership, progress, cancellation, dateline and atomic validation");
    }
}
