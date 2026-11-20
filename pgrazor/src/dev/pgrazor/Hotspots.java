package dev.pgrazor;

import java.text.SimpleDateFormat;
import java.util.Arrays;
import java.util.Collections;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.TimeZone;

/** Curated locations supplied by the user, each with its time zone for the local date and time. */
public final class Hotspots {
    private Hotspots() {}
    public static final class Spot {
        public final String country, city, name;
        public final double latitude, longitude;
        public final TimeZone zone;
        Spot(String zone,String country,String city,String name,double latitude,double longitude) {
            this.zone=timeZone(zone);this.country=country;this.city=city;this.name=name;
            this.latitude=latitude;this.longitude=longitude;
        }
        /** The spot's date, time and current UTC offset at {@code nowMs}, e.g. "Fri, Nov 20 · 9:00 PM · UTC+9". */
        public String local(long nowMs,boolean hours24) {
            SimpleDateFormat format=new SimpleDateFormat(hours24?"EEE, MMM d · HH:mm":"EEE, MMM d · h:mm a",Locale.US);
            format.setTimeZone(zone);
            return format.format(new Date(nowMs))+" · "+offset(zone.getOffset(nowMs));
        }
    }
    // Older Android time zone data only knows Nuuk by its former name.
    private static TimeZone timeZone(String id) {
        TimeZone zone=TimeZone.getTimeZone(id);
        if(zone.getID().equals("GMT")&&id.equals("America/Nuuk"))zone=TimeZone.getTimeZone("America/Godthab");
        return zone;
    }
    static String offset(int ms) {
        int minutes=Math.abs(ms)/60000;
        String label="UTC"+(ms<0?"-":"+")+minutes/60;
        return minutes%60==0?label:label+String.format(Locale.US,":%02d",minutes%60);
    }
    public static final List<Spot> ALL=Collections.unmodifiableList(Arrays.asList(
        new Spot("Pacific/Honolulu","Hawaii","Honolulu","Moana Mall",21.291100,-157.843600),
        new Spot("America/Los_Angeles","USA","San Francisco","Pier 39",37.808600,-122.409800),
        new Spot("America/Mexico_City","Mexico","Mexico City","Alameda Central",19.43585,-99.14434),
        new Spot("America/Chicago","USA","Chicago","Lincoln Zoo Park",41.918776,-87.635437),
        new Spot("America/New_York","USA","New York","Bryant Park",40.753751,-73.983537),
        new Spot("America/Sao_Paulo","Brasil","São Paulo","Ibirapuera Park",-23.5864600,-46.6581793),
        new Spot("America/Nuuk","Greenland","Nuuk","Downtown",64.175642,-51.737517),
        new Spot("Europe/London","UK","London","Trafalgar Square",51.5080,-0.1281),
        new Spot("Europe/Madrid","España","Zaragoza","Plaza de Europa",41.659268,-0.892103),
        new Spot("Europe/Istanbul","Türkiye","Bornova","Buyuk Park",38.462677,27.217599),
        new Spot("Asia/Dubai","UAE","Dubai","Jumeirah Beach",25.074792,55.130958),
        new Spot("Asia/Taipei","Taiwan","Taipei","Main Station",25.0480,121.5168),
        new Spot("Asia/Tokyo","Japan","Tokyo","Ikebukuro Station",35.729503,139.710900),
        new Spot("Pacific/Auckland","New Zealand","Wellington","Botanic Garden",-41.282348,174.767127)
    ));
}
