package dev.pgrazor;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Bundle;
import android.view.Gravity;
import android.widget.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import org.json.*;

/** User-selected, bounded backups. Never enumerates account or game storage. */
final class SettingsBackup {
    private static final int EXPORT=71, IMPORT=72, LIMIT=512*1024;
    private static final String[] SCOPES={"pgrazor-map","pgrazor-overlays","pgrazor-saved-places-v1","pgrazor-livefeed"};
    private static final String[][] KEYS={{"Stops","Gyms","Raids"},{"cooldown.fx","cooldown.fy","vgp.fx","vgp.fy","transfer.fx","transfer.fy","shiny.fx","shiny.fy"},{"places"},{"category","radius","minIv","myShinies","filters_v1","filter_presets_v1"}};
    private final Activity activity;
    private TextView status;
    private Button export,restore,back;
    private boolean busy;
    SettingsBackup(Activity activity){this.activity=activity;}
    void show(){
        LinearLayout root=new LinearLayout(activity);root.setOrientation(LinearLayout.VERTICAL);
        int p=MapTheme.dp(activity,20);root.setPadding(p,p*3,p,p*2);
        TextView title=new TextView(activity);title.setText("Settings backup");title.setTextSize(22);root.addView(title);
        TextView detail=new TextView(activity);detail.setText("Save a backup outside the app to keep settings through reinstalls. Includes favorites and their coordinates. Login tokens, current location, cooldown history and active routes are excluded.\n\nImport replaces the included settings and favorites. VGP stays off.");detail.setPadding(0,p,0,p);root.addView(detail);
        export=new Button(activity);export.setText("Export settings");root.addView(export);
        restore=new Button(activity);restore.setText("Import settings");root.addView(restore);
        status=new TextView(activity);status.setPadding(0,p,0,p);root.addView(status);
        back=new Button(activity);back.setText("Back to game");back.setOnClickListener(v->activity.finish());root.addView(back);
        export.setOnClickListener(v->pick(EXPORT));restore.setOnClickListener(v->pick(IMPORT));
        ScrollView scroll=new ScrollView(activity);scroll.addView(root);MapTheme.style(scroll);activity.setContentView(scroll);
    }
    private void pick(int code){
        Intent i=new Intent(code==EXPORT?Intent.ACTION_CREATE_DOCUMENT:Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("application/json");
        if(code==EXPORT)i.putExtra(Intent.EXTRA_TITLE,"PGRazor-settings.json");
        try{activity.startActivityForResult(i,code);}catch(android.content.ActivityNotFoundException e){status.setText("No file picker is available.");}
    }
    void result(int request,int result,Intent data){
        if((request!=EXPORT&&request!=IMPORT)||result!=Activity.RESULT_OK||data==null||data.getData()==null)return;
        if(busy)return;busy=true;
        Uri uri=data.getData();export.setEnabled(false);restore.setEnabled(false);back.setEnabled(false);status.setText(request==EXPORT?"Saving backup...":"Restoring backup...");
        new Thread(()->{
            String message;
            try{
                if(request==EXPORT){
                    byte[] bytes=snapshot(activity).toString(2).getBytes(StandardCharsets.UTF_8);
                    if(bytes.length>LIMIT)throw new IOException("Backup is too large");
                    try(OutputStream out=activity.getContentResolver().openOutputStream(uri,"wt")){if(out==null)throw new IOException("Cannot open destination");out.write(bytes);}
                    message="Settings exported.";
                }else{
                    JSONObject document;
                    try(InputStream in=activity.getContentResolver().openInputStream(uri)){
                        if(in==null)throw new IOException("Cannot open backup");
                        ByteArrayOutputStream out=new ByteArrayOutputStream();byte[] buf=new byte[8192];int n;
                        while((n=in.read(buf))!=-1){if(out.size()+n>LIMIT)throw new IOException("Backup is too large");out.write(buf,0,n);}
                        document=new JSONObject(new String(out.toByteArray(),StandardCharsets.UTF_8));
                    }
                    restore(activity,document);message="Settings restored. Return to the game to use them. VGP is off.";
                }
            }catch(Exception e){message="Backup failed: "+(e.getMessage()==null?"Invalid file or storage unavailable":e.getMessage());}
            final String text=message;activity.runOnUiThread(()->{busy=false;if(activity.isFinishing()||activity.isDestroyed())return;status.setText(text);export.setEnabled(true);restore.setEnabled(true);back.setEnabled(true);});
        },"pgrazor-settings-backup").start();
    }
    boolean isBusy(){return busy;}
    private static synchronized JSONObject snapshot(Context context)throws Exception{
        JSONObject result=new JSONObject();result.put("format","PGRazor settings");result.put("version",1);result.put("speedKmh",Control.engine.speedKmh);
        JSONObject settings=new JSONObject();Properties p=Control.exportSettings();
        for(String key:SettingsBackupRules.SETTINGS)if(p.containsKey(key))settings.put(key,p.getProperty(key));result.put("settings",settings);
        JSONObject prefs=new JSONObject();for(int i=0;i<SCOPES.length;i++){
            Map<String,?> all=context.getSharedPreferences(SCOPES[i],0).getAll();JSONObject group=new JSONObject();
            for(String key:KEYS[i])if(all.containsKey(key))group.put(key,all.get(key));prefs.put(SCOPES[i],group);
        }result.put("preferences",prefs);SettingsBackupRules.validate(result);return result;
    }
    private static void writeGroup(Context c,String scope,JSONObject data,String[] keys)throws Exception{
        SharedPreferences.Editor edit=c.getSharedPreferences(scope,0).edit();for(String key:keys)edit.remove(key);
        for(String key:keys)if(data.has(key)){
            Object value=data.get(key);
            if(value instanceof Boolean)edit.putBoolean(key,(Boolean)value);
            else if(value instanceof Number){if(scope.equals("pgrazor-overlays"))edit.putFloat(key,((Number)value).floatValue());else edit.putInt(key,((Number)value).intValue());}
            else edit.putString(key,(String)value);
        }if(!edit.commit())throw new IOException("Could not save settings");
    }
    private static synchronized void restore(Context context,JSONObject document)throws Exception{
        SettingsBackupRules.validate(document);JSONObject previous=snapshot(context),prefs=document.getJSONObject("preferences");
        Properties props=new Properties();JSONObject settings=document.getJSONObject("settings");for(String key:SettingsBackupRules.SETTINGS)if(settings.has(key))props.setProperty(key,settings.getString(key));
        try{
            for(int i=0;i<SCOPES.length;i++)if(prefs.has(SCOPES[i]))writeGroup(context,SCOPES[i],prefs.getJSONObject(SCOPES[i]),KEYS[i]);
            Control.importSettings((android.app.Application)context.getApplicationContext(),props,document.getDouble("speedKmh"));
        }catch(Exception error){
            JSONObject old=previous.getJSONObject("preferences");for(int i=0;i<SCOPES.length;i++)try{writeGroup(context,SCOPES[i],old.getJSONObject(SCOPES[i]),KEYS[i]);}catch(Exception rollback){error.addSuppressed(rollback);}
            throw error;
        }
    }
}
