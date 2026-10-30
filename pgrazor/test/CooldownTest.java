package dev.pgrazor;
public class CooldownTest {
 static void check(boolean b){if(!b)throw new AssertionError();}
 public static void main(String[] args){
 check(CooldownState.duration(10)==420000);check(CooldownState.duration(1350)==7200000);check(CooldownState.duration(10000)==7200000);check(CooldownState.duration(0)==0);
 check(CooldownState.distance(0,179.99,0,-179.99)<3);
 CooldownState s=new CooldownState();long now=100000000;check(s.remaining(now,45,45)==0);s.add(new CooldownState.Entry(1,now,0,0,"Ball throw"));
 check(s.remaining(now,0,0)==0);check(s.remaining(now,45,45)==7200000);check(s.remaining(now+3600000,45,45)==3600000);
 s.add(new CooldownState.Entry(2,now+1000,45,45,"Spin"));check(s.remaining(now+1000,45,45)==7199000);
 CooldownState restored=new CooldownState();restored.decode(s.encode());check(restored.rows.size()==2);check(restored.remaining(now+1000,45,45)==7199000);check(restored.remaining(now+7200000,45,45)==0);
 check(restored.remaining(now-1000,45,45)<=7200000);restored.decode("corrupt\n"+s.encode());check(restored.rows.size()==2);
 System.out.println("PASS cooldown distance chart, dateline, elapsed time, previous-location anchor, persistence, corrupt rows, clock rollback");
 }
}
