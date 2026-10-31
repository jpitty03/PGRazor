package dev.pgrazor;

import java.util.*;
import org.json.*;

/** Validation is completed before any settings are written. No credential stores are accepted. */
public final class SettingsBackupRules {
    public static final String[] SETTINGS=("uiScalePercent locationSpoofing darkTheme joystickVisible spawnBooster giftOptions vgpOverlay vgpStopOnCooldown vgpOptions buddyBerry autoIncubate incubateLongest cooldownOverlay autoBerry berryPriority encounterBerry battleOptions rocketOptions itemTransfer itemRules itemLimits quickSpin autoTransfer transferProtect transferKeepAll transferOverlay transferKeepSpecies autoDismiss throwAr curve grade mapTap ivCatch ivOwned smartCatchEnabled smartCatchBall smartCatchBerry smartCatchPremium smartCatchHighCp smartCatchHighLevel smartCatchGrade smartCatchCurve smartCatchCandySpecies gymAutomationDeploy gymAutomationFeed gymAutomationOverlay gymAutomationFallback gymAutomationNanabReserve gymAutomationRazzReserve buddyEmotionOverlay buddyEmotionEnabled buddyEmotionBerry buddyEmotionBerries buddyEmotionReserve buddyEmotionGifts").split(" ");
    private static final Set<String> BOOLEANS=new HashSet<>(Arrays.asList(("locationSpoofing darkTheme joystickVisible spawnBooster vgpOverlay vgpStopOnCooldown autoIncubate incubateLongest cooldownOverlay encounterBerry itemTransfer quickSpin autoTransfer transferKeepAll transferOverlay autoDismiss throwAr curve ivCatch ivOwned smartCatchEnabled smartCatchPremium smartCatchCurve gymAutomationDeploy gymAutomationFeed gymAutomationOverlay gymAutomationFallback buddyEmotionOverlay buddyEmotionEnabled buddyEmotionGifts").split(" ")));
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
            if(key.equals("transferKeepSpecies")){keepSpecies(value);continue;}
            if(key.equals("smartCatchCandySpecies")){keepSpecies(value);require(value.isEmpty()||value.split(",").length<=200,"Too many candy species");continue;}
            if(key.equals("berryPriority")){try{BerryPriority.parse(value);}catch(IllegalArgumentException invalid){throw new JSONException("Invalid berry priorities");}continue;}
            int n;try{n=Integer.parseInt(value);}catch(NumberFormatException e){throw new JSONException("Invalid "+key);}
            if(key.equals("uiScalePercent")){require(n==25||n==50||n==75||n==100,"Invalid UI scale");continue;}
            if(key.equals("buddyBerry")||key.equals("autoBerry")){require(n==0||n==701||n==703||n==705||n==706||n==707,"Invalid berry");continue;}
            if(key.equals("smartCatchBerry")){require(n==0||n==701||n==703||n==705||n==706||n==708,"Invalid smart catch berry");continue;}
            if(key.equals("buddyEmotionBerry")){require(n==0||n==701||n==703||n==705,"Invalid buddy emotion berry");continue;}
            if(key.equals("buddyEmotionReserve")){require(n>=0&&n<=1000,"Invalid buddy emotion reserve");continue;}
            if(key.equals("gymAutomationNanabReserve")||key.equals("gymAutomationRazzReserve")){require(n>=0&&n<=1000,"Invalid gym berry reserve");continue;}
            if(key.equals("smartCatchBall")){require(n>=1&&n<=3,"Invalid smart catch ball");continue;}
            if(key.equals("smartCatchHighCp")){require(n>=0&&n<=10000,"Invalid high CP threshold");continue;}
            if(key.equals("smartCatchHighLevel")){require(n>=0&&n<=50,"Invalid high level threshold");continue;}
            if(key.equals("smartCatchGrade")){require(n>=0&&n<=3,"Invalid smart catch throw grade");continue;}
            int max=key.equals("battleOptions")?1023:key.equals("transferProtect")?63:key.equals("grade")?3:key.equals("mapTap")?3:7;
            require(n>=0&&n<=max,"Invalid "+key);
        }
        JSONObject preferences=root.getJSONObject("preferences");
        for(Iterator<String> it=preferences.keys();it.hasNext();){String scope=it.next();JSONObject group=preferences.getJSONObject(scope);
            require(Arrays.asList("pgrazor-map","pgrazor-overlays","pgrazor-saved-places-v1","pgrazor-livefeed").contains(scope),"Unknown settings group");
            for(Iterator<String> keys=group.keys();keys.hasNext();){String key=keys.next();Object value=group.get(key);
                if(scope.equals("pgrazor-map")){require(Arrays.asList("Stops","Gyms","Raids").contains(key),"Unknown map setting");bool(value,key);}
                else if(scope.equals("pgrazor-overlays")){require(Arrays.asList("cooldown.fx","cooldown.fy","vgp.fx","vgp.fy","transfer.fx","transfer.fy","gym-feed.fx","gym-feed.fy").contains(key),"Unknown overlay setting");number(value,0,1,key);}
                else if(scope.equals("pgrazor-saved-places-v1")){require(key.equals("places")&&value instanceof String,"Invalid favorites");places((String)value);}
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
