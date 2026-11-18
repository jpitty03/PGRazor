import dev.pgrazor.CameraHeadingState;
import dev.pgrazor.CameraSteering;

public final class CameraHeadingStateTest {
    static void check(boolean condition,String message) { if(!condition) throw new AssertionError(message); }
    static void near(double actual,double expected) { check(Math.abs(actual-expected)<1e-12,"Expected "+expected+", got "+actual); }
    static void blocked(CameraHeadingState state,long gesture,long now) {
        CameraSteering.Direction result=state.direction(gesture,0,1,now);
        check(!result.valid && result.east==0 && result.north==0,"Invalid gesture must stop without fallback");
    }
    public static void main(String[] args) {
        final long M=CameraHeadingState.MAX_AGE_MILLIS;
        check(M>=1000,"A held drag must survive the game's twice-a-second camera readings");
        CameraHeadingState state=new CameraHeadingState();
        long unavailable=state.beginGesture(1000);
        check(unavailable<0,"No gesture before first heading");
        check(state.update(0,7,true,100,1000),"Fresh aged sample accepted");
        blocked(state,unavailable,1000);
        long gesture=state.beginGesture(1000);
        CameraSteering.Direction direction=state.direction(gesture,0,1,1000);
        check(direction.valid,"New gesture accepted"); near(direction.east,0); near(direction.north,1);
        check(state.update(90,7,true,0,1100),"Same camera turns");
        direction=state.direction(gesture,0,1,1100);
        check(direction.valid,"Continuous fresh heading preserves held gesture"); near(direction.east,1); near(direction.north,0);
        // Readings 500 ms apart (the slow game cadence) keep a held drag alive.
        long t=1100;
        for(int i=0;i<6;i++){t+=500;check(state.direction(gesture,0,1,t).valid,"Held drag survives the gap before a slow reading");check(state.update(90,7,true,0,t),"Slow reading accepted");}
        check(state.direction(gesture,0,1,t+M).valid,"Maximum age inclusive");
        blocked(state,gesture,t+M+1);
        long r=t+M+2;
        check(state.update(180,7,true,0,r),"Fresh heading recovers");
        blocked(state,gesture,r); // Never restart a held vector after staleness.
        gesture=state.beginGesture(r);
        check(state.direction(gesture,0,1,r).valid,"New drag after recovery");
        check(state.update(270,-100,true,0,r+100),"Signed camera identities accepted");
        blocked(state,gesture,r+100);
        gesture=state.beginGesture(r+100);
        direction=state.direction(gesture,0,1,r+100); near(direction.east,-1); near(direction.north,0);

        // Expiration must be noticed even when no tick checked the stale interval.
        long gap=r+100+M+100;
        check(state.update(450,-100,true,0,gap),"Update after unnoticed gap");
        blocked(state,gesture,gap);
        gesture=state.beginGesture(gap); near(state.direction(gesture,0,1,gap).east,1);
        state.invalidate(); blocked(state,gesture,gap);
        long n=gap+1;
        check(state.update(0,-100,true,0,n),"Recovery after explicit invalidation"); blocked(state,gesture,n);

        double[] invalidHeadings={Double.NaN,Double.POSITIVE_INFINITY,Double.NEGATIVE_INFINITY};
        for(double heading:invalidHeadings) {
            gesture=state.beginGesture(n);
            check(!state.update(heading,-100,true,0,n),"Reject nonfinite heading");
            state.update(0,-100,true,0,n); blocked(state,gesture,n);
        }
        for(long age:new long[]{-1,M+1,Long.MAX_VALUE}) {
            gesture=state.beginGesture(n);
            check(!state.update(0,-100,true,age,n),"Reject invalid age");
            state.update(0,-100,true,0,n); blocked(state,gesture,n);
        }
        gesture=state.beginGesture(n);
        check(!state.update(0,-100,false,0,n),"Hidden map rejected");
        state.update(0,-100,true,0,n); blocked(state,gesture,n);
        gesture=state.beginGesture(n);
        check(!state.update(180,-100,true,1,n),"Out-of-order sample rejected");
        state.update(0,-100,true,0,n); blocked(state,gesture,n);
        check(!state.update(0,7,true,10,5),"Impossible preboot timestamp rejected");
        check(!state.update(0,7,true,0,-1),"Negative monotonic time rejected");
        state.update(0,7,true,0,n+200); gesture=state.beginGesture(n+200); blocked(state,gesture,n+199);
        state.update(0,7,true,0,Long.MAX_VALUE); gesture=state.beginGesture(Long.MAX_VALUE);
        check(state.direction(gesture,0,1,Long.MAX_VALUE).valid,"Large monotonic timestamp remains valid");
        CameraHeadingState delivered=new CameraHeadingState();
        check(delivered.updateAt(90,7,true,100000,100100),"Original boot timestamp accepted with 100 ms delivery delay");
        long held=delivered.beginGesture(100100);
        check(!delivered.updateAt(90,7,true,100050,100050+M+1),"Bridge delay cannot renew an old reading");
        long d=100050+M+1;
        check(delivered.updateAt(90,7,true,d,d),"New reading after delayed delivery");
        blocked(delivered,held,d);
        check(delivered.updateAt(0,7,true,d,d+M),"Timestamp freshness boundary inclusive");
        check(!delivered.updateAt(0,7,true,d,d+M+1),"Timestamp past boundary rejected");
        check(!delivered.updateAt(0,7,true,d+100000,d+M+1),"Future boot timestamp rejected");
        check(!delivered.updateAt(0,7,true,-1,d+M+1),"Negative timestamp rejected");
        check(!delivered.updateAt(0,7,true,d,d+100000),"Suspend time ages sample and blocks old heading");
        System.out.println("PASS: continuous rotation, slow-cadence held drag, sample age, expiry boundaries, fresh gesture after recovery, camera identity, hidden map, invalid/reordered samples, monotonic bounds");
    }
}
