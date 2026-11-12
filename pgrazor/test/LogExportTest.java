package dev.pgrazor;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.zip.*;
import org.json.JSONObject;

public final class LogExportTest {
    public static void main(String[] args)throws Exception {
        Properties settings=new Properties();
        settings.setProperty("smartCatchEnabled","true");
        settings.setProperty("escaped","quotes\" and newline\nUnicode \u2603");
        byte[] json=LogExport.settingsJson(settings,9.5);
        String line=new String(json,StandardCharsets.UTF_8);
        if(line.contains("\n")||line.contains("\r"))throw new AssertionError("Not single line");
        JSONObject parsed=new JSONObject(line);
        if(!parsed.getJSONObject("settings").getString("escaped").equals(settings.getProperty("escaped")))throw new AssertionError("JSON escaping");
        if(parsed.getDouble("speedKmh")!=9.5)throw new AssertionError("Speed");
        byte[] log=new byte[25000];new Random(42).nextBytes(log);
        ByteArrayOutputStream bytes=new ByteArrayOutputStream();
        LogExport.bundle(new ByteArrayInputStream(log),"session.txt",json,bytes);
        try(ZipInputStream zip=new ZipInputStream(new ByteArrayInputStream(bytes.toByteArray()))) {
            for(String name:new String[]{"session.txt","settings.json"}) {
                ZipEntry entry=zip.getNextEntry();
                if(entry==null||!name.equals(entry.getName()))throw new AssertionError("Entry name");
                ByteArrayOutputStream data=new ByteArrayOutputStream();byte[] buffer=new byte[1024];int n;
                while((n=zip.read(buffer))!=-1)data.write(buffer,0,n);
                if(!Arrays.equals(data.toByteArray(),name.endsWith(".txt")?log:json))throw new AssertionError("Archive content");
            }
            if(zip.getNextEntry()!=null)throw new AssertionError("Unexpected entry");
        }
        System.out.println("PASS compact JSON escaping and ZIP log/settings byte-exact round trip");
    }
}
