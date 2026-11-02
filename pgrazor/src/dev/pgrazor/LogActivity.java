package dev.pgrazor;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.widget.*;
import java.io.*;
import java.nio.charset.StandardCharsets;

/** Private activity declared in the host APK for user-selected log export. */
public final class LogActivity extends Activity {
    private TextView text;
    private SettingsBackup settingsBackup;
    @Override public void onBackPressed(){
        if(settingsBackup!=null&&settingsBackup.isBusy())return;
        super.onBackPressed();
    }
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        if(getIntent().getBooleanExtra("settingsBackup",false)){
            settingsBackup=new SettingsBackup(this);settingsBackup.show();return;
        }
        LinearLayout layout = new LinearLayout(this); layout.setOrientation(LinearLayout.VERTICAL); layout.setPadding(20,48,20,20);
        TextView heading = new TextView(this); heading.setText("PGRazor · current run"); layout.addView(heading);
        ScrollView scroll = new ScrollView(this); text = new TextView(this); text.setTextIsSelectable(true); scroll.addView(text); layout.addView(scroll,new LinearLayout.LayoutParams(-1,0,1));
        Button refresh = new Button(this); refresh.setText("Refresh log"); refresh.setOnClickListener(v -> refresh()); layout.addView(refresh);
        Button export = new Button(this); export.setText("Save .txt file"); layout.addView(export);
        export.setOnClickListener(v -> {
            File file = Control.currentLog(); if(file == null) return;
            Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("text/plain");
            intent.putExtra(Intent.EXTRA_TITLE,file.getName()); startActivityForResult(intent,40);
        });
        Button back = new Button(this); back.setText("Back to controls"); back.setOnClickListener(v -> finish()); layout.addView(back);
        MapTheme.style(layout); MapTheme.styleTitle(heading); heading.setPadding(0,0,0,24); MapTheme.page(this);
        // The log itself sits on a white card, like the game's panels.
        scroll.setBackground(MapTheme.cardBackground(this)); MapTheme.elevate(scroll,2); int cp=MapTheme.dp(this,MapTheme.Size.CARD_PADDING_DP); scroll.setPadding(cp,cp/2,cp,cp/2);
        setContentView(layout); refresh();
    }
    private void refresh() {
        File file = Control.currentLog();
        if(file == null) { text.setText("No log file is available. Android logcat may contain startup errors."); return; }
        try (RandomAccessFile input = new RandomAccessFile(file,"r")) {
            long start = Math.max(0,input.length()-65536); input.seek(start);
            byte[] bytes = new byte[(int)(input.length()-start)]; input.readFully(bytes);
            text.setText((start>0 ? "Showing latest 64 KiB. Export saves the complete run.\n" : "") + new String(bytes,StandardCharsets.UTF_8));
        } catch(IOException error) { text.setText("Cannot read log: "+error.getMessage()); }
    }
    protected void onActivityResult(int request,int result,Intent data) {
        super.onActivityResult(request,result,data);
        if(settingsBackup!=null){settingsBackup.result(request,result,data);return;}
        if(request!=40 || result!=RESULT_OK || data==null || data.getData()==null || Control.currentLog()==null) return;
        try {
            LogExport.save(this,data.getData(),Control.currentLog());
            Toast.makeText(this,"Log saved",Toast.LENGTH_SHORT).show();
        } catch(IOException error) { Toast.makeText(this,"Save failed: "+error.getMessage(),Toast.LENGTH_LONG).show(); }
    }
}
