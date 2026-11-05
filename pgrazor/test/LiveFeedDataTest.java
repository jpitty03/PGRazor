import dev.pgrazor.LiveFeedData;
import org.json.JSONObject;
import org.json.JSONArray;

/** Synthetic server records covering unknown stats, stale data, and fort types. */
public final class LiveFeedDataTest {
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
    public static void main(String[] args) throws Exception {
        long now=1800000000000L;
        JSONArray pokemon=new JSONArray();
        pokemon.put(new JSONObject().put("encounter_id","18446744073709551615").put("dex_id",6)
            .put("lat",37.5).put("lon",-122.4).put("despawn_ms",now+60000)
            .put("has_full_stats",true).put("iv_percent",100).put("cp",2345).put("level",30));
        pokemon.put(new JSONObject().put("encounter_id","expired").put("dex_id",1)
            .put("lat",37.5).put("lon",-122.4).put("despawn_ms",now-1));
        pokemon.put(new JSONObject().put("encounter_id","unknown").put("dex_id",2)
            .put("lat",37.5).put("lon",-122.4).put("has_full_stats",false)
            .put("iv_percent",0).put("cp",0).put("level",0));
        pokemon.put(new JSONObject().put("encounter_id","bad-coordinate").put("dex_id",3)
            .put("lat",100).put("lon",-122.4));
        JSONArray forts=new JSONArray();
        forts.put(new JSONObject().put("fort_id","checkpoint").put("fort_type","CHECKPOINT").put("lat",37.5).put("lon",-122.4));
        forts.put(new JSONObject().put("fort_id","stop").put("fort_type","POKESTOP").put("lat",37.5).put("lon",-122.4));
        forts.put(new JSONObject().put("fort_id","old-raid").put("fort_type","GYM").put("lat",37.5).put("lon",-122.4)
            .put("raid_end_ms",now-1).put("raid_pokemon_id",6).put("slots_available",JSONObject.NULL).put("team",1));
        forts.put(new JSONObject().put("fort_id","live-raid").put("fort_type","GYM").put("lat",37.5).put("lon",-122.4)
            .put("raid_end_ms",now+60000).put("raid_battle_ms",now-60000).put("raid_pokemon_id",6).put("slots_available",1).put("team",2));
        forts.put(new JSONObject().put("fort_id","egg-raid").put("fort_type","GYM").put("lat",37.5).put("lon",-122.4)
            .put("raid_end_ms",now+120000).put("raid_battle_ms",now+30000).put("raid_pokemon_id",6).put("team",3));
        JSONObject data=new JSONObject().put("pokemon",pokemon).put("forts",forts);
        LiveFeedData.Snapshot snapshot=LiveFeedData.parse(new JSONObject().put("success",true).put("data",data),now);
        int stops=0,gyms=0,raids=0,spawns=0;
        for(LiveFeedData.Entry e:snapshot.entries) {
            check(!e.id.contains("expired")&&!e.id.contains("bad-coordinate"),"Drop expired or invalid-coordinate spawns");
            if(e.kind==LiveFeedData.Kind.STOP)stops++;
            if(e.kind==LiveFeedData.Kind.GYM){gyms++;check(e.team==1,"Gym retains team id");}
            if(e.kind==LiveFeedData.Kind.RAID){raids++;
                if(e.id.contains("egg-raid")){check(e.team==3 && e.raidHatchAt==now+30000 && !e.raidBossLive,"Egg keeps hatch timestamp and never confirms predicted boss");}
                else check(e.team==2 && e.raidBossLive,"Active raid preserves confirmed boss and team");
            }
            if(e.kind==LiveFeedData.Kind.POKEMON){
                spawns++;
                if(e.dexId==6){check(e.id.contains("18446744073709551615"),"Preserve unsigned encounter ID precision");check(e.iv==100&&e.cp==2345&&e.level==30,"Keep known stats");check(!e.expired(now)&&e.expired(now+60001),"Expiry milliseconds");}
                if(e.dexId==2)check(!e.hasStats&&e.iv<0,"Unknown IV must not become a zero-IV claim");
            }
        }
        check(stops==2,"CHECKPOINT and POKESTOP both render as stops");
        check(gyms==1&&raids==2,"Expired raid falls back to gym, live raid remains raid");
        check(spawns==2,"Keep valid known and unknown-stat spawns");
        JSONArray ranked=new JSONArray().put(new JSONObject().put("encounter_id","9").put("dex_id",126)
            .put("lat",37.5).put("lon",-122.4).put("despawn_ms",now+60000).put("has_full_stats",true)
            .put("iv_percent",60).put("cp",900).put("level",20).put("pvp_great_rank",8).put("pvp_ultra_rank",400));
        String rankedRow=LiveFeedData.parse(new JSONObject().put("pokemon",ranked),now).entries.get(0).details;
        check(rankedRow.contains("GL #8")&&!rankedRow.contains("UL #"),"Row shows top-100 league ranks only: "+rankedRow);
        check(LiveFeedData.parse(new JSONObject().put("data",new JSONObject().put("span_tier","perfect")),now).note
            .contains("100% IV"),"Wide-span tier explains the 100%-only results");
        check(LiveFeedData.parse(new JSONObject().put("data",new JSONObject().put("span_tier","size")),now).note
            .isEmpty(),"Nearby tier adds no note");
        System.out.println("PASS: live feed fort types, expiry, unknown stats, coordinates and 64-bit IDs");
    }
}
