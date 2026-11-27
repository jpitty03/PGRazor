package dev.pgrazor;

import java.util.Arrays;
import java.util.HashMap;
import java.util.Map;
import java.util.function.IntConsumer;

/** Simulation of the reported live bug: rare Pokemon (e.g. Drampa), 0 Silver Pinap.
 *  Mirrors tag_catch.cpp rule packing + encounter_host.cpp with_rule_berry +
 *  encounter_berries.cpp stocked pick, for the installed (old) and fixed (new) paths. */
public final class TagBerryLiveBugSimulationTest {
    static final int RAZZ=701,NANAB=703,PINAP=705,GOLDEN=706,SILVER=707,LEGACY_SILVER=708;
    static String name(int id){switch(id){case RAZZ:return "Razz";case NANAB:return "Nanab";case PINAP:return "Pinap";case GOLDEN:return "GoldenRazz";case SILVER:return "SilverPinap";default:return "None";}}

    // tag_catch.cpp note_encounter: rare berry, Golden Razz, candy berry (deduped).
    static int[] rules(int rareBerry,int candyBerry,boolean rare,boolean candy){
        int[] cand=new int[3];int[] n={0};
        IntConsumer add=id->{if(id>0&&n[0]<3&&id!=cand[0]&&id!=cand[1])cand[n[0]++]=id;};
        if(rare){add.accept(rareBerry);add.accept(GOLDEN);}
        if(candy)add.accept(candyBerry);
        return Arrays.copyOf(cand,n[0]);
    }
    static int packed(int[] order){int p=0;for(int i=0;i<order.length;i++)p|=order[i]<<(10*i);return p;}
    static int[] unpack(int p){int[] r=new int[3];for(int i=0;i<3;i++)r[i]=(p>>(10*i))&1023;return r;}
    static Map<Integer,Integer> stock(Object... kv){Map<Integer,Integer> m=new HashMap<>();for(int i=0;i<kv.length;i+=2)m.put((Integer)kv[i],(Integer)kv[i+1]);return m;}
    static int count(Map<Integer,Integer> s,int id){return s.getOrDefault(id,0);}

    // OLD with_rule_berry: rule berries unfiltered, then user priorities (up to 3 slots).
    static int[] oldTransport(int p,int[] user){
        int[] out=new int[3];int n=0;
        for(int id:unpack(p))if(id>0&&n<3)out[n++]=id;
        int rules=n;
        for(int id:user){
            if(n>=3)break;boolean dup=false;
            for(int j=0;j<rules;j++)if(out[j]==id)dup=true;
            if(!dup)out[n++]=id;
        }
        return Arrays.copyOf(out,n);
    }
    // encounter_berries inventory stage: first stocked id in the transport list.
    static int pick(int[] ids,Map<Integer,Integer> live){for(int id:ids)if(id>0&&count(live,id)>0)return id;return 0;}
    // OLD BerryOverlay.shownBerry: first stocked rule, else Control.autoBerrySelected().
    static int oldOverlay(int p,int[] user,Map<Integer,Integer> cached){
        for(int id:unpack(p))if(id>0&&count(cached,id)>0)return id;
        for(int id:user)if(count(cached,id)>0)return id;
        return user.length==0?0:user[0];
    }
    // NEW note_encounter: primary rule unavailable -> no rule berries at all; the
    // player's 3-item priority order decides. Otherwise only stocked rules are packed.
    static int[] newRules(int[] order,Map<Integer,Integer> live){
        if(order.length==0||count(live,order[0])<=0)return new int[0];
        return Arrays.stream(order).filter(id->count(live,id)>0).toArray();
    }
    static int newPick(int[] rules,int[] user,Map<Integer,Integer> live){
        return newPick(rules,new int[0],user,live);
    }
    // Dedicated setting: used only when the configured rule berry is unavailable.
    static int[] effectiveRules(int[] rules,int[] dedicated,Map<Integer,Integer> live){
        if(rules.length==0||count(live,rules[0])>0)
            return Arrays.stream(rules).filter(id->count(live,id)>0).toArray();
        return Arrays.stream(dedicated).filter(id->count(live,id)>0).toArray();
    }
    static int newPick(int[] rules,int[] dedicated,int[] user,Map<Integer,Integer> live){
        return pick(BerryPriority.fallback(packed(effectiveRules(rules,dedicated,live)),user,id->count(live,id)),live);
    }
    static void scenario(String label,int rareBerry,int candyBerry,boolean rare,boolean candy,int[] user,Map<Integer,Integer> live){
        int[] rule=rules(rareBerry,candyBerry,rare,candy);
        int p=packed(rule);
        int oldNative=pick(oldTransport(p,user),live),oldUi=oldOverlay(p,user,live);
        int newNative=newPick(newRules(rule,live),user,live);
        System.out.println(label);
        System.out.println("  rules="+Arrays.toString(rule)+" userPriority="+Arrays.toString(user));
        System.out.println("  OLD installed: transport="+Arrays.toString(oldTransport(p,user))+" native="+name(oldNative)+" overlay="+name(oldUi));
        System.out.println("  NEW source  : rules="+Arrays.toString(newRules(rule,live))+" native="+name(newNative));
    }
    public static void main(String[] args){
        int[] user={PINAP,RAZZ,NANAB};
        scenario("1) Drampa rare, rare berry = Silver Pinap, 0 in stock; Golden Razz + Pinap available",
                 SILVER,PINAP,true,false,user,stock(SILVER,0,GOLDEN,3,PINAP,8,RAZZ,5));
        scenario("2) Drampa rare+candy, all rule berries out of stock; user priorities stocked",
                 SILVER,PINAP,true,true,user,stock(SILVER,0,GOLDEN,0,PINAP,0,RAZZ,5,NANAB,2));
        scenario("3) Drampa rare, default rare berry = Golden Razz out of stock",
                 GOLDEN,PINAP,true,false,user,stock(GOLDEN,0,PINAP,4,RAZZ,5));

        int[] dedicated={GOLDEN,PINAP};
        Map<Integer,Integer> live4=stock(SILVER,0,GOLDEN,3,PINAP,8,RAZZ,5);
        System.out.println("5) New setting: dedicated fallback GoldenRazz->Pinap, 0 Silver Pinap");
        System.out.println("   effective="+Arrays.toString(effectiveRules(rules(SILVER,PINAP,true,false),dedicated,live4))
                           +" -> native="+name(newPick(rules(SILVER,PINAP,true,false),dedicated,user,live4)));
        Map<Integer,Integer> live5=stock(SILVER,0,GOLDEN,0,PINAP,0,RAZZ,5);
        System.out.println("6) Dedicated fallback also unstocked -> global priority wins: "
                           +name(newPick(rules(SILVER,PINAP,true,false),dedicated,user,live5)));
        // The reported symptom: overlay preview stuck on None while old code had no rule stock.
        int p4=packed(rules(SILVER,PINAP,true,true));
        int[] noUser={};
        System.out.println("4) Empty berry priorities (preview None): OLD overlay="+name(oldOverlay(p4,noUser,stock()))
                           +" while native="+name(pick(oldTransport(p4,user),stock(PINAP,8))));

        // Assertions: unavailable rule must defer to the player's 3-item priority order.
        Map<Integer,Integer> live=stock(SILVER,0,GOLDEN,3,PINAP,8,RAZZ,5);
        int got=newPick(newRules(rules(SILVER,PINAP,true,false),live),user,live);
        if(got!=PINAP)throw new AssertionError("Expected user #1 Pinap, got "+name(got));
        Map<Integer,Integer> live2=stock(SILVER,0,GOLDEN,0,PINAP,0,RAZZ,5,NANAB,2);
        if(newPick(newRules(rules(SILVER,PINAP,true,true),live2),user,live2)!=RAZZ)
            throw new AssertionError("Expected user #2 Razz when Pinap unstocked");
        Map<Integer,Integer> live3=stock(SILVER,2,GOLDEN,3,PINAP,8);
        if(newPick(newRules(rules(SILVER,PINAP,true,true),live3),user,live3)!=SILVER)
            throw new AssertionError("Stocked configured rule must still win");
        // Legacy 708 saves are migrated to 707 by TagCatchSettings.berry() before reaching the rules.
        if(rules(LEGACY_SILVER,PINAP,true,false)[0]!=LEGACY_SILVER)throw new AssertionError("Legacy berry");
        if(newPick(rules(SILVER,PINAP,true,false),dedicated,user,live4)!=GOLDEN)
            throw new AssertionError("Dedicated fallback must win over global priority");
        System.out.println("PASS simulation: 0 Silver Pinap -> dedicated fallback, else user 3-berry order; stocked rule still wins");
    }
}
