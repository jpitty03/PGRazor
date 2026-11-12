package dev.pgrazor;
public final class RuntimeHealthTest {
    private static void check(boolean ok){if(!ok)throw new AssertionError();}
    public static void main(String[] args){
        RuntimeHealth health=new RuntimeHealth();long[] s={0,0,0,0,0,0,0};
        check(health.sample(0,true,s).contains("state=not-installed"));
        s[2]=-3;check(health.sample(5000,true,s).contains("state=faulted"));
        s[0]=1;s[2]=0;s[4]=1;
        check(health.sample(10000,true,s).contains("state=observing"));
        s[3]=1;check(health.sample(15000,true,s).contains("state=progressing"));
        check(health.sample(20000,true,s)==null);
        check(health.sample(30000,true,s).contains("state=no-pulse-progress"));
        check(health.sample(35000,true,s)==null);
        s[3]=2;check(health.sample(40000,true,s).contains("state=progressing"));
        check(health.sample(45000,false,s).contains("state=background"));
        check(health.sample(145000,true,s).contains("state=observing"));
        check(health.sample(160000,true,s).contains("state=no-pulse-progress"));
        s[4]=2;check(health.sample(165000,true,s).contains("state=observing"));
        s[1]=1;check(health.sample(170000,true,s).contains("state=stopped"));
        check(health.sample(175000,true,null)==null);
        check(health.sample(180000,true,new long[3])==null);
        System.out.println("PASS runtime health: startup failure, stall, recovery, resume grace, generation, quiet cadence");
    }
}
