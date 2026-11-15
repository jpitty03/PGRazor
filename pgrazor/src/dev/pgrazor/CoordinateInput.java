package dev.pgrazor;

/** Decimal latitude/longitude separated by a comma or by spaces, independent of device locale. */
final class CoordinateInput {
    private CoordinateInput() {}
    static double[] parse(String input) {
        String text=input==null?"":input.trim();
        // "lat, lon" or, as copied from many maps and chats, "lat lon".
        String[] parts=text.indexOf(',')>=0?text.split(",",-1):text.split("\\s+",-1);
        if(parts.length!=2)throw new IllegalArgumentException("Enter latitude, longitude");
        double[] point=new double[2];
        for(int i=0;i<2;i++){
            String value=parts[i].trim();
            if(!value.matches("[+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)"))
                throw new IllegalArgumentException("Enter numeric latitude, longitude");
            point[i]=Double.parseDouble(value);
        }
        if(!Double.isFinite(point[0])||Math.abs(point[0])>90)throw new IllegalArgumentException("Latitude must be between -90 and 90");
        if(!Double.isFinite(point[1])||Math.abs(point[1])>180)throw new IllegalArgumentException("Longitude must be between -180 and 180");
        return point;
    }
    /** The coordinates in a search box entry, or null when the text is something else (a place name). */
    static double[] find(String input) {
        try{return parse(input);}catch(IllegalArgumentException notCoordinates){return null;}
    }
}
