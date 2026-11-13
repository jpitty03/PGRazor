package org.json;

/** Checked exception matching org.json's signature; AutoRaid catches it as Exception. */
public class JSONException extends Exception {
    public JSONException(String message) { super(message); }
}
