package org.json;

import java.util.HashMap;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Boundary shell: parses flat JSON objects whose values are quoted strings or
 * bare numeric tokens (the only shapes AutoRaid events use). The tests format
 * their own event strings; this shell is not a general JSON parser and is not
 * the subject under test. {@code getLong}/{@code getDouble} throw like the real
 * API when a key is missing or non-numeric; {@code opt*} fall back silently.
 */
public class JSONObject {
    private static final Pattern PAIR =
        Pattern.compile("\"([^\"]*)\"\\s*:\\s*(?:\"([^\"]*)\"|([^,}\\s]+))");
    private final Map<String, String> values = new HashMap<>();

    public JSONObject(String text) throws JSONException {
        if (text == null) throw new JSONException("null text");
        String trimmed = text.trim();
        if (!trimmed.startsWith("{") || !trimmed.endsWith("}")) throw new JSONException("not an object");
        Matcher m = PAIR.matcher(trimmed);
        while (m.find()) values.put(m.group(1), m.group(2) != null ? m.group(2) : m.group(3));
    }

    public long getLong(String key) throws JSONException {
        String v = values.get(key);
        if (v == null) throw new JSONException("missing " + key);
        try { return Long.parseLong(v); } catch (NumberFormatException e) { throw new JSONException("long " + key); }
    }

    public double getDouble(String key) throws JSONException {
        String v = values.get(key);
        if (v == null) throw new JSONException("missing " + key);
        try { return Double.parseDouble(v); } catch (NumberFormatException e) { throw new JSONException("double " + key); }
    }

    public int optInt(String key, int fallback) {
        String v = values.get(key);
        if (v == null) return fallback;
        try { return Integer.parseInt(v); } catch (NumberFormatException e) { return fallback; }
    }

    public String optString(String key, String fallback) {
        String v = values.get(key);
        return v == null ? fallback : v;
    }
}
