// Experimental, supported ARM64 build only. No native work on load.
// Only the exact ordinary ball response caller can request a per-instance 20x
// Animator speed. stop() retains cleanup ownership until a Unity-thread tick
// restores or proves the target destroyed/external speed change.
// The same leased btxu return also reports a confirmed catch outcome. Pending
// (null) responses report nothing; Status values other than success/escape/flee,
// including btxu's synthetic CatchMissed fallback, are logged but never confirmed.
// The same success also starts an optional, read-only Animator state trace
// (trace-* events): no Animator writes, bounded, and isolated from speed/outcomes.
(function(root){
'use strict';
const BALL_SPEED=20;
const LIMITS=Object.freeze({nameBytes:256,assemblies:256,retries:240,leaseMs:2500,pollMs:250,queue:128,timeoutMs:8000,hitsPerSecond:1000,outcomeSeen:4,outcomeMs:8000,outcomesPerSecond:4,outcomeReserve:32,
 traceMs:8000,traceTicks:960,traceEvents:48,traceReserve:32,traceName:64,traceMethods:2048,traceList:24});
const BALL='Niantic.Holoholo.Encounter.Pokeball',CAPTURE='Niantic.Holoholo.Encounter.EncounterCaptureState',RESPONSE='Holoholo.Rpc.CatchPokemonOutProto',ENUM=RESPONSE+'.Types.Status';
const LITERALS=['CatchError','CatchSuccess','CatchEscape','CatchFlee','CatchMissed','CatchItemReplacement'],CONFIRMED=Object.freeze({CatchSuccess:'caught',CatchEscape:'broke-free',CatchFlee:'fled'});
const CODE={btxu:[0xad58170,'fe0f1df8f65701a9f44f02a9b41b02f0f30300aa8886623928040037e0ef01d0'],get_speed:[0xd954a40,'fe57bea9f44f01a974bc00d0f30300aa8866793908010037809700d000800c91'],set_speed:[0xd954b48,'e80f1dfcfe5701a9f44f02a90840201e74bc00d0f30300aa886a793908010037'],op_Implicit:[0xda6bba0,'fe0f1df8f65701a9f44f02a9f4b30090f30300aa882e673988020036f48800d0']};
const SPEC={domain_get:['pointer',[]],domain_get_assemblies:['pointer',['pointer','pointer']],assembly_get_image:['pointer',['pointer']],class_from_name:['pointer',['pointer','pointer','pointer']],class_get_type:['pointer',['pointer']],class_get_field_from_name:['pointer',['pointer','pointer']],field_get_type:['pointer',['pointer']],field_get_flags:['int',['pointer']],field_get_parent:['pointer',['pointer']],field_get_offset:['size_t',['pointer']],class_get_method_from_name:['pointer',['pointer','pointer','int']],method_get_class:['pointer',['pointer']],method_get_name:['pointer',['pointer']],method_get_flags:['uint32',['pointer','pointer']],method_get_param_count:['uint32',['pointer']],method_get_param:['pointer',['pointer','uint32']],method_get_return_type:['pointer',['pointer']],method_is_generic:['bool',['pointer']],method_is_inflated:['bool',['pointer']],class_from_type:['pointer',['pointer']],class_is_enum:['bool',['pointer']],class_enum_basetype:['pointer',['pointer']],class_is_valuetype:['bool',['pointer']],class_value_size:['int',['pointer','pointer']],field_static_get_value:['void',['pointer','pointer']],type_get_name:['pointer',['pointer']],free:['void',['pointer']],thread_current:['pointer',[]],thread_attach:['pointer',['pointer']],thread_detach:['void',['pointer']],object_get_class:['pointer',['pointer']],field_get_value:['void',['pointer','pointer','pointer']],runtime_invoke:['pointer',['pointer','pointer','pointer','pointer']],object_unbox:['pointer',['pointer']],gchandle_new:['pointer',['pointer','bool']],gchandle_get_target:['pointer',['pointer']],gchandle_free:['void',['pointer']]};
// Trace-only exports are optional: a missing one disables only the trace part using it.
const OPTIONAL={object_new:['pointer',['pointer']],field_set_value:['void',['pointer','pointer','pointer']],object_get_virtual_method:['pointer',['pointer','pointer']],string_new:['pointer',['pointer']],type_is_byref:['bool',['pointer']],class_is_assignable_from:['bool',['pointer','pointer']],class_get_methods:['pointer',['pointer','pointer']],string_length:['int',['pointer']],string_chars:['pointer',['pointer']]};
// Read-only trace constants. Hashes are Animator.StringToHash values compared as
// unsigned. Sources: build/feature-research/BALL-PRESENTATION-CONTROLS.txt:76-90,
// 119-125 and BALL-CONTROLLER-DECODED.txt:29-33,173-187.
const NAMEPLATE='Niantic.Holoholo.Encounter.EncounterNameplate',PK='Holoholo.Rpc.PokemonProto',MAPPK='Niantic.Holoholo.Map.IMapPokemon',LEVELS='Niantic.Holoholo.GameMasterDataExt_PlayerLevelSettings',UITEXT='UnityEngine.UI.Text';
// Owned-Pokemon details panel (inventory/collection view). Offsets from
// supported-build metadata (type 13577), with a pinned DoUpdate RVA and
// instruction prefix. Reads the displayed
// PokemonProto and overlays IV/level on the existing CP label, snapshotting the
// game's authentic label so disabling restores it exactly.
const PANEL='Niantic.Holoholo.UI.Pokemon.PokemonInfoPanel';
const OWNED_UPDATE=[0x9a3a8e0,'ff0302d1fd7b02a9fc6f03a9fa6704a9f85f05a9f65706a9'];
const WILDMAP='Niantic.Holoholo.Map.WildMapPokemon',WILDPROTO='Holoholo.Rpc.WildPokemonProto',ENCOUNTEROUT='Holoholo.Rpc.EncounterOutProto',TAPPABLE='Holoholo.Rpc.TappableEncounterProto';
const GROUND='Niantic.Holoholo.Map.GroundTapHandler',MAPVIEW='Niantic.Holoholo.Map.MapViewHandler',LATLNG='Niantic.LatLng',VEC3='UnityEngine.Vector3';
const ARVALUES='Holoholo.Rpc.ARPlusEncounterValuesProto',MAPPOKEMON='Niantic.Holoholo.Map.MapPokemon';
const THROW='Niantic.Holoholo.Encounter.PokeballThrow',ENCOUNTER='Niantic.Holoholo.Encounter.EncounterState';
const INFO='UnityEngine.AnimatorStateInfo',RAC='UnityEngine.RuntimeAnimatorController',OVERRIDE='UnityEngine.AnimatorOverrideController',OPTIONAL_CLASSES=[ARVALUES,MAPPOKEMON,WILDMAP,WILDPROTO,ENCOUNTEROUT,TAPPABLE,NAMEPLATE,PK,MAPPK,LEVELS,UITEXT,PANEL,'UnityEngine.UI.Outline','UnityEngine.UI.Shadow','UnityEngine.Behaviour','UnityEngine.Component','UnityEngine.GameObject','UnityEngine.Color','UnityEngine.Vector2','UnityEngine.TextAnchor','UnityEngine.HorizontalWrapMode',GROUND,MAPVIEW,LATLNG,VEC3,THROW,ENCOUNTER,INFO,'System.Int32','System.String',RAC,OVERRIDE];
const INFO_FIELDS=[['m_Name','System.Int32'],['m_Path','System.Int32'],['m_FullPath','System.Int32'],['m_NormalizedTime','System.Single'],['m_Length','System.Single'],['m_Speed','System.Single'],['m_SpeedMultiplier','System.Single'],['m_Tag','System.Int32'],['m_Loop','System.Int32']];
const BALL_HASHES=[['dsmh','Close',3535843008],['dsmi','Closed',57362642],['dsmj','Spawn',1372578019],['dsmk','Open',71445658],['dsml','Drop',3492260380],['dsmm','Shake',3049724742],['dsmn','CodeDrivenDrop',1232599366],['dsmo','Fly',1808254291],['dsmp','End',951154001],['dsmq','Breakout',596303718],['dsmr','Wait',3722194139],['dsms','ShrinkFinished',382342365],['dsmt','Land',137525990]];
const NUM_SHAKES=3410899964,WAIT=3722194139,END=951154001,CONTROLLER='PokeballAnimator';
// State labels from the decoded controller. Any other hash is reported as unknown.
const STATES=Object.freeze({57362642:'Closed',3492260380:'Drop',2240868719:'Bounce',1281384737:'ShakeWait',824257959:'3Shakes',2003394077:'Caught',951154001:'End',137525990:'Land',1232599366:'CodeDrivenDrop',3722194139:'Wait',3535843008:'Close',1372578019:'Spawn',71445658:'Open',1808254291:'Fly',596303718:'Breakout'});
const FLAGGED=new Set([137525990,1232599366,3722194139]);
let running=null;
// Keep native callback storage alive through any invocation already in flight.
const retainedFilters=[];
const FILTER=`
#include <gum/guminterceptor.h>
#include <glib.h>
extern gpointer execute_method;
extern int enter_match(unsigned int tid);
extern void leave_match(unsigned int tid);
typedef struct {int accepted;} Match;
void on_enter(GumInvocationContext *ic){Match *m=GUM_IC_GET_INVOCATION_DATA(ic,Match);m->accepted=0;if(gum_invocation_context_get_nth_argument(ic,0)==execute_method)m->accepted=enter_match(gum_invocation_context_get_thread_id(ic));}
void on_leave(GumInvocationContext *ic){Match *m=GUM_IC_GET_INVOCATION_DATA(ic,Match);if(m->accepted)leave_match(gum_invocation_context_get_thread_id(ic));}
`;
    const DEBUG=[0x90051008,0xf9474508,0xb4000148,0x90051009,0xf9474929,0xb40000e9,
        0xf940040a,0xeb0a011f,0x2a1f03e8,0x54000128,0xeb0a013f,0x540000e3,
        0xb40000a1,0xf9400408,0xb900083f,0xf900083f,0xf9000028,0x52800028,
        0x2a0803e0,0xd65f03c0];
    function text(p) {
        if(p.isNull())throw Error('null-name');
        let n=0;while(n<LIMITS.nameBytes && p.add(n).readU8())n++;
        if(n===LIMITS.nameBytes)throw Error('name-limit');
        return n?p.readUtf8String(n):'';
    }
    function bind(module) {
        const api={};
        for(const [key,[result,args]] of Object.entries(SPEC)) {
            const address=module.findExportByName('il2cpp_'+key);
            if(!address || address.isNull())throw Error('missing-export:'+key);
            api[key]=new NativeFunction(address,result,args,{scheduling:'exclusive'});
        }
        for(const [key,[result,args]] of Object.entries(OPTIONAL)) {
            try {
                const address=module.findExportByName('il2cpp_'+key);
                if(address && !address.isNull())api[key]=new NativeFunction(address,result,args,{scheduling:'exclusive'});
            }catch(_){}
        }
        return api;
    }
    function typeName(api,type) {
        const value=api.type_get_name(type);
        if(value.isNull())throw Error('null-type-name');
        try{return text(value);}finally{api.free(value);}
    }
    function clock() {
        const libc=Process.findModuleByName('libc.so');
        const address=libc&&libc.findExportByName('clock_gettime');
        if(!address || address.isNull())throw Error('missing-monotonic-clock');
        const call=new NativeFunction(address,'int',['int','pointer'],{scheduling:'exclusive'});
        const out=Memory.alloc(16);let previous=-1;
        return ()=>{
            if(call(7,out)!==0)throw Error('monotonic-clock-failed');
            const sec=out.readS64().toNumber(),ns=out.add(8).readS64().toNumber(),now=sec*1000+ns/1e6;
            if(!Number.isSafeInteger(sec)||sec<0||ns<0||ns>=1e9||!Number.isFinite(now)||now<previous)throw Error('invalid-monotonic-clock');
            previous=now;return now;
        };
    }
    function checkBytes(address,hex) {
        const bytes=new Uint8Array(address.readByteArray(hex.length/2));
        if(bytes.length!==hex.length/2)throw Error('short-instruction-read');
        for(let i=0;i<bytes.length;i++)if(bytes[i]!==parseInt(hex.slice(i*2,i*2+2),16))throw Error('unsupported-instructions');
    }
    function executable(module,address,size) {
        if(address.isNull()||!address.and(3).isNull()||address.compare(module.base)<0||address.add(size).compare(module.base.add(module.size))>0)throw Error('invalid-code-range');
        const range=Process.findRangeByAddress(address);
        if(!range||range.protection.indexOf('r')<0||!(range.protection.includes('x')||(typeof PGRazorGuestCode==='function'&&PGRazorGuestCode(module,address,size,range)))||address.add(size).compare(range.base.add(range.size))>0)throw Error('code-not-rx');
    }
    function pointerHandleAbi(module) {
        // Narrow fingerprint of the shipped 6000.3.22f1 ARM64 implementation.
        // No GC handle function is called until this pointer ABI is verified.
        // See build/feature-research/GC-HANDLE-ABI.txt; unknown code fails closed.
        function readableCode(address,length) {
            if(address.compare(module.base)<0 || address.add(length).compare(module.base.add(module.size))>0)return false;
            const range=Process.findRangeByAddress(address);
            return range!==null && range.protection.includes('r') && (range.protection.includes('x')||(typeof PGRazorGuestCode==='function'&&PGRazorGuestCode(module,address,length,range))) &&
                address.add(length).compare(range.base.add(range.size))<=0;
        }
        function words(address,expected) {
            return readableCode(address,expected.length*4) && expected.every((word,i)=>address.add(i*4).readU32()===word);
        }
        function branch(address) {
            if(!readableCode(address,4))return null;
            const word=address.readU32();
            if((word>>>26)!==5)return null; // AArch64 B imm26, never BL or an indirect target.
            let immediate=word&0x03ffffff;if(immediate&0x02000000)immediate-=0x04000000;
            const target=address.add(immediate*4);
            return readableCode(target,4)?target:null;
        }
        try {
            const create=module.findExportByName('il2cpp_gchandle_new'),get=module.findExportByName('il2cpp_gchandle_get_target'),free=module.findExportByName('il2cpp_gchandle_free');
            if(!create || !get || !free || create.isNull() || get.isNull() || free.isNull())return false;
            if(!words(create,[0xaa0003e8,0x52800049,0x7200003f,0x1a890520,0xaa0803e1]))return false;
            const createBody=branch(create.add(20)),getBody=branch(get),freeBody=branch(free);
            return createBody!==null && getBody!==null && freeBody!==null &&
                words(createBody.add(0x384),[0x9a953120]) && // csel x0,x9,x21,lo: full-width return
                words(getBody,[0xf81b0ffe]) && words(getBody.add(20),[0x9273c817,0x394082e8]) &&
                words(freeBody,[0xf81b0ffe]) && words(freeBody.add(24),[0x9273c816,0x394082c8]);
        }catch(_){return false;}
    }

function prepare(module,api,domain){
 let owned=null;
 try{
  if(api.thread_current().isNull()){owned=api.thread_attach(domain);if(owned.isNull())throw Error('metadata-thread-attach');}
  const count=Memory.alloc(8);count.writeU64(0);const list=api.domain_get_assemblies(domain,count),total=count.readU64();
  if(total.compare(0)===0)return null;if(total.compare(LIMITS.assemblies)>0||list.isNull())throw Error('assembly-limit');
  const names=[BALL,CAPTURE,RESPONSE,'UnityEngine.Animator','UnityEngine.Object','UnityEngine.UnitySynchronizationContext','System.Single','System.Boolean'],classes={};
  // Trace-only classes are looked up separately: absence or ambiguity disables only the trace.
  const extra={},conflict=new Set();
  const inputs=names.concat(OPTIONAL_CLASSES).map(n=>{const i=n.lastIndexOf('.');return[n,Memory.allocUtf8String(n.slice(0,i)),Memory.allocUtf8String(n.slice(i+1))];});
  for(let i=0;i<total.toNumber();i++){const image=api.assembly_get_image(list.add(i*8).readPointer());if(image.isNull())continue;
   for(const [name,ns,short]of inputs){const k=api.class_from_name(image,ns,short);if(k.isNull())continue;
    if(OPTIONAL_CLASSES.includes(name)){try{if(typeName(api,api.class_get_type(k))!==name||(extra[name]&&!extra[name].equals(k)))conflict.add(name);else extra[name]=k;}catch(_){conflict.add(name);}continue;}
    if(typeName(api,api.class_get_type(k))!==name||(classes[name]&&!classes[name].equals(k)))throw Error('class-mismatch:'+name);classes[name]=k;}}
  if(Object.keys(classes).length!==names.length)return null;
  const fieldMeta=[];
  function check(owner,name,type,offset){const f=api.class_get_field_from_name(owner,Memory.allocUtf8String(name));
   if(f.isNull()||!api.field_get_parent(f).equals(owner)||(api.field_get_flags(f)&0x50)||typeName(api,api.field_get_type(f))!==type||Number(api.field_get_offset(f).toString())!==offset)throw Error('field-mismatch:'+name);
   return f;}
  function field(owner,name,type,offset){const f=check(owner,name,type,offset);fieldMeta.push({name,type,offset});return f;}
  const fields={animator:field(classes[BALL],'animator','UnityEngine.Animator',0xb0),bread:field(classes[BALL],'<HoldingBreadBall>k__BackingField','System.Boolean',0x240),status:field(classes[RESPONSE],'Status',ENUM,0x10)};
  const enumClass=api.class_from_type(api.field_get_type(fields.status)),align=Memory.alloc(4);
  if(enumClass.isNull()||!api.class_is_enum(enumClass)||typeName(api,api.class_enum_basetype(enumClass))!=='System.Int32'||api.class_value_size(enumClass,align)!==4||align.readU32()!==4)throw Error('enum-abi');
  const literal=api.class_get_field_from_name(enumClass,Memory.allocUtf8String('CatchSuccess'));
  if(literal.isNull()||!api.field_get_parent(literal).equals(enumClass)||(api.field_get_flags(literal)&0x8050)!==0x8050||typeName(api,api.field_get_type(literal))!==ENUM)throw Error('success-literal');
  const out=Memory.alloc(4);api.field_static_get_value(literal,out);const success=out.readS32();
  // Outcome reporting needs every known literal by name with distinct values.
  // A mismatch disables only outcome reporting; the success guard above is unchanged.
  let outcomes;
  try{const values={},used=new Set();
   for(const name of LITERALS){const f=api.class_get_field_from_name(enumClass,Memory.allocUtf8String(name));
    if(f.isNull()||!api.field_get_parent(f).equals(enumClass)||(api.field_get_flags(f)&0x8050)!==0x8050||typeName(api,api.field_get_type(f))!==ENUM)throw Error('outcome-literal:'+name);
    const v=Memory.alloc(4);api.field_static_get_value(f,v);const n=v.readS32();if(used.has(n))throw Error('outcome-literal-duplicate:'+name);used.add(n);values[name]=n;}
   if(values.CatchSuccess!==success)throw Error('outcome-success-mismatch');outcomes={available:true,values:Object.freeze(values)};
  }catch(e){outcomes={available:false,reason:String(e&&e.message||e).slice(0,120)};}
  for(const[name,size]of[['System.Single',4],['System.Boolean',1]])if(!api.class_is_valuetype(classes[name])||api.class_value_size(classes[name],align)!==size||align.readU32()!==size)throw Error('boxed-abi:'+name);
  if(!pointerHandleAbi(module))throw Error('gc-pointer-abi');
  const debugAddress=module.findExportByName('il2cpp_debug_get_method_info');if(!debugAddress)throw Error('debug-missing');executable(module,debugAddress,80);
  for(let i=0;i<DEBUG.length;i++)if(debugAddress.add(i*4).readU32()!==DEBUG[i])throw Error('debug-abi');
  const debug=new NativeFunction(debugAddress,'bool',['pointer','pointer'],{scheduling:'exclusive'}),addresses=new Set();
  function method(owner,name,ret,params,isStatic,code){const m=api.class_get_method_from_name(owner,Memory.allocUtf8String(name),params.length),impl=Memory.alloc(4);
   if(m.isNull()||!api.method_get_class(m).equals(owner)||text(api.method_get_name(m))!==name||Boolean(api.method_get_flags(m,impl)&16)!==isStatic||api.method_get_param_count(m)!==params.length||api.method_is_generic(m)||api.method_is_inflated(m)||(typeof ret==='function'?!ret(typeName(api,api.method_get_return_type(m))):typeName(api,api.method_get_return_type(m))!==ret))throw Error('method-mismatch:'+name);
   for(let i=0;i<params.length;i++)if(typeName(api,api.method_get_param(m,i))!==params[i])throw Error('parameter-mismatch:'+name);
   let address=null;if(code){const info=Memory.alloc(24);info.writeByteArray(new Uint8Array(24));if(!debug(m,info))throw Error('debug-failed:'+name);address=info.readPointer();
    if(!address.equals(module.base.add(code[0]))||addresses.has(address.toString()))throw Error('method-address:'+name);addresses.add(address.toString());executable(module,address,code[1].length/2);checkBytes(address,code[1]);}
   return {method:m,address};}
  const methods={escaped:method(classes[CAPTURE],'btdf','System.Void',[RESPONSE],false,[0xacf104c,'fd7bbaa9fc6f01a9fa6702a9f85f03a9f65704a9f44f05a9f51e02d0f40301aa']),btxu:method(classes[BALL],'btxu',RESPONSE,[],false,CODE.btxu),get:method(classes['UnityEngine.Animator'],'get_speed','System.Single',[],false,CODE.get_speed),set:method(classes['UnityEngine.Animator'],'set_speed','System.Void',['System.Single'],false,CODE.set_speed),alive:method(classes['UnityEngine.Object'],'op_Implicit','System.Boolean',['UnityEngine.Object'],true,CODE.op_Implicit),execute:method(classes['UnityEngine.UnitySynchronizationContext'],'ExecuteTasks','System.Void',[],true,null),caught:method(classes[CAPTURE],'btdd','System.Void',[RESPONSE],false,[0xacf0a8c,'fd7bbaa9fc6f01a9fa6702a9f85f03a9f65704a9f44f05a9f51e02f0f40301aa'])};
  executable(module,module.base.add(0xad691c8),16);checkBytes(module.base.add(0xad691c8),'e00314aae9bbff97c82c02b0f60313aa');
  // Read-only trace metadata. A failure disables only the trace (or one optional
  // part of it) and never fails prepare(), speed or outcomes. Trace methods are
  // only invoked through metadata (never hooked), so no code address is pinned.
  function part(fn){try{return Object.assign({available:true},fn());}catch(e){return{available:false,reason:String(e&&e.message||e).slice(0,120)};}}
  function need(name){if(conflict.has(name))throw Error('trace-class-mismatch:'+name);if(!extra[name])throw Error('trace-class-missing:'+name);return extra[name];}
  // GetBool/GetInteger also have (String) overloads, so resolve the (Int32) one exactly.
  function overload(owner,name,ret){if(!api.class_get_methods)throw Error('trace-export:class_get_methods');
   const iter=Memory.alloc(8),impl=Memory.alloc(4);iter.writePointer(ptr(0));let found=null;
   for(let i=0;;i++){if(i>=LIMITS.traceMethods)throw Error('method-limit:'+name);const m=api.class_get_methods(owner,iter);if(m.isNull())break;
    if(api.method_get_param_count(m)!==1||text(api.method_get_name(m))!==name||typeName(api,api.method_get_param(m,0))!=='System.Int32')continue;
    if(found)throw Error('overload-duplicate:'+name);found=m;}
   if(!found)throw Error('overload-missing:'+name);
   if(!api.method_get_class(found).equals(owner)||(api.method_get_flags(found,impl)&16)||api.method_is_generic(found)||api.method_is_inflated(found)||typeName(api,api.method_get_return_type(found))!==ret)throw Error('method-mismatch:'+name);
   return{method:found,address:null};}
  const animator=classes['UnityEngine.Animator'],ballClass=classes[BALL];
  const trace=part(()=>{const info=need(INFO),int32=need('System.Int32');
   if(!api.class_is_valuetype(int32)||api.class_value_size(int32,align)!==4||align.readU32()!==4)throw Error('trace-abi:System.Int32');
   // AnimatorStateInfo comes back boxed from runtime_invoke (its invoker owns the
   // struct-return ABI) and is read through field metadata; the 36-byte, 4-aligned
   // layout and each field's name/type/offset are checked, not assumed.
   if(!api.class_is_valuetype(info)||api.class_value_size(info,align)!==36||align.readU32()!==4)throw Error('trace-abi:'+INFO);
   const layout={};INFO_FIELDS.forEach(([name,type],i)=>{layout[name]=check(info,name,type,0x10+i*4);});
   return{classes:{info,int32},layout,
    methods:{current:method(animator,'GetCurrentAnimatorStateInfo',INFO,['System.Int32'],false,null),next:method(animator,'GetNextAnimatorStateInfo',INFO,['System.Int32'],false,null),transition:method(animator,'IsInTransition','System.Boolean',['System.Int32'],false,null)},
    identity:part(()=>{const controller=need(RAC);
     const hashes=BALL_HASHES.map(([name,label,expected],i)=>({label,expected,field:check(ballClass,name,'System.Int32',0x340+i*4)}));
     // Metadata only here: the static value is read on the Unity thread, never from this timer thread.
     const shakes=api.class_get_field_from_name(ballClass,Memory.allocUtf8String('dsjv'));
     if(shakes.isNull()||!api.field_get_parent(shakes).equals(ballClass)||(api.field_get_flags(shakes)&0x50)!==0x10||typeName(api,api.field_get_type(shakes))!=='System.Int32'||Number(api.field_get_offset(shakes).toString())!==4)throw Error('static-mismatch:dsjv');
     return{hashes,shakes,controller,override:extra[OVERRIDE]||null,
      methods:{controller:method(animator,'get_runtimeAnimatorController',RAC,[],false,null),layers:method(animator,'get_layerCount','System.Int32',[],false,null),parameters:method(animator,'get_parameterCount','System.Int32',[],false,null)}};}),
    name:part(()=>{const string=need('System.String');if(!api.string_length||!api.string_chars)throw Error('trace-export:string');
     return{string,method:method(classes['UnityEngine.Object'],'get_name','System.String',[],false,null)};}),
    params:part(()=>({bool:overload(animator,'GetBool','System.Boolean'),int:overload(animator,'GetInteger','System.Int32')}))};});
  const throws=part(()=>{const k=need(THROW),owner=need(ENCOUNTER);
   if(!api.class_is_assignable_from)throw Error('throw-assignability-unavailable');
   if(!api.class_is_valuetype(k)||api.class_value_size(k,align)!==12||align.readU32()!==4)throw Error('throw-value-abi');
   for(const [name,type,off] of [['BallType','Holoholo.Rpc.Item',16],['ReticleSize','System.Single',20],['HitBullseye','System.Boolean',24],['Spinning','System.Boolean',25],['Missed','System.Boolean',26]])check(k,name,type,off);
   const submit=method(classes[BALL],'btyl','System.Void',[THROW],false,[0xad5fbdc,'fe5fbda9f65701a9f44f02a9961b0290f50302aaf40301aac8026539f30300aa']);
   const hit=method(owner,'NotifyPokeballHit','System.Void',[THROW],false,[0xad93280,'080441f9c80000b4002140f9031540f9427c4092040d40f980001fd6c0035fd6']);
   return{submit,hit,owner};
  });
  const arThrows=part(()=>{
   if(!throws.available||!api.object_new||!api.field_set_value)throw Error('ar-throw-api-unavailable');
   const values=need(ARVALUES),owner=need(MAPPOKEMON),wild=need(WILDMAP);
   const proximity=check(values,'Proximity','System.Single',0x10),awareness=check(values,'Awareness','System.Single',0x14),frightened=check(values,'PokemonFrightened','System.Boolean',0x18);
   return{values,wild,proximity,awareness,frightened,capture:method(owner,'TryCapture',n=>/^Niantic\.Promises\.IPromise(?:`1)?<Holoholo\.Rpc\.CatchPokemonOutProto>$/.test(n),[THROW,ARVALUES],false,[0x9e1cba0,'ff4302d1e81300fdfd7b03a9fc6f04a9fa6705a9f85f06a9'])};
  });
  const mapTap=part(()=>{
   const ground=need(GROUND),map=need(MAPVIEW),latlng=need(LATLNG),vector=need(VEC3);
   for(const [k,size,alignment] of [[latlng,16,8],[vector,12,4]])if(!api.class_is_valuetype(k)||api.class_value_size(k,align)!==size||align.readU32()!==alignment)throw Error('map-value-abi');
   for(const [name,off] of [['Latitude',16],['Longitude',24]])check(latlng,name,'System.Double',off);
   for(const [name,off] of [['x',16],['y',20],['z',24]])check(vector,name,'System.Single',off);
   const gm=api.class_get_method_from_name(map,Memory.allocUtf8String('GetGroundPosition'),2);
   if(gm.isNull()||!api.type_is_byref)throw Error('map-ground-signature');
   const outType=api.method_get_param(gm,1),outName=typeName(api,outType);
   if(outName.replace(/&$/,'')!==VEC3||!api.type_is_byref(outType))throw Error('map-ground-byref');
   return{ground,map,latlng,
    tap:method(ground,'OnTap','System.Void',['System.Object','System.EventArgs'],false,[0x9e8307c,'ff0302d1e923036dfe2300f9f85f05a9f65706a9f44f07a9349202f0f30300aa']),
    position:method(map,'GetGroundPosition','System.Boolean',['UnityEngine.Vector2',outName],false,[0x9e87638,'e923bc6dfe0b00f9f65702a9f44f03a92840201e0940201e159202f0a8026539']),
    location:method(map,'GetWorldLocation',LATLNG,[VEC3],false,[0x9e87568,'ea0f1cfce923016dfe5702a9f44f03a94840201e2940201e149202f00a40201e'])};
  });
  const iv=part(()=>{
   const plate=need(NAMEPLATE),pokemon=need(PK),mapPokemon=need(MAPPK),textClass=need(UITEXT),levels=need(LEVELS),integer=need('System.Int32');
   if(!api.object_get_virtual_method||!api.string_new||!api.class_is_assignable_from)throw Error('iv-export-unavailable');
   const values={};for(const [name,type,off]of [['cp_','System.Int32',0x2c],['individualAttack_','System.Int32',0x70],['individualDefense_','System.Int32',0x74],['individualStamina_','System.Int32',0x78],['cpMultiplier_','System.Single',0x7c],['additionalCpMultiplier_','System.Single',0xac]])values[name]=check(pokemon,name,type,off);
   function literal(owner,name){const k=need(owner),f=api.class_get_field_from_name(k,Memory.allocUtf8String(name));if(f.isNull()||!api.field_get_parent(f).equals(k)||(api.field_get_flags(f)&0x8050)!==0x8050||typeName(api,api.field_get_type(f))!==owner)throw Error('iv-enum:'+name);const out=Memory.alloc(4);api.field_static_get_value(f,out);return out.readS32();}
   return{plate,pokemon,mapPokemon,textClass,integer,values,string:need('System.String'),
    textField:check(plate,'cpText',UITEXT,0x80),gmField:check(plate,'dsqf','IGameMasterData',0x68),
    setup:method(plate,'SetPokemonUI','System.Void',[MAPPK,'Niantic.Rpc.GameplayWeatherProto.Types.WeatherCondition'],false,[0xad84364,'ff8303d1fd7b08a9fc6f09a9fa670aa9f85f0ba9f6570ca9f44f0da9541a02f0']),
    wildMap:need(WILDMAP),wildProto:need(WILDPROTO),encounterOut:need(ENCOUNTEROUT),tappable:need(TAPPABLE),
    mapWild:check(need(WILDMAP),'eluo',WILDPROTO,0x270),mapResponse:check(need(WILDMAP),'elup',ENCOUNTEROUT,0x278),mapTappable:check(need(WILDMAP),'elus',TAPPABLE,0x290),
    wildLatitude:check(need(WILDPROTO),'Latitude','System.Double',0x20),wildLongitude:check(need(WILDPROTO),'Longitude','System.Double',0x28),
    responseWild:check(need(ENCOUNTEROUT),'Pokemon',WILDPROTO,0x10),wildPokemon:check(need(WILDPROTO),'Pokemon',PK,0x38),wildId:check(need(WILDPROTO),'EncounterId','System.UInt64',0x10),tappablePokemon:check(need(TAPPABLE),'Pokemon',PK,0x18),
    getLevel:method(levels,'GetPokemonLevelOfCpMultiplier','System.Int32',['IGameMasterData','System.Single'],true,[0xa85eed4,'ff4301d1e81300fdfe5703a9f44f04a90840201e744302f0f30300aa88d67239']),
    getMultiplier:method(levels,'CpMultiplierAtLevel','System.Single',['IGameMasterData','System.Int32'],true,[0xa85f504,'ff4301d1fe1300f9f65703a9f44f04a9754302d0f303012af40300aaa8de7239']),
    setText:method(textClass,'set_text','System.Void',['System.String'],false,null),
    rich:method(textClass,'set_supportRichText','System.Void',['System.Boolean'],false,null),
    font:method(textClass,'get_fontSize','System.Int32',[],false,null),setFont:method(textClass,'set_fontSize','System.Void',['System.Int32'],false,null),
    fit:method(textClass,'set_resizeTextForBestFit','System.Void',['System.Boolean'],false,null),
    overflow:method(textClass,'set_horizontalOverflow','System.Void',['UnityEngine.HorizontalWrapMode'],false,null),
    alignment:method(textClass,'set_alignment','System.Void',['UnityEngine.TextAnchor'],false,null),
    overflowValue:literal('UnityEngine.HorizontalWrapMode','Overflow'),alignmentValue:literal('UnityEngine.TextAnchor','MiddleCenter')};
  });
  // Snapshot getters are shared so either display can restore the native label.
  const ivText=part(()=>{
   if(!iv.available||!api.string_length||!api.string_chars)throw Error('iv-text-api');
   const textClass=need(UITEXT),hwm=need('UnityEngine.HorizontalWrapMode'),ta=need('UnityEngine.TextAnchor');
   return{hwm,ta,
    getText:method(textClass,'get_text','System.String',[],false,null),
    getRich:method(textClass,'get_supportRichText','System.Boolean',[],false,null),
    getFit:method(textClass,'get_resizeTextForBestFit','System.Boolean',[],false,null),
    getOverflow:method(textClass,'get_horizontalOverflow','UnityEngine.HorizontalWrapMode',[],false,null),
    getAlignment:method(textClass,'get_alignment','UnityEngine.TextAnchor',[],false,null)};
  });
  const ivOwned=part(()=>{
   if(!ivText.available)throw Error('iv-text-unavailable');
   const panel=need(PANEL);
   return{panel,cpText:check(panel,'cpValueText',UITEXT,0x3c8),
    displayed:check(panel,'<PokemonProto>k__BackingField',PK,0xa60),gmField:check(panel,'egfu','IGameMasterData',0x80),
    update:method(panel,'DoUpdate','System.Void',[PK],false,OWNED_UPDATE)};
  });
  const ivOutline=part(()=>{
   const component=need('UnityEngine.Component'),gameObject=need('UnityEngine.GameObject'),outline=need('UnityEngine.UI.Outline'),shadow=need('UnityEngine.UI.Shadow'),behaviour=need('UnityEngine.Behaviour');
   const color=need('UnityEngine.Color'),vector=need('UnityEngine.Vector2');
   for(const [k,size]of [[color,16],[vector,8]])if(!api.class_is_valuetype(k)||api.class_value_size(k,align)!==size||align.readU32()!==4)throw Error('iv-outline-value-abi');
   const address=module.findExportByName('il2cpp_type_get_object');if(!address)throw Error('iv-outline-type-api');
   return{outline,color,vector,typeObject:new NativeFunction(address,'pointer',['pointer'],{scheduling:'exclusive'}),
    gameObject:method(component,'get_gameObject','UnityEngine.GameObject',[],false,null),
    get:method(gameObject,'GetComponent','UnityEngine.Component',['System.Type'],false,null),
    add:method(gameObject,'AddComponent','UnityEngine.Component',['System.Type'],false,null),
    enabled:method(behaviour,'get_enabled','System.Boolean',[],false,null),setEnabled:method(behaviour,'set_enabled','System.Void',['System.Boolean'],false,null),
    colorGet:method(shadow,'get_effectColor','UnityEngine.Color',[],false,null),colorSet:method(shadow,'set_effectColor','System.Void',['UnityEngine.Color'],false,null),
    distanceGet:method(shadow,'get_effectDistance','UnityEngine.Vector2',[],false,null),distanceSet:method(shadow,'set_effectDistance','System.Void',['UnityEngine.Vector2'],false,null)};
  });
  // Map-only shiny scanner metadata (read-only; never hooked for capture). Reuses
  // iv's validated encounter-response field map and adds the species field, the
  // VisibleOnMap/IsShinyPokemon getters, FindObjectsOfType and the map request.
  // Unavailable here disables ONLY the scanner.
  const shinyScanner=part(()=>{
   if(!iv.available)throw Error('scanner-iv-metadata');if(!api.class_get_methods)throw Error('scanner-export:class_get_methods');
   const request=method(need(WILDMAP),'SendEncounterRequest',n=>n.includes('<Niantic.Holoholo.Map.PokemonEncounterResponse>'),[],false,null);
   const iter=Memory.alloc(8);iter.writePointer(ptr(0));let findAll=null;
   for(let i=0;;i++){if(i>=LIMITS.traceMethods)throw Error('scanner-method-limit');const m=api.class_get_methods(classes['UnityEngine.Object'],iter);if(m.isNull())break;
    if(text(api.method_get_name(m))!=='FindObjectsOfType'||api.method_is_generic(m)||api.method_get_param_count(m)!==1||typeName(api,api.method_get_param(m,0))!=='System.Type'||typeName(api,api.method_get_return_type(m))!=='UnityEngine.Object[]')continue;
    if(findAll)throw Error('scanner-find-all-duplicate');findAll=m;}
   if(!findAll)throw Error('scanner-find-all');
   const typeExport=module.findExportByName('il2cpp_type_get_object');if(!typeExport)throw Error('scanner-type-object');
   const statusType=ENCOUNTEROUT+'.Types.Status',status=check(need(ENCOUNTEROUT),'Status',statusType,0x1c);
   const statusClass=api.class_from_type(api.field_get_type(status)),statusAlign=Memory.alloc(4);
   if(statusClass.isNull()||!api.class_is_enum(statusClass)||typeName(api,api.class_enum_basetype(statusClass))!=='System.Int32'||api.class_value_size(statusClass,statusAlign)!==4||statusAlign.readU32()!==4)throw Error('scanner-status-abi');
   const successField=api.class_get_field_from_name(statusClass,Memory.allocUtf8String('EncounterSuccess'));
   if(successField.isNull()||!api.field_get_parent(successField).equals(statusClass)||(api.field_get_flags(successField)&0x8050)!==0x8050||typeName(api,api.field_get_type(successField))!==statusType)throw Error('scanner-status-success');
   const successValue=Memory.alloc(4);api.field_static_get_value(successField,successValue);
   return{request,findAll,typeObject:new NativeFunction(typeExport,'pointer',['pointer'],{scheduling:'exclusive'}),
    status,success:successValue.readS32(),
    species:check(need(PK),'pokemonId_','System.Int32',0x28),
    visible:method(need(WILDMAP),'get_VisibleOnMap','System.Boolean',[],false,null),
    shiny:method(need(PK),'get_IsShinyPokemon','System.Boolean',[],false,null)};
  });
  return{module,api,classes,fields,fieldMeta,methods,success,outcomes,trace,throws,arThrows,mapTap,iv,ivText,ivOwned,ivOutline,shinyScanner,caller:module.base.add(0xad691d0)};
 }finally{if(owned&&!owned.isNull())api.thread_detach(owned);}
}
function event(s,kind,fields){if(s.queue.length>=LIMITS.queue){fault(s,Error('queue-limit'));return false;}let time=null;try{time=s.now();}catch(_){}s.queue.push(Object.assign({kind,boottimeMs:time},fields||{}));return true;}
function fault(s,error){s.allowedUntil=0;s.stopping=true;s.reason=s.reason||String(error).slice(0,180);}
function flush(s){for(const e of s.queue.splice(0)){try{console.log('[pgrazor-quick-ball] '+JSON.stringify(e));}catch(_){}if(typeof s.onEvent==='function')try{s.onEvent(e);}catch(_){}}}
// ARM64 AAPCS: the 12-byte value arrives in x1 (item + float) and low w2 (three bools + padding).
function throwWords(first,second,grade,curve,chooseRadius){
 const hex=first.replace(/^0x/,'').padStart(16,'0');if(!/^[0-9a-f]{16}$/i.test(hex))return null;
 const item=parseInt(hex.slice(8),16),flags=second>>>0;
 if(![1,2,3,5].includes(item)||(flags&255)>1||((flags>>>8)&255)>1||((flags>>>16)&255)!==0)return null;
 const bits=parseInt(hex.slice(0,8),16),view=new DataView(new ArrayBuffer(4));view.setUint32(0,bits);const radius=view.getFloat32(0);
 if(!Number.isFinite(radius)||radius<0||radius>1)return null;
 // Test ranges stay inside the published rating bands, away from their cutoffs.
 const ranges=[null,[0.75,0.95],[0.35,0.65],[0.10,0.25]];
 if(grade){const [low,high]=ranges[grade];view.setFloat32(0,chooseRadius(()=>low+(high-low)*Math.random()));}
 const outBits=grade?view.getUint32(0):bits;
 return{first:'0x'+outBits.toString(16).padStart(8,'0')+hex.slice(8),second:(flags|(grade?1:0)|(curve?256:0))>>>0,reticle:view.getFloat32(0)};
}
function throwForRoute(s,first,second,route,now){
 const previous=s.throwSample;s.throwSample=null;
 const paired=route==='hit'&&previous&&previous.grade===s.throwGrade&&now>=previous.at&&now-previous.at<LIMITS.leaseMs&&(first===previous.original||first===previous.changed);
 const changed=throwWords(first,second,s.throwGrade,s.throwCurve,sample=>paired?previous.radius:sample());
 // The submit and hit hooks can see the same throw. Keep its one sampled float,
 // not native object pointers; consume once and discard on the next submit.
 if(changed&&s.throwGrade&&route==='submit')s.throwSample={grade:s.throwGrade,at:now,original:first,changed:changed.first,radius:changed.reticle};
 return changed;
}
function encounterLabel(level,attack,defense,stamina,cp){
 const validIv=[attack,defense,stamina].every(v=>Number.isInteger(v)&&v>=0&&v<=15);
 const iv=validIv?(Math.round((attack+defense+stamina)/45*1000)/10)+'%':'--';
 const lv=Number.isFinite(level)&&level>=1&&level<=100&&Number.isInteger(level*2)?String(level):'--';
 const percent=validIv?(attack+defense+stamina)/45*100:null;
 function shade(a,b,t){return '#'+a.map((v,i)=>Math.round(v+(b[i]-v)*Math.max(0,Math.min(1,t))).toString(16).padStart(2,'0')).join('');}
 const color=percent===100?'#ff5252':percent>=90?shade([190,145,255],[235,190,255],(percent-90)/10):percent>=80?shade([105,215,135],[160,245,175],(percent-80)/10):'#dedede';
 // cpText already has a separate native CP prefix. Keep its number first.
 return (Number.isInteger(cp)&&cp>0?cp:'--')+' \u00b7 Lv '+lv+' \u00b7 <color="'+color+'">IV '+iv+'</color>';
}
function install(s,p){
 const {api,classes,fields,methods}=p;
 function exact(o,k){return !o.isNull()&&api.object_get_class(o).equals(k);}
 function hold(o){if(o.isNull())throw Error('null-managed-object');const h=api.gchandle_new(o,0);if(h.isNull())throw Error('gc-handle-new');return h;}
 function target(h){const o=api.gchandle_get_target(h);if(o.isNull())throw Error('gc-target-null');return o;}
 function invoke(m,o,args){const ex=Memory.alloc(8);ex.writePointer(ptr(0));const result=api.runtime_invoke(m,o,args||ptr(0),ex);if(!ex.readPointer().isNull())throw Error('managed-exception');return result;}
 function boxed(m,o,args,klass,read){const h=hold(invoke(m,o,args));try{const obj=target(h);if(!exact(obj,klass))throw Error('boxed-class-mismatch');const v=api.object_unbox(obj);if(v.isNull())throw Error('boxed-null');return read(v,obj);}finally{api.gchandle_free(h);}}
 function bool(m,o,args){return boxed(m,o,args,classes['System.Boolean'],v=>{const b=v.readU8();if(b>1)throw Error('invalid-bool');return b===1;});}
 function alive(h){const args=Memory.alloc(8);args.writePointer(target(h));return bool(methods.alive.method,ptr(0),args);}
 function speed(h){return boxed(methods.get.method,target(h),null,classes['System.Single'],v=>v.readFloat());}
 function write(h,value){const f=Memory.alloc(4),args=Memory.alloc(8);f.writeFloat(value);args.writePointer(f);invoke(methods.set.method,target(h),args);}
 function field(o,f,type){const out=Memory.alloc(8);out.writePointer(ptr(0));api.field_get_value(o,f,out);return type==='pointer'?out.readPointer():type==='bool'?out.readU8():type==='float'?out.readFloat():out.readS32();}
 function unity(tid){return s.thread!==null&&s.matches>=3&&tid===s.thread&&!api.thread_current().isNull();}
 // One catch label and a bounded set of inventory page labels. Restore before native refresh so pooled panels
 // cannot carry styling or stats from the previously displayed Pokemon.
 function ivText(obj){const str=invoke(p.ivText.getText.method,obj);if(str.isNull())return '';
  const h=hold(str);try{const o=target(h);if(!exact(o,p.iv.string))throw Error('iv-string');
   const n=api.string_length(o);if(n<0||n>512)throw Error('iv-string-length');return n?api.string_chars(o).readUtf16String(n):'';
  }finally{api.gchandle_free(h);}}
 function ivSet(method,obj,n){const v=Memory.alloc(4),args=Memory.alloc(8);v.writeS32(n);args.writePointer(v);invoke(method.method,obj,args);}
 function ivWrite(obj,text){const h=hold(api.string_new(Memory.allocUtf8String(text)));try{const args=Memory.alloc(8);args.writePointer(target(h));invoke(p.iv.setText.method,obj,args);}finally{api.gchandle_free(h);}}
 function outlineVector(method,obj,klass,n){return boxed(method.method,obj,null,klass,v=>Array.from({length:n},(_,i)=>v.add(i*4).readFloat()));}
 function writeOutline(method,obj,values){const v=Memory.alloc(values.length*4),args=Memory.alloc(8);values.forEach((n,i)=>v.add(i*4).writeFloat(n));args.writePointer(v);invoke(method.method,obj,args);}
 function prepareOutline(r,obj){const o=p.ivOutline;if(!o.available||r.kind!=='owned')return;const roots=[];
  const root=v=>{const h=hold(v);roots.push(h);return target(h);};
  try{const go=root(invoke(o.gameObject.method,obj)),type=root(o.typeObject(api.class_get_type(o.outline))),args=Memory.alloc(8);args.writePointer(type);
   let effect=invoke(o.get.method,go,args),created=false;if(effect.isNull()){effect=invoke(o.add.method,go,args);created=true;}
   if(effect.isNull()||!exact(effect,o.outline))throw Error('iv-outline-component');
   r.outline=hold(effect);r.outlineEnabled=created?false:bool(o.enabled.method,effect);
   r.outlineColor=outlineVector(o.colorGet,effect,o.color,4);r.outlineDistance=outlineVector(o.distanceGet,effect,o.vector,2);
   // Newly added components belong to this display and remain disabled when OFF.
   if(created)ivSet(o.setEnabled,effect,0);
  }catch(e){if(r.outline){api.gchandle_free(r.outline);r.outline=null;}event(s,'iv-outline-unavailable',{reason:String(e)});}
  finally{for(const h of roots)api.gchandle_free(h);}}
 function renderOutline(r,enabled){if(!r.outline)return;const o=p.ivOutline,obj=target(r.outline);
  try{writeOutline(o.colorSet,obj,enabled?[0,0,0,0.95]:r.outlineColor);writeOutline(o.distanceSet,obj,enabled?[1.5,-1.5]:r.outlineDistance);ivSet(o.setEnabled,obj,enabled||r.outlineEnabled?1:0);}
  catch(e){event(s,'iv-outline-error',{reason:String(e)});}}
 function ivEnabled(kind){return !s.stopping&&s.now()<s.ivUntil&&(kind==='catch'?s.ivCatch&&!s.ivFault:s.ivOwned&&!s.ivOwnedFault);}
 function renderIv(r,enabled){if(r.applied===enabled)return;const obj=target(r.text);
  if(enabled){r.applied=true;renderOutline(r,true);ivSet(p.iv.rich,obj,1);ivSet(p.iv.fit,obj,0);ivSet(p.iv.setFont,obj,Math.max(12,Math.min(r.fontSize,24)));
   ivSet(p.iv.overflow,obj,p.iv.overflowValue);ivSet(p.iv.alignment,obj,p.iv.alignmentValue);ivWrite(obj,r.label);
  }else{const current=ivText(obj);renderOutline(r,false);ivSet(p.iv.rich,obj,r.rich);ivSet(p.iv.fit,obj,r.fit);ivSet(p.iv.setFont,obj,r.fontSize);
   ivSet(p.iv.overflow,obj,r.overflow);ivSet(p.iv.alignment,obj,r.alignment);
   // Do not overwrite a label the game has independently refreshed.
   if(current===r.label)ivWrite(obj,r.original);r.applied=false;}
 }
 function sameIv(a,b){return a===b||(typeof a.equals==='function'&&a.equals(b));}
 function ivList(kind){return kind==='owned'?s.ivOwnedRecords:(s.ivRecords.catch?[s.ivRecords.catch]:[]);}
 function dropIv(kind,obj){for(const r of [...ivList(kind)]){if(obj&&!sameIv(target(r.text),obj))continue;
  if(kind==='owned')s.ivOwnedRecords.splice(s.ivOwnedRecords.indexOf(r),1);else s.ivRecords.catch=null;
  try{if(alive(r.text))renderIv(r,false);}catch(e){event(s,'iv-restore-error',{context:kind,reason:String(e)});}
  finally{if(r.outline)api.gchandle_free(r.outline);api.gchandle_free(r.text);}}}
 function captureIv(kind,obj,label){dropIv(kind,kind==='owned'?obj:null);
  if(kind==='owned'&&s.ivOwnedRecords.length>=8)dropIv(kind,target(s.ivOwnedRecords[0].text));const r={kind,text:hold(obj),applied:false};
  try{r.original=ivText(obj);r.label=r.original+label.slice(label.indexOf(' \u00b7 '));if(kind==='owned')r.label='<b>'+r.label+'</b>';
   r.fontSize=boxed(p.iv.font.method,obj,null,p.iv.integer,x=>x.readS32());
   r.rich=bool(p.ivText.getRich.method,obj)?1:0;r.fit=bool(p.ivText.getFit.method,obj)?1:0;
   r.overflow=boxed(p.ivText.getOverflow.method,obj,null,p.ivText.hwm,x=>x.readS32());
   r.alignment=boxed(p.ivText.getAlignment.method,obj,null,p.ivText.ta,x=>x.readS32());
   prepareOutline(r,obj);
   if(kind==='owned')s.ivOwnedRecords.push(r);else s.ivRecords.catch=r;renderIv(r,ivEnabled(kind));
  }catch(e){if(ivList(kind).includes(r))dropIv(kind,obj);else api.gchandle_free(r.text);throw e;}}
 function ivTick(){const now=s.stopping?0:s.now();if(!s.stopping&&now<s.ivNext)return;s.ivNext=now+250;
  for(const kind of ['catch','owned'])for(const r of [...ivList(kind)]){
   try{if(s.stopping||!alive(r.text)){dropIv(kind,target(r.text));continue;}renderIv(r,ivEnabled(kind));}
   catch(e){if(kind==='catch')s.ivFault=true;else s.ivOwnedFault=true;dropIv(kind,target(r.text));event(s,'iv-display-error',{context:kind,reason:String(e)});}}
 }
 function release(a){if(a.responseHandle){api.gchandle_free(a.responseHandle);a.responseHandle=null;}if(a.handle){api.gchandle_free(a.handle);a.handle=null;}s.attempt=null;}
 // Outcome identity uses strong, non-pinned GC handles: a reported response
 // stays alive, so a later target comparison cannot match a reused address.
 // Entries are bounded and freed only on the verified Unity thread (expiry,
 // OFF/lease loss, stop or outcome failure). Outcome errors never fault speed.
 function note(kind,fields){if(s.queue.length>=LIMITS.queue-LIMITS.outcomeReserve){s.outcomeDropped++;return;}event(s,kind,Object.assign({dropped:s.outcomeDropped},fields));}
 function outcomeFault(e){if(s.outcomeOff)return;s.outcomeOff=true;note('outcome-unavailable',{reason:String(e).slice(0,140),tid:s.thread});}
 function pruneSeen(){if(!s.seen.length)return;let now=null;try{now=s.now();}catch(_){}const all=s.stopping||s.outcomeOff||now===null||now>=s.allowedUntil;
  for(let i=0;i<s.seen.length;){if(!all&&now<s.seen[i].expires){i++;continue;}try{api.gchandle_free(s.seen[i].handle);s.seen.splice(i,1);}catch(e){i++;outcomeFault(e);}}}
 function outcome(result,status,tid){if(!p.outcomes.available||s.outcomeOff)return;
  try{for(const e of s.seen)if(target(e.handle).equals(result))return; // repeated poll of a reported response
   const now=s.now();if(now-s.outcomeWindow>=1000){s.outcomeWindow=now;s.outcomeHits=0;}
   if(++s.outcomeHits>LIMITS.outcomesPerSecond||s.queue.length>=LIMITS.queue-LIMITS.outcomeReserve){s.outcomeDropped++;return;}
   if(s.seen.length>=LIMITS.outcomeSeen){api.gchandle_free(s.seen[0].handle);s.seen.shift();}
   s.seen.push({handle:hold(result),expires:now+LIMITS.outcomeMs});
   const v=p.outcomes.values,name=LITERALS.find(k=>v[k]===status)||null,confirmed=name&&CONFIRMED[name];
   if(confirmed)note('outcome-confirmed',{outcome:confirmed,status,statusName:name,tid});
   else note('outcome-suppressed',{status,statusName:name,reason:name==='CatchMissed'?'ambiguous-status':name?'not-confirmed':'unknown-status',tid});
  }catch(e){outcomeFault(e);}}
 // Read-only Animator trace: one confirmed success at a time, sampled on the
 // verified Unity tick. It never writes Animator state, parameters or results.
 // Strong GC handles root the Animator and response and are freed only on the
 // Unity thread; stop() waits for them. Trace errors disable only the trace, and
 // its events use a queue quota below the outcome reserve. Logs carry hashes,
 // fields and timing only, never managed object addresses.
 const t=p.trace,r2=x=>Math.round(x*100)/100;
 s.traceOff=!t.available;
 function traceNote(kind,fields,tr){if(s.queue.length>=LIMITS.queue-LIMITS.outcomeReserve-LIMITS.traceReserve){if(tr)tr.dropped++;else s.traceDropped++;return false;}return event(s,kind,fields);}
 function traceEnd(reason){const tr=s.trace;if(!tr)return;
  let now=null;try{now=s.now();}catch(_){}
  if(tr.endReason&&now!==null&&now<tr.releaseRetryAt)return;
  tr.endReason=tr.endReason||reason;
  for(const k of['animator','response']){const h=tr[k];if(!h)continue;try{api.gchandle_free(h);tr[k]=null;}catch(e){
   s.traceOff=true;if(!tr.freeFailed){tr.freeFailed=[];traceNote('trace-unavailable',{reason:'handle-release-failed:'+String(e).slice(0,100),pending:true,tid:tr.tid});}if(!tr.freeFailed.includes(k))tr.freeFailed.push(k);}}
  if(tr.animator||tr.response){tr.releaseRetryAt=now===null?0:now+1000;return;}
  s.trace=null;reason=tr.endReason;
  traceNote('trace-end',{reason,tid:tr.tid,graphApplicable:tr.graph,ticks:tr.ticks,durationMs:now===null?null:r2(now-tr.t0),visited:tr.visited,unknown:tr.unknown,listOverflow:tr.overflow,
   waitSeen:tr.waitSeen,numShakesSeen:tr.numShakes,firstEndMs:tr.firstEndMs,speedResetMs:tr.speedResetMs,btddMs:tr.btddMs,maxTickGapMs:r2(tr.maxGap),endState:tr.endState,
   events:tr.events,capped:tr.capped,dropped:tr.dropped,sessionDropped:s.traceDropped,freeFailed:tr.freeFailed||[]});}
 function traceFault(e){if(s.traceOff)return;s.traceOff=true;traceEnd('fault');traceNote('trace-unavailable',{reason:String(e).slice(0,140),tid:s.thread});}
 // Keep payload in the returned allocation: writePointer does not root a separate Memory.alloc buffer.
 function arg32(value){const args=Memory.alloc(16),n=args.add(8);n.writeS32(value|0);args.writePointer(n);return args;}
 function int32(m,o,args){return boxed(m,o,args,t.classes.int32,v=>v.readS32());}
 function stateInfo(h,m){const L=t.layout;return boxed(m,target(h),arg32(0),t.classes.info,(_,o)=>({h:field(o,L.m_Name,'int')>>>0,full:field(o,L.m_FullPath,'int')>>>0,t:field(o,L.m_NormalizedTime,'float'),len:field(o,L.m_Length,'float')}));}
 function staticInt(f){const out=Memory.alloc(4);out.writeS32(0);api.field_static_get_value(f,out);return out.readS32()>>>0;}
 // UnityEngine.Object.get_name on the rooted controller; the string is rooted before reading.
 function managedName(ch){const o=invoke(t.name.method.method,target(ch),null);if(o.isNull())return null;const sh=hold(o);
  try{const str=target(sh);if(!exact(str,t.name.string))throw Error('name-class');const n=api.string_length(str);if(!Number.isInteger(n)||n<0||n>LIMITS.traceName)throw Error('name-limit');
   return n?api.string_chars(str).readUtf16String(n):'';}finally{api.gchandle_free(sh);}}
 function controller(h,id){const o=invoke(id.methods.controller.method,target(h),null);if(o.isNull())return{controllerKind:'none',controllerClass:null,controllerName:null};
  const ch=hold(o);
  try{const k=api.object_get_class(target(ch)),r={controllerClass:typeName(api,api.class_get_type(k)),controllerKind:k.equals(id.controller)?'base':id.override!==null&&k.equals(id.override)?'override':'other',controllerName:null};
   if(!t.name.available)r.nameReason=t.name.reason;else try{r.controllerName=managedName(ch);}catch(e){r.nameReason=String(e).slice(0,100);}
   return r;}finally{api.gchandle_free(ch);}}
 // One-shot identity on the Unity thread. Any mismatch or unreadable part makes
 // the decoded graph inapplicable; the trace still samples states.
 function identity(tr,bh){const id=t.identity,r={tid:tr.tid,graphApplicable:false,identityStrength:'name-shape-hashes-only',skipAuthorized:false,reasons:[]};
  if(!id.available){r.reasons.push('identity-metadata');r.identityReason=id.reason;return r;}
  try{const ball=target(bh),hashes={};
   for(const x of id.hashes){const v=field(ball,x.field,'int')>>>0;hashes[x.label]=v;if(v!==x.expected)r.reasons.push('ball-hash:'+x.label);}
   r.ballHashes=hashes;r.numShakesHash=staticInt(id.shakes);if(r.numShakesHash!==NUM_SHAKES)r.reasons.push('num-shakes-hash');
   r.layerCount=int32(id.methods.layers.method,target(tr.animator),null);if(r.layerCount!==1)r.reasons.push('layer-count');
   r.parameterCount=int32(id.methods.parameters.method,target(tr.animator),null);if(r.parameterCount!==13)r.reasons.push('parameter-count');
   Object.assign(r,controller(tr.animator,id));
   // An override's base controller is not read in this tier, so it is never graph-applicable.
   if(r.controllerKind!=='base')r.reasons.push(r.controllerKind==='override'?'override-base-unverified':'controller-'+(r.controllerKind==='none'?'none':'class'));
   if(r.controllerName!==CONTROLLER)r.reasons.push(r.controllerName===null?'controller-name-unavailable':'controller-name');
  }catch(e){r.reasons.push('identity-error:'+String(e&&e.message||e).slice(0,80));}
  r.graphApplicable=r.reasons.length===0;return r;}
 function label(x){const name=STATES[x.h]||null,r={h:x.h,full:x.full,name,t:x.t,len:x.len};if(!name)r.unknown=true;if(FLAGGED.has(x.h))r.flagged=true;return r;}
 function remember(tr,x){if(!tr.visited.some(v=>v.h===x.h)){if(tr.visited.length<LIMITS.traceList)tr.visited.push({h:x.h,name:STATES[x.h]||null});else tr.overflow=true;}
  if(!STATES[x.h]&&!tr.unknown.includes(x.h)){if(tr.unknown.length<LIMITS.traceList)tr.unknown.push(x.h);else tr.overflow=true;}}
 function sample(tr,now){
  if(!alive(tr.animator)){traceEnd('animator-destroyed');return;}
  const cur=stateInfo(tr.animator,t.methods.current.method),trans=bool(t.methods.transition.method,target(tr.animator),arg32(0));
  const next=trans?stateInfo(tr.animator,t.methods.next.method):null,sp=speed(tr.animator);
  // Wait/NumShakes are read only when identity proved these parameter hashes exist.
  let wait=null,shakes=null;if(tr.params){wait=bool(t.params.bool.method,target(tr.animator),arg32(WAIT));shakes=int32(t.params.int.method,target(tr.animator),arg32(NUM_SHAKES));}
  if(tr.ticks&&now-tr.last>tr.maxGap)tr.maxGap=now-tr.last;tr.last=now;tr.ticks++;
  remember(tr,cur);if(next)remember(tr,next);
  if(cur.h===WAIT||(next&&next.h===WAIT)||wait===true)tr.waitSeen=true;
  if(shakes!==null&&!tr.numShakes.includes(shakes)&&tr.numShakes.length<8)tr.numShakes.push(shakes);
  if(cur.h===END&&tr.firstEndMs===null)tr.firstEndMs=r2(now-tr.t0);
  if(tr.speed!==null&&Math.abs(tr.speed-1)>1e-5&&Math.abs(sp-1)<=1e-5&&tr.speedResetMs===null)tr.speedResetMs=r2(now-tr.t0);
  tr.speed=sp;tr.endState={h:cur.h,name:STATES[cur.h]||null,trans,next:next&&next.h};
  // normalizedTime is reported but is not part of the change key.
  const key=[cur.h,trans,next&&next.h,wait,shakes,sp].join('|'),prevT=tr.prevT;tr.prevT=cur.t;
  if(key===tr.key)return;tr.key=key;
  if(tr.events>=LIMITS.traceEvents){tr.capped++;return;}
  if(traceNote('trace-state',{tid:tr.tid,tick:tr.ticks,dtMs:r2(now-tr.t0),cur:label(cur),prevT,trans,next:next&&label(next),wait,numShakes:shakes,speed:sp},tr))tr.events++;}
 // Started from the btxu leave after a confirmed success, whether or not speed applied.
 function traceStart(bh,result,tid){if(s.traceOff||s.stopping||!s.active)return;let ah=null,rh=null;
  try{if(s.now()>=s.allowedUntil||!unity(tid))return;
   if(s.trace){traceNote('trace-skip',{reason:'concurrent-trace',tid});return;}
   const ball=target(bh),bread=field(ball,fields.bread,'bool');if(bread){traceNote('trace-skip',{reason:bread>1?'invalid-bread-bool':'special-ball',tid});return;}
   const animator=field(ball,fields.animator,'pointer');if(!exact(animator,classes['UnityEngine.Animator'])){traceNote('trace-skip',{reason:'animator-class',tid});return;}
   ah=hold(animator);if(!alive(ah)){traceNote('trace-skip',{reason:'destroyed-animator',tid});return;}
   rh=hold(result);const now=s.now();
   const tr={animator:ah,response:rh,tid,t0:now,deadline:now+LIMITS.traceMs,last:now,ticks:0,key:null,prevT:null,speed:null,graph:false,params:false,events:0,capped:0,dropped:0,
    visited:[],unknown:[],overflow:false,numShakes:[],waitSeen:false,firstEndMs:null,speedResetMs:null,btddMs:null,maxGap:0,endState:null};
   s.trace=tr;ah=rh=null;
   const id=identity(tr,bh);tr.graph=id.graphApplicable;tr.params=tr.graph&&t.params.available;
   id.paramsRead=tr.params;if(!t.params.available)id.paramsReason=t.params.reason;else if(!tr.params)id.paramsReason='graph-not-applicable';
   traceNote('trace-identity',id,tr);sample(tr,now);
  }catch(e){traceFault(e);}finally{for(const h of[ah,rh])if(h)try{api.gchandle_free(h);}catch(e){traceFault(e);}}}
 function traceTick(){const tr=s.trace;if(!tr)return;
  try{if(tr.endReason){traceEnd(tr.endReason);return;}if(s.stopping){traceEnd('stop');return;}if(s.allowedUntil===0){traceEnd('off');return;}
   const now=s.now();if(now>=s.allowedUntil){traceEnd('lease-expired');return;}if(now>=tr.deadline){traceEnd('timeout');return;}if(tr.ticks>=LIMITS.traceTicks){traceEnd('tick-limit');return;}
   sample(tr,now);
  }catch(e){traceFault(e);}}
 // Final sample at the natural PokemonCaught dispatch, then end. Other threads are ignored.
 function traceBtdd(response,tid){const tr=s.trace;if(!tr||!unity(tid))return;
  try{if(tr.endReason){traceEnd(tr.endReason);return;}if(!response.equals(target(tr.response)))return;
   if(s.stopping){traceEnd('stop');return;}if(s.allowedUntil===0){traceEnd('off');return;}
   const now=s.now();if(now>=s.allowedUntil){traceEnd('lease-expired');return;}if(now>=tr.deadline){traceEnd('timeout');return;}
   tr.btddMs=r2(now-tr.t0);if(tr.ticks<LIMITS.traceTicks)sample(tr,now);if(s.trace===tr)traceEnd('btdd');}catch(e){traceFault(e);}}
 function restore(reason,force){
  const a=s.attempt;if(!a)return;
  let now=null;try{now=s.now();}catch(_){}if(!force){if(now!==null&&now<a.retryAt)return;if(now===null&&a.retryTicks>0){a.retryTicks--;return;}}
  try{
   if(!alive(a.handle)){release(a);event(s,'speed-target-destroyed',{reason,tid:s.thread});return;}
   const current=speed(a.handle);if(!Number.isFinite(current))throw Error('restore-invalid-speed');
   if(Math.abs(current-BALL_SPEED)>1e-5){release(a);event(s,'speed-external-change',{reason,currentSpeed:current,originalSpeed:a.original,tid:s.thread});return;}
   write(a.handle,a.original);const readback=speed(a.handle);if(!Number.isFinite(readback)||Math.abs(readback-a.original)>1e-5)throw Error('restore-readback');
   release(a);event(s,'speed-restored',{reason,originalSpeed:a.original,readback,tid:s.thread});
  }catch(e){a.retryAt=now===null?0:now+1000;a.retryTicks=60;s.allowedUntil=0;s.stopping=true;s.reason=s.reason||'restoration-failed';if(!a.reported){a.reported=true;event(s,'cleanup-failed',{reason:String(e).slice(0,140),pending:true,tid:s.thread});}}
 }
 function cleanupTick(tid){if(s.busy)return;s.busy=true;try{if(!unity(tid))return;pruneSeen();traceTick();
  // Optional IV work must not bypass restoration of an owned animation speed.
  // In particular, a clock failure must still reach the stop/restore path below.
  try{ivTick();}catch(e){fault(s,e);}
  // Read-only shiny scanner on the verified Unity thread. Fully isolated: a scanner
  // fault (or clock failure) must never disable the quick-catch speed path or IV.
  if(s.shiny){let sn=null;try{sn=s.now();}catch(_){}try{s.shiny.tick(sn);}catch(_){}}
  const a=s.attempt;if(!a)return;if(s.stopping||s.allowedUntil===0){restore(s.stopping?'stop':'off',false);return;}
  let now;try{now=s.now();}catch(e){fault(s,e);restore('clock-failed',true);return;}
  if(now>=s.allowedUntil||now>=a.deadline)restore(now>=a.deadline?'timeout':'lease-expired',false);
 }catch(e){fault(s,e);}finally{s.busy=false;}}
 const depths=new Map();
 const enter=new NativeCallback(tid=>{if(!s.active)return 0;const depth=depths.get(tid)||0;depths.set(tid,depth+1);return 1;},'int',['uint32']);
 const leave=new NativeCallback(tid=>{if(!s.active)return;const depth=(depths.get(tid)||1)-1;if(depth){depths.set(tid,depth);return;}depths.delete(tid);
  if(s.thread===null)s.thread=tid;else if(s.thread!==tid){fault(s,Error('unity-thread-changed'));return;}
  s.matches++;cleanupTick(tid);
 },'void',['uint32']);
 if(retainedFilters.length>=16)throw Error('native-filter-session-limit');
 const slot=Memory.alloc(8);slot.writePointer(methods.execute.method);const filter=new CModule(FILTER,{execute_method:slot,enter_match:enter,leave_match:leave});
 retainedFilters.push({filter,slot,enter,leave});const exports=new Set();
 for(const name of ['il2cpp_runtime_invoke','il2cpp_runtime_invoke_convert_args']){const address=p.module.findExportByName(name);if(!address||address.isNull()||exports.has(address.toString()))continue;exports.add(address.toString());s.listeners.push(Interceptor.attach(address,{onEnter:filter.on_enter,onLeave:filter.on_leave}));}
 if(!exports.size)throw Error('invocation-filter-unavailable');
 let windowStart=s.now(),hits=0;
 s.listeners.push(Interceptor.attach(methods.btxu.address,{
  onEnter(args){this.ballHandle=null;try{if(!s.active||s.stopping||s.busy||s.now()>=s.allowedUntil||!this.returnAddress.equals(p.caller)||!unity(this.threadId))return;
   const now=s.now();if(now-windowStart>=1000){windowStart=now;hits=0;}if(++hits>LIMITS.hitsPerSecond)throw Error('ball-rate-limit');
    if(!exact(args[0],classes[BALL]))throw Error('ball-class');this.ballHandle=hold(args[0]);this.ballTid=this.threadId;s.inflight++;
   }catch(e){fault(s,e);}},
  onLeave(result){const bh=this.ballHandle;this.ballHandle=null;if(!bh)return;s.busy=true;let traced=false;
   try{
    if(s.stopping||s.now()>=s.allowedUntil||!unity(this.ballTid)||result.isNull())return;
    if(!exact(result,classes[RESPONSE]))throw Error('response-class');const status=field(result,fields.status,'int');
    outcome(result,status,this.ballTid);if(status!==p.success&&(!p.outcomes.available||status!==p.outcomes.values.CatchEscape))return;
    const ball=target(bh);traced=status===p.success;
    if(s.attempt){event(s,'skip',{reason:'concurrent-attempt',tid:this.ballTid});return;}
    const bread=field(ball,fields.bread,'bool');if(bread>1)throw Error('bread-bool');if(bread){event(s,'skip',{reason:'special-ball',tid:this.ballTid});return;}
    const animator=field(ball,fields.animator,'pointer');if(!exact(animator,classes['UnityEngine.Animator']))throw Error('animator-class');
    const ah=hold(animator);let transferred=false;
    try{if(!alive(ah)){event(s,'skip',{reason:'destroyed-animator',tid:this.ballTid});return;}
     const original=speed(ah);if(!Number.isFinite(original)||Math.abs(original-1)>1e-5){event(s,'skip',{reason:'nonstandard-speed',originalSpeed:original,tid:this.ballTid});return;}
     const now=s.now();if(s.stopping||now>=s.allowedUntil)return;
     // Ownership is recorded BEFORE the setter: even a throwing setter might
     // have changed native state. Cleanup must inspect and restore it.
     const responseHandle=hold(result);s.attempt={handle:ah,responseHandle,original,deadline:now+LIMITS.timeoutMs,retryAt:0,retryTicks:0,reported:false};transferred=true;
     write(ah,BALL_SPEED);const readback=speed(ah);if(!Number.isFinite(readback)||Math.abs(readback-BALL_SPEED)>1e-5)throw Error('apply-readback');
     event(s,'speed-applied',{tid:this.ballTid,resultStatus:status,originalSpeed:original,requestedSpeed:BALL_SPEED,readback,timeoutMs:LIMITS.timeoutMs});
    }finally{if(!transferred)api.gchandle_free(ah);}
   }catch(e){fault(s,e);}finally{if(traced)traceStart(bh,result,this.ballTid);try{api.gchandle_free(bh);}catch(e){fault(s,e);}s.inflight--;s.busy=false;}
  }
 }));
 for(const [completion,reason] of [[methods.caught,'normal-caught'],[methods.escaped,'normal-broke-free']])s.listeners.push(Interceptor.attach(completion.address,{onEnter(args){if(!s.active||s.busy)return;
  // The trace samples first (before the speed restore) and never faults or ends on another thread.
  if(completion===methods.caught&&s.trace){s.busy=true;try{traceBtdd(args[1],this.threadId);}finally{s.busy=false;}}
  if(!s.attempt)return;
  s.busy=true;try{if(!unity(this.threadId))throw Error('completion-thread-mismatch');
  if(!args[1].equals(target(s.attempt.responseHandle)))return;restore(reason,true);}catch(e){fault(s,e);}finally{s.busy=false;}
 }}));
 if(p.iv.available)s.listeners.push(Interceptor.attach(p.iv.setup.address,{onEnter(args){
  this.ivHandles=null;if(!s.active||s.stopping||s.busy||s.ivFault)return;
  const owned=[];s.busy=true;try{
   if(!unity(this.threadId)||args[0].isNull()||args[1].isNull()||!exact(args[0],p.iv.plate)||!api.class_is_assignable_from(p.iv.mapPokemon,api.object_get_class(args[1])))return;
   dropIv('catch');event(s,'cooldown-encounter',{source:typeName(api,api.class_get_type(api.object_get_class(args[1])))});owned.push(hold(args[0]));owned.push(hold(args[1]));this.ivHandles=owned;s.inflight++;
  }catch(e){for(const h of owned)api.gchandle_free(h);event(s,'iv-unavailable',{reason:String(e).slice(0,140)});s.ivFault=true;}finally{s.busy=false;}
 },onLeave(){const owned=this.ivHandles;if(!owned)return;this.ivHandles=null;s.busy=true;
  try{
   if(s.stopping||s.ivFault||!unity(this.threadId))return;
   // budn() scans the player's bag for highest CP; it is NOT the encounter.
   // WildMapPokemon.get_Pokemon can return a placeholder behind a feature gate.
   // Read the response attached to this exact map object instead; never cache it.
   const map=target(owned[1]);if(!exact(map,p.iv.wildMap))return;
   function rootedField(o,f,k){const x=field(o,f,'pointer');if(x.isNull())return null;const h=hold(x);owned.push(h);if(!exact(target(h),k))throw Error('iv-response-class');return h;}
   let pokemon=ptr(0);
   const rh=rootedField(map,p.iv.mapResponse,p.iv.encounterOut);
   if(rh){
    const wh=rootedField(target(rh),p.iv.responseWild,p.iv.wildProto),mh=rootedField(map,p.iv.mapWild,p.iv.wildProto);
    if(!wh||!mh)return;
    function encounterId(h){const out=Memory.alloc(8);api.field_get_value(target(h),p.iv.wildId,out);return out.readU64();}
    if(encounterId(wh).compare(encounterId(mh))!==0){event(s,'iv-pending',{reason:'encounter-id-mismatch'});return;}
    pokemon=field(target(wh),p.iv.wildPokemon,'pointer');
    const pos=Memory.alloc(8);api.field_get_value(target(wh),p.iv.wildLatitude,pos);const latitude=pos.readDouble();api.field_get_value(target(wh),p.iv.wildLongitude,pos);const longitude=pos.readDouble();event(s,'encounter-position',{latitude,longitude,encounterId:encounterId(wh).toString()});
   }else{
    const th=rootedField(map,p.iv.mapTappable,p.iv.tappable);if(th)pokemon=field(target(th),p.iv.tappablePokemon,'pointer');
   }
   if(pokemon.isNull())return;
   const ph=hold(pokemon);owned.push(ph);if(!exact(target(ph),p.iv.pokemon))throw Error('iv-pokemon-class');
   const v={};for(const [name,f]of Object.entries(p.iv.values))v[name]=field(target(ph),f,name.includes('Multiplier')?'float':'int');
   if(!Number.isInteger(v.cp_)||v.cp_<=0||!Number.isFinite(v.cpMultiplier_)||v.cpMultiplier_<=0){event(s,'iv-pending',{reason:'encounter-stats-not-populated'});return;}
   if(!p.ivText.available)return;
   const th=hold(field(target(owned[0]),p.iv.textField,'pointer'));owned.push(th);
   if(!api.class_is_assignable_from(p.iv.textClass,api.object_get_class(target(th))))throw Error('iv-text-class');
   let level=null;const multiplier=Math.fround(v.cpMultiplier_+v.additionalCpMultiplier_);
   if(Number.isFinite(multiplier)&&multiplier>0){
    const gh=hold(field(target(owned[0]),p.iv.gmField,'pointer'));owned.push(gh);
    const value=Memory.alloc(4),args=Memory.alloc(16);value.writeFloat(multiplier);args.writePointer(target(gh));args.add(8).writePointer(value);
    const lower=boxed(p.iv.getLevel.method,ptr(0),args,p.iv.integer,x=>x.readS32());
    if(lower>=1&&lower<=100){
     value.writeS32(lower);args.writePointer(target(gh));const lo=boxed(p.iv.getMultiplier.method,ptr(0),args,classes['System.Single'],x=>x.readFloat());
     if(Math.abs(lo-multiplier)<0.00002)level=lower;
     else{value.writeS32(lower+1);args.writePointer(target(gh));const hi=boxed(p.iv.getMultiplier.method,ptr(0),args,classes['System.Single'],x=>x.readFloat());if(Math.abs(Math.sqrt((lo*lo+hi*hi)/2)-multiplier)<0.00002)level=lower+0.5;}
    }
   }
   const label=encounterLabel(level,v.individualAttack_,v.individualDefense_,v.individualStamina_,v.cp_);
   captureIv('catch',target(th),label);
   event(s,'iv-label',{text:label,level,attack:v.individualAttack_,defense:v.individualDefense_,stamina:v.individualStamina_,cp:v.cp_,tid:this.threadId});
  }catch(e){s.ivFault=true;event(s,'iv-unavailable',{reason:String(e).slice(0,140)});}
  finally{try{for(let i=owned.length-1;i>=0;i--)api.gchandle_free(owned[i]);}finally{s.inflight--;s.busy=false;}}
 }}));
 // Owned details use the panel's current Pokemon; encounter data never enters here.
 if(p.ivOwned.available){
  const o=p.ivOwned;
  s.listeners.push(Interceptor.attach(o.update.address,{onEnter(args){
   this.ownedProto=null;this.ownedPanel=null;if(!s.active||s.stopping||s.busy||s.ivOwnedFault)return;
   s.busy=true;try{
    if(!unity(this.threadId)||args[0].isNull()||!exact(args[0],o.panel))return;
    const proto=args[1];if(proto.isNull()||!exact(proto,p.iv.pokemon))return;
    const label=field(args[0],o.cpText,'pointer');if(!label.isNull())dropIv('owned',label);this.ownedPanel=hold(args[0]);
    try{this.ownedProto=hold(proto);}catch(e){api.gchandle_free(this.ownedPanel);this.ownedPanel=null;throw e;}s.inflight++;
   }catch(e){s.ivOwnedFault=true;event(s,'iv-owned-unavailable',{reason:String(e).slice(0,140)});}finally{s.busy=false;}
  },onLeave(){const ph=this.ownedProto;if(!ph)return;this.ownedProto=null;
   const panelH=this.ownedPanel;const owned=[ph];if(panelH)owned.push(panelH);s.busy=true;
   try{
    if(s.stopping||s.ivOwnedFault||!unity(this.threadId))return;
    const proto=target(ph);if(!exact(proto,p.iv.pokemon)||!field(target(panelH),o.displayed,'pointer').equals(proto))throw Error('owned-pokemon-identity');
    const v={};for(const[name,f]of Object.entries(p.iv.values))v[name]=field(proto,f,name.includes('Multiplier')?'float':'int');
    // Fail closed on unpopulated/invalid stats: leave the game's own label intact.
    if(!Number.isInteger(v.cp_)||v.cp_<=0||!Number.isFinite(v.cpMultiplier_)||v.cpMultiplier_<=0){event(s,'iv-owned-pending',{reason:'stats-not-populated'});return;}
    const th=hold(field(target(panelH),o.cpText,'pointer'));owned.push(th);const textObj=target(th);
    if(!api.class_is_assignable_from(p.iv.textClass,api.object_get_class(textObj)))throw Error('owned-text-class');
    let level=null;const multiplier=Math.fround(v.cpMultiplier_+v.additionalCpMultiplier_);
    if(Number.isFinite(multiplier)&&multiplier>0){
     const gh=hold(field(target(panelH),o.gmField,'pointer'));owned.push(gh);
     if(!target(gh).isNull()){
      const value=Memory.alloc(4),args=Memory.alloc(16);value.writeFloat(multiplier);args.writePointer(target(gh));args.add(8).writePointer(value);
      const lower=boxed(p.iv.getLevel.method,ptr(0),args,p.iv.integer,x=>x.readS32());
      if(lower>=1&&lower<=100){
       value.writeS32(lower);args.writePointer(target(gh));const lo=boxed(p.iv.getMultiplier.method,ptr(0),args,classes['System.Single'],x=>x.readFloat());
       if(Math.abs(lo-multiplier)<0.00002)level=lower;
       else{value.writeS32(lower+1);args.writePointer(target(gh));const hi=boxed(p.iv.getMultiplier.method,ptr(0),args,classes['System.Single'],x=>x.readFloat());if(Math.abs(Math.sqrt((lo*lo+hi*hi)/2)-multiplier)<0.00002)level=lower+0.5;}
      }
     }
    }
    const label=encounterLabel(level,v.individualAttack_,v.individualDefense_,v.individualStamina_,v.cp_);
    captureIv('owned',textObj,label);
    event(s,'iv-owned-label',{text:label,level,attack:v.individualAttack_,defense:v.individualDefense_,stamina:v.individualStamina_,cp:v.cp_,tid:this.threadId});
   }catch(e){s.ivOwnedFault=true;event(s,'iv-owned-unavailable',{reason:String(e).slice(0,140)});}
   finally{try{for(let i=owned.length-1;i>=0;i--)api.gchandle_free(owned[i]);}finally{s.inflight--;s.busy=false;}}
  }}));
 }
 if(p.mapTap.available){
  const scopes=new Map();
  s.listeners.push(Interceptor.attach(p.mapTap.tap.address,{onEnter(args){
   this.mapScope=false;
   if(!s.active||s.stopping||s.busy||s.mapFault||!s.mapMode)return;
   try{if(s.now()>=s.mapUntil||!unity(this.threadId)||!exact(args[0],p.mapTap.ground))return;
    scopes.set(this.threadId,(scopes.get(this.threadId)||0)+1);this.mapScope=true;
   }catch(_){}
  },onLeave(){if(this.mapScope){const n=scopes.get(this.threadId)-1;if(n)scopes.set(this.threadId,n);else scopes.delete(this.threadId);}}}));
  s.listeners.push(Interceptor.attach(p.mapTap.position.address,{onEnter(args){
   this.mapHandle=null;
   if(!scopes.has(this.threadId)||s.busy||s.mapFault||!s.mapMode||s.stopping)return;
   try{if(!unity(this.threadId)||s.now()>=s.mapUntil||!exact(args[0],p.mapTap.map)||args[1].isNull())return;
    this.mapHandle=hold(args[0]);this.mapOut=args[1];this.mapEpoch=s.mapEpoch;this.mapMode=s.mapMode;s.inflight++;
   }catch(_){}
  },onLeave(result){const h=this.mapHandle;if(!h)return;this.mapHandle=null;
   s.busy=true;try{
    if(s.stopping||s.mapFault||s.mapEpoch!==this.mapEpoch||s.mapMode!==this.mapMode||s.now()>=s.mapUntil||!unity(this.threadId)||result.toInt32()!==1)return;
    const vec=Memory.alloc(12),args=Memory.alloc(8);for(let i=0;i<3;i++){const v=this.mapOut.add(i*4).readFloat();if(!Number.isFinite(v))throw Error('map-position-invalid');vec.add(i*4).writeFloat(v);}args.writePointer(vec);
    const point=boxed(p.mapTap.location.method,target(h),args,p.mapTap.latlng,v=>({latitude:v.readDouble(),longitude:v.add(8).readDouble()}));
    if(!Number.isFinite(point.latitude)||!Number.isFinite(point.longitude)||Math.abs(point.latitude)>90||Math.abs(point.longitude)>180)throw Error('map-location-invalid');
    event(s,'map-tap',Object.assign({mode:this.mapMode,epoch:this.mapEpoch},point));
   }catch(e){s.mapFault=true;s.mapUntil=0;event(s,'map-tap-unavailable',{reason:String(e).slice(0,140)});}
   finally{try{api.gchandle_free(h);}finally{s.inflight--;s.busy=false;}}
  }}));
 }
 // AR+ data is separate from the packed grade/curve bits. Only normal wild
 // capture requests are eligible; never change ball type, hit/miss or results.
 if(p.arThrows.available)s.listeners.push(Interceptor.attach(p.arThrows.capture.address,{
  onEnter(args){this.arHandle=null;if(!s.active||s.stopping||s.busy||s.arFault||!s.throwAr||s.now()>=s.arUntil||!unity(this.threadId))return;
   s.busy=true;try{
    if(args[0].isNull()||!api.class_is_assignable_from(p.arThrows.wild,api.object_get_class(args[0])))return;
    const ball=args[1].and(ptr('0xffffffff')).toUInt32(),flags=args[2].toUInt32();
    if(ball<1||ball>3||((flags>>>16)&255)!==0)return;
    const values=api.object_new(p.arThrows.values);if(values.isNull())throw Error('ar-values-allocation');
    this.arHandle=hold(values);s.inflight++;
    const zero=Memory.alloc(4);zero.writeFloat(0);
    api.field_set_value(values,p.arThrows.proximity,zero);api.field_set_value(values,p.arThrows.awareness,zero);api.field_set_value(values,p.arThrows.frightened,zero);
    args[3]=values;event(s,'ar-throw-applied',{proximity:0,awareness:0,frightened:false,tid:this.threadId});
   }catch(e){s.arFault=true;event(s,'ar-throw-unavailable',{reason:String(e)});}finally{s.busy=false;}
  },onLeave(){if(this.arHandle){api.gchandle_free(this.arHandle);this.arHandle=null;s.inflight--;}}
 }));
 if(p.throws.available)for(const [method,route] of [[p.throws.submit,'submit'],[p.throws.hit,'hit']])s.listeners.push(Interceptor.attach(method.address,{onEnter(args){
  if(!s.active||s.stopping||s.busy||s.throwFault||(!s.throwGrade&&!s.throwCurve))return;
  s.busy=true;try{
   const now=s.now();if(now>=s.throwUntil||!unity(this.threadId)||args[0].isNull())return;
   if(route==='submit'){if(!exact(args[0],classes[BALL])||field(args[0],fields.bread,'bool')!==0)return;}
   else if(!api.class_is_assignable_from(p.throws.owner,api.object_get_class(args[0])))return;
   const changed=throwForRoute(s,args[1].toString(),args[2].toInt32(),route,now);if(!changed)return;
   args[1]=ptr(changed.first);args[2]=ptr(changed.second);
   event(s,'throw-applied',{route,grade:['Off','Nice','Great','Excellent'][s.throwGrade],curve:s.throwCurve,reticle:changed.reticle,tid:this.threadId});
  }catch(e){s.throwFault=true;s.throwUntil=0;event(s,'throw-unavailable',{reason:String(e).slice(0,140)});}finally{s.busy=false;}
 }}));
 const part=x=>x.available?{available:true}:{available:false,reason:x.reason};
 s.ready=true;event(s,'metadata-ready',{fields:p.fieldMeta,iv:part(p.iv),ivOwned:part(p.ivOwned),ivOutline:part(p.ivOutline),mapTap:part(p.mapTap),throws:part(p.throws),arThrows:part(p.arThrows),successValue:p.success,multiplier:BALL_SPEED,leaseMs:LIMITS.leaseMs,timeoutMs:LIMITS.timeoutMs,
  outcomes:p.outcomes.available?{available:true,values:p.outcomes.values}:{available:false,reason:p.outcomes.reason},
  trace:t.available?{available:true,stateInfo:{valueSize:36,align:4,offsets:INFO_FIELDS.map((_,i)=>0x10+i*4)},identity:part(t.identity),name:part(t.name),params:part(t.params),
   limits:{ms:LIMITS.traceMs,ticks:LIMITS.traceTicks,events:LIMITS.traceEvents}}:{available:false,reason:t.reason}});
 // Extracted shiny scanner (bundled before this file). Absent when quick-ball runs
 // standalone; its own faults never reach the speed/IV paths.
 const F=root.PGRazorFeatures;
 if(F&&typeof F.shinyScanner==='function'){try{s.shiny=F.shinyScanner({s,p,event,Interceptor,unity,exact,hold,target,invoke,api,Memory,ptr,alive,bool,field,LIMITS});s.shiny.install();}catch(e){s.shiny=null;event(s,'shiny-ready',{available:false,reason:String(e).slice(0,140)});}}
}
function start(options){
 if(!options||options.enabled!==true)return Promise.resolve({status:'disabled'});if(running)return Promise.resolve({status:'already-running'});
 let resolve;const promise=new Promise(r=>resolve=r);const s={promise,resolve,onEvent:options.onEvent,active:true,ready:false,stopping:false,reason:null,listeners:[],queue:[],attempt:null,seen:[],trace:null,traceOff:false,traceDropped:0,outcomeOff:false,outcomeHits:0,outcomeWindow:-Infinity,outcomeDropped:0,ivFault:false,ivCatch:true,ivOwned:false,ivOwnedFault:false,ivRecords:{catch:null},ivOwnedRecords:[],ivUntil:0,ivNext:0,mapMode:0,mapEpoch:0,mapUntil:0,mapFault:false,throwAr:false,arUntil:0,arFault:false,throwGrade:0,throwCurve:false,throwSample:null,throwUntil:0,throwFault:false,allowedUntil:0,thread:null,matches:0,busy:false,inflight:0,retries:0,timer:null,now:()=>0};running=s;
 function finish(){for(const h of s.listeners)try{h.detach();}catch(_){}s.listeners=[];s.active=false;const result={status:s.reason?'unavailable':'cancelled'};if(s.reason)result.reason=s.reason;event(s,'finished',result);flush(s);if(running===s)running=null;s.resolve(result);}
 function poll(){s.timer=null;if(!s.active)return;
  try{
   if(!s.ready&&!s.stopping){if(Process.arch!=='arm64'||Process.pointerSize!==8)throw Error('unsupported-architecture');
    if(!s.clockReady){s.now=clock();s.clockReady=true;}
    const module=Process.findModuleByName('libil2cpp.so');if(module){const api=bind(module),domain=api.domain_get();if(!domain.isNull()){const p=prepare(module,api,domain);if(p)install(s,p);}}
    if(!s.ready)s.retries++; /* Wait for game metadata during slow cold starts. */
   }
   if(s.stopping&&s.attempt&&!s.cleanupReported){s.cleanupReported=true;event(s,'cleanup-pending',{reason:s.reason||'stop',pending:true});}
   // Outcome identity and trace handles, like an attempt, wait for a Unity-thread release.
   flush(s);if(s.stopping&&!s.attempt&&!s.trace&&!s.seen.length&&!s.ivRecords.catch&&!s.ivOwnedRecords.length&&!(s.shiny&&s.shiny.hasRoots())&&!s.busy&&s.inflight===0){finish();return;}
  }catch(e){fault(s,e);}
  s.timer=setTimeout(poll,s.ready?LIMITS.pollMs:1000);
 }
 s.timer=setTimeout(poll,0);return promise;
}
function setAllowed(value){const s=running;if(!s||!s.active||!s.ready||s.stopping)return false;
 try{s.allowedUntil=value===true?s.now()+LIMITS.leaseMs:0;return value===true;}catch(e){fault(s,e);return false;}}
function setMapTapOptions(mode,epoch,allowed){const s=running;if(!s||!s.active||!s.ready||s.stopping||s.mapFault)return false;
 if(!Number.isInteger(mode)||mode<0||mode>3||!Number.isSafeInteger(epoch)){s.mapUntil=0;return false;}
 try{s.mapMode=mode;s.mapEpoch=epoch;s.mapUntil=allowed===true?s.now()+LIMITS.leaseMs:0;return allowed===true;}catch(_){s.mapUntil=0;return false;}}
function setArThrowOptions(enabled,allowed){const s=running;if(!s||!s.active||!s.ready||s.stopping||s.arFault)return false;s.throwAr=enabled===true;s.arUntil=allowed===true?s.now()+LIMITS.leaseMs:0;return true;}
function setThrowOptions(grade,curve,allowed){const s=running;if(!s||!s.active||!s.ready||s.stopping||s.throwFault)return false;
 if(!Number.isInteger(grade)||grade<0||grade>3){s.throwSample=null;s.throwUntil=0;return false;}
 try{if(s.throwGrade!==grade||s.throwCurve!==(curve===true)||allowed!==true)s.throwSample=null;s.throwGrade=grade;s.throwCurve=curve===true;s.throwUntil=allowed===true?s.now()+LIMITS.leaseMs:0;return allowed===true;}catch(e){s.throwSample=null;s.throwUntil=0;return false;}}
// Preferences are independent of Quick Catch; only a foreground lease permits writes.
function setIvDisplayOptions(catchEnabled,ownedEnabled,allowed){const s=running;if(!s||!s.active||!s.ready||s.stopping)return false;
 if(s.ivCatch!==(catchEnabled===true)||s.ivOwned!==(ownedEnabled===true)||allowed!==true)s.ivNext=0;
 s.ivCatch=catchEnabled===true;s.ivOwned=ownedEnabled===true;s.ivUntil=allowed===true?s.now()+LIMITS.leaseMs:0;return true;}
// Shiny scanner lease; the coordinator folds the map-visible AND VGP-off guard
// into `allowed`. Delegates to the extracted module and never faults quick catch.
function setShinyScannerOptions(enabled,allowed,latitude,longitude){const s=running;if(!s||!s.active||!s.ready||s.stopping||!s.shiny)return false;
 try{return s.shiny.setOptions(enabled,allowed,latitude,longitude);}catch(_){return false;}}
function stop(){const s=running;if(!s)return Promise.resolve({status:'idle'});s.allowedUntil=0;s.stopping=true;return s.promise;}
const api=Object.freeze({start,setAllowed,setThrowOptions,setArThrowOptions,setMapTapOptions,setIvDisplayOptions,setShinyScannerOptions,stop});root.PGRazorQuickBall=api;return api;
})(globalThis);
