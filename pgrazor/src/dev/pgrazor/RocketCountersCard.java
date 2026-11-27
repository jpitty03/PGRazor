package dev.pgrazor;

import android.app.Activity;
import android.os.SystemClock;
import android.view.Gravity;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.TextView;
import java.util.Locale;
import org.json.JSONObject;

/** Smart battling: the counters picked against a Team GO Rocket line-up, shown at the top of the screen. */
final class RocketCountersCard {
    private static final long SHOW_MS = 30000;
    private final Activity activity;
    private final FrameLayout parent;
    private final TextView card;
    private long hideAt;
    private boolean unavailable;
    private String searches="";
    private String cps="";

    RocketCountersCard(Activity activity, FrameLayout parent) {
        this.activity = activity; this.parent = parent;
        card = SettingsUi.text(activity, "", 14, SettingsUi.ink());
        int pad = SettingsUi.dp(activity, 12);
        card.setPadding(pad, pad, pad, pad); card.setBackground(SettingsUi.cardBackground(activity));
        card.setLineSpacing(0, 1.2f); card.setVisibility(View.GONE);
        card.setOnClickListener(v -> { hideAt = 0; card.setVisibility(View.GONE); });
        card.setOnLongClickListener(v -> {
            if (!cps.isEmpty()) {
                android.content.ClipboardManager cm=(android.content.ClipboardManager)activity.getSystemService(android.content.Context.CLIPBOARD_SERVICE);
                if(cm!=null)cm.setPrimaryClip(android.content.ClipData.newPlainText("Rocket counter CPs",cps));
                card.setText("Copied CPs: "+cps+"\n(tap to close)");
                hideAt=SystemClock.elapsedRealtime()+SHOW_MS;
            }
            return true;
        });
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(-1, -2, Gravity.TOP);
        lp.leftMargin = lp.rightMargin = SettingsUi.dp(activity, 16); lp.topMargin = SettingsUi.dp(activity, 140);
        parent.addView(card, lp);
    }

    void refresh(boolean allowed) {
        String text = null;
        if (!unavailable) try { text = NativeCore.rocketCounters(); } catch (UnsatisfiedLinkError e) { unavailable = true; }
        if (text != null) show(text);
        if (card.getVisibility() == View.VISIBLE && (!allowed || SystemClock.elapsedRealtime() >= hideAt)) card.setVisibility(View.GONE);
    }

    private void show(String text) {
        searches="";cps="";
        String[] lines = text.split("\n");
        if (lines.length < 2) return;
        JSONObject names = PokemonSpeciesDialog.readNames(activity);
        StringBuilder sb = new StringBuilder("Smart battling counters vs ").append(title(lines[0])).append(":");
        for (int i = 1; i < lines.length; i++) {
            String[] f = lines[i].split(",");
            if (f.length < 4) continue;
            String name = names == null ? "" : names.optString(f[0], "");
            if (name.isEmpty()) name = "#" + f[0];
            String nick=f.length>4?f[4]:"";
            sb.append("\n").append(i).append(". ").append(f[3].equals("1") ? "Shadow " : "").append(name)
              .append(nick.isEmpty()?"":" \""+nick+"\"").append("  ").append(f[2]).append(" CP");
            String term=name+" cp"+f[2];
            searches+=(searches.isEmpty()?"":",")+term;
            cps+=(cps.isEmpty()?"":", ")+"CP"+f[2];
            sb.append("\n    filter: ").append(term);
        }
        sb.append("\nPick them with \"Tap to swap Pokémon\". Tap to close.");
        card.setText(sb); card.setTextColor(SettingsUi.ink()); card.setBackground(SettingsUi.cardBackground(activity));
        card.setVisibility(View.VISIBLE); hideAt = SystemClock.elapsedRealtime() + SHOW_MS;
    }

    /** "female-fire-grunt" -> "Female Fire Grunt". */
    private static String title(String key) {
        if (key.isEmpty()) return "Team GO Rocket";
        StringBuilder sb = new StringBuilder();
        for (String w : key.split("-")) {
            if (w.isEmpty()) continue;
            if (sb.length() > 0) sb.append(' ');
            sb.append(Character.toUpperCase(w.charAt(0))).append(w.substring(1).toLowerCase(Locale.US));
        }
        return sb.toString().replace("Metal", "Steel");
    }

    void close() { parent.removeView(card); }
}
