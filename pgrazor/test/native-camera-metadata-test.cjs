// VM boundary and camera-probe integration test for Feature 59 (API 1): the real compiled resolver
// is covered by camera_metadata_test.cpp. This test drives native-camera-metadata.js against a fake
// host, verifies it marshals the 31 feature47 exports + PgrCameraMetaExtra + module range + thread
// accessor + domain/mode flags into the 112-byte PgrCameraMetaConfig and 112-byte PgrCameraMetaExtra,
// maps the 368-byte output into the exact discover() return shapes across passive, sampleCamera, and
// continuous modes, and proves camera-probe.js native selection has no fallback on native failure.
const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const code=fs.readFileSync(__dirname+'/../runtime/native-camera-metadata.js','utf8');

const CLASS_COUNT=11,FIELD_COUNT=3,METHOD_COUNT=8,REASON_MAX=120;
const CONFIG_BYTES=112,EXTRA_BYTES=112,OUTPUT_BYTES=368;
const NATIVE_OPTIONAL_BITS={
 class_get_field_from_name:0,
 field_get_type:1,
 field_get_parent:2,
 field_get_flags:3,
 runtime_invoke:4,
 object_get_class:5,
 object_unbox:6,
 gchandle_new:7,
 gchandle_get_target:8,
 gchandle_free:9,
 field_get_value:10,
 get_corlib:11,
 class_is_assignable_from:12,
 class_is_valuetype:13,
 class_value_size:14,
 debug_get_method_info:15,
 method_is_generic:16,
 method_is_inflated:17,
 method_get_param:18,
 string_new:19
};

function fixture(options={}){
 const sampleCamera=options.sampleCamera!==undefined?Boolean(options.sampleCamera):true;
 const continuous=options.continuous!==undefined?Boolean(options.continuous):true;
 const vfCode=options.vfCode!==undefined?options.vfCode:4;
 let next=4096;const memory=Buffer.alloc(1<<20),functions=new Map(),calls=[];
 class P{constructor(n){this.n=Number(n);}add(n){return new P(this.n+n);}isNull(){return this.n===0;}
  and(mask){return new P(this.n&mask);}compare(o){return this.n<o.n?-1:this.n>o.n?1:0;}equals(o){return Boolean(o&&this.n===o.n);}
  readU8(){return memory.readUInt8(this.n);}writeU8(v){memory.writeUInt8(v,this.n);}
  readU32(){return memory.readUInt32LE(this.n);}writeU32(v){memory.writeUInt32LE(v>>>0,this.n);}
  readU64(){return memory.readBigUInt64LE(this.n);}writeU64(v){memory.writeBigUInt64LE(BigInt(v),this.n);}
  readPointer(){return new P(Number(memory.readBigUInt64LE(this.n)));}writePointer(v){memory.writeBigUInt64LE(BigInt(v.n),this.n);}}
 const ptr=n=>new P(n),alloc=n=>{const p=ptr(next);next+=Math.max(8,n);return p;};
 const allocUtf8String=s=>{const b=Buffer.from(s+'\0','utf8');const p=alloc(b.length);b.copy(memory,p.n);return p;};
 const table=alloc(24);table.writeU32(1);table.add(4).writeU32(24);
 table.add(8).writeU32(options.badLayout?96:CONFIG_BYTES);table.add(12).writeU32(OUTPUT_BYTES);
 const fnPtr=ptr(200);table.add(16).writePointer(fnPtr);

 functions.set(200,(c,o)=>{calls.push('resolve');
  assert.equal(c.readU32(),1);assert.equal(c.add(4).readU32(),CONFIG_BYTES);
  assert.equal(c.add(8).readU32(),1);assert.equal(c.add(12).readU32(),64);
  assert.equal(c.add(24).readPointer().n,5000);
  assert.equal(c.add(32).readU64(),1000000000n);
  assert.equal(c.add(40).readPointer().isNull(),true);
  assert.equal(c.add(48).readPointer().isNull(),true);
  assert.equal(c.add(56).readPointer().isNull(),true);
  assert.equal(c.add(64).readU32(),0);assert.equal(c.add(68).readU32(),0);
  assert.equal(c.add(72).readPointer().n,700);
  assert.equal(c.add(80).readU64(),512n);
  assert.equal(c.add(88).readPointer().n,900);
  assert.equal(c.add(104).readU32(),sampleCamera?1:0);
  assert.equal(c.add(108).readU32(),continuous?1:0);

  const ex=c.add(96).readPointer();
  assert.equal(ex.isNull(),false);
  assert.equal(ex.readU32(),1);assert.equal(ex.add(4).readU32(),EXTRA_BYTES);
  assert.equal(ex.add(8).readPointer().n,700); // image_get_name
  assert.equal(ex.add(88).readPointer().isNull(),true); // clock_ms
  assert.equal(ex.add(96).readPointer().isNull(),true); // clock_ctx
  const missingBit=options.missingOptional?NATIVE_OPTIONAL_BITS[options.missingOptional.replace('il2cpp_','')]:undefined;
  const expectedMask=missingBit===undefined?0xfffff:(0xfffff&~(1<<missingBit));
  assert.equal(ex.add(104).readU32(),expectedMask>>>0);
  assert.equal(ex.add(108).readU32(),0);

  const status=options.status!==undefined?options.status:0;
  if(status<0)return status;

  o.writeU32(options.badOutput?2:1);
  o.add(4).readU32();
  o.add(4).writeU32(OUTPUT_BYTES);
  o.add(8).writeU32(status>>>0);
  const reasonText=options.reason!==undefined?options.reason:(status===1?'core-module-not-found-within-bound':status===2?'map-debug-info-abi-unverified':'');
  for(let i=0;i<reasonText.length;i++)o.add(248+i).writeU8(reasonText.charCodeAt(i));
  o.add(248+reasonText.length).writeU8(0);
  if(status===1||status===2)return status;

  const targetCount=options.badTargetCount?3:(sampleCamera?5:4);
  o.add(12).writeU32(vfCode>>>0);
  o.add(16).writeU32(targetCount>>>0);
  o.add(20).writeU32(0);
  o.add(24).writePointer(ptr(800)); // core_image
  o.add(32).writePointer(ptr(continuous?801:0)); // game_image
  o.add(40).writePointer(ptr(sampleCamera?802:0)); // corlib_image
  o.add(48).writePointer(ptr(options.nullExecute?0:4003)); // execute
  o.add(56).writePointer(ptr(continuous?(options.nullDebug?0:700):0)); // debug_address
  o.add(64).writePointer(ptr(continuous?(options.nullMainCamCode?0:5000+0xae09554):0)); // get_main_camera_address
  for(let i=0;i<CLASS_COUNT;i++){
   const isNull=(options.nullClass===i)||(!sampleCamera&&i===4)||(!sampleCamera&&i>=6)||(sampleCamera&&!continuous&&i>=7)||(vfCode!==4&&i===5);
   o.add(72+i*8).writePointer(ptr(isNull?0:1000+i));
  }
  for(let i=0;i<FIELD_COUNT;i++){
   const isNull=(options.nullField===i)||(vfCode!==4);
   o.add(160+i*8).writePointer(ptr(isNull?0:2000+i));
  }
  for(let i=0;i<METHOD_COUNT;i++){
   const isNull=(options.nullMethod===i)||(!sampleCamera&&i===4)||(!continuous&&i>=5);
   o.add(184+i*8).writePointer(ptr(isNull?0:4000+i));
  }
  return status;
 });

 const context={
  PGRazorNativeCorePath:options.absent?'':'libpgrazor.so',
  Uint8Array,
  PGRazorNativeFeature:options.noLoader?undefined:function(id,version,size){
   assert.deepEqual([id,version,size],[59,1,24]);
   return options.noTable?null:table;
  }
 };
 vm.createContext(context);
 vm.runInContext(code,context);
 const fakeApi={marker:'il2cpp-api'};
 const host={
  Memory:{alloc,allocUtf8String},
  ptr,
  api:fakeApi,
  domain:ptr(900),
  sampleCamera,
  continuous,
  Process:{getCurrentThreadId:()=>512},
  NativeFunction:function(p,ret,args){
   if(p.n===200)return functions.get(200);
   return {boundPtr:p.n,ret,args};
  },
  module:{
   base:ptr(5000),
   size:1000000000,
   findExportByName(name){
    calls.push(name);
    if(options.missingExport&&name==='il2cpp_domain_get')return null;
    if(options.missingImageName&&name==='il2cpp_image_get_name')return null;
    if(options.missingThreadCurrent&&name==='il2cpp_thread_current')return null;
    if(options.missingOptional&&name===options.missingOptional)return null;
    return ptr(700);
   }
  },
  executable(mod,p,size){
   assert.equal(p.n,700);assert.equal(size,4);
   if(options.notExecutable)throw Error('code-not-rx');
  }
 };
 return {calls,fakeApi,run:()=>context.PGRazorNativeCameraMetadata.discover({host})};
}

// 1) Continuous mode (sampleCamera=true, continuous=true) full success shape.
let fx=fixture({sampleCamera:true,continuous:true}),res=fx.run();
assert.equal(res.execute.n,4003);
assert.equal(res.vectorFields,'verified');
assert.equal(res.events.length,5);
assert.deepEqual({...res.events[0]},{className:'UnityEngine.Camera',name:'get_main',parameterCount:0,returnType:'UnityEngine.Camera',isStatic:true});
assert.deepEqual({...res.events[3]},{className:'UnityEngine.UnitySynchronizationContext',name:'ExecuteTasks',parameterCount:0,returnType:'System.Void',isStatic:true});
assert.deepEqual({...res.events[4]},{className:'UnityEngine.Object',name:'GetInstanceID',parameterCount:0,returnType:'System.Int32',isStatic:false});
assert.ok(res.sample);
assert.equal(res.sample.api,fx.fakeApi);
assert.equal(res.sample.vector.n,1005);
assert.equal(res.sample.integer.n,1006);
assert.equal(res.sample.rig.n,1007);
assert.equal(res.sample.string.n,1009);
assert.equal(res.sample.debugAddress.n,700);
assert.equal(res.sample.getMainCameraAddress.n,5000+0xae09554);
assert.equal(res.sample.fields.x.n,2000);
assert.equal(res.sample.fields.y.n,2001);
assert.equal(res.sample.fields.z.n,2002);
assert.equal(res.sample.methods.get_main.n,4000);
assert.equal(res.sample.methods.GetInstanceID.n,4004);
assert.equal(res.sample.methods.get_MainCamera.n,4005);
assert.equal(res.sample.methods.get_cullingMask.n,4006);
assert.equal(res.sample.methods.NameToLayer.n,4007);
assert.equal(res.sample.classes.Camera.n,1000);
assert.equal(res.sample.classes.Object.n,1004);
assert.equal(res.sample.exception.isNull(),false);
assert.equal(res.sample.float.isNull(),false);
assert.equal(res.sample.worldMapName.isNull(),false);
assert.equal(fx.fakeApi.runtime_invoke.boundPtr,700);
assert.equal(fx.fakeApi.debug_get_method_info.boundPtr,700);
assert.equal(fx.fakeApi.string_new.boundPtr,700);
// 31 feature47 exports + 1 image_get_name + 9 optional extra + 1 thread_current = 42 exports, then 1 resolve.
assert.equal(fx.calls.filter(x=>x.startsWith('il2cpp_')).length,42);
assert.equal(fx.calls.filter(x=>x==='resolve').length,1);

// 2) Bounded sampleCamera mode (sampleCamera=true, continuous=false).
fx=fixture({sampleCamera:true,continuous:false});res=fx.run();
assert.equal(res.events.length,5);
assert.ok(res.sample);
assert.equal(res.sample.vector.n,1005);
assert.equal(res.sample.integer.n,1006);
assert.equal(res.sample.rig,undefined);
assert.equal(res.sample.debugAddress,undefined);
assert.equal(res.sample.methods.get_MainCamera,undefined);
assert.equal(fx.fakeApi.runtime_invoke.boundPtr,700);
assert.equal(fx.fakeApi.debug_get_method_info,undefined);

// 3) Passive mode (sampleCamera=false, continuous=false) + degraded vectorFields statuses.
fx=fixture({sampleCamera:false,continuous:false});res=fx.run();
assert.equal(res.events.length,4);
assert.equal(res.vectorFields,'verified');
assert.equal(res.sample,null);
assert.equal(fx.fakeApi.runtime_invoke,undefined);

for(const [vfCode,expected] of [[1,'exports-unavailable'],[2,'class-unavailable'],[3,'signature-mismatch']]){
 const r=fixture({sampleCamera:false,continuous:false,vfCode}).run();
 assert.equal(r.vectorFields,expected);
 assert.equal(r.sample,null);
}

// 4) Retry (status=1) and unavailable (status=2) shapes.
assert.deepEqual({...fixture({status:1,reason:'assemblies-not-ready'}).run()},{retry:'assemblies-not-ready'});
assert.deepEqual({...fixture({status:2,reason:'gc-handle-abi-unverified'}).run()},{unavailable:'gc-handle-abi-unverified'});

// 5) Fail-closed boundary checks.
assert.equal(fixture({absent:true}).run(),null);
for(const [option,pattern] of [
 ['noLoader',/native-camera-metadata-loader/],
 ['noTable',/native-camera-metadata-table/],
 ['badLayout',/native-camera-metadata-layout/],
 ['missingExport',/native-camera-metadata-export:domain_get/],
 ['missingImageName',/native-camera-metadata-export:image_get_name/],
 ['missingThreadCurrent',/native-camera-metadata-export:thread_current/],
 ['notExecutable',/code-not-rx/],
 ['badOutput',/native-camera-metadata-output/],
 ['badTargetCount',/native-camera-metadata-output/],
 ['nullExecute',/native-camera-metadata-null:execute/],
 ['nullDebug',/native-camera-metadata-null:debugAddress/],
 ['nullMainCamCode',/native-camera-metadata-null:getMainCameraAddress/]
]){
 const f=fixture({[option]:true});
 assert.throws(()=>f.run(),pattern);
 if(['noLoader','noTable','badLayout'].includes(option))assert.ok(!f.calls.includes('resolve'));
}
assert.throws(()=>fixture({status:1,reason:''}).run(),/native-camera-metadata-output/);
assert.throws(()=>fixture({status:2,reason:''}).run(),/native-camera-metadata-output/);
assert.throws(()=>fixture({vfCode:0}).run(),/native-camera-metadata-output/);
assert.throws(()=>fixture({nullClass:0}).run(),/native-camera-metadata-null:class:Camera/);
assert.throws(()=>fixture({nullField:1}).run(),/native-camera-metadata-null:field:y/);
assert.throws(()=>fixture({nullMethod:5}).run(),/native-camera-metadata-null:method:get_MainCamera/);
assert.throws(()=>fixture({status:-30}).run(),/native-camera-metadata-resolve:-30/);
assert.throws(()=>fixture({status:-35}).run(),/native-camera-metadata-resolve:-35/);

// 6) All 20 optional export presence bits in PgrCameraExport.
for(const name of Object.keys(NATIVE_OPTIONAL_BITS)){
 assert.throws(()=>fixture({missingOptional:'il2cpp_'+name,status:-99}).run(),/native-camera-metadata-resolve:-99/);
}

// Unused feature47 slots are marshalled as null without requiring a presence bit.
for(const name of ['class_get_type','field_get_offset','class_from_type','class_is_enum',
 'class_enum_basetype','class_get_methods','class_get_nested_types','class_get_declaring_type']){
 const result=fixture({sampleCamera:false,continuous:false,missingOptional:'il2cpp_'+name}).run();
 assert.equal(result.events.length,4);assert.equal(result.sample,null);
}
// Bounded sampler neither binds nor invokes continuous-only exports in the adapter.
for(const name of ['method_is_generic','method_is_inflated','method_get_param','debug_get_method_info']){
 const result=fixture({sampleCamera:true,continuous:false,missingOptional:'il2cpp_'+name}).run();
 assert.equal(result.events.length,5);assert(result.sample);
}

// 7) camera-probe.js discover() integration: native selection, retry/unavailable, thread detach, and strict NO fallback.
const probeCode=fs.readFileSync(__dirname+'/../diagnostics/camera-probe.js','utf8');
function runCameraProbeIntegration({nativeResult,nativeThrow,missingAdapter}){
 let attached=0,detached=0,jsAssembliesCalled=0,discoverCalled=0;
 const events=[],timers=[];
 const elfHeader=new Uint8Array(20);
 elfHeader.set([127,69,76,70,2,1]);
 elfHeader[18]=183;elfHeader[19]=0;
 const ptr0={isNull:()=>true,equals:()=>false,and:()=>ptr0,add:()=>ptr0,compare:()=>0,toString:()=>'0x0'};
 const ptrVal=n=>({
  n,
  isNull:()=>false,
  equals:o=>Boolean(o&&o.n===n),
  and:m=> (n&m)===0?ptr0:ptrVal(n&m),
  add:d=>ptrVal(n+d),
  compare:o=>n<o.n?-1:n>o.n?1:0,
  readByteArray:()=>elfHeader.buffer,
  writeU64(){},
  writeU32(){},
  writePointer(){},
  toString:()=>'0x'+n.toString(16)
 });
 const ctx={
  PGRazorNativeCorePath:'libpgrazor.so',
  PGRazorNativeCameraMetadata:missingAdapter?undefined:{
   discover({host}){
    discoverCalled++;
    assert.equal(typeof host.executable,'function');
    assert.equal(host.sampleCamera,false);
    assert.equal(host.continuous,false);
    // Verify host.executable validates in-module rx ranges and rejects out-of-range pointers.
    host.executable(host.module,ptrVal(5004),4);
    assert.throws(()=>host.executable(host.module,ptrVal(100),4),/invalid-code-range/);
    if(nativeThrow)throw Error(nativeThrow);
    return nativeResult;
   }
  },
  Process:{
   arch:'arm64',
   pointerSize:8,
   getCurrentThreadId:()=>512,
   findModuleByName(name){
    if(name==='libil2cpp.so')return {base:ptrVal(5000),size:1000000000,findExportByName:()=>ptrVal(5004)};
    if(name==='libc.so')return {findExportByName:()=>ptrVal(6000)};
    return null;
   },
   findRangeByAddress:()=>({base:ptrVal(5000),size:1000000000,protection:'r-x'})
  },
  Memory:{alloc:()=>ptrVal(8000),allocUtf8String:()=>ptrVal(8100)},
  NativeFunction:function(){return ()=>ptr0;},
  NativeCallback:function(fn){return fn;},
  CModule:function(){return {on_enter:ptrVal(1),on_leave:ptrVal(2),monotonic_ms:ptrVal(3)};},
  Interceptor:{attach:()=>({detach(){}})},
  ptr:()=>ptr0,
  Uint8Array,
  console:{log(){}},
  setTimeout(fn){timers.push(fn);return timers.length;},
  clearTimeout(){}
 };
 vm.createContext(ctx);
 vm.runInContext(probeCode,ctx);
 // SIGNATURES order in camera-probe.js:
 // 0=domain_get, 1=domain_get_assemblies, ..., 13=thread_current, 14=thread_attach, 15=thread_detach
 let sigIdx=0;
 ctx.NativeFunction=function(){
  const idx=sigIdx++;
  if(idx===0)return ()=>ptrVal(900); // domain_get
  if(idx===1)return ()=>{jsAssembliesCalled++;return ptr0;}; // domain_get_assemblies (must NEVER be called)
  if(idx===13)return ()=>ptr0; // thread_current -> null so discover() attaches
  if(idx===14)return ()=>{attached++;return ptrVal(901);}; // thread_attach
  if(idx===15)return ()=>{detached++;}; // thread_detach
  return ()=>ptr0;
 };
 ctx.PGRazorCameraProbe.start({enabled:true,sampleCamera:false,continuous:false,onEvent:e=>events.push(e)});
 let steps=0;
 while(timers.length&&steps<6){
  steps++;
  const fn=timers.shift();
  fn();
  if(events.some(e=>e.kind==='ready'||e.kind==='finished'))break;
 }
 if(events.some(e=>e.kind==='ready'))ctx.PGRazorCameraProbe.stop();
 while(timers.length&&!events.some(e=>e.kind==='finished')){
  const fn=timers.shift();
  fn();
 }
 return {attached,detached,jsAssembliesCalled,discoverCalled,events};
}

// 7a) Native discover success -> emits signature + ready, detaches owned thread, never runs JS assembly scan.
const nativePassive=fixture({sampleCamera:false,continuous:false}).run();
const okProbe=runCameraProbeIntegration({nativeResult:nativePassive});
assert.equal(okProbe.discoverCalled,1);
assert.equal(okProbe.jsAssembliesCalled,0);
assert.equal(okProbe.attached,1);
assert.equal(okProbe.detached,1);
assert.equal(okProbe.events.filter(e=>e.kind==='signature').length,4);
const readyEvt=okProbe.events.find(e=>e.kind==='ready');
assert.ok(readyEvt);
assert.equal(readyEvt.stage,'passive-thread-observation');
assert.equal(readyEvt.vectorFields,'verified');

// 7b) Native discover unavailable -> finishes unavailable without falling back to JS discover().
const unavailProbe=runCameraProbeIntegration({nativeResult:{unavailable:'gc-handle-abi-unverified'}});
assert.equal(unavailProbe.discoverCalled,1);
assert.equal(unavailProbe.jsAssembliesCalled,0);
assert.equal(unavailProbe.attached,1);
assert.equal(unavailProbe.detached,1);
const unavailFin=unavailProbe.events.find(e=>e.kind==='finished');
assert.ok(unavailFin);
assert.equal(unavailFin.status,'unavailable');
assert.equal(unavailFin.reason,'gc-handle-abi-unverified');

// 7c) Native discover throws -> finishes error, detaches thread, NO fallback to JS discover().
const errProbe=runCameraProbeIntegration({nativeThrow:'native-camera-metadata-resolve:-35'});
assert.equal(errProbe.discoverCalled,1);
assert.equal(errProbe.jsAssembliesCalled,0);
assert.equal(errProbe.attached,1);
assert.equal(errProbe.detached,1);
const errFin=errProbe.events.find(e=>e.kind==='finished');
assert.ok(errFin);
assert.equal(errFin.status,'error');
assert.match(errFin.message,/native-camera-metadata-resolve:-35/);

// 7d) Missing PGRazorNativeCameraMetadata when PGRazorNativeCorePath is active -> throws with no fallback.
const missingProbe=runCameraProbeIntegration({missingAdapter:true});
assert.equal(missingProbe.discoverCalled,0);
assert.equal(missingProbe.jsAssembliesCalled,0);
assert.equal(missingProbe.attached,1);
assert.equal(missingProbe.detached,1);
const missingFin=missingProbe.events.find(e=>e.kind==='finished');
assert.ok(missingFin);
assert.equal(missingFin.status,'error');
assert.match(missingFin.message,/native-camera-metadata-missing/);

console.log('PASS native camera metadata adapter (Feature 59 API 1): 11 classes/3 Vector3 fields/8 methods positional mapping, '
 +'passive + sampleCamera + continuous shapes, 20 optional export presence bits, camera-probe.js native selection + no-fallback');
