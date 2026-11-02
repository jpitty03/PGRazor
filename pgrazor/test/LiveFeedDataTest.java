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
        System.out.println("PASS: live feed fort types, expiry, unknown stats, coordinates and 64-bit IDs");

        // --- My Shinies: epoch-seconds expiry, personal flag, 64-bit ids, individual IVs, bounded refresh ---
        long nowSec=now/1000;
        JSONArray shinies=new JSONArray();
        shinies.put(new JSONObject().put("encounter_id","18446744073709551614").put("name","").put("dex_id",399)
            .put("form",1667).put("costume",0).put("gender",1).put("level",8).put("cp",137)
            .put("iv",53.333332).put("atk_iv",7).put("def_iv",2).put("sta_iv",15).put("size",3)
            .put("pvp_rank1",JSONObject.NULL).put("lat",37.5).put("lon",-122.4)
            .put("distance_km",2.33).put("cooldown_sec",140).put("despawn_ts",nowSec+300).put("despawn_verified",true));
        shinies.put(new JSONObject().put("encounter_id","shiny-expired").put("dex_id",179)
            .put("lat",37.5).put("lon",-122.4).put("iv",44.4).put("despawn_ts",nowSec-10));
        shinies.put(new JSONObject().put("encounter_id","18446744073709551614").put("dex_id",399)
            .put("lat",37.5).put("lon",-122.4).put("iv",53.3).put("despawn_ts",nowSec+300)); // duplicate id
        JSONObject shinyData=new JSONObject().put("refresh_sec",30).put("shinies",shinies);
        LiveFeedData.Snapshot ss=LiveFeedData.parseShinies(new JSONObject().put("success",true).put("data",shinyData),now);
        check(ss.refreshSeconds==30,"My Shinies honours bounded refresh_sec");
        check(ss.entries.size()==1,"Drop expired and de-duplicate shinies");
        LiveFeedData.Entry shiny=ss.entries.get(0);
        check(shiny.kind==LiveFeedData.Kind.POKEMON,"Shiny keeps POKEMON kind so the sprite resolves");
        check(shiny.personalShiny,"Shiny row is flagged personal");
        check(shiny.id.contains("18446744073709551614"),"Preserve 64-bit shiny encounter id as a string");
        check(!shiny.expired(now)&&shiny.expired(now+300001),"despawn_ts seconds converted to millis");
        check(Math.abs(shiny.iv-53.333332)<0.001&&shiny.cp==137&&shiny.hasStats,"Shiny IV percentage and CP preserved");
        check(shiny.attrs.atkIv==7&&shiny.attrs.defIv==2&&shiny.attrs.staIv==15,"Individual shiny IVs parsed");
        check(shiny.attrs.size==3&&shiny.attrs.form==1667&&shiny.attrs.costume==0,"Observed shiny attrs parsed");
        check(shiny.attrs.pvpUltraRank==-1&&shiny.attrs.pvpGreatRank==-1,"Null/ambiguous PvP rank stays unknown");

        // A despawn_ts already in milliseconds must not be double-scaled into the far future.
        JSONArray msShiny=new JSONArray();
        msShiny.put(new JSONObject().put("encounter_id","ms-ts").put("dex_id",1).put("lat",37.5).put("lon",-122.4)
            .put("iv",10).put("despawn_ts",now+60000));
        LiveFeedData.Snapshot ms=LiveFeedData.parseShinies(new JSONObject().put("shinies",msShiny),now);
        check(ms.entries.size()==1&&!ms.entries.get(0).expired(now)&&ms.entries.get(0).expired(now+60001),
            "Millisecond despawn_ts passed through without double-scaling");
        JSONArray badTime=new JSONArray().put(new JSONObject().put("encounter_id","overflow").put("lat",1).put("lon",1).put("despawn_ts",Long.MAX_VALUE));
        check(LiveFeedData.parseShinies(new JSONObject().put("shinies",badTime),now).entries.isEmpty(),"Overflowing expiry is rejected");
        LiveFeedData.Snapshot gated=LiveFeedData.parseShinies(new JSONObject().put("shinies",new JSONArray()).put("shiny_access",false),now);
        check(gated.refreshSeconds==30&&!gated.note.isEmpty(),"Default shiny interval and access status");
        System.out.println("PASS: my shinies seconds-expiry, personal flag, 64-bit ids, individual IVs");
        // PGRazorX: country flag from the place that ends a row's details.
        check(LiveFeedData.countryFlag("CP 403 · Lv 20 · 100.0% IV · M · Male · Take Down / Stomp · Saint-Sébastien-sur-Loire, Loire-Atlantique, France").equals("\uD83C\uDDEB\uD83C\uDDF7"),"France flag");
        check(LiveFeedData.countryFlag("CP 2513 · Lv 35 · Boosted · Roanoke, Texas, United States").equals("\uD83C\uDDFA\uD83C\uDDF8"),"United States flag");
        check(LiveFeedData.countryFlag("CP 523 · Yongkang, Tainan City, Taiwan").equals("\uD83C\uDDF9\uD83C\uDDFC"),"Taiwan flag");
        check(LiveFeedData.countryFlag("CP 274 · Bairro Silva Braga, Porto, Portugal").equals("\uD83C\uDDF5\uD83C\uDDF9"),"Portugal flag");
        check(LiveFeedData.countryFlag("Seoul, South Korea").equals("\uD83C\uDDF0\uD83C\uDDF7"),"Alias: South Korea");
        check(LiveFeedData.countryFlag("CP 10 · Singapore").equals("\uD83C\uDDF8\uD83C\uDDEC"),"A city-state without commas");
        check(LiveFeedData.countryFlag("CP 403 · Lv 20 · Take Down / Stomp").isEmpty(),"Moves are not a country");
        check(LiveFeedData.countryFlag("Somewhere, Atlantis").isEmpty()&&LiveFeedData.countryFlag(null).isEmpty()&&LiveFeedData.countryFlag("").isEmpty(),"Unknown place: no flag");
        System.out.println("PASS: feed country flags");
    }
}
