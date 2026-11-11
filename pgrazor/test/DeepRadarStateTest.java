package dev.pgrazor;
import org.json.JSONObject;
import org.json.JSONArray;
import java.util.Collections;
public final class DeepRadarStateTest {
 static void check(boolean b){if(!b)throw new AssertionError("radar state");}
 static JSONObject row()throws Exception {return new JSONObject().put("encounter","18446744073709551615").put("cell","0").put("species",885).put("form",0).put("costume",0).put("gender",0).put("kind","exact").put("lat",12.5).put("lon",179.999).put("seenAt",1000).put("expiresAt",91000).put("uncaught",-1).put("fort","").put("corners",new JSONArray());}
 static JSONObject frame(long seq)throws Exception{return new JSONObject().put("version",1).put("generation",1).put("sequence",seq).put("sampledAt",1000).put("status","ready").put("message","").put("packets",1).put("cells",1).put("nearby",1).put("offstop",1).put("wild",1).put("catchable",0).put("rows",new JSONArray().put(row()));}
 public static void main(String[]args)throws Exception {
  DeepRadarState s=new DeepRadarState();check(s.accept(frame(1).toString(),1000));DeepRadarState.Row r=s.get(1000).rows.get(0);check(r.encounter.equals("18446744073709551615"));check(r.lat==12.5&&r.lon==179.999);check(DeepRadarState.visible(r,1000,Collections.emptySet(),false));check(!DeepRadarState.visible(r,1000,Collections.emptySet(),true));check(!DeepRadarState.visible(r,91000,Collections.emptySet(),false));check(!s.accept(frame(1).toString(),1000));check(s.get(1000)!=null);
  JSONObject f=frame(2);f.getJSONArray("rows").getJSONObject(0).put("encounter",1844);check(!s.accept(f.toString(),1000));check(s.get(1000)==null);
  check(s.accept(frame(3).toString(),1000));check(s.get(91000)==null);check(!s.accept(frame(4).toString(),92000));check(!s.accept(frame(5).put("sampledAt",999999).toString(),1000));
  f=frame(6);f.getJSONArray("rows").put(row());check(!s.accept(f.toString(),1000));
  f=frame(7);f.getJSONArray("rows").getJSONObject(0).put("encounter","18446744073709551616");check(!s.accept(f.toString(),1000));
  f=frame(8);f.getJSONArray("rows").getJSONObject(0).put("lat",91);check(!s.accept(f.toString(),1000));
  check(s.accept(frame(9).toString(),1000));s.clear(2000);check(!s.accept(frame(10).toString(),2000));check(s.get(2000)==null);
  f=frame(11).put("sampledAt",2001);check(s.accept(f.toString(),2001));check(DeepRadarState.speciesFilter(" 885, 1,885").size()==2);
  boolean threw=false;try{DeepRadarState.speciesFilter("885,,1");}catch(IllegalArgumentException e){threw=true;}check(threw);
  System.out.println("PASS DeepRadarState: uint64 IDs, atomic invalidation, ordering, stale/lifecycle bounds, expiry and filters");
 }
}
