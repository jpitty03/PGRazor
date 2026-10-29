// VM adapter test for runtime/native-camera-sampler.js (feature 41): verifies the 216-byte config packing
// (8-byte header + 25 pointers in exact PgrCameraSamplerConfig order + expected_layer(13)/reserved0(0)), the
// 40-byte request packing (version/size, tid, remaining_ms clamp, raw rig, raw camera), the 72-byte output
// decoding into the legacy reading shapes (success {x,y,z,yawDegrees,cameraId,timeMs,tid,mapVisible:true,
// cameraCullingMask,worldMapLayer,mapGate}; unavailable {unavailable:true,reason,timeMs,tid,mapVisible:false}
// with map facts only on map-hidden), a negative fault THROWING for the host to latch, the output
// header/status-agreement guards, and fail-closed metadata/install + pilot-absent -> null. The native core
// is mocked; the real C++ is native/camera_sampler.cpp. No managed call is made from the adapter.
//
const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');

class Mem{
 constructor(store={},offset=0){this.store=store;this.offset=offset;}
 isNull(){return false;}
 add(n){return new Mem(this.store,this.offset+n);}
 writeU32(v){this.store[this.offset]={t:'u32',v:v>>>0};return this;}
 writeS32(v){this.store[this.offset]={t:'s32',v:v|0};return this;}
 writeU64(v){this.store[this.offset]={t:'u64',v:BigInt(v)};return this;}
 writeDouble(v){this.store[this.offset]={t:'f64',v:+v};return this;}
 writePointer(v){this.store[this.offset]={t:'ptr',v};return this;}
 readU32(){const c=this.store[this.offset];return c?c.v>>>0:0;}
 readS32(){const c=this.store[this.offset];return c?c.v|0:0;}
 readU64(){const c=this.store[this.offset];return c?BigInt(c.v):0n;}
 readDouble(){const c=this.store[this.offset];return c?+c.v:0;}
 readPointer(){const c=this.store[this.offset];return c?c.v:undefined;}
}

let tag=0;
const P=()=>({tag:++tag,isNull:()=>false});
const NULLPTR={isNull:()=>true};

function makeFound(){
 return {sample:{
  classes:{Camera:P(),Transform:P()},vector:P(),rig:P(),integer:P(),string:P(),
  methods:{get_main:P(),get_transform:P(),get_forward:P(),NameToLayer:P(),get_cullingMask:P(),GetInstanceID:P()},
  fields:{x:P(),y:P(),z:P()}}};
}
function makeModule(){return {findExportByName:n=>({name:n,isNull:()=>false})};}

// Default output writer: fills a well-formed 72-byte output from the control object and returns the status.
function writeOut(o,c){
 o.writeU32(c.version==null?1:c.version);
 o.add(4).writeU32(c.size==null?72:c.size);
 o.add(8).writeS32(c.outStatus==null?c.status:c.outStatus);
 o.add(12).writeU32(c.reason||0);
 o.add(16).writeS32(c.cameraId||0);
 o.add(20).writeU32(c.mask||0);
 o.add(24).writeS32(c.layer||0);
 o.add(28).writeS32(c.reserved==null?0:c.reserved);
 o.add(32).writeDouble(c.x||0);o.add(40).writeDouble(c.y||0);o.add(48).writeDouble(c.z||0);
 o.add(56).writeDouble(c.yaw||0);o.add(64).writeDouble(c.timeMs||0);
 return c.status;
}

function harness(){
 const table=new Mem();
 const calls=[];
 let configMem,installRc=0;
 const ctl={status:1,reason:1,x:1,y:0,z:0,yaw:90,cameraId:12,mask:8192,layer:13,timeMs:456};
 const flags={throwSample:false};
 table.writeU32(1);table.add(4).writeU32(24);
 table.add(8).writePointer(c=>{configMem=c;return installRc;});
 table.add(16).writePointer((reqPtr,outPtr)=>{calls.push({reqPtr,outPtr});if(flags.throwSample)throw new Error('boom');return writeOut(outPtr,ctl);});
 const sandbox={Memory:{alloc:()=>new Mem()},NativeFunction:function(f){return f;},PGRazorNativeFeature:()=>table,ptr:()=>NULLPTR};
 vm.runInNewContext(fs.readFileSync(__dirname+'/../runtime/native-camera-sampler.js','utf8'),sandbox);
 return {sandbox,calls,configMem:()=>configMem,setInstallRc(v){installRc=v;},setCtl(o){Object.assign(ctl,o);},flags};
}

function create(h,found,module){
 return h.sandbox.PGRazorNativeCameraSampler.create({
  state:{clock:()=>123},found:found||makeFound(),module:module||makeModule(),
  Memory:h.sandbox.Memory,NativeFunction:h.sandbox.NativeFunction,ptr:h.sandbox.ptr});
}

// --- 1. config packing: 216-byte layout, 25-pointer order, trailing expected_layer/reserved0 -----------
{
 const h=harness();const found=makeFound(),module=makeModule();
 const c=create(h,found,module);assert.ok(c&&typeof c.sample==='function','controller created');
 const cfg=h.configMem();
 assert.equal(cfg.readU32(),1,'config version 1');
 assert.equal(cfg.add(4).readU32(),216,'config size 216');
 const exports=['runtime_invoke','object_get_class','class_is_assignable_from','field_get_value','object_unbox',
  'gchandle_new','gchandle_get_target','gchandle_free','thread_current','string_new'];
 exports.forEach((n,i)=>assert.equal(cfg.add(8+i*8).readPointer().name,'il2cpp_'+n,'export '+i+' '+n));
 const meta=[found.sample.classes.Camera,found.sample.classes.Transform,found.sample.vector,found.sample.rig,
  found.sample.integer,found.sample.string,found.sample.methods.get_main,found.sample.methods.get_transform,
  found.sample.methods.get_forward,found.sample.methods.NameToLayer,found.sample.methods.get_cullingMask,
  found.sample.methods.GetInstanceID,found.sample.fields.x,found.sample.fields.y,found.sample.fields.z];
 meta.forEach((v,i)=>assert.equal(cfg.add(8+(10+i)*8).readPointer(),v,'metadata slot '+(10+i)));
 assert.equal(cfg.add(208).readS32(),13,'expected_layer 13 at 208');
 assert.equal(cfg.add(212).readS32(),0,'reserved0 0 at 212');
 console.log('PASS camera-sampler adapter: 216-byte config packing, 25-pointer order, expected_layer/reserved0');
}

// --- 2. request packing + success reading ---------------------------------------------------------------
{
 const h=harness();const c=create(h);
 h.setCtl({status:1,reason:1,x:1,y:0,z:0,yaw:90,cameraId:12,mask:8192,layer:13,timeMs:456});
 const rig=P(),camera=P();
 const res=c.sample(42,rig,camera,250);
 const req=h.calls.at(-1).reqPtr;
 assert.equal(req.readU32(),1,'request version 1');
 assert.equal(req.add(4).readU32(),40,'request size 40');
 assert.equal(req.add(8).readU64(),42n,'proven owner tid at 8');
 assert.equal(req.add(16).readU64(),250n,'remaining_ms at 16');
 assert.equal(req.add(24).readPointer(),rig,'raw rig at 24');
 assert.equal(req.add(32).readPointer(),camera,'raw camera at 32');
 deepRecord(res,{x:1,y:0,z:0,yawDegrees:90,cameraId:12,timeMs:456,tid:42,mapVisible:true,
  cameraCullingMask:8192,worldMapLayer:13,mapGate:'world-map-layer-visible'},'success reading mapped');
 console.log('PASS camera-sampler adapter: request packing + success reading shape');
}

// --- 3. remaining clamp (floor, non-negative) -----------------------------------------------------------
{
 const h=harness();const c=create(h);
 c.sample(7,P(),P(),250.9);assert.equal(h.calls.at(-1).reqPtr.add(16).readU64(),250n,'floors fractional remaining');
 c.sample(7,P(),P(),-5);assert.equal(h.calls.at(-1).reqPtr.add(16).readU64(),0n,'negative remaining -> 0');
 console.log('PASS camera-sampler adapter: remaining clamp');
}

// --- 4. unavailable mappings, including map-hidden map facts ---------------------------------------------
{
 const reasons={2:'off',3:'lease-expired',4:'owner-thread-unavailable',5:'non-map-rig',
  6:'camera-getter-returned-null',7:'camera-mismatch',8:'camera-transform-unavailable'};
 for(const [code,text] of Object.entries(reasons)){
  const h=harness();const c=create(h);h.setCtl({status:0,reason:+code,timeMs:9,mask:0,layer:0,x:0,y:0,z:0,yaw:0,cameraId:0});
  const res=c.sample(42,P(),P(),250);
  deepRecord(res,{unavailable:true,reason:text,timeMs:9,tid:42,mapVisible:false},'unavailable '+text);
 }
 // map-hidden carries mask/layer/mapGate.
 const h=harness();const c=create(h);h.setCtl({status:0,reason:9,timeMs:11,mask:4,layer:13});
 deepRecord(c.sample(42,P(),P(),250),{unavailable:true,reason:'map-hidden',timeMs:11,tid:42,mapVisible:false,
  cameraCullingMask:4,worldMapLayer:13,mapGate:'world-map-layer-visible'},'map-hidden map facts');
 console.log('PASS camera-sampler adapter: unavailable reasons + map-hidden facts');
}

// --- 5. native fault throws (host latches), output guards throw ------------------------------------------
{
 const h=harness();const c=create(h);h.setCtl({status:-13,reason:12});
 assert.throws(()=>c.sample(42,P(),P(),250),/native-camera-sampler-fault:managed-getter-exception/,'fault throws');
 // status/return disagreement -> output guard throw.
 const h2=harness();const c2=create(h2);h2.setCtl({status:1,outStatus:0,reason:1,timeMs:1});
 assert.throws(()=>c2.sample(42,P(),P(),250),/native-camera-sampler-output/,'status mismatch throws');
 // bad header version -> output guard throw.
 const h3=harness();const c3=create(h3);h3.setCtl({status:0,reason:2,version:2,timeMs:1});
 assert.throws(()=>c3.sample(42,P(),P(),250),/native-camera-sampler-output/,'bad header version throws');
 // nonzero reserved -> output guard throw.
 const h4=harness();const c4=create(h4);h4.setCtl({status:0,reason:2,reserved:1,timeMs:1});
 assert.throws(()=>c4.sample(42,P(),P(),250),/native-camera-sampler-output/,'nonzero reserved throws');
 // a thrown native call propagates (the host latches a native-sampler-error).
 const h5=harness();const c5=create(h5);h5.flags.throwSample=true;
 assert.throws(()=>c5.sample(42,P(),P(),250),/boom/,'native throw propagates');
 console.log('PASS camera-sampler adapter: fault throws, output guards, native-throw propagation');
}

// --- 6. fail-closed metadata / install, pilot-absent -> null -------------------------------------------
{
 const h=harness();h.setInstallRc(-9);
 assert.throws(()=>create(h),/native-camera-sampler-install:-9/,'non-zero install throws');

 const missing=[
  ['no Camera class',f=>delete f.sample.classes.Camera],
  ['no vector',f=>delete f.sample.vector],
  ['no get_main',f=>delete f.sample.methods.get_main],
  ['inert NameToLayer',f=>f.sample.methods.NameToLayer={isNull:()=>true}],
  ['no field x',f=>delete f.sample.fields.x]];
 for(const [name,mutate] of missing){
  const hh=harness();const found=makeFound();mutate(found);
  assert.throws(()=>create(hh,found),/config-null|metadata/,'fail-closed: '+name);
 }
 // absent found.sample -> metadata throw.
 assert.throws(()=>create(harness(),{}),/metadata/,'absent found.sample throws');

 // pilot absent (no CorePath, no feature query) -> null.
 const sandbox={Memory:{alloc:()=>new Mem()},NativeFunction:function(f){return f;},PGRazorNativeFeature:undefined,ptr:()=>NULLPTR};
 vm.runInNewContext(fs.readFileSync(__dirname+'/../runtime/native-camera-sampler.js','utf8'),sandbox);
 assert.equal(sandbox.PGRazorNativeCameraSampler.create({state:{},found:makeFound(),module:makeModule(),
  Memory:sandbox.Memory,NativeFunction:sandbox.NativeFunction,ptr:sandbox.ptr}),null,'pilot absent -> null');
 console.log('PASS camera-sampler adapter: fail-closed install/metadata, pilot-absent null');
}

// --- 7. selected pilot cannot silently omit the native loader/table ------------------------------------
{
 const h=harness();h.sandbox.PGRazorNativeFeature=undefined;
 assert.equal(create(h),null,'feature query absent (no CorePath) -> null');
 h.sandbox.PGRazorNativeCorePath='selected';
 assert.throws(()=>create(h),/native-camera-sampler-loader/,'selected pilot + no loader throws');
 const h2=harness();h2.sandbox.PGRazorNativeFeature=()=>null;h2.sandbox.PGRazorNativeCorePath='selected';
 assert.throws(()=>create(h2),/native-camera-sampler-table/,'selected pilot + missing table throws');
 console.log('PASS camera-sampler adapter: selected pilot requires loader + table');
}

console.log('native-camera-sampler-test: all adapter checks passed');

for(const invalid of [{status:2},{yaw:360},{x:Infinity},{timeMs:Infinity},{layer:12},{mask:0},{x:0,z:0}]){
 const h=harness(),c=create(h);h.setCtl(invalid);
 assert.throws(()=>c.sample(42,P(),P(),250),/native-camera-sampler-/,'reject malformed successful reading '+JSON.stringify(invalid));
}
console.log('PASS camera-sampler adapter: unknown status and invalid scalar/map facts rejected');

function deepRecord(actual,expected,message){assert.deepEqual({...actual},expected,message);}
