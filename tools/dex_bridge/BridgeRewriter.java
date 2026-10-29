import java.io.File;
import java.nio.file.*;
import java.util.*;
import org.jf.dexlib2.*;
import org.jf.dexlib2.iface.*;
import org.jf.dexlib2.iface.instruction.*;
import org.jf.dexlib2.iface.reference.MethodReference;
import org.jf.dexlib2.immutable.*;
import org.jf.dexlib2.immutable.reference.*;
import org.jf.dexlib2.immutable.instruction.*;
import org.jf.dexlib2.builder.*;
import org.jf.dexlib2.builder.instruction.*;
import org.jf.dexlib2.writer.pool.DexPool;

/** Build-time method-body rewrite. Exact descriptors, preserved original bodies,
 * transactional output; no ART hooks and no runtime code generation. */
public final class BridgeRewriter {
    static final String DISPATCH="Ldev/pgrazor/GameBridgeDispatch;";
    static final String OBJECT="Ljava/lang/Object;", STRING="Ljava/lang/String;";
    static final String ACTIVITY="Landroid/app/Activity;";
    static final String PERMISSION="Lcom/nianticlabs/platform/niaadvsynccore/permission/PermissionStatus;";
    static final String PREFIX="$pgrazor$original$";
    static final Map<String,Spec> SPECS=new LinkedHashMap<>();
    static final class Spec {
        final String owner,name,ret,kind; final List<String> args; final boolean statik;
        Spec(String owner,String name,String ret,boolean statik,String kind,String...args){
            this.owner=owner;this.name=name;this.ret=ret;this.statik=statik;this.kind=kind;this.args=Arrays.asList(args);
            SPECS.put(owner+"->"+name,this);
        }
    }
    static {
        new Spec("Lcom/nianticlabs/platform/authgooglesignin/SignInManager$Companion;","signIn","V",false,"signin",ACTIVITY,"Lcom/google/android/gms/auth/api/signin/GoogleSignInClient;","Lkotlin/jvm/functions/Function1;");
        new Spec("Lcom/unity3d/player/UnityPermissions;","hasUserAuthorizedPermission","Z",true,"permission","Landroid/content/Context;",STRING);
        new Spec("Lcom/unity3d/plugin/UnityAndroidPermissions;","IsPermissionGranted","Z",false,"permissionActivity",ACTIVITY,STRING);
        new Spec("Lcom/nianticlabs/platform/niaadvsyncpermissions/internal/InternalPermissionManager;","permissionStatus",PERMISSION,false,"status","J");
        for(String[] x:new String[][]{{"invokeOnStart","afterStart"},{"invokeOnResume","afterResume"},{"invokeOnStop","beforeStop"},{"invokeOnPause","beforePause"}})
            new Spec("Lcom/nianticlabs/nia/contextservice/ContextService;",x[0],"V",false,x[1]);
        for(String x:new String[]{"InitialDialog","ManualPermissionDialog","FinalDialog"})
            new Spec("Lcom/nianticlabs/background/configuration/"+x+";","onCreate","V",false,x.equals("FinalDialog")?"dialogClose":"dialogCancel","Landroid/os/Bundle;");
        new Spec("Lcom/unity3d/player/W;","handleMessage","Z",false,"renderLoop","Landroid/os/Message;");
    }
    static void require(boolean b,String why){if(!b)throw new IllegalArgumentException(why);}
    static void invoke(MethodImplementationBuilder b,Opcode op,String owner,String name,List<? extends CharSequence> args,String ret,int...regs){
        require(regs.length<=5,"too many invoke words");int[] r=new int[5];System.arraycopy(regs,0,r,0,regs.length);
        b.addInstruction(new BuilderInstruction35c(op,regs.length,r[0],r[1],r[2],r[3],r[4],new ImmutableMethodReference(owner,name,args,ret)));
    }
    static void helper(MethodImplementationBuilder b,String name,String ret,List<String> args,int...r){invoke(b,Opcode.INVOKE_STATIC,DISPATCH,name,args,ret,r);}
    static void original(MethodImplementationBuilder b,Method m,int words){
        int[] r=new int[words];for(int i=0;i<words;i++)r[i]=2+i;
        invoke(b,(m.getAccessFlags()&8)!=0?Opcode.INVOKE_STATIC:Opcode.INVOKE_DIRECT,m.getDefiningClass(),PREFIX+m.getName(),m.getParameterTypes(),m.getReturnType(),r);
    }
    static MethodImplementation wrapper(Method m,Spec s){
        int words=s.statik?0:1;for(String a:s.args)words+=a.equals("J")||a.equals("D")?2:1;
        MethodImplementationBuilder b=new MethodImplementationBuilder(2+words);
        int p=s.statik?2:3;
        if(s.kind.equals("signin")){
            helper(b,"browserSignInEnabled","Z",Collections.emptyList());
            b.addInstruction(new BuilderInstruction11x(Opcode.MOVE_RESULT,0));
            b.addInstruction(new BuilderInstruction21t(Opcode.IF_EQZ,0,b.getLabel("original")));
            helper(b,"signIn","V",Arrays.asList(ACTIVITY,OBJECT),p,p+2);
            b.addInstruction(new BuilderInstruction10x(Opcode.RETURN_VOID));
            b.addLabel("original");original(b,m,words);
        }else if(s.kind.startsWith("permission")){
            if(s.kind.equals("permissionActivity"))b.addInstruction(new BuilderInstruction21t(Opcode.IF_EQZ,p,b.getLabel("original")));
            helper(b,"locationPermission","Z",Arrays.asList(STRING),p+1);
            b.addInstruction(new BuilderInstruction11x(Opcode.MOVE_RESULT,0));
            b.addInstruction(new BuilderInstruction21t(Opcode.IF_EQZ,0,b.getLabel("original")));
            b.addInstruction(new BuilderInstruction11n(Opcode.CONST_4,0,1));
            b.addInstruction(new BuilderInstruction11x(Opcode.RETURN,0));
            b.addLabel("original");original(b,m,words);
            b.addInstruction(new BuilderInstruction11x(Opcode.MOVE_RESULT,0));
            b.addInstruction(new BuilderInstruction11x(Opcode.RETURN,0));
            return b.getMethodImplementation();
        }else if(s.kind.equals("status")){
            helper(b,"permissionStatus",OBJECT,Arrays.asList("J"),p,p+1);
            b.addInstruction(new BuilderInstruction11x(Opcode.MOVE_RESULT_OBJECT,0));
            b.addInstruction(new BuilderInstruction21t(Opcode.IF_EQZ,0,b.getLabel("original")));
            b.addInstruction(new BuilderInstruction21c(Opcode.CHECK_CAST,0,new ImmutableTypeReference(PERMISSION)));
            b.addInstruction(new BuilderInstruction11x(Opcode.RETURN_OBJECT,0));
            b.addLabel("original");original(b,m,words);
            b.addInstruction(new BuilderInstruction11x(Opcode.MOVE_RESULT_OBJECT,0));
            b.addInstruction(new BuilderInstruction11x(Opcode.RETURN_OBJECT,0));
            return b.getMethodImplementation();
        }else if(s.kind.startsWith("dialog")){
            original(b,m,words);
            b.addInstruction(new BuilderInstruction21c(Opcode.CONST_STRING,0,new ImmutableStringReference(s.kind.equals("dialogClose")?"onClickClose":"onClickCancel")));
            helper(b,"dismissDialog","V",Arrays.asList(OBJECT,STRING),2,0);
        }else{
            if(s.kind.startsWith("before"))helper(b,s.kind,"V",Arrays.asList(OBJECT),2);
            original(b,m,words);
            if(s.kind.startsWith("after"))helper(b,s.kind,"V",Arrays.asList(OBJECT),2);
        }
        b.addInstruction(new BuilderInstruction10x(Opcode.RETURN_VOID));
        return b.getMethodImplementation();
    }
    static Method method(Method m,String name,int flags,MethodImplementation body){
        return new ImmutableMethod(m.getDefiningClass(),name,m.getParameters(),m.getReturnType(),flags,m.getAnnotations(),m.getHiddenApiRestrictions(),body);
    }
    static MethodImplementation renderLoop(Method m){
        MutableMethodImplementation body=new MutableMethodImplementation(m.getImplementation());
        int found=0,index=0,insert=-1;
        for(Instruction instruction:m.getImplementation().getInstructions()){
            if(instruction instanceof ReferenceInstruction&&((ReferenceInstruction)instruction).getReference() instanceof MethodReference){
                MethodReference ref=(MethodReference)((ReferenceInstruction)instruction).getReference();
                if(ref.getDefiningClass().equals("Lcom/unity3d/player/UnityPlayerForActivityOrService;")&&ref.getName().equals("nativeRender")){
                    require(instruction.getOpcode()==Opcode.INVOKE_VIRTUAL&&ref.getParameterTypes().isEmpty()&&ref.getReturnType().equals("Z"),"nativeRender call mismatch");
                    require(((FiveRegisterInstruction)instruction).getRegisterCount()==1,"nativeRender receiver mismatch");
                    found++;insert=index+2;
                }
            }
            index++;
        }
        require(found==1&&insert<=body.getInstructions().size()&&body.getInstructions().get(insert-1).getOpcode()==Opcode.MOVE_RESULT,"nativeRender result boundary mismatch");
        // Keep the native method, call and result intact. The callback runs only
        // after normal native return, without consuming or replacing its result.
        body.addInstruction(insert,new BuilderInstruction35c(Opcode.INVOKE_STATIC,0,0,0,0,0,0,
            new ImmutableMethodReference("Ldev/pgrazor/NativeRuntimeBootstrap;","onRendered",Collections.emptyList(),"V")));
        return body;
    }
    static Method preservedBody(Method m){
        List<MethodParameter> parameters=new ArrayList<>();
        for(MethodParameter p:m.getParameters())parameters.add(new ImmutableMethodParameter(p.getType(),Collections.emptySet(),p.getName()));
        // Runtime annotations belong to the entry point, not a second reflected method.
        return new ImmutableMethod(m.getDefiningClass(),PREFIX+m.getName(),parameters,m.getReturnType(),
            ((m.getAccessFlags()&~5)|2|0x1000),Collections.emptySet(),m.getHiddenApiRestrictions(),m.getImplementation());
    }
    static final String LOCATION="Landroid/location/Location;", LOCATION_VALUES="Ldev/pgrazor/NativeLocationValues;";
    static final Set<String> LOCATION_NAMES=new HashSet<>(Arrays.asList("getLatitude","getLongitude","getSpeed","getBearing"));
    static String descriptor(MethodReference m){StringBuilder b=new StringBuilder(m.getDefiningClass()).append("->").append(m.getName()).append('(');
        for(CharSequence p:m.getParameterTypes())b.append(p);return b.append(')').append(m.getReturnType()).toString();}
    static boolean exactLocation(MethodReference ref){return ref.getDefiningClass().equals(LOCATION)&&ref.getParameterTypes().isEmpty()&&
        ((ref.getName().equals("getLatitude")||ref.getName().equals("getLongitude"))?ref.getReturnType().equals("D"):
        (ref.getName().equals("getSpeed")||ref.getName().equals("getBearing"))&&ref.getReturnType().equals("F"));}
    static Method rewriteLocations(Method m,String dex,List<String> inventory,int[] counts){
        MethodImplementation body=m.getImplementation();if(body==null)return m;
        List<Instruction> instructions=new ArrayList<>();boolean dirty=false;int offset=0;
        for(Instruction instruction:body.getInstructions()){
            Instruction rewritten=instruction;
            if(instruction instanceof ReferenceInstruction&&((ReferenceInstruction)instruction).getReference() instanceof MethodReference){
                MethodReference ref=(MethodReference)((ReferenceInstruction)instruction).getReference();
                if(LOCATION_NAMES.contains(ref.getName())){
                    String status="ignored-owner",receiver="-";
                    if(ref.getDefiningClass().equals(LOCATION)){
                        status="ignored-descriptor";
                        if(exactLocation(ref)){
                            status="unsupported-opcode";
                            ImmutableMethodReference target=new ImmutableMethodReference(LOCATION_VALUES,ref.getName(),Collections.singletonList(LOCATION),ref.getReturnType());
                            if(instruction.getOpcode()==Opcode.INVOKE_VIRTUAL){
                                FiveRegisterInstruction registers=(FiveRegisterInstruction)instruction;
                                require(registers.getRegisterCount()==1,"Location receiver words: "+descriptor(m)+"@"+offset);
                                rewritten=new ImmutableInstruction35c(Opcode.INVOKE_STATIC,1,registers.getRegisterC(),registers.getRegisterD(),registers.getRegisterE(),registers.getRegisterF(),registers.getRegisterG(),target);
                                receiver=Integer.toString(registers.getRegisterC());status="rewritten";
                            }else if(instruction.getOpcode()==Opcode.INVOKE_VIRTUAL_RANGE){
                                RegisterRangeInstruction registers=(RegisterRangeInstruction)instruction;
                                require(registers.getRegisterCount()==1,"Location range receiver words: "+descriptor(m)+"@"+offset);
                                rewritten=new ImmutableInstruction3rc(Opcode.INVOKE_STATIC_RANGE,registers.getStartRegister(),1,target);
                                receiver=Integer.toString(registers.getStartRegister());status="rewritten";
                            }
                            if(status.equals("rewritten")){dirty=true;counts[0]++;require(rewritten.getCodeUnits()==instruction.getCodeUnits(),"Location invoke width changed");}
                            else counts[2]++;
                        }
                    }
                    if(!status.equals("rewritten")&&!status.equals("unsupported-opcode"))counts[1]++;
                    inventory.add(status+"\t"+dex+"\t"+descriptor(m)+"\t"+offset+"\t"+descriptor(ref)+"\t"+instruction.getOpcode().name+"\t"+receiver+
                        "\t"+(status.equals("rewritten")?descriptor((MethodReference)((ReferenceInstruction)rewritten).getReference()):"-"));
                }
            }
            instructions.add(rewritten);offset+=instruction.getCodeUnits();
        }
        if(!dirty)return m;
        return method(m,m.getName(),m.getAccessFlags(),new ImmutableMethodImplementation(body.getRegisterCount(),instructions,body.getTryBlocks(),body.getDebugItems()));
    }
    static void verifyLocations(DexFile dex){for(ClassDef cls:dex.getClasses())for(Method m:cls.getMethods()){
        if(m.getImplementation()==null)continue;
        for(Instruction instruction:m.getImplementation().getInstructions())if(instruction instanceof ReferenceInstruction&&((ReferenceInstruction)instruction).getReference() instanceof MethodReference)
            require(!exactLocation((MethodReference)((ReferenceInstruction)instruction).getReference()),"Remaining exact Location getter: "+descriptor(m));
    }}
    public static void main(String[] args)throws Exception{
        require(args.length==2,"usage: BridgeRewriter input.apk output-directory");
        Path out=Paths.get(args[1]);require(!Files.exists(out),"output must not exist");
        MultiDexContainer<? extends DexFile> container=DexFileFactory.loadDexContainer(new File(args[0]),Opcodes.getDefault());
        Map<String,DexFile> changed=new LinkedHashMap<>();Set<String> seen=new HashSet<>();List<String> receipt=new ArrayList<>(),locations=new ArrayList<>();int[] locationCounts=new int[3];
        for(String entry:container.getDexEntryNames()){
            DexFile dex=container.getEntry(entry).getDexFile();List<ClassDef> classes=new ArrayList<>();boolean dirty=false;
            for(ClassDef cls:dex.getClasses()){
                List<Method> methods=new ArrayList<>();boolean classDirty=false;
                for(Method input:cls.getMethods()){
                    Method m=rewriteLocations(input,entry,locations,locationCounts);
                    if(m!=input)dirty=classDirty=true;
                    require(!m.getName().startsWith(PREFIX),"input already patched");
                    String key=cls.getType()+"->"+m.getName();Spec s=SPECS.get(key);
                    if(s==null){methods.add(m);continue;}
                    require(seen.add(key),"duplicate/overloaded target "+key);
                    require(m.getParameterTypes().equals(s.args)&&m.getReturnType().equals(s.ret),"descriptor mismatch "+key);
                    require(((m.getAccessFlags()&8)!=0)==s.statik&&m.getImplementation()!=null,"flags/body mismatch "+key);
                    require((m.getAccessFlags()&(0x100|0x400|0x10000))==0,"native/abstract/constructor target "+key);
                    // Preserve original bytecode/debug/handlers and its synchronization. The public
                    // entry keeps its original ABI/annotations. Nested monitors remain reentrant.
                    if(s.kind.equals("renderLoop"))methods.add(method(m,m.getName(),m.getAccessFlags(),renderLoop(m)));
                    else{methods.add(preservedBody(m));methods.add(method(m,m.getName(),m.getAccessFlags(),wrapper(m,s)));}
                    receipt.add(entry+"\t"+key+"\t"+s.kind);dirty=classDirty=true;
                }
                classes.add(classDirty?new ImmutableClassDef(cls.getType(),cls.getAccessFlags(),cls.getSuperclass(),cls.getInterfaces(),cls.getSourceFile(),cls.getAnnotations(),cls.getFields(),methods):cls);
            }
            if(dirty){String basename=Paths.get(entry).getFileName().toString();require(basename.matches("classes[0-9]*\\.dex"),"unexpected dex name");changed.put(basename,new ImmutableDexFile(dex.getOpcodes(),classes));}
        }
        require(seen.equals(SPECS.keySet()),"missing targets "+difference(SPECS.keySet(),seen));
        Files.createDirectories(out);
        locations.add("#count\trewritten\t"+locationCounts[0]);locations.add("#count\tignored\t"+locationCounts[1]);locations.add("#count\tunsupported\t"+locationCounts[2]);
        Files.write(out.resolve("locations.tsv"),locations);
        require(locationCounts[2]==0,"Unsupported exact Location getter opcode; see locations.tsv");
        for(Map.Entry<String,DexFile> e:changed.entrySet()){
            verifyLocations(e.getValue());DexPool.writeTo(out.resolve(e.getKey()).toString(),e.getValue());
            verifyLocations(DexFileFactory.loadDexFile(out.resolve(e.getKey()).toFile(),Opcodes.getDefault()));
        }
        Files.write(out.resolve("bridges.tsv"),receipt);
        System.out.println("Patched "+seen.size()+" exact methods in "+changed.size()+" DEX; runtime verification pending");
    }
    static Set<String> difference(Set<String>a,Set<String>b){Set<String>r=new TreeSet<>(a);r.removeAll(b);return r;}
}
