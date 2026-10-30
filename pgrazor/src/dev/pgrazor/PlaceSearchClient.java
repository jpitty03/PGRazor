package dev.pgrazor;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;

/** Explicit, bounded place searches. No game state, credentials, or movement access. */
public final class PlaceSearchClient {
    static final int LIMIT = 20, MAX_BYTES = 512 * 1024;
    private static final String ENDPOINT = "https://photon.komoot.io/api/";
    private static long nextRequest;
    private static final LinkedHashMap<String, Cached> CACHE = new LinkedHashMap<>(24, .75f, true);
    private static final class Cached {
        final List<Place> places; final long at;
        Cached(List<Place> places, long at) { this.places=places; this.at=at; }
    }
    public static final class Place {
        public final String name, detail;
        public final double latitude, longitude;
        Place(String name, String detail, double latitude, double longitude) {
            this.name=name; this.detail=detail; this.latitude=latitude; this.longitude=longitude;
        }
    }
    private boolean closed;
    private HttpURLConnection active;

    static boolean valid(double lat, double lon) {
        return Double.isFinite(lat) && Double.isFinite(lon) && Math.abs(lat)<=90 && Math.abs(lon)<=180;
    }
    static String requestUrl(String query, double lat, double lon) throws IOException {
        if (query==null || query.trim().length()<2 || query.trim().length()>200 || !valid(lat,lon))
            throw new IOException("Enter a place name or address (2 to 200 characters).");
        // Round the viewed-map bias; do not send the device/account location.
        return ENDPOINT+"?q="+URLEncoder.encode(query.trim(),"UTF-8")
            +String.format(Locale.US,"&lat=%.3f&lon=%.3f&limit=%d",lat,lon,LIMIT);
    }
    public List<Place> search(String query, double lat, double lon) throws IOException {
        String url=requestUrl(query,lat,lon);
        synchronized(this) { if(closed) throw new IOException("Search closed."); }
        long now=System.nanoTime();
        synchronized(CACHE) {
            Cached cached=CACHE.get(url);
            if(cached!=null && now-cached.at<600_000_000_000L) return cached.places;
            if(now<nextRequest) throw new IOException("Please wait before searching again.");
            nextRequest=now+1_000_000_000L;
        }
        HttpURLConnection http=(HttpURLConnection)new URL(url).openConnection();
        synchronized(this) { if(closed) { http.disconnect(); throw new IOException("Search closed."); } active=http; }
        try {
            http.setConnectTimeout(8000); http.setReadTimeout(8000);
            http.setInstanceFollowRedirects(false);
            http.setRequestProperty("User-Agent","PGRazor-PlaceSearch/1.0 (+https://github.com/jpitty03/PGRazor)");
            http.setRequestProperty("Accept","application/json");
            http.setRequestProperty("Accept-Language","en");
            int code=http.getResponseCode();
            if(code==429 || code==503) {
                long seconds=60;
                try { seconds=Math.max(seconds,Math.min(3600,Long.parseLong(http.getHeaderField("Retry-After")))); }
                catch(Exception ignored) {}
                synchronized(CACHE) { nextRequest=System.nanoTime()+seconds*1_000_000_000L; }
                throw new IOException("Place search is busy. Please try again later.");
            }
            if(code!=200) throw new IOException("Place search is unavailable. Please try again later.");
            if(http.getContentLength()>MAX_BYTES) throw new IOException("Search response is too large.");
            byte[] body;
            try(InputStream input=http.getInputStream()) { body=readBounded(input); }
            List<Place> places=parse(new String(body,StandardCharsets.UTF_8));
            synchronized(this) { if(closed) throw new IOException("Search closed."); }
            synchronized(CACHE) {
                CACHE.put(url,new Cached(places,System.nanoTime()));
                while(CACHE.size()>20) CACHE.remove(CACHE.keySet().iterator().next());
            }
            return places;
        } finally {
            synchronized(this) { if(active==http) active=null; }
            http.disconnect();
        }
    }
    static byte[] readBounded(InputStream input) throws IOException {
        ByteArrayOutputStream out=new ByteArrayOutputStream(); byte[] buffer=new byte[4096]; int count;
        long started=System.nanoTime();
        while((count=input.read(buffer))!=-1) {
            if(Thread.currentThread().isInterrupted()) throw new IOException("Search cancelled.");
            if(System.nanoTime()-started>20_000_000_000L) throw new IOException("Search timed out.");
            if(out.size()+count>MAX_BYTES) throw new IOException("Search response is too large.");
            out.write(buffer,0,count);
        }
        return out.toByteArray();
    }
    private static String text(JSONObject props,String key) {
        Object value=props.opt(key); if(!(value instanceof String)) return "";
        String s=((String)value).replaceAll("[\\p{Cntrl}]"," ").trim();
        return s.length()>160?s.substring(0,160):s;
    }
    static List<Place> parse(String json) throws IOException {
        try {
            JSONArray features=new JSONObject(json).optJSONArray("features");
            if(features==null) throw new IOException("Invalid place search response.");
            List<Place> places=new ArrayList<>(); LinkedHashSet<String> seen=new LinkedHashSet<>();
            for(int i=0;i<Math.min(features.length(),200) && places.size()<LIMIT;i++) {
                JSONObject feature=features.optJSONObject(i);
                if(feature==null) continue;
                JSONObject geometry=feature.optJSONObject("geometry"),props=feature.optJSONObject("properties");
                if(geometry==null || props==null || !"Point".equals(geometry.optString("type"))) continue;
                JSONArray coordinates=geometry.optJSONArray("coordinates");
                if(coordinates==null || coordinates.length()<2) continue;
                double lon=coordinates.optDouble(0,Double.NaN),lat=coordinates.optDouble(1,Double.NaN);
                if(!valid(lat,lon)) continue;
                String name=text(props,"name");
                if(name.isEmpty()) name=(text(props,"housenumber")+" "+text(props,"street")).trim();
                if(name.isEmpty()) name=text(props,"city");
                if(name.isEmpty()) continue;
                String identity=name+"|"+lat+"|"+lon;
                if(!seen.add(identity)) continue;
                LinkedHashSet<String> parts=new LinkedHashSet<>();
                String street=(text(props,"housenumber")+" "+text(props,"street")).trim();
                if(!street.isEmpty()&&!street.equals(name)) parts.add(street);
                for(String key:new String[]{"district","city","state","country"}) {
                    String part=text(props,key); if(!part.isEmpty()&&!part.equals(name)) parts.add(part);
                }
                places.add(new Place(name,String.join(", ",parts),lat,lon));
            }
            return Collections.unmodifiableList(places);
        } catch(JSONException e) { throw new IOException("Invalid place search response.",e); }
    }
    public void close() {
        HttpURLConnection http;
        synchronized(this) { closed=true; http=active; active=null; }
        if(http!=null) http.disconnect();
    }
}
