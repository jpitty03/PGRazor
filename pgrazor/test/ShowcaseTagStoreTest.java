package dev.pgrazor;
import java.io.IOException;
public final class ShowcaseTagStoreTest {
    private static void check(boolean ok){if(!ok)throw new AssertionError();}
    private static final class Memory implements ShowcaseTagStore.Sink {
        String value;boolean fail;
        public String read(){return value;}
        public void write(String next)throws IOException{if(fail)throw new IOException("disk full");value=next;}
    }
    public static void main(String[] args)throws Exception {
        Memory disk=new Memory();ShowcaseTagStore store=new ShowcaseTagStore(disk);
        check(store.snapshot().equals("T1|0"+"|0".repeat(18)));
        check(store.update("T1|0|0|0|18446744073709551615"));
        String saved=store.snapshot();check(new ShowcaseTagStore(disk).snapshot().equals(saved));
        check(!store.update("T1|0|1|0|9")); // stale account/session revision
        check(!store.update("T1|1|1|0|18446744073709551615")); // duplicate ownership
        check(!store.update("T1|1|0|0|9")); // old ID mismatch
        check(!store.update("T1|1|18|0|9"));
        check(!store.update("T1|1|1|0|09"));
        check(!store.update("T1|1|1|0|-1"));
        check(!store.update("T1|1|1|0|18446744073709551616"));
        check(!store.update("T1|1|1|0|0"));
        disk.fail=true;
        try{store.update("T1|1|1|0|9");throw new AssertionError();}catch(IOException expected){}
        check(store.snapshot().equals(saved));check(disk.value.equals(saved));
        disk.fail=false;check(store.update("T1|1|1|0|9"));
        String beforeJournal=store.snapshot();disk.fail=true;
        try{store.update("B1|2|9|0");throw new AssertionError();}catch(IOException expected){}
        check(store.snapshot().equals(beforeJournal));disk.fail=false;
        check(store.update("B1|2|9|0"));
        String journal=store.snapshot();check(journal.startsWith("T2|3|10|"));
        ShowcaseTagStore resumed=new ShowcaseTagStore(disk);check(resumed.snapshot().equals(journal));
        check(!resumed.update("B1|3|9|0")); // never begin another create after restart
        check(!resumed.update("T1|3|8|0|700")); // wrong type cannot clear the journal
        disk.fail=true;try{resumed.update("T1|3|9|0|700");throw new AssertionError();}catch(IOException expected){}
        check(resumed.snapshot().equals(journal));disk.fail=false;
        check(resumed.update("T1|3|9|0|700"));check(resumed.snapshot().startsWith("T1|4|"));
        check(new ShowcaseTagStore(disk).snapshot().equals(resumed.snapshot()));
        for(String malformed:new String[]{"T2|0|0"+"|0".repeat(18),"T2|0|19"+"|0".repeat(18),"T2|0|01"+"|0".repeat(18)}){
            disk.value=malformed;try{new ShowcaseTagStore(disk);throw new AssertionError();}catch(IOException expected){}
        }
        // Separate account sink starts with no adopted tag IDs.
        check(new ShowcaseTagStore(new Memory()).snapshot().equals("T1|0"+"|0".repeat(18)));
        disk.value="T1|2|garbage";
        try{new ShowcaseTagStore(disk);throw new AssertionError();}catch(IOException expected){}
        System.out.println("PASS Showcase tag ownership: uint64, durable CAS, failure rollback, reload, account isolation, malformed input");
    }
}
