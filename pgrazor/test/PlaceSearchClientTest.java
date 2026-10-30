package dev.pgrazor;

import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.util.List;

public class PlaceSearchClientTest {
    static void check(boolean yes,String message) { if(!yes) throw new AssertionError(message); }
    interface Action { void run() throws IOException; }
    static void rejects(Action action) throws IOException {
        try { action.run(); } catch(IOException expected) { return; }
        throw new AssertionError("Expected rejection");
    }
    static String point(String name,String coordinates) {
        return "{\"geometry\":{\"type\":\"Point\",\"coordinates\":"+coordinates+"},\"properties\":{\"name\":\""+name+"\",\"city\":\"San Francisco\"}}";
    }
    public static void main(String[] args) throws Exception {
        String url=PlaceSearchClient.requestUrl("Pier 39 & parks",37.80867,-122.40981);
        check(url.contains("q=Pier+39+%26+parks")&&url.contains("lat=37.809&lon=-122.410"),"Escaped query and lat/lon bias");
        rejects(()->PlaceSearchClient.requestUrl(" ",0,0));
        rejects(()->PlaceSearchClient.requestUrl("park",Double.NaN,0));
        rejects(()->PlaceSearchClient.requestUrl("park",0,181));
        rejects(()->PlaceSearchClient.requestUrl(new String(new char[201]).replace('\0','a'),0,0));
        List<PlaceSearchClient.Place> places=PlaceSearchClient.parse("{\"features\":["+point("Pier 39","[-122.41,37.81]")+","+point("Pier 39","[-122.41,37.81]")+","+point("Bad","[200,91]")+",null]}");
        check(places.size()==1,"Duplicate and malformed results filtered");
        check(places.get(0).latitude==37.81&&places.get(0).longitude==-122.41,"GeoJSON lon/lat is correctly reversed");
        check(places.get(0).detail.equals("San Francisco"),"Result includes disambiguating city");
        check(PlaceSearchClient.parse("{\"features\":[]}").isEmpty(),"Valid empty result");
        rejects(()->PlaceSearchClient.parse("{\"error\":\"unavailable\"}"));
        rejects(()->PlaceSearchClient.parse("not json"));
        StringBuilder many=new StringBuilder("{\"features\":[");
        for(int i=0;i<50;i++){if(i>0)many.append(',');many.append(point("Park "+i,"[0,0]"));}
        many.append("]}");check(PlaceSearchClient.parse(many.toString()).size()==20,"Results capped");
        check(PlaceSearchClient.readBounded(new ByteArrayInputStream(new byte[16])).length==16,"Bounded body read");
        rejects(()->PlaceSearchClient.readBounded(new ByteArrayInputStream(new byte[PlaceSearchClient.MAX_BYTES+1])));
        PlaceSearchClient closed=new PlaceSearchClient();closed.close();rejects(()->closed.search("park",0,0));
        System.out.println("PASS: place search validation, URL encoding, coordinate order, malformed results, dedupe, limits and cancellation");
    }
}
