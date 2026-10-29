package dev.pgrazor;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import org.json.JSONArray;
import org.json.JSONObject;

public final class RuntimeMailboxTest {
    private static Path base,dir;private static int cases;
    private static RuntimeMailbox host;
    private static void require(boolean yes){if(!yes)throw new AssertionError("mailbox fixture assertion");}
    private static RuntimeMailbox fresh() throws Exception {
        dir=Files.createDirectory(base.resolve("case-"+(++cases)));Control.reset();
        host=RuntimeMailbox.create(dir.toString(),"fixture-session");Control.mailbox=host;
        require(Control.reads==0&&Control.mutations==0&&!Files.exists(dir.resolve("state.json")));
        Files.write(dir.resolve("events.json"),"{}".getBytes(StandardCharsets.UTF_8));return host;
    }
    private static void input(String text) throws Exception {Files.write(dir.resolve("events.json"),text.getBytes(StandardCharsets.UTF_8));}
    private static JSONObject state() throws Exception {return new JSONObject(new String(Files.readAllBytes(dir.resolve("state.json")),StandardCharsets.UTF_8));}
    private static void packet(String seq,String calls) throws Exception {input("{\"session\":\"fixture-session\",\"seq\":"+seq+",\"calls\":"+calls+"}");}
    private static void failed() throws Exception {require(state().getString("fault").equals("Mailbox transport failed"));}
    public static void main(String[] args) throws Exception {
        require(args.length==1);base=Paths.get(args[0]);
        fresh();Control.pulse=true;host.tick();JSONObject s=state();
        require(s.getInt("version")==1&&s.getLong("ack")==0&&s.getString("fault").equals(""));
        require(Math.abs(s.getLong("at")-System.currentTimeMillis())<5000);
        JSONArray snap=s.getJSONObject("values").getJSONArray("accountCommandSnapshot");
        require(snap.length()==2&&snap.getString(1).equals("18446744073709551615"));
        require(s.getJSONObject("pulses").getLong("takeCameraSampleRequest")==1);
        require(s.getJSONObject("values").isNull("latitude"));
        packet("1","[[\"accountIdentity\",[\"id\",\"name\"]],[\"accountSessionSaved\",[]]]");host.tick();
        require(Control.mutations==2&&state().getLong("ack")==1);
        host.tick();require(Control.mutations==2);
        packet("1","[\"malformed stale payload\"]");host.tick();require(Control.mutations==2&&state().getString("fault").equals(""));
        input("{\"session\":\"other-session\",\"seq\":999,\"calls\":[\"bad\"]}");host.tick();require(state().getString("fault").equals(""));
        packet("3","[]");host.tick();failed();require(state().getLong("ack")==1);
        fresh();packet("1","[[\"accountIdentity\",[\"id\",\"name\"]],[\"setSpawnReady\",[1]]]");host.tick();failed();require(Control.mutations==0&&state().getLong("ack")==0);
        fresh();Control.throwMutation=true;packet("1","[[\"accountIdentity\",[\"id\",\"name\"]],[\"accountSessionSaved\",[]]]");host.tick();failed();require(Control.mutations==1&&state().getLong("ack")==1);require(!Control.lastDiagnostic.contains("id-secret"));host.tick();require(Control.mutations==1);
        fresh();Control.onMutation=()->host.tick();packet("1","[[\"accountIdentity\",[\"id\",\"name\"]]]");host.tick();require(Control.mutations==1&&state().getLong("ack")==1&&state().getString("fault").equals(""));
        fresh();Control.onMutation=()->host.stop();packet("1","[[\"accountIdentity\",[\"id\",\"name\"]],[\"accountSessionSaved\",[]]]");host.tick();require(Control.mutations==1&&state().getLong("ack")==1&&state().getString("fault").equals("Host stopped"));host.tick();require(Control.mutations==1);
        fresh();host.stop();packet("1","[[\"accountSessionSaved\",[]]]");host.tick();require(Control.mutations==0&&state().getString("fault").equals("Host stopped"));
        for(String number:new String[]{"0","-1","1.5","9007199254740992","9007199254740990.5","\"1\""}){
            fresh();packet(number,"[]");host.tick();failed();
        }
        fresh();packet("1","[[\"updateCameraHeadingAt\",[20,7,true,9007199254740991]]]");host.tick();require(Control.timestamp==9007199254740991L&&state().getString("fault").equals(""));
        fresh();packet("1","[[\"updateCameraHeadingAt\",[20,7,true,9007199254740990.5]]]");host.tick();failed();
        fresh();packet("1","[[\"applyMapTap\",[91,0,1,0,100]]]");host.tick();failed();
        fresh();packet("1","[[\"setVgpTarget\",[true,1e999,0,false]]]");host.tick();failed();
        fresh();packet("1","[[\"setSpawnReady\",[null]]]");host.tick();failed();
        fresh();packet("1","[[\"unlistedMethod\",[]]]");host.tick();failed();
        for(String text:new String[]{"{'session':'fixture-session'}","{\"session\":\"fixture-session\",\"seq\":1,\"seq\":2,\"calls\":[]}","{} trailing","{\"x\":[[[[[[[[[0]]]]]]]]]}","{\"x\":true,}"}){
            fresh();input(text);host.tick();failed();
        }
        fresh();Control.snapshot=new String[]{"bad"};host.tick();failed();
        fresh();Control.snapshot=new String[]{"bad",null};host.tick();failed();
        fresh();StringBuilder tooMany=new StringBuilder("[");for(int i=0;i<129;i++){if(i>0)tooMany.append(',');tooMany.append("[\"accountSessionSaved\",[]]");}tooMany.append(']');packet("1",tooMany.toString());host.tick();failed();require(Control.mutations==0);
        fresh();Files.write(dir.resolve("events.json"),new byte[]{(byte)0xc3,0x28});host.tick();failed();
        fresh();Files.write(dir.resolve("events.json"),new byte[1048577]);host.tick();failed();
        fresh();Path outside=base.resolve("outside.json");Files.write(outside,"unchanged".getBytes(StandardCharsets.UTF_8));
        try{
            Files.createSymbolicLink(dir.resolve("state.json.tmp"),outside);host.tick();
            require(new String(Files.readAllBytes(outside),StandardCharsets.UTF_8).equals("unchanged"));
            require(!Files.exists(dir.resolve("state.json")));
        }catch(UnsupportedOperationException|java.nio.file.FileSystemException unavailable){
            System.out.println("LIMITATION: host symlink fixture unavailable");
        }
        // Legitimate aliased parent directories resolve to the same private base.
        fresh();Path real=dir;Path alias=base.resolve("private-alias");
        try{
            Files.createSymbolicLink(alias,real);
            RuntimeMailbox aliased=RuntimeMailbox.create(alias.toString(),"fixture-session");aliased.tick();
            require(Files.exists(real.resolve("state.json")));aliased.stop();
        }catch(UnsupportedOperationException|java.nio.file.FileSystemException unavailable){
            System.out.println("LIMITATION: host parent-alias fixture unavailable");
        }
        fresh();
        try{
            Files.delete(dir.resolve("events.json"));Files.createSymbolicLink(dir.resolve("events.json"),outside);
            host.tick();failed();
        }catch(UnsupportedOperationException|java.nio.file.FileSystemException unavailable){
            System.out.println("LIMITATION: host child-link fixture unavailable");
        }
        // Java owns cadence; stop is nonblocking and cannot be restarted.
        fresh();host.start();host.start();
        long deadline=System.nanoTime()+2000000000L;
        while(!Files.exists(dir.resolve("state.json"))&&System.nanoTime()<deadline)Thread.sleep(10);
        host.stop();
        require(Control.reads>0&&state().getString("fault").equals("Host stopped"));
        int afterStop=Control.reads;host.start();Thread.sleep(80);require(Control.reads==afterStop);
        fresh();Control.onMutation=()->host.stop();packet("1","[[\"accountIdentity\",[\"id\",\"name\"]],[\"accountSessionSaved\",[]]]");host.start();
        deadline=System.nanoTime()+2000000000L;
        while(!Files.exists(dir.resolve("state.json"))&&System.nanoTime()<deadline)Thread.sleep(10);
        host.stop();require(Control.mutations==1&&state().getString("fault").equals("Host stopped"));
        require(!Control.lastDiagnostic.contains("id-secret"));
        System.out.println("PASS RuntimeMailbox: real JSON, complete validation, no replay, reentry, stop, snapshot, safe integers, bounded files");
    }
}
