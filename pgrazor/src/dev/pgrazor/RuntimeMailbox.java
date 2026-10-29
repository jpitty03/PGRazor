package dev.pgrazor;

import java.io.IOException;
import java.math.BigDecimal;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.nio.file.attribute.BasicFileAttributes;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import org.json.JSONArray;
import org.json.JSONObject;
import org.json.JSONTokener;

/** Compiled host owner for the existing translated ARM mailbox wire protocol.
 * No timers, Control reads, file writes or action invocation during create().
 * Caller supplies a private, process-specific code-cache directory and calls start().
 */
public final class RuntimeMailbox {
    private static final int MAX_BYTES=1048576, MAX_CALLS=128;
    private static final long MAX_SAFE=9007199254740991L;
    private static final String[] READERS = {
        "accountCommand::String",
        "accountManagementAllowed::boolean",
        "adventureSyncDismissAllowed::boolean",
        "gameRoutesAllowed::boolean",
        "gameRoutesCommand::String",
        "active::boolean",
        "autoIncubate::boolean",
        "autoTransferAllowed::boolean",
        "battleOptions::int",
        "battlesAllowed::boolean",
        "berriesAllowed::boolean",
        "buddyAllowed::boolean",
        "buddyBerry::int",
        "buddySamplingRequested::boolean",
        "cameraSteeringRequested::boolean",
        "desiredEnabled::boolean",
        "effectiveBerryPriority::String",
        "giftOptions::int",
        "giftsAllowed::boolean",
        "incubateLongest::boolean",
        "incubatorAllowed::boolean",
        "interactionDiagnosticsReady::boolean",
        "itemAccessAllowed::boolean",
        "itemCatalogMode::int",
        "itemRules::String",
        "itemTransferEnabled::boolean",
        "ivCatchEnabled::boolean",
        "ivDisplayAllowed::boolean",
        "ivOwnedEnabled::boolean",
        "latitude::double",
        "longitude::double",
        "mapPoiAllowed::boolean",
        "mapTapAllowed::boolean",
        "mapTapEpoch::int",
        "mapTapMode::int",
        "quickBallAllowed::boolean",
        "quickSpinAllowed::boolean",
        "quickSummaryAllowed::boolean",
        "rocketAllowed::boolean",
        "rocketOptions::int",
        "spawnAllowed::boolean",
        "spawnBooster::boolean",
        "throwAllowed::boolean",
        "throwAlwaysHit::boolean",
        "throwAlwaysHitReady::boolean",
        "throwAr::boolean",
        "throwArReady::boolean",
        "throwCurve::boolean",
        "throwGrade::int",
        "transferKeepAll::boolean",
        "transferKeepSpecies::String",
        "transferProtect::int",
        "vgpAllowed::boolean",
        "vgpEnabled::boolean",
        "vgpMapVisible::boolean",
        "vgpOptions::int",
        "accountCommandSnapshot::String[]"
    };
    private static final String[] PULSES = {
        "takeCameraSampleRequest::boolean",
        "takeInteractionStopRequest::boolean",
        "takeInteractionTraceRequest::boolean"
    };
    private static final String[] ACTIONS = {
        "accountIdentity:String,String:void",
        "setAccountsReady:boolean:void",
        "accountSessionEnded::void",
        "accountLogoutComplete::void",
        "accountLogoutFailed::void",
        "accountSignInResetComplete::void",
        "accountSessionSaved::void",
        "setGameRoutesReady:boolean,String:void",
        "setGameRoutesSnapshot:String:void",
        "gameRoutesAction:String:void",
        "applyMapTap:double,double,int,int,double:void",
        "cameraDiagnostic:String:void",
        "configureCameraDiagnostics:boolean:void",
        "configureCameraSteering:boolean:void",
        "configureInteractionDiagnostics:boolean:void",
        "configureQuickBall:boolean:void",
        "configureQuickSummary:boolean:void",
        "cooldownAction:String:void",
        "embedded:String:void",
        "event:String,String:void",
        "interactionDiagnostic:String:void",
        "invalidateCameraHeading:String:void",
        "quickBallOutcome:String:boolean",
        "setAutoTransferReady:boolean,String:void",
        "setBattleStatus:String:void",
        "setBattlesReady:boolean,String:void",
        "setBerryReady:boolean:void",
        "setBuddyReady:boolean,String:void",
        "setBuddyStatus:String:void",
        "setCameraDiagnosticRunning:boolean:void",
        "setCooldownEncounter:String:void",
        "setEncounterBerry:boolean,boolean:void",
        "setGiftStatus:String:void",
        "setGiftsReady:boolean,String:void",
        "setIncubatorReady:boolean,String:void",
        "setIncubatorStatus:String:void",
        "setInteractionDiagnosticRunning:boolean:void",
        "setItemCatalog:String:void",
        "setItemReady:boolean,String:void",
        "setItemStatus:String:void",
        "setIvOwnedReady:boolean:void",
        "setMapPoiSnapshot:String:void",
        "setMapTapReady:boolean:void",
        "setQuickBallReady:boolean,String:void",
        "setQuickSpinReady:boolean,String:void",
        "setQuickSummaryReady:boolean,String:void",
        "setRocketReady:boolean,String:void",
        "setRocketStatus:String:void",
        "setSpawnRange:double,double:void",
        "setSpawnReady:boolean:void",
        "setThrowArReady:boolean:void",
        "setThrowAlwaysHitReady:boolean:void",
        "setThrowReady:boolean:void",
        "setVgpReady:boolean,String:void",
        "setVgpStatus:String:void",
        "setVgpTarget:boolean,double,double,boolean:void",
        "transferResult:String:void",
        "updateCameraHeadingAt:double,int,boolean,long:void",
        "vgpResult:String,boolean:void"
    };
    private final Path directory;
    private final String session;
    private final Map<String,Method> readers=new LinkedHashMap<>();
    private final Map<String,Method> pulses=new LinkedHashMap<>();
    private final Map<String,Method> actions=new LinkedHashMap<>();
    private final Map<String,Long> counts=new LinkedHashMap<>();
    private long ack;
    private boolean stopped,ticking,started;
    private ScheduledExecutorService executor;
    private ScheduledFuture<?> scheduled;
    private String fault="";

    private RuntimeMailbox(String path,String id) throws Exception {
        if(path==null||id==null||id.length()==0||id.length()>256||!id.matches("[A-Za-z0-9._-]+")) throw invalid();
        // Android's /data/user/0 alias may legitimately resolve to /data/data.
        // Canonicalize the private base once; child links remain forbidden.
        directory=canonicalBase(Paths.get(path));session=id;
        if(!Files.isDirectory(directory,LinkOption.NOFOLLOW_LINKS)||Files.isSymbolicLink(directory))throw invalid();
        resolve(READERS,readers);resolve(PULSES,pulses);resolve(ACTIONS,actions);
        for(String name:pulses.keySet())counts.put(name,0L);
    }
    public static RuntimeMailbox create(String directory,String session) {
        try{return new RuntimeMailbox(directory,session);}
        catch(Exception error){throw new IllegalArgumentException("Runtime mailbox configuration invalid");}
    }
    private static Class<?> type(String name) throws Exception {
        switch(name){case "boolean":return boolean.class;case "int":return int.class;
            case "long":return long.class;case "double":return double.class;
            case "String":return String.class;case "String[]":return String[].class;
            case "void":return void.class;default:throw invalid();}
    }
    private static void resolve(String[] specs,Map<String,Method> target) throws Exception {
        for(String spec:specs){String[] fields=spec.split(":",-1);
            String[] args=fields[1].isEmpty()?new String[0]:fields[1].split(",");
            Class<?>[] signature=new Class<?>[args.length];for(int i=0;i<args.length;i++)signature[i]=type(args[i]);
            Method method=Control.class.getMethod(fields[0],signature);
            if(!Modifier.isStatic(method.getModifiers())||method.getReturnType()!=type(fields[2]))throw invalid();
            target.put(fields[0],method);
        }
    }
    private static IOException invalid(){return new IOException("Mailbox protocol invalid");}
    private static Path canonicalBase(Path input) throws IOException {
        // Windows toRealPath opens directory handles that restricted JVMs may be
        // denied, even when ordinary reads/writes and readSymbolicLink work.
        // File canonicalization alone does not resolve Windows directory symlinks.
        Path value=input.toAbsolutePath().normalize();
        for(int links=0;links<32;links++){
            Path current=value.getRoot();boolean changed=false;
            for(int i=0;i<value.getNameCount();i++){
                current=current.resolve(value.getName(i));
                if(Files.isSymbolicLink(current)){
                    Path target=Files.readSymbolicLink(current);
                    Path resolved=target.isAbsolute()?target:current.getParent().resolve(target);
                    for(int j=i+1;j<value.getNameCount();j++)resolved=resolved.resolve(value.getName(j));
                    value=resolved.toAbsolutePath().normalize();changed=true;break;
                }
            }
            if(!changed)return value.toFile().getCanonicalFile().toPath();
        }
        throw invalid(); // cycle or unreasonable alias chain
    }
    private void paths() throws IOException {
        if(!Files.isDirectory(directory,LinkOption.NOFOLLOW_LINKS)||
            !directory.toFile().getCanonicalFile().toPath().equals(directory))throw invalid();
    }
    private Path checked(String name) throws IOException {
        paths();Path path=directory.resolve(name);
        if(!path.getParent().equals(directory)||Files.isSymbolicLink(path))throw invalid();
        if(!path.toFile().getCanonicalFile().toPath().equals(path))throw invalid();
        return path;
    }
    private JSONObject read() throws Exception {
        Path path=checked("events.json");
        if(!Files.exists(path,LinkOption.NOFOLLOW_LINKS))return null;
        BasicFileAttributes attrs=Files.readAttributes(path,BasicFileAttributes.class,LinkOption.NOFOLLOW_LINKS);
        if(!attrs.isRegularFile()||attrs.size()>MAX_BYTES)throw invalid();
        byte[] data;
        try(FileChannel file=FileChannel.open(path,StandardOpenOption.READ,LinkOption.NOFOLLOW_LINKS)){
            long length=file.size();if(length<0||length>MAX_BYTES)throw invalid();
            // One sentinel byte detects growth without unbounded read/allocation.
            ByteBuffer buffer=ByteBuffer.allocate((int)length+1);
            while(buffer.hasRemaining()&&file.read(buffer)!=-1){}
            if(buffer.position()!=length||file.size()!=length)throw invalid();
            data=new byte[(int)length];buffer.flip();buffer.get(data);
        }
        checked("events.json");
        String text=StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(data)).toString();
        Object value=new JsonInput(text).value(0);
        if(!(value instanceof JSONObject))throw invalid();
        return (JSONObject)value;
    }
    // Strict bounded grammar before org.json containers: Android's JSONTokener
    // alone accepts comments/unquoted strings and rounds decimal integers.
    private static final class JsonInput {
        final String text;int at;
        JsonInput(String value){text=value;}
        void space(){while(at<text.length()&&" \t\r\n".indexOf(text.charAt(at))>=0)at++;}
        char peek(){return at<text.length()?text.charAt(at):0;}
        Object value(int depth) throws Exception {
            if(depth>8)throw invalid();space();char c=peek();Object result;
            if(c=='{'){
                at++;JSONObject object=new JSONObject();space();
                if(peek()!='}')while(true){if(peek()!='"')throw invalid();String key=string();space();
                    if(peek()!=':'||object.has(key))throw invalid();at++;object.put(key,value(depth+1));space();
                    if(peek()!=',')break;at++;space();}
                if(peek()!='}')throw invalid();at++;result=object;
            }else if(c=='['){
                at++;JSONArray array=new JSONArray();space();
                if(peek()!=']')while(true){array.put(value(depth+1));space();if(peek()!=',')break;at++;space();}
                if(peek()!=']')throw invalid();at++;result=array;
            }else if(c=='"')result=string();
            else if(text.startsWith("true",at)){at+=4;result=Boolean.TRUE;}
            else if(text.startsWith("false",at)){at+=5;result=Boolean.FALSE;}
            else if(text.startsWith("null",at)){at+=4;result=JSONObject.NULL;}
            else result=number();
            space();if(depth==0&&at!=text.length())throw invalid();return result;
        }
        String string() throws Exception {
            int start=at++;boolean done=false;
            while(at<text.length()){
                char c=text.charAt(at++);if(c<32)throw invalid();if(c=='"'){done=true;break;}
                if(c=='\\'){
                    if(at>=text.length())throw invalid();char escape=text.charAt(at++);
                    if(escape=='u'){for(int i=0;i<4;i++){if(at>=text.length()||Character.digit(text.charAt(at++),16)<0)throw invalid();}}
                    else if("\"\\/bfnrt".indexOf(escape)<0)throw invalid();
                }
            }
            if(!done)throw invalid();Object decoded=new JSONTokener(text.substring(start,at)).nextValue();
            if(!(decoded instanceof String))throw invalid();return (String)decoded;
        }
        boolean digit(){return peek()>='0'&&peek()<='9';}
        BigDecimal number() throws Exception {
            int start=at;if(peek()=='-')at++;
            if(peek()=='0')at++;else{if(peek()<'1'||peek()>'9')throw invalid();while(digit())at++;}
            if(peek()=='.'){at++;if(!digit())throw invalid();while(digit())at++;}
            if(peek()=='e'||peek()=='E'){at++;if(peek()=='+'||peek()=='-')at++;if(!digit())throw invalid();while(digit())at++;}
            if(at-start>128)throw invalid();BigDecimal value=new BigDecimal(text.substring(start,at));
            if(Math.abs((long)value.scale())>4096)throw invalid();return value;
        }
    }
    private static long safeInteger(Object value) throws IOException {
        if(value instanceof BigDecimal){BigDecimal n=(BigDecimal)value;
            if(n.abs().compareTo(BigDecimal.valueOf(MAX_SAFE))>0)throw invalid();
            try{return n.longValueExact();}catch(ArithmeticException error){throw invalid();}}
        if(!(value instanceof Number))throw invalid();
        double d=((Number)value).doubleValue();
        if(!Double.isFinite(d)||Math.rint(d)!=d||d < -MAX_SAFE||d>MAX_SAFE)throw invalid();
        if(value instanceof Long&&(((Long)value)<-MAX_SAFE||((Long)value)>MAX_SAFE))throw invalid();
        return (long)d;
    }
    private static Object argument(Object value,Class<?> target) throws Exception {
        if(target==String.class){if(!(value instanceof String))throw invalid();bounded((String)value);return value;}
        if(target==boolean.class){if(!(value instanceof Boolean))throw invalid();return value;}
        if(target==int.class){long n=safeInteger(value);if(n<Integer.MIN_VALUE||n>Integer.MAX_VALUE)throw invalid();return (int)n;}
        if(target==long.class)return safeInteger(value);
        if(target==double.class){if(!(value instanceof Number))throw invalid();double d=((Number)value).doubleValue();
            if(!Double.isFinite(d)||Math.abs(d)>MAX_SAFE)throw invalid();return d;}
        throw invalid();
    }
    private static void bounded(String value) throws IOException {
        if(value.length()>MAX_BYTES||value.getBytes(StandardCharsets.UTF_8).length>MAX_BYTES)throw invalid();
    }
    private static final class Call {
        final Method method;final Object[] args;
        Call(Method m,Object[] a){method=m;args=a;}
    }
    private Call[] validate(JSONObject packet) throws Exception {
        Object raw=packet.get("calls");if(!(raw instanceof JSONArray))throw invalid();
        JSONArray array=(JSONArray)raw;if(array.length()>MAX_CALLS)throw invalid();
        Call[] result=new Call[array.length()];
        for(int i=0;i<array.length();i++){
            Object entry=array.get(i);if(!(entry instanceof JSONArray))throw invalid();JSONArray call=(JSONArray)entry;
            if(call.length()!=2||!(call.get(0) instanceof String)||!(call.get(1) instanceof JSONArray))throw invalid();
            String name=(String)call.get(0);Method method=actions.get(name);if(method==null)throw invalid();
            JSONArray values=(JSONArray)call.get(1);Class<?>[] types=method.getParameterTypes();
            if(values.length()!=types.length)throw invalid();Object[] args=new Object[types.length];
            for(int j=0;j<types.length;j++)args[j]=argument(values.get(j),types[j]);
            if(name.equals("applyMapTap")&&(Math.abs((Double)args[0])>90||Math.abs((Double)args[1])>180||
                (Integer)args[2]<0||(Integer)args[2]>3||(Double)args[4]<0))throw invalid();
            if(name.equals("updateCameraHeadingAt")&&(Long)args[3]<0)throw invalid();
            result[i]=new Call(method,args);
        }
        return result;
    }
    private Object reader(String name,Method method) throws Exception {
        Object value=method.invoke(null);
        if(name.equals("accountCommandSnapshot")){
            if(!(value instanceof String[])||((String[])value).length!=2)throw invalid();
            JSONArray snapshot=new JSONArray();for(String part:(String[])value){if(part==null)throw invalid();bounded(part);snapshot.put(part);}
            return snapshot;
        }
        if(value instanceof String){bounded((String)value);return value;}
        if(value instanceof Boolean)return value;
        if(value instanceof Number){double d=((Number)value).doubleValue();
            // JSON.stringify(NaN) in the legacy host produces null for unknown fix.
            if((name.equals("latitude")||name.equals("longitude"))&&!Double.isFinite(d))return JSONObject.NULL;
            if(!Double.isFinite(d)||Math.abs(d)>MAX_SAFE)throw invalid();return value;}
        throw invalid();
    }
    private void write(JSONObject values) throws Exception {
        JSONObject pulse=new JSONObject();for(Map.Entry<String,Long> item:counts.entrySet())pulse.put(item.getKey(),item.getValue());
        JSONObject state=new JSONObject().put("version",1).put("session",session).put("at",System.currentTimeMillis())
            .put("ack",ack).put("values",values).put("pulses",pulse).put("fault",fault);
        byte[] bytes=state.toString().getBytes(StandardCharsets.UTF_8);if(bytes.length>MAX_BYTES)throw invalid();
        Path temp=checked("state.json.tmp"),target=checked("state.json");
        try(FileChannel file=FileChannel.open(temp,StandardOpenOption.CREATE,StandardOpenOption.TRUNCATE_EXISTING,
                StandardOpenOption.WRITE,LinkOption.NOFOLLOW_LINKS)){
            // Ephemeral freshness mailbox: atomic visibility is required, crash
            // durability is not. A 20Hz fsync would add unnecessary storage work.
            ByteBuffer buffer=ByteBuffer.wrap(bytes);while(buffer.hasRemaining())file.write(buffer);
        }
        checked("state.json.tmp");checked("state.json");
        // No truncate/copy fallback: old complete snapshot remains if atomic move fails.
        Files.move(temp,target,StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
    }
    private void report(){try{actions.get("event").invoke(null,"translated-bridge-error","Mailbox transport failed");}catch(Exception ignored){}}
    private void fail(){stopped=true;cancelTimer();fault="Mailbox transport failed";try{write(new JSONObject());}catch(Exception ignored){}report();}
    // start is idempotent; construction remains inert. No JS cadence is required.
    public synchronized void start(){
        if(stopped||started)return;started=true;
        try{
            executor=Executors.newSingleThreadScheduledExecutor(task->{
                Thread thread=new Thread(task,"PGRazor-RuntimeMailbox");thread.setDaemon(true);return thread;
            });
            scheduled=executor.scheduleWithFixedDelay(this::tick,0,50,TimeUnit.MILLISECONDS);
        }catch(Exception error){fail();}
    }
    private void cancelTimer(){
        if(scheduled!=null){scheduled.cancel(false);scheduled=null;}
        if(executor!=null){executor.shutdown();executor=null;}
    }
    public synchronized void tick(){
        if(stopped||ticking)return;ticking=true;
        try{
            JSONObject packet=read();
            if(packet!=null&&packet.has("session")){
                Object id=packet.get("session");if(!(id instanceof String))throw invalid();
                if(session.equals(id)){
                    if(packet.has("version")&&safeInteger(packet.get("version"))!=1)throw invalid();
                    long seq=safeInteger(packet.get("seq"));if(seq<=0)throw invalid();
                    if(seq>ack){if(seq!=ack+1)throw invalid();Call[] calls=validate(packet);
                        ack=seq; // consume entire mutation batch BEFORE any callback
                        for(Call call:calls){if(stopped)return;call.method.invoke(null,call.args);}
                    }
                }
            }else if(packet!=null&&packet.length()!=0)throw invalid();
            if(stopped)return;
            JSONObject values=new JSONObject();int budget=0;
            for(Map.Entry<String,Method> item:readers.entrySet()){
                Object value=reader(item.getKey(),item.getValue());if(stopped)return;
                budget+=value.toString().length();if(budget>MAX_BYTES)throw invalid();values.put(item.getKey(),value);
            }
            for(Map.Entry<String,Method> item:pulses.entrySet()){
                boolean value=(Boolean)item.getValue().invoke(null);if(stopped)return;
                if(value){long count=counts.get(item.getKey());if(count>=MAX_SAFE)throw invalid();counts.put(item.getKey(),count+1);}
            }
            if(!stopped)write(values);
        }catch(Exception error){if(!stopped)fail();}
        finally{ticking=false;}
    }
    public synchronized void stop(){
        if(stopped)return;stopped=true;cancelTimer();fault="Host stopped";
        try{write(new JSONObject());}catch(Exception error){report();}
    }
}
