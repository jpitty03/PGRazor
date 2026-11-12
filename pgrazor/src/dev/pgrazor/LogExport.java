package dev.pgrazor;

import android.content.Context;
import android.net.Uri;
import java.io.*;
import java.security.*;
import java.util.Arrays;
import java.util.Properties;
import java.util.TreeSet;
import java.util.Map;
import java.nio.charset.StandardCharsets;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;
import org.json.JSONObject;
import org.json.JSONException;

/** Verify exports through the document URI; its backing folder is provider-owned. */
public final class LogExport {
    public static void save(Context context,Uri uri,File file) throws IOException {
        save(context,uri,file,false);
    }
    static byte[] settingsJson(Properties preferences,double speedKmh) throws IOException {
        return settingsJson(preferences,speedKmh,new JSONObject());
    }
    private static byte[] settingsJson(Properties preferences,double speedKmh,JSONObject groups) throws IOException {
        try {
            JSONObject settings=new JSONObject();
            for(String key:new TreeSet<>(preferences.stringPropertyNames()))settings.put(key,preferences.getProperty(key));
            JSONObject root=new JSONObject();
            root.put("format","PGRazor diagnostic settings");root.put("version",1);
            root.put("capturedAtEpochMs",System.currentTimeMillis());
            root.put("speedKmh",speedKmh);root.put("settings",settings);
            root.put("preferences",groups);
            return root.toString().getBytes(StandardCharsets.UTF_8);
        } catch(JSONException error) { throw new IOException("Cannot capture settings",error); }
    }
    private static JSONObject preferenceGroups(Context context)throws IOException {
        String[] scopes={"pgrazor-map","pgrazor-overlays","pgrazor-livefeed","pgrazor-pogofeed"};
        String[][] keys={{"Stops","Gyms","Raids"},
            {"cooldown.fx","cooldown.fy","vgp.fx","vgp.fy","transfer.fx","transfer.fy","gym-feed.fx","gym-feed.fy","local-time.fx","local-time.fy"},
            {"category","radius","minIv","myShinies","filters_v1"},
            {"feedMode","radiusKm","refreshIntervalIndex","filters_v1"}};
        try {
            JSONObject groups=new JSONObject();
            for(int i=0;i<scopes.length;i++) {
                Map<String,?> all=AccountProfiles.preferences(context,scopes[i]).getAll();
                JSONObject group=new JSONObject();
                for(String key:keys[i])if(all.containsKey(key))group.put(key,all.get(key));
                groups.put(scopes[i],group);
            }
            JSONObject events=new JSONObject();
            events.put("halloween_2026.box_collector",dev.pgrazor.events.EventSettings.boxCollectorRequested(context));
            groups.put("pgrazor-events-v1",events);
            return groups;
        } catch(JSONException error) { throw new IOException("Cannot capture preferences",error); }
    }
    static void bundle(InputStream log,String logName,byte[] settings,OutputStream destination) throws IOException {
        // The caller owns the provider stream; close the compressor without closing that stream.
        try(ZipOutputStream zip=new ZipOutputStream(new FilterOutputStream(destination) {
            @Override public void close()throws IOException{flush();}
        })) {
            zip.putNextEntry(new ZipEntry(logName));
            byte[] buffer=new byte[8192];int n;
            while((n=log.read(buffer))!=-1)zip.write(buffer,0,n);
            zip.closeEntry();zip.putNextEntry(new ZipEntry("settings.json"));
            zip.write(settings);zip.closeEntry();zip.finish();zip.flush();
        }
    }
    public static void save(Context context,Uri uri,File file,boolean includeSettings) throws IOException {
        if(uri==null || file==null) throw new IOException("No log or document selected");
        try {
            byte[] settings=includeSettings?settingsJson(Control.diagnosticSettings(),Control.engine.speedKmh,preferenceGroups(context)):null;
            MessageDigest written=MessageDigest.getInstance("SHA-256");
            MessageDigest checked=MessageDigest.getInstance("SHA-256");
            long count=0,verified=0;
            Control.event("export-start",uri.toString());
            try(InputStream in=new FileInputStream(file); OutputStream out=context.getContentResolver().openOutputStream(uri,"wt")) {
                if(out==null) throw new IOException("Document provider returned no output stream");
                // Digest the actual archive bytes, including the ZIP directory, before read-back verification.
                CountingOutputStream counted=new CountingOutputStream(new DigestOutputStream(out,written));
                if(includeSettings)bundle(in,file.getName(),settings,counted);
                else {
                    byte[] buffer=new byte[8192]; int n;
                    while((n=in.read(buffer))!=-1)counted.write(buffer,0,n);
                }
                count=counted.count;
                out.flush();
            }
            try(InputStream in=context.getContentResolver().openInputStream(uri)) {
                if(in==null) throw new IOException("Document provider returned no verification stream");
                byte[] buffer=new byte[8192]; int n;
                while((n=in.read(buffer))!=-1) { checked.update(buffer,0,n); verified+=n; }
            }
            if(count!=verified || !Arrays.equals(written.digest(),checked.digest())) throw new IOException("Saved document verification failed");
            Control.event("export-complete","verifiedBytes="+count+" uri="+uri);
        } catch(NoSuchAlgorithmException impossible) { throw new IOException(impossible); }
        catch(IOException error) { Control.event("export-error",error.toString()); throw error; }
    }

    /**
     * PGRazor Root has no document picker (the run log is shown as a dialog, not a private activity),
     * so save a verified copy into the game's own app-visible external files directory instead. The
     * same two-pass digest check guards the written file; the destination path is returned so the
     * dialog can tell the user where to find it. No new storage permission is required.
     */
    public static File copyToExternal(Context context,File file) throws IOException {
        return copyToExternal(context,file,false);
    }
    public static File copyToExternal(Context context,File file,boolean includeSettings) throws IOException {
        if(file==null) throw new IOException("No log is available");
        File dir=context.getExternalFilesDir("pgrazor-logs");
        if(dir==null) dir=new File(context.getFilesDir(),"pgrazor-logs/exports");
        if(!dir.isDirectory() && !dir.mkdirs()) throw new IOException("Cannot create export folder");
        String name=includeSettings?file.getName().replaceFirst("\\.txt$","")+".zip":file.getName();
        File dest=new File(dir,name),temp=new File(dir,name+".tmp");
        try {
            byte[] settings=includeSettings?settingsJson(Control.diagnosticSettings(),Control.engine.speedKmh,preferenceGroups(context)):null;
            MessageDigest written=MessageDigest.getInstance("SHA-256");
            MessageDigest checked=MessageDigest.getInstance("SHA-256");
            long count=0,verified=0;
            Control.event("export-start",dest.getAbsolutePath());
            try(InputStream in=new FileInputStream(file); OutputStream out=new FileOutputStream(temp)) {
                CountingOutputStream counted=new CountingOutputStream(new DigestOutputStream(out,written));
                if(includeSettings)bundle(in,file.getName(),settings,counted);
                else {
                    byte[] buffer=new byte[8192]; int n;
                    while((n=in.read(buffer))!=-1)counted.write(buffer,0,n);
                }
                count=counted.count;
                out.flush();
            }
            if(dest.exists() && !dest.delete()) throw new IOException("Could not replace the previous export");
            if(!temp.renameTo(dest)) throw new IOException("Could not finish writing the export");
            try(InputStream in=new FileInputStream(dest)) {
                byte[] buffer=new byte[8192]; int n;
                while((n=in.read(buffer))!=-1) { checked.update(buffer,0,n); verified+=n; }
            }
            if(count!=verified || !Arrays.equals(written.digest(),checked.digest())) throw new IOException("Saved copy verification failed");
            Control.event("export-complete","verifiedBytes="+count+" path="+dest.getAbsolutePath());
            return dest;
        } catch(NoSuchAlgorithmException impossible) { throw new IOException(impossible); }
        catch(IOException error) { temp.delete(); Control.event("export-error",error.toString()); throw error; }
    }
    private static final class CountingOutputStream extends FilterOutputStream {
        long count;
        CountingOutputStream(OutputStream out){super(out);}
        @Override public void write(int value)throws IOException{out.write(value);count++;}
        @Override public void write(byte[] bytes,int offset,int length)throws IOException{out.write(bytes,offset,length);count+=length;}
    }
}
