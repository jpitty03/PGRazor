import dev.pgrazor.MotionEngine;

public final class IdleWalkTest {
    static void check(boolean ok,String why){if(!ok)throw new AssertionError(why);}
    static double distance(double a,double b,double c,double d){
        double x=Math.toRadians(c-a),y=Math.toRadians(d-b);
        double h=Math.sin(x/2)*Math.sin(x/2)+Math.cos(Math.toRadians(a))*Math.cos(Math.toRadians(c))*Math.sin(y/2)*Math.sin(y/2);
        return 6371008.8*2*Math.asin(Math.sqrt(Math.min(1,Math.max(0,h))));
    }
    static MotionEngine engine(double lat,double lon){
        MotionEngine e=new MotionEngine();e.position(lat,lon);e.enabled=true;e.speed(5);
        e.setIdleWalking(true);e.setIdleWalkingAllowed(true);return e;
    }
    static void start(MotionEngine e){for(int i=0;i<12;i++)e.step(.25);check(e.idleWalkingActive(),"Starts after three idle seconds");}
    static void frozen(MotionEngine e){double a=e.latitude,b=e.longitude;for(int i=0;i<50;i++)e.step(.1);check(a==e.latitude&&b==e.longitude,"Must stay in place");}
    public static void main(String[] args){
        MotionEngine e=engine(37,-122);for(int i=0;i<11;i++)e.step(.25);
        check(!e.idleWalkingActive()&&e.latitude==37&&e.longitude==-122,"No early movement");
        e.step(.25);check(e.idleWalkingActive()&&e.latitude==37&&e.longitude==-122,"Activation does not jump");
        long generation=e.routeGeneration();
        double period=2*Math.PI*10/(5/3.6);
        for(int i=0;i<4096;i++)e.step(period/4096);
        check(distance(37,-122,e.latitude,e.longitude)<.000001,"Loop closes at starting point");
        check(!e.walking&&e.activeRouteToken()==0&&e.routeGeneration()==generation,"Idle cannot acquire route ownership");
        for(double[] pos:new double[][]{{37,-122},{0,179.99999},{90,0},{-90,180},{89.999999,-179.999999}}){
            e=engine(pos[0],pos[1]);start(e);
            for(int i=0;i<100000;i++){
                double a=e.latitude,b=e.longitude;e.step(.1);
                check(Double.isFinite(e.bearing)&&e.bearing>=0&&e.bearing<360,"Finite heading");
                check(distance(pos[0],pos[1],e.latitude,e.longitude)<=20.000001,"Fixed anchor bounds across many loops");
                check(distance(a,b,e.latitude,e.longitude)<.140,"No jumps across poles or dateline");
            }
        }
        e=engine(0,0);start(e);e.speed(30);double a=e.latitude,b=e.longitude;e.step(1000);
        check(e.metersPerSecond()==5/3.6&&distance(a,b,e.latitude,e.longitude)<.348,"Idle speed and frame cap");
        a=e.latitude;b=e.longitude;e.step(Double.NaN);e.step(-1);e.step(Double.POSITIVE_INFINITY);
        check(e.latitude==a&&e.longitude==b,"Invalid frame time cannot move");
        e.setIdleWalking(false);frozen(e);check(!e.idleWalkingActive(),"Off ends idle");
        e.setIdleWalking(true);start(e);e.step(.2);e.setIdleWalkingAllowed(false);frozen(e);
        e.setIdleWalkingAllowed(true);a=e.latitude;b=e.longitude;start(e);check(a==e.latitude&&b==e.longitude,"Permission recovery no snap");
        e.step(.1);e.speed(0);frozen(e);check(e.metersPerSecond()==0,"Zero speed reports stopped");
        e.speed(3);start(e);check(e.metersPerSecond()==3/3.6,"Lower configured speed respected");
        e.input(1,0);check(!e.idleWalkingActive(),"Manual input takes over immediately");a=e.latitude;b=e.longitude;e.step(.25);
        check(distance(a,b,e.latitude,e.longitude)>.20,"Manual movement retains configured speed");
        e.input(0,0);a=e.latitude;b=e.longitude;start(e);check(e.latitude==a&&e.longitude==b,"Centered input restarts idle delay at current point");
        e.walkRoute(new double[][]{{1,1},{2,2}});long route=e.activeRouteToken();check(route!=0&&!e.idleWalkingActive(),"Route takes over");
        e.step(.1);check(e.activeRouteToken()==route&&e.walking,"Idle cannot alter route ownership");
        try{e.walkRoute(new double[][]{{Double.NaN,1}});throw new AssertionError("Invalid route accepted");}catch(IllegalArgumentException expected){}
        check(e.activeRouteToken()==route,"Invalid route preserves ownership");
        e.stop();start(e);e.position(-20,170);a=e.latitude;b=e.longitude;start(e);check(e.latitude==a&&e.longitude==b,"Teleport reanchors without jump");
        try{e.position(Double.NaN,0);throw new AssertionError("Invalid position accepted");}catch(IllegalArgumentException expected){}
        check(e.idleWalkingActive(),"Invalid position preserves current idle loop");
        e.pause();frozen(e);check(!e.idleWalkingActive(),"Background pause stops idle");
        e.enabled=true;start(e);check(e.idleWalkingActive(),"Foreground resumes after fresh delay");
        System.out.println("PASS idle walking: closed bounded loops, polar/dateline continuity, speed/frame caps, takeover, lifecycle and reanchoring");
    }
}
