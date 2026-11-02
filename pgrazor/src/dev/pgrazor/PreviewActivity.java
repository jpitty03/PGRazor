package dev.pgrazor;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.location.Location;
import android.widget.*;
import java.io.*;

public final class PreviewActivity extends Activity {
    private final Handler handler = new Handler(Looper.getMainLooper());
    private TextView consumer;
    private int samples;
    public void onCreate(Bundle saved) {
        super.onCreate(saved);
        LinearLayout screen = new LinearLayout(this); screen.setOrientation(LinearLayout.VERTICAL); screen.setPadding(24,48,24,24); screen.setBackground(MapTheme.pageBackground(this));
        TextView heading = new TextView(this); heading.setText("PGRazor · control preview"); MapTheme.styleTitle(heading); screen.addView(heading);
        TextView description = new TextView(this); description.setText("Joystick and coordinate controls.\nThis preview does not change device GPS or connect to Pokémon GO.\nMovement pauses when you leave the app."); description.setTextSize(15); description.setTextColor(MapTheme.muted()); screen.addView(description);
        Space space = new Space(this); screen.addView(space,new LinearLayout.LayoutParams(1,0,1));
        consumer = new TextView(this); consumer.setTextColor(MapTheme.ink()); consumer.setTextSize(16); consumer.setBackground(MapTheme.cardBackground(this)); consumer.setPadding(32,24,32,24); screen.addView(consumer);
        Button export = new Button(this); export.setText("Export this run's log (.txt)"); screen.addView(export);
        export.setOnClickListener(v -> {
            Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT).setType("text/plain").addCategory(Intent.CATEGORY_OPENABLE);
            intent.putExtra(Intent.EXTRA_TITLE, Control.currentLog()==null?"pgrazor-log.txt":Control.currentLog().getName()); startActivityForResult(intent,40);
        }); MapTheme.style(export); MapTheme.page(this); setContentView(screen); Control.begin(getApplication());
    }
    private final Runnable sample = new Runnable() {
        public void run() {
            Location value = new Location("pgrazor-synthetic-probe"); value.setLatitude(12.5); value.setLongitude(45.25); value.setSpeed(2); value.setBearing(30);
            double latitude = value.getLatitude(), longitude = value.getLongitude();
            boolean active = Control.active();
            boolean match = active ? Math.abs(latitude-Control.engine.latitude)<1e-8 && Math.abs(longitude-Control.engine.longitude)<1e-8 : latitude==12.5 && longitude==45.25;
            consumer.setText("Synthetic location consumer\n"+latitude+", "+longitude+"\n"+(match?"PASS":"MISMATCH")+" · "+(active?"override":"original values"));
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
