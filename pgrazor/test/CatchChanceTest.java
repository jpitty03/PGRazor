package dev.pgrazor;

import org.json.JSONObject;

public final class CatchChanceTest {
    private static void require(boolean ok, String what) { if (!ok) throw new AssertionError(what); }

    public static void main(String[] args) throws Exception {
        JSONObject o = new JSONObject("{\"phase\":\"encounter\",\"active\":true,\"show\":true,\"auto\":true,\"target\":75,\"current\":2,\"pick\":2,"
            + "\"berry\":701,\"throw\":\"Excellent\",\"curve\":true,\"balls\":["
            + "{\"item\":1,\"chance\":0.4123,\"count\":30},{\"item\":2,\"chance\":0.555,\"count\":12},{\"item\":3,\"chance\":0.6649,\"count\":0}]}");
        String text = CatchChance.describe(o);
        require(text.equals("Poké 41% · ▸Great 56% · Ultra 66%"), text);

        JSONObject plain = new JSONObject("{\"active\":true,\"show\":true,\"current\":1,\"berry\":0,\"throw\":\"\",\"curve\":false,"
            + "\"balls\":[{\"item\":1,\"chance\":0.2,\"count\":5}]}");
        require(CatchChance.describe(plain).equals("▸Poké 20%"), CatchChance.describe(plain));
        JSONObject curveOnly = new JSONObject("{\"current\":0,\"berry\":706,\"throw\":\"\",\"curve\":true,\"balls\":[{\"item\":3,\"chance\":0.9}]}");
        require(CatchChance.describe(curveOnly).equals("Ultra 90%"), CatchChance.describe(curveOnly));
        JSONObject throwOnly = new JSONObject("{\"current\":1,\"berry\":0,\"throw\":\"Excellent\",\"curve\":false,\"auto\":true,\"pick\":1,\"target\":85,"
            + "\"balls\":[{\"item\":1,\"chance\":0.91}]}");
        require(CatchChance.describe(throwOnly).equals("▸Poké 91%"), CatchChance.describe(throwOnly));
        require(CatchChance.describe(new JSONObject("{\"balls\":[]}")).isEmpty(), "no balls, no text");
        JSONObject safari = new JSONObject("{\"current\":7,\"balls\":[{\"item\":7,\"name\":\"Safari\",\"chance\":0.8}]}");
        require(CatchChance.describe(safari).equals("\u25b8Safari 80%"), "event ball uses resolved name");

        CatchChance.publish(o.toString());
        require(CatchChance.visible() && CatchChance.text().startsWith("Poké 41%"), "published encounter is shown");
        CatchChance.publish("{\"phase\":\"encounter\",\"active\":false}");
        require(!CatchChance.visible(), "closed encounter hides the overlay");
        CatchChance.publish("{\"phase\":\"encounter\",\"active\":true,\"show\":false,\"balls\":[{\"item\":1,\"chance\":0.5}]}");
        require(!CatchChance.visible(), "show off hides the overlay (auto ball only)");
        System.out.println("CatchChanceTest passed");
    }
}
