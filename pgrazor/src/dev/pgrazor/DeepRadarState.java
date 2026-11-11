package dev.pgrazor;

import org.json.JSONArray;
import org.json.JSONObject;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/** Immutable, bounded radar snapshots. No Android, persistence or network code. */
public final class DeepRadarState {
    public static final class Row {
        public final String encounter, cell, kind, fort;
        public final int species,form,costume,gender,uncaught;
        public final long seenAt,expiresAt;
        public final double lat,lon;
        private final double[][] corners;
        Row(JSONObject o,long now)throws Exception {
            encounter=id(o.get("encounter"),false);cell=id(o.get("cell"),true);
            kind=o.getString("kind");require(kind.equals("exact")||kind.equals("fort")||kind.equals("cell")||kind.equals("unlocated"));
            species=integer(o,"species",1,100000);form=integer(o,"form",0,Integer.MAX_VALUE);costume=integer(o,"costume",0,Integer.MAX_VALUE);gender=integer(o,"gender",0,Integer.MAX_VALUE);uncaught=integer(o,"uncaught",-1,1);
            seenAt=positive(o,"seenAt");expiresAt=positive(o,"expiresAt");require(seenAt<=now+1000&&expiresAt>=seenAt&&expiresAt-seenAt<=90000);
            lat=o.getDouble("lat");lon=o.getDouble("lon");coordinate(lat,lon);fort=o.getString("fort");require(fort.length()<=256);
            JSONArray a=o.getJSONArray("corners");require(a.length()==(kind.equals("cell")?4:0));corners=new double[a.length()][2];
            for(int i=0;i<a.length();i++){JSONArray p=a.getJSONArray(i);require(p.length()==2);corners[i][0]=p.getDouble(0);corners[i][1]=p.getDouble(1);coordinate(corners[i][0],corners[i][1]);}
            if(kind.equals("cell")){long bits=Long.parseUnsignedLong(cell);require((bits>>>61)<6&&(bits&0x7fffffffL)==0x40000000L);}
            if(kind.equals("fort"))require(!fort.isEmpty());
        }
        public boolean located(){return !kind.equals("unlocated");}
        public int cornerCount(){return corners.length;}
        public double cornerLat(int i){return corners[i][0];}
        public double cornerLon(int i){return corners[i][1];}
        public String locationLabel(){return kind.equals("exact")?"Exact spawn location":kind.equals("fort")?"Near this PokéStop":kind.equals("cell")?"Approximate S2 cell":"Location not supplied";}
    }
    public static final class Snapshot {
        public final long generation,sequence,sampledAt;
        public final String status,message;
        public final int packets,cells,nearby,offstop,wild,catchable;
        public final List<Row> rows;
        Snapshot(long g,long s,long at,String state,String message,int packets,int cells,int nearby,int offstop,int wild,int catchable,List<Row> rows){generation=g;sequence=s;sampledAt=at;status=state;this.message=message;this.packets=packets;this.cells=cells;this.nearby=nearby;this.offstop=offstop;this.wild=wild;this.catchable=catchable;this.rows=Collections.unmodifiableList(rows);}
    }
    private Snapshot snapshot;
    private long generation,sequence,notBefore;
    public synchronized void clear(long now){snapshot=null;notBefore=now;}
    public synchronized boolean accept(String text,long now){
        try {
            require(text!=null&&text.length()<=1048576&&now>=0);
            JSONObject o=new JSONObject(text);require(integer(o,"version",1,1)==1);
            long g=positive(o,"generation"),s=positive(o,"sequence"),at=positive(o,"sampledAt");
            if(g<generation||(g==generation&&s<=sequence)||at<notBefore)return false;
            require(at<=now+1000&&now-at<90000);
            String status=o.getString("status"),message=o.getString("message");require(status.equals("waiting")||status.equals("ready")||status.equals("unsupported")||status.equals("stale")||status.equals("overflow"));require(message.length()<=256);
            JSONArray a=o.getJSONArray("rows");require(a.length()<=1024);List<Row> rows=new ArrayList<>(a.length());Set<String> ids=new HashSet<>();
            for(int i=0;i<a.length();i++){Row row=new Row(a.getJSONObject(i),now);require(ids.add(row.encounter));if(row.expiresAt>now)rows.add(row);}
            int packets=integer(o,"packets",0,Integer.MAX_VALUE),cells=integer(o,"cells",0,128),nearby=integer(o,"nearby",0,2048),offstop=integer(o,"offstop",0,2048),wild=integer(o,"wild",0,2048),catchable=integer(o,"catchable",0,2048);
            if(status.equals("unsupported")||status.equals("stale"))rows.clear();
            snapshot=new Snapshot(g,s,at,status,message,packets,cells,nearby,offstop,wild,catchable,rows);generation=g;sequence=s;return true;
        }catch(Exception ignored){snapshot=null;return false;}
    }
    public synchronized Snapshot get(long now){if(snapshot==null||now<snapshot.sampledAt-1000||now-snapshot.sampledAt>=90000)return null;return snapshot;}
    public static boolean visible(Row r,long now,Set<Integer> species,boolean unregistered){return r.expiresAt>now&&(species.isEmpty()||species.contains(r.species))&&(!unregistered||r.uncaught==1);}
    public static Set<Integer> speciesFilter(String value){
        Set<Integer> result=new HashSet<>();if(value==null||value.trim().isEmpty())return result;
        if(value.length()>4096)throw new IllegalArgumentException("Species filter is too long");
        for(String part:value.split(",",-1)){int n;try{n=Integer.parseInt(part.trim());}catch(Exception e){throw new IllegalArgumentException("Enter Pokédex numbers separated by commas");}if(n<1||n>100000||result.size()>=256)throw new IllegalArgumentException("Invalid species filter");result.add(n);}return result;
    }
    private static void require(boolean b){if(!b)throw new IllegalArgumentException("Invalid radar snapshot");}
    private static long positive(JSONObject o,String key)throws Exception {Object v=o.get(key);require(v instanceof Integer||v instanceof Long);long n=((Number)v).longValue();require(n>0);return n;}
    private static int integer(JSONObject o,String key,int min,int max)throws Exception {Object v=o.get(key);require(v instanceof Integer||v instanceof Long);long n=((Number)v).longValue();require(n>=min&&n<=max);return(int)n;}
    private static String id(Object v,boolean zero){require(v instanceof String);String s=(String)v;require(s.length()>0&&s.length()<=20&&s.matches("0|[1-9][0-9]*"));Long.parseUnsignedLong(s);require(zero||!s.equals("0"));return s;}
    private static void coordinate(double lat,double lon){require(Double.isFinite(lat)&&Double.isFinite(lon)&&Math.abs(lat)<=90&&Math.abs(lon)<=180);}
}
