package dev.pgrazor;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.location.Location;
import android.view.Gravity;
import android.view.View;
import android.widget.*;
import java.io.*;

/**
 * Standalone preview of the PGRazor control surface: the same design tokens as the in-game panel,
 * plus a live synthetic-location probe. It never changes device GPS and never talks to the game.
 */
public final class PreviewActivity extends Activity {
    private final Handler handler = new Handler(Looper.getMainLooper());
    private TextView consumer, statusText, modeText;
    private SettingsUi.Dot statusDot;
    private int samples;
    public void onCreate(Bundle saved) {
        super.onCreate(saved);
        Control.begin(getApplication());
        boolean dark = SettingsUi.dark();
        getWindow().setStatusBarColor(SettingsUi.bg());
        getWindow().setNavigationBarColor(SettingsUi.bg());
        int flags = dark ? 0 : View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR | View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR;
        getWindow().getDecorView().setSystemUiVisibility(flags);
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true); scroll.setBackgroundColor(SettingsUi.bg()); scroll.setVerticalScrollBarEnabled(false);
        LinearLayout screen = new LinearLayout(this); screen.setOrientation(LinearLayout.VERTICAL);
        int pad = SettingsUi.dp(this, 20);
        screen.setPadding(pad, SettingsUi.dp(this, 24), pad, SettingsUi.dp(this, 24));
        scroll.addView(screen, new ScrollView.LayoutParams(-1, -2));

        // Masthead: a quiet monogram tile, the product name, and one honest line about the probe.
        LinearLayout brand = new LinearLayout(this); brand.setOrientation(LinearLayout.HORIZONTAL); brand.setGravity(Gravity.CENTER_VERTICAL);
        TextView mark = SettingsUi.text(this, "P", 16, SettingsUi.accent()); mark.setGravity(Gravity.CENTER);
        mark.setTypeface(SettingsUi.medium()); mark.setBackground(SettingsUi.accentPill(this));
        brand.addView(mark, new LinearLayout.LayoutParams(SettingsUi.dp(this, 36), SettingsUi.dp(this, 36)));
        TextView name = SettingsUi.text(this, BuildVersion.NAME, 11, SettingsUi.faint());
        name.setTypeface(SettingsUi.mono()); name.setPadding(SettingsUi.dp(this, 10), 0, 0, 0);
        brand.addView(name);
        screen.addView(brand);

        TextView heading = SettingsUi.text(this, "Control preview", (int) SettingsUi.TYPE_TITLE, SettingsUi.ink());
        heading.setTypeface(SettingsUi.medium());
        LinearLayout.LayoutParams headingParams = new LinearLayout.LayoutParams(-1, -2); headingParams.topMargin = SettingsUi.dp(this, 18);
        screen.addView(heading, headingParams);
        TextView description = SettingsUi.text(this,
            "Joystick and coordinate controls, drawn with the same tokens as the in-game panel.\n"
            + "This preview does not change device GPS or connect to Pokémon GO. Movement pauses when you leave the app.",
            14, SettingsUi.muted());
        description.setLineSpacing(0, 1.35f);
        LinearLayout.LayoutParams descriptionParams = new LinearLayout.LayoutParams(-1, -2); descriptionParams.topMargin = SettingsUi.dp(this, 6);
        screen.addView(description, descriptionParams);

        // Live probe card: status dot, synthetic coordinates, and the PASS/MISMATCH verdict.
        LinearLayout card = new LinearLayout(this); card.setOrientation(LinearLayout.VERTICAL);
        card.setBackground(SettingsUi.cardBackground(this)); card.setPadding(SettingsUi.dp(this, 16), SettingsUi.dp(this, 14), SettingsUi.dp(this, 16), SettingsUi.dp(this, 14));
        LinearLayout.LayoutParams cardParams = new LinearLayout.LayoutParams(-1, -2); cardParams.topMargin = SettingsUi.dp(this, 24);
        screen.addView(card, cardParams);
        LinearLayout statusRow = new LinearLayout(this); statusRow.setOrientation(LinearLayout.HORIZONTAL); statusRow.setGravity(Gravity.CENTER_VERTICAL);
        statusDot = new SettingsUi.Dot(this, SettingsUi.faint());
        statusRow.addView(statusDot, new LinearLayout.LayoutParams(SettingsUi.dp(this, 8), SettingsUi.dp(this, 8)));
        statusText = SettingsUi.text(this, "Waiting for the first sample", 12, SettingsUi.muted());
        LinearLayout.LayoutParams statusParams = new LinearLayout.LayoutParams(0, -2, 1f); statusParams.leftMargin = SettingsUi.dp(this, 8);
        statusRow.addView(statusText, statusParams);
        TextView label = SettingsUi.text(this, "SYNTHETIC CONSUMER", 11, SettingsUi.faint());
        label.setTypeface(SettingsUi.mono()); statusRow.addView(label);
        card.addView(statusRow);
        consumer = SettingsUi.text(this, "—", 20, SettingsUi.ink());
        consumer.setTypeface(SettingsUi.mono()); consumer.setSingleLine(true);
        LinearLayout.LayoutParams consumerParams = new LinearLayout.LayoutParams(-1, -2); consumerParams.topMargin = SettingsUi.dp(this, 12);
        card.addView(consumer, consumerParams);
        modeText = SettingsUi.text(this, "Reads the synthetic provider; the device location is untouched.", 11, SettingsUi.faint());
        modeText.setTypeface(SettingsUi.mono());
        LinearLayout.LayoutParams modeParams = new LinearLayout.LayoutParams(-1, -2); modeParams.topMargin = SettingsUi.dp(this, 10);
        card.addView(modeText, modeParams);

        Button export = SettingsUi.button(this, "Export this run's log (.txt)");
        LinearLayout.LayoutParams exportParams = new LinearLayout.LayoutParams(-1, SettingsUi.dp(this, 52)); exportParams.topMargin = SettingsUi.dp(this, 32);
        screen.addView(export, exportParams);
        export.setOnClickListener(v -> {
            Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT).setType("text/plain").addCategory(Intent.CATEGORY_OPENABLE);
            intent.putExtra(Intent.EXTRA_TITLE, Control.currentLog()==null?"pgrazor-log.txt":Control.currentLog().getName()); startActivityForResult(intent,40);
        });
        TextView exportNote = SettingsUi.text(this, "Writes a text file you choose. Nothing is uploaded.", 11, SettingsUi.faint());
        exportNote.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams noteParams = new LinearLayout.LayoutParams(-1, -2); noteParams.topMargin = SettingsUi.dp(this, 8);
        screen.addView(exportNote, noteParams);

        setContentView(scroll);
    }
    private final Runnable sample = new Runnable() {
        public void run() {
            Location value = new Location("pgrazor-synthetic-probe"); value.setLatitude(12.5); value.setLongitude(45.25); value.setSpeed(2); value.setBearing(30);
            double latitude = value.getLatitude(), longitude = value.getLongitude();
            boolean active = Control.active();
            boolean match = active ? Math.abs(latitude-Control.engine.latitude)<1e-8 && Math.abs(longitude-Control.engine.longitude)<1e-8 : latitude==12.5 && longitude==45.25;
            consumer.setText(latitude+", "+longitude);
            statusText.setText(match ? "Synthetic consumer passing" : "Synthetic consumer mismatch");
            statusDot.setColor(match ? SettingsUi.success() : SettingsUi.danger());
            String verdict=active?"override":"original values";
            modeText.setText((match?"PASS · ":"MISMATCH · ")+verdict);
            if(samples++ % 5 == 0) Control.event("probe-consumer", "active="+active+" match="+match+" latitude="+latitude+" longitude="+longitude);
            handler.postDelayed(this,1000);
        }
    };
    protected void onResume() { super.onResume(); handler.post(sample); }
    protected void onPause() { handler.removeCallbacks(sample); super.onPause(); }
    protected void onActivityResult(int request,int result,Intent data) {
        super.onActivityResult(request,result,data);
        if(request!=40 || result!=RESULT_OK || data==null || data.getData()==null || Control.currentLog()==null) return;
        try {
            LogExport.save(this,data.getData(),Control.currentLog());
            Toast.makeText(this,"Log exported",Toast.LENGTH_SHORT).show();
        } catch(IOException error) { Toast.makeText(this,"Export failed: "+error.getMessage(),Toast.LENGTH_LONG).show(); }
    }
}
