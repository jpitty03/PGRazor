package dev.pgrazor;

import java.io.IOException;
import java.util.HashSet;

/** Account-local tag ownership. Publish a new revision only after durable storage succeeds. */
final class ShowcaseTagStore {
    interface Sink { String read() throws IOException; void write(String value) throws IOException; }
    private final Sink sink;
    private long revision;
    private int pendingCreate;
    private long[] ids = new long[18];
    ShowcaseTagStore(Sink sink) throws IOException {
        this.sink=sink;
        String saved=sink.read();
        if(saved==null)return;
        try {
            boolean journal=saved.startsWith("T2|");
            String[] fields=split(saved,journal?21:20,journal?"T2":"T1");
            revision=revision(fields[1]);
            if(journal){pendingCreate=type(fields[2],1,18);}
            HashSet<Long> seen=new HashSet<>();
            for(int i=0;i<18;i++) {
                ids[i]=id(fields[i+(journal?3:2)]);
                if(ids[i]!=0&&!seen.add(ids[i]))throw new IllegalArgumentException();
            }
        } catch(IllegalArgumentException bad) {throw new IOException("Invalid Showcase tag ownership",bad);}
    }
    synchronized String snapshot(){return encode(revision,ids,pendingCreate);}
    // T1|expectedRevision|typeIndex|oldTagId|newTagId. Retry of an already
    // committed command is rejected; the caller observes the durable snapshot.
    synchronized boolean update(String command) throws IOException {
        if(command!=null&&command.startsWith("B1|"))return begin(command);
        final String[] f;
        final long expected,oldId,newId;
        final int type;
        try {
            f=split(command,5,"T1");expected=revision(f[1]);
            type=type(f[2],0,17);
            oldId=id(f[3]);newId=id(f[4]);
        } catch(IllegalArgumentException bad){return false;}
        if(expected!=revision||revision==Long.MAX_VALUE||ids[type]!=oldId||newId==0)return false;
        if(pendingCreate!=0&&pendingCreate!=type+1)return false;
        for(int i=0;i<18;i++)if(i!=type&&ids[i]==newId)return false;
        long[] next=ids.clone();next[type]=newId;
        sink.write(encode(revision+1,next,0));
        ids=next;revision++;pendingCreate=0;return true;
    }
    // Persist intent BEFORE native sends AddTag. An interrupted creation remains
    // visible after process/account restart and cannot silently create duplicates.
    private boolean begin(String command) throws IOException {
        final String[] f;final long expected,oldId;final int type;
        try {f=split(command,4,"B1");expected=revision(f[1]);type=type(f[2],0,17);oldId=id(f[3]);}
        catch(IllegalArgumentException bad){return false;}
        if(expected!=revision||revision==Long.MAX_VALUE||pendingCreate!=0||ids[type]!=oldId)return false;
        sink.write(encode(revision+1,ids,type+1));revision++;pendingCreate=type+1;return true;
    }
    private static int type(String value,int min,int max) {
        int type=Integer.parseInt(value);
        if(type<min||type>max||!Integer.toString(type).equals(value))throw new IllegalArgumentException();return type;
    }
    private static String[] split(String value,int length,String prefix) {
        if(value==null||value.length()>512)throw new IllegalArgumentException();
        String[] fields=value.split("\\|",-1);
        if(fields.length!=length||!prefix.equals(fields[0]))throw new IllegalArgumentException();
        return fields;
    }
    private static long revision(String value) {
        long result=Long.parseLong(value);
        if(result<0||!Long.toString(result).equals(value))throw new IllegalArgumentException();
        return result;
    }
    private static long id(String value) {
        long result=Long.parseUnsignedLong(value);
        if(!Long.toUnsignedString(result).equals(value))throw new IllegalArgumentException();
        return result;
    }
    private static String encode(long revision,long[] ids,int pending) {
        StringBuilder out=new StringBuilder(pending==0?"T1|":"T2|").append(revision);
        if(pending!=0)out.append('|').append(pending);
        for(long id:ids)out.append('|').append(Long.toUnsignedString(id));
        return out.toString();
    }
}
