package dev.pgrazor;
import java.util.HashSet;
public class HotspotsTest {
    static void check(boolean value){if(!value)throw new AssertionError();}
    public static void main(String[] args){
        check(Hotspots.ALL.size()==14);
        double[][] expected={{21.291100,-157.843600},{37.808600,-122.409800},{19.43585,-99.14434},{41.918776,-87.635437},{40.753751,-73.983537},{-23.5864600,-46.6581793},{64.175642,-51.737517},{51.5080,-0.1281},{41.659268,-0.892103},{38.462677,27.217599},{25.074792,55.130958},{25.0480,121.5168},{35.729503,139.710900},{-41.282348,174.767127}};
        HashSet<String> names=new HashSet<>();
        for(int i=0;i<14;i++){
            Hotspots.Spot spot=Hotspots.ALL.get(i);
            check(CooldownState.valid(spot.latitude,spot.longitude));
            check(spot.latitude==expected[i][0]&&spot.longitude==expected[i][1]);
            check(names.add(spot.city+spot.name));
        }
        try{Hotspots.ALL.clear();throw new AssertionError();}catch(UnsupportedOperationException expectedError){}
        CooldownState state=new CooldownState();long now=100000000;
        Hotspots.Spot sf=Hotspots.ALL.get(1),tokyo=Hotspots.ALL.get(12);
        state.add(new CooldownState.Entry(1,now-3600000,sf.latitude,sf.longitude,"Spin"));
        check(state.remaining(now,tokyo.latitude,tokyo.longitude)==3600000);
        check(state.remaining(now,sf.latitude,sf.longitude)==0);
        check(state.rows.size()==1); // Previewing a destination does not record movement.
        System.out.println("PASS: 14 exact supplied hotspot coordinates, immutable catalog, destination cooldown uses elapsed action history");
    }
}
