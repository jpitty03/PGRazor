package dev.pgrazor;

import java.util.*;
import org.json.*;

/** Validation is completed before any settings are written. No credential stores are accepted. */
public final class SettingsBackupRules {
    public static final String[] SETTINGS=("autoBall autoBallTarget autoBerry autoDismiss autoIncubate autoLuckyEgg autoRaidBosses autoRaidFreePass autoRaidGoldenRazzBerry autoRaidMaxDistanceKm autoRaidMaxRaids autoRaidMinPlayers autoRaidPremiumPass autoRaidRazzBerry autoRaidStopOnShiny autoRaidTierMask autoRaidWaitSeconds autoRaidWalk autoRocketMaxBattles autoRocketMaxDistanceKm autoRocketTargets autoRocketTransferRules autoRocketWalk autoStarPiece autoThrow autoThrowOnMap autoTransfer batteryFps20 batteryLowQuality batteryLowRes battleOptions berryPriority buddyBerry buddyEmotionBerries buddyEmotionBerry buddyEmotionEnabled buddyEmotionGifts buddyEmotionOverlay buddyEmotionReserve catchChance catchPreview cooldownOverlay curve darkTheme encounterBerry encounterCounter encounterOverlay evolveTransferEnabled evolveTransferMode evolveTransferSpecies evolveTransferStages giftOptions grade gymAutomationDeploy gymAutomationFallback gymAutomationFeed gymAutomationNanabReserve gymAutomationOverlay gymAutomationRazzReserve gymAutomationSpin gymRepeatEnabled idleWalking incubateLongest itemLimits itemRules itemTransfer ivCatch ivCatchFormat ivOwned ivOwnedFormat joystickVisible localTimeOverlay locationSpoofing mapTap mapTapTeleportNoAsk pvpGreatRank pvpKeepLeagues pvpMasterRank pvpUltraRank quickMenuHidden quickSpin raidCounterShowCard raidCounterTeamSize raidCounters rocketOptions routeGiftExchange shinyScan shinyScanDailyLimit shinyScanHold shinyScanNotify shinyScanSpecies shinyScanSpeed shinyScanStopWalk shundoFeed shundoGap shundoLive shundoPogo shundoSpecies skipCutscenes skipTeleportConfirm smartBattle smartCatchAlsoRare smartCatchAutoBall smartCatchAutoBallTarget smartCatchBall smartCatchBerry smartCatchCandyBerry smartCatchCandySpecies smartCatchCandyTags smartCatchCatchSpecies smartCatchCatchTags smartCatchCurve smartCatchEnabled smartCatchFallbackBerries smartCatchGrade smartCatchHighCp smartCatchHighLevel smartCatchPremium smartCatchRareBerry smartCatchRareIvMin smartCatchRareMask smartCatchShinyOnly smartCatchSpeed smartCatchSpinGyms smartCatchTagsActive smartCatchTagsOverlay smartCatchUltraSpecies smartCatchVariety smartHealEnabled smartHealFull smartHealRevive smartHealThreshold smartHealWhen spawnBooster storageIv storageIvFormat throwAr throwVariety tmShortcuts transferKeepAll transferKeepIv transferKeepSpecies transferOverlay transferProtect transferProtectMax transferProtectShadow transferProtectSplit uiScalePercent vgpOptions vgpOverlay vgpStatusText vgpStopOnCooldown").split(" ");
    private static final Set<String> BOOLEANS=new HashSet<>(Arrays.asList(("autoBall autoDismiss autoIncubate autoLuckyEgg autoRaidFreePass autoRaidGoldenRazzBerry autoRaidPremiumPass autoRaidRazzBerry autoRaidStopOnShiny autoRaidWalk autoRocketTransferRules autoRocketWalk autoStarPiece autoThrow autoThrowOnMap autoTransfer batteryFps20 batteryLowQuality batteryLowRes buddyEmotionEnabled buddyEmotionGifts buddyEmotionOverlay catchChance catchPreview cooldownOverlay curve darkTheme encounterBerry encounterCounter encounterOverlay evolveTransferEnabled gymAutomationDeploy gymAutomationFallback gymAutomationFeed gymAutomationOverlay gymAutomationSpin gymRepeatEnabled idleWalking incubateLongest itemTransfer ivCatch ivOwned joystickVisible localTimeOverlay locationSpoofing mapTapTeleportNoAsk quickSpin raidCounterShowCard raidCounters routeGiftExchange shinyScan shinyScanHold shinyScanNotify shinyScanStopWalk shundoFeed shundoLive shundoPogo skipCutscenes skipTeleportConfirm smartBattle smartCatchAlsoRare smartCatchAutoBall smartCatchCurve smartCatchEnabled smartCatchPremium smartCatchShinyOnly smartCatchSpinGyms smartCatchTagsActive smartCatchTagsOverlay smartHealEnabled smartHealFull smartHealRevive spawnBooster throwAr tmShortcuts transferKeepAll transferOverlay vgpOverlay vgpStatusText vgpStopOnCooldown").split(" ")));
    private static void require(boolean yes,String message)throws JSONException{if(!yes)throw new JSONException(message);}
    private static double number(Object o,double min,double max,String label)throws JSONException{
        require(o instanceof Number,"Invalid "+label);double n=((Number)o).doubleValue();require(Double.isFinite(n)&&n>=min&&n<=max,"Invalid "+label);return n;
    }
    private static void integer(Object o,int max,String label)throws JSONException{double n=number(o,0,max,label);require(n==Math.rint(n),"Invalid "+label);}
    private static void bool(Object o,String label)throws JSONException{require(o instanceof Boolean,"Invalid "+label);}
    public static void validate(JSONObject root)throws JSONException{
        require("PGRazor settings".equals(root.optString("format")),"Unsupported settings backup");
        require(number(root.get("version"),1,1,"backup version")==1,"Unsupported settings backup");
        double speed=number(root.get("speedKmh"),0,30,"speed");require(speed==0||speed>=0.5,"Invalid speed");
        JSONObject settings=root.getJSONObject("settings");Set<String> allowed=new HashSet<>(Arrays.asList(SETTINGS));
        for(Iterator<String> it=settings.keys();it.hasNext();){String key=it.next();require(allowed.contains(key),"Unknown setting: "+key);Object raw=settings.get(key);require(raw instanceof String,"Invalid setting: "+key);String value=(String)raw;
            if(BOOLEANS.contains(key)){require(value.equals("true")||value.equals("false"),"Invalid "+key);continue;}
            if(key.equals("buddyEmotionBerries")){require(BuddyEmotionPreferences.validBerries(value),"Invalid buddy berry priority");continue;}
            if(key.equals("itemRules")||key.equals("itemLimits")){itemRules(value);continue;}
            if(key.equals("transferKeepSpecies")||key.equals("evolveTransferSpecies")){keepSpecies(value);continue;}
            if(key.equals("shinyScanSpecies")){try{AutoRaidPreferences.normalizeBosses(value);}catch(IllegalArgumentException invalid){throw new JSONException("Invalid Shiny Scanner species");}continue;}
            if(key.equals("autoRaidBosses")){try{AutoRaidPreferences.normalizeBosses(value);}catch(IllegalArgumentException invalid){throw new JSONException(invalid.getMessage());}continue;}
            if(key.equals("shundoGap")){require(value.equals("0")||value.equals("30")||value.equals("60")||value.equals("90"),"Invalid Shundo Sniper teleport gap");continue;}
            if(key.equals("shundoSpecies")){keepSpecies(value);require(value.isEmpty()||value.split(",").length<=500,"Too many Shundo Sniper species");continue;}
            if(key.equals("smartCatchCandySpecies")){keepSpecies(value);require(value.isEmpty()||value.split(",").length<=200,"Too many candy species");continue;}
            if(key.equals("smartCatchCatchSpecies")){keepSpecies(value);require(value.isEmpty()||value.split(",").length<=200,"Too many catch-only species");continue;}
            if(key.equals("berryPriority")){try{BerryPriority.parse(value);}catch(IllegalArgumentException invalid){throw new JSONException("Invalid berry priorities");}continue;}
            if(key.equals("smartCatchFallbackBerries")){try{BerryPriority.parse(value);}catch(IllegalArgumentException invalid){throw new JSONException("Invalid fallback berries");}continue;}
            // Tag-rule values: strings first (they are not integers), then numeric ranges.
            if(key.equals("smartCatchUltraSpecies")){keepSpecies(value);require(value.isEmpty()||value.split(",").length<=200,"Too many ultra-rare species");continue;}
            if(key.equals("smartCatchCatchTags")||key.equals("smartCatchCandyTags")){tagNames(value);continue;}
            if(key.equals("smartCatchRareBerry")||key.equals("smartCatchCandyBerry")){require(value.equals("0")||value.equals("701")||value.equals("703")||value.equals("705")||value.equals("706")||value.equals("707")||value.equals("708"),"Invalid tag-rule berry");continue;}
            int n;try{n=Integer.parseInt(value);}catch(NumberFormatException e){throw new JSONException("Invalid "+key);}
            if(key.equals("smartCatchRareMask")){require(n>=0&&n<=511,"Invalid rare criteria mask");continue;}
            if(key.equals("smartCatchRareIvMin")){require(n>=1&&n<=100,"Invalid rare IV threshold");continue;}
            if(key.equals("shinyScanSpeed")){require(n>=0&&n<=2,"Invalid Shiny Scanner speed");continue;}
            if(key.equals("shinyScanDailyLimit")){require(n==0||n==2000||n==5000||n==8000,"Invalid Shiny Scanner daily limit");continue;}
            if(key.equals("autoRocketTargets")){require(n==0||n==1,"Invalid Auto Rocket battles");continue;}
            if(key.equals("autoRocketMaxBattles")){require(n>=1&&n<=100,"Invalid Auto Rocket battle count");continue;}
            if(key.equals("autoRocketMaxDistanceKm")){require(n==0||n==1||n==2||n==5||n==10,"Invalid Auto Rocket distance limit");continue;}
            if(key.equals("autoRaidTierMask")){require(n>=0&&n<=127,"Invalid raid tiers");continue;}
            if(key.equals("autoRaidMaxRaids")){require(n>=1&&n<=100,"Invalid raid count");continue;}
            if(key.equals("autoRaidMinPlayers")){require(n>=1&&n<=20,"Invalid raid players");continue;}
            if(key.equals("autoRaidWaitSeconds")){require(n>=15&&n<=120,"Invalid raid wait");continue;}
            if(key.equals("autoRaidMaxDistanceKm")){require(AutoRaidPreferences.validDistance(n),"Invalid raid distance limit");continue;}
            if(key.equals("quickMenuHidden")){require(n>=0&&n<=8191,"Invalid quick menu visibility");continue;}
            if(key.equals("uiScalePercent")){require(n==25||n==50||n==75||n==100,"Invalid UI scale");continue;}
            if(key.equals("buddyBerry")||key.equals("autoBerry")){require(n==0||n==701||n==703||n==705||n==706||n==707,"Invalid berry");continue;}
            if(key.equals("smartCatchBerry")){require(n==0||n==701||n==703||n==705||n==706||n==708,"Invalid smart catch berry");continue;}
            if(key.equals("buddyEmotionBerry")){require(n==0||n==701||n==703||n==705,"Invalid buddy emotion berry");continue;}
            if(key.equals("buddyEmotionReserve")){require(n>=0&&n<=1000,"Invalid buddy emotion reserve");continue;}
            if(key.equals("gymAutomationNanabReserve")||key.equals("gymAutomationRazzReserve")){require(n>=0&&n<=1000,"Invalid gym berry reserve");continue;}
            if(key.equals("smartCatchBall")){require(n>=1&&n<=3,"Invalid smart catch ball");continue;}
            if(key.equals("smartCatchHighCp")){require(n>=0&&n<=10000,"Invalid high CP threshold");continue;}
            if(key.equals("smartCatchHighLevel")){require(n>=0&&n<=50,"Invalid high level threshold");continue;}
            if(key.equals("autoBallTarget")||key.equals("smartCatchAutoBallTarget")){require(n>=1&&n<=99,"Invalid auto ball target");continue;}
            if(key.equals("smartCatchSpeed")){require(n>=0&&n<=2,"Invalid smart catch speed");continue;}
            if(key.equals("smartCatchGrade")){require(n>=0&&n<=3,"Invalid smart catch throw grade");continue;}
            if(key.equals("throwVariety")||key.equals("smartCatchVariety")){require(n>=0&&n<=3,"Invalid throw variety");continue;}
            if(key.equals("raidCounterTeamSize")){require(n>=1&&n<=6,"Invalid raid counter team size");continue;}
            if(key.equals("smartHealWhen")){require(n>=0&&n<=2,"Invalid smart healing schedule");continue;}
            if(key.equals("smartHealThreshold")){require(n==25||n==50||n==75||n==100,"Invalid smart healing threshold");continue;}
            if(key.equals("transferKeepIv")){require(n>=0&&n<=100,"Invalid transfer IV cutoff");continue;}
            if(key.equals("evolveTransferMode")){require(n==0||n==1,"Invalid evolve transfer mode");continue;}
            if(key.equals("evolveTransferStages")){require(n==1||n==2,"Invalid evolve transfer stages");continue;}
            if(key.equals("pvpKeepLeagues")){require(n>=0&&n<=7,"Invalid PvP leagues");continue;}
            if(key.equals("pvpGreatRank")||key.equals("pvpUltraRank")||key.equals("pvpMasterRank")){require(n>=1&&n<=4096,"Invalid PvP rank");continue;}
            int max=key.equals("battleOptions")?1023:key.equals("transferProtect")?2047:key.equals("transferProtectSplit")||key.equals("transferProtectShadow")||key.equals("transferProtectMax")?1:key.equals("grade")?3:key.equals("mapTap")?3:key.equals("storageIv")||key.equals("storageIvFormat")||key.equals("ivCatchFormat")||key.equals("ivOwnedFormat")?4:7;
            require(n>=0&&n<=max,"Invalid "+key);
        }
        JSONObject preferences=root.getJSONObject("preferences");
        for(Iterator<String> it=preferences.keys();it.hasNext();){String scope=it.next();JSONObject group=preferences.getJSONObject(scope);
            require(PreferenceSchema.scope(scope),"Unknown settings group");
            for(Iterator<String> keys=group.keys();keys.hasNext();){String key=keys.next();Object value=group.get(key);
                require(PreferenceSchema.has(scope,key),"Unknown setting: "+scope+"."+key);
                if(scope.equals("pgrazor-map"))bool(value,key);
                else if(scope.equals("pgrazor-overlays"))number(value,0,1,key);
                else if(scope.equals("pgrazor-saved-places-v1")){require(value instanceof String,"Invalid favorites");places((String)value);}
                else if(key.equals("category"))integer(value,5,key);
                else if(key.equals("radius"))integer(value,2,key);
                else if(key.equals("minIv"))integer(value,3,key);
                else if(key.equals("myShinies"))bool(value,key);
                else if(key.equals("filters_v1")||key.equals("filter_presets_v1")){
                    require(value instanceof String&&((String)value).length()<=150000,"Invalid feed filters");
                    Object parsed=new JSONTokener((String)value).nextValue();require(parsed instanceof JSONObject||parsed instanceof JSONArray,"Invalid feed filters");
                    require(parsed instanceof JSONObject,"Invalid feed filters");
                    if(key.equals("filters_v1"))LiveFeedFilters.fromJson((JSONObject)parsed);
                    else{
                        JSONObject presets=(JSONObject)parsed;require(presets.length()<=25,"Too many filter presets");
                        for(Iterator<String> names=presets.keys();names.hasNext();){String name=names.next();require(!name.trim().isEmpty()&&name.length()<=40,"Invalid preset name");LiveFeedFilters.fromJson(presets.getJSONObject(name));}
                    }
                }else throw new JSONException("Unknown feed setting");
            }
        }
    }
    /** Storage-tag names: same 4000-char cap as TagCatchSettings.tags(); no native config separators. */
    private static void tagNames(String value)throws JSONException{
        require(value.length()<=4000,"Tag list too large");
        if(value.isEmpty())return;
        for(String name:value.split("\t",-1))
            require(!name.trim().isEmpty()&&name.indexOf(';')<0&&name.indexOf('\n')<0&&name.indexOf('\r')<0,"Invalid tag name");
    }
    private static void keepSpecies(String value)throws JSONException{
        require(value.length()<=10000,"Keep list too large");Set<Integer> ids=new HashSet<>();
        if(value.isEmpty())return;
        for(String token:value.split(",",-1)){require(token.matches("[1-9][0-9]{0,3}"),"Invalid keep species");int id=Integer.parseInt(token);require(id>=1&&id<=2000&&ids.add(id),"Invalid keep species");}
        require(ids.size()<=2000,"Too many keep species");
    }
    private static void itemRules(String value)throws JSONException{
        require(value.length()<=30000,"Item limits too large");Set<Integer> ids=new HashSet<>();
        for(String row:value.split(";")){if(row.isEmpty())continue;require(row.matches("[0-9]{1,7}:[0-9]{1,6}"),"Invalid item limit");String[] pair=row.split(":");int id=Integer.parseInt(pair[0]),cap=Integer.parseInt(pair[1]);require(id>0&&cap<=100000&&ids.add(id),"Invalid item limit");}
    }
    private static void places(String value)throws JSONException{
        require(value.length()<=65536,"Too many favorites");JSONArray list=new JSONArray(value);require(list.length()<=50,"Too many favorites");Set<String> ids=new HashSet<>(),names=new HashSet<>();
        for(int i=0;i<list.length();i++){JSONObject p=list.getJSONObject(i);String id=p.getString("id"),name=p.getString("name");require(!id.isEmpty()&&id.length()<=64&&ids.add(id),"Invalid favorite ID");require(!name.trim().isEmpty()&&name.length()<=40&&names.add(name.trim().toLowerCase(Locale.ROOT)),"Invalid favorite name");number(p.get("latitude"),-90,90,"favorite latitude");number(p.get("longitude"),-180,180,"favorite longitude");}
    }
}
