import dev.pgrazor.CountryLookup;
import java.io.FileInputStream;
import java.io.InputStream;

/** PGRazorX: offline country lookup against the bundled Natural Earth borders. */
public class CountryLookupTest {
    static void check(boolean ok, String message) { if (!ok) throw new AssertionError(message); }
    public static void main(String[] args) throws Exception {
        CountryLookup table;
        try (InputStream in = new FileInputStream(args[0])) { table = CountryLookup.read(in); }
        check(table.size() > 200, "borders loaded: " + table.size());
        String[][] places = {
            {"18.4861", "-69.9312", "DO"}, // Santo Domingo
            {"4.7110", "-74.0721", "CO"},  // Bogota
            {"6.2442", "-75.5812", "CO"},  // Medellin
            {"-16.5000", "-68.1500", "BO"}, // La Paz
            {"40.7128", "-74.0060", "US"}, // New York (coast)
            {"35.6762", "139.6503", "JP"}, // Tokyo
            {"47.2184", "-1.5536", "FR"},  // Nantes
            {"41.1579", "-8.6291", "PT"},  // Porto (coast)
            {"23.0", "120.2", "TW"},       // Tainan
            {"-33.8688", "151.2093", "AU"}, // Sydney (coast)
            {"1.3521", "103.8198", "SG"},  // Singapore
            {"51.5074", "-0.1278", "GB"},  // London
            {"-34.6037", "-58.3816", "AR"}, // Buenos Aires (river coast)
            {"19.4326", "-99.1332", "MX"}, // Mexico City
            {"-29.6000", "28.2000", "LS"},  // Lesotho inside South Africa
            {"-18.1248", "178.4501", "FJ"}, // Suva (Fiji crosses the antimeridian)
            {"64.7333", "177.5167", "RU"}, // Anadyr (Russia crosses the antimeridian)
            {"55.7558", "37.6173", "RU"},  // Moscow
        };
        for (String[] p : places) {
            String code = table.code(Double.parseDouble(p[0].replace(",", "").trim()), Double.parseDouble(p[1]));
            check(p[2].equals(code), p[2] + " expected, got " + code + " at " + p[0] + "," + p[1]);
        }
        check(table.code(0, -30) == null, "mid Atlantic is no country");
        check(table.code(Double.NaN, 0) == null && table.code(91, 0) == null, "invalid coordinates");
        check("🇩🇴".equals(table.flag(18.4861, -69.9312)), "Dominican flag emoji");
        check(table.flag(0, -30).isEmpty(), "no flag at sea");
        System.out.println("PASS: offline country borders: capitals, coastal cities, enclave, antimeridian, open sea, invalid input, flag emoji");
    }
}
