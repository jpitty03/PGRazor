'use strict';
const assert=require('node:assert/strict'),test=require('node:test'),fs=require('node:fs'),vm=require('node:vm');
// Reuse the existing metadata/observer fixture without running its legacy test main.
// Its fixture evaluates the complete, current production camera-probe.js source.
const fixtureSource=fs.readFileSync(__dirname+'/camera-probe-test.cjs','utf8');
const boundary=fixtureSource.indexOf('async function main(){');assert.ok(boundary>0);
// Mock feature59's validated output shape, not its retired JS discovery. The
// surrounding complete production probe still owns attachment, timers and hooks.
const metadataStub=`PGRazorNativeCameraMetadata:options.metadataMissing?undefined:{discover({host}){
 stats.nativeMetadata=(stats.nativeMetadata||0)+1;
 if(options.metadataFailure)throw Error('native-camera-metadata-status:-17');
 const methods={get_main:new P('Camera.get_main'),get_transform:new P('Component.get_transform'),
 get_forward:new P('Transform.get_forward'),ExecuteTasks:new P('UnitySynchronizationContext.ExecuteTasks'),
 GetInstanceID:new P('Object.GetInstanceID'),get_MainCamera:new P('CameraStack.get_MainCamera'),
 get_cullingMask:new P('Camera.get_cullingMask'),NameToLayer:new P('LayerMask.NameToLayer')};
 const classes={Camera:new P('Camera'),Component:new P('Component'),Transform:new P('Transform'),
 UnitySynchronizationContext:new P('UnitySynchronizationContext'),Object:new P('Object')};
 const fields={x:new P('Vector3.x'),y:new P('Vector3.y'),z:new P('Vector3.z')};
 const sample={api:Object.assign(host.api,api),methods,classes,fields,vector:new P('Vector3'),integer:new P('Int32'),
 exception:host.Memory.alloc(8),float:host.Memory.alloc(4),debugAddress:new P(debugAddress),
 getMainCameraAddress:new P(getterAddress),rig:new P('MainCameraRig'),string:new P('String'),worldMapName:new P('WorldMap')};
 return {execute:methods.ExecuteTasks,events:[],vectorFields:'verified',sample};
}},`;
const prefix=fixtureSource.slice(0,boundary).replace('const context={Process:',
 'const context={PGRazorNativeCorePath:options.nativeSelected,PGRazorNativeCameraSampler:options.nativeFactory,'+metadataStub+'Process:');
assert.notEqual(prefix,fixtureSource.slice(0,boundary),'fixture global injection seam exists');
const fixture=vm.runInNewContext(prefix+'\nfixture;',{require,__dirname});
function setup(options={}){
 const calls=[],events=[];let state,created=0,behavior=null,f;
 const factory={create(deps){created++;state=deps.state;assert.ok(deps.found&&deps.module&&deps.Memory&&deps.NativeFunction&&deps.ptr);
  if(options.installError)throw Error('native-install-failed');
  return {sample(tid,rig,camera,remaining){calls.push({tid,rig:rig.value,camera:camera.value,remaining});
   assert.equal(remaining,250);assert.equal(tid,42);
   if(behavior)return behavior();
   return {x:1,y:0,z:0,yawDegrees:90,cameraId:12,timeMs:state.clock(),tid,mapVisible:true,cameraCullingMask:8192,worldMapLayer:13,mapGate:'world-map-layer-visible'};
  }};
 }};
 f=fixture({sampling:true,nativeSelected:options.selected!==false,nativeFactory:options.missing?undefined:factory,
  metadataMissing:options.metadataMissing,metadataFailure:options.metadataFailure});
 const promise=f.probe.start({enabled:true,sampleCamera:true,continuous:options.continuous!==false,
  shouldSample:()=>options.requested!==false,onEvent:e=>events.push(e)});
 return {f,calls,events,promise,get created(){return created;},get state(){return state;},behavior:fn=>behavior=fn,
  prime(){f.tick();if(options.continuous!==false)f.tick();for(let i=0;i<3;i++)f.enter()();},
  async stop(){f.probe.stop();f.drain();return await promise;}};
}
test('selected continuous path queues native samples, throttles at100ms and invokes no managed getters',async()=>{
 const x=setup();x.prime();assert.equal(x.created,1);assert.equal(x.calls.length,0);
 x.f.getter()();assert.equal(x.calls.length,1);assert.equal(x.events.filter(e=>e.kind==='camera-sample').length,0);
 assert.deepEqual(x.calls[0],{tid:42,rig:'objRig',camera:'objCamera',remaining:250});
 x.f.getter()();assert.equal(x.calls.length,1);x.f.advance(100);x.f.getter()();assert.equal(x.calls.length,2);
 x.f.tick();assert.equal(x.events.filter(e=>e.kind==='camera-sample').length,1,'latest-only queue');
 assert.equal(x.f.stats.invocations.length,0);assert.equal(x.f.stats.handlesCreated,0);
 await x.stop();assert.equal(x.f.handles.size,0);assert.ok(x.f.listeners.every(l=>l.detached===1));
});
test('native sampling retains proof, demand, foreign-thread and active-depth gates',async()=>{
 const x=setup();x.f.tick();x.f.tick();x.f.enter()();x.f.enter()();x.f.getter()();assert.equal(x.calls.length,0);
 x.f.enter()();const leave=x.f.enter();x.f.getter()();assert.equal(x.calls.length,0);leave();
 x.f.getter(999)();assert.equal(x.calls.length,0);x.f.getter()();assert.equal(x.calls.length,1);await x.stop();
 const off=setup({requested:false});off.prime();off.f.getter()();assert.equal(off.calls.length,0);await off.stop();
});
test('native unavailable survives a newer good sample until poll and never falls back',async()=>{
 const x=setup();x.prime();x.behavior(()=>({unavailable:true,reason:'map-layer-hidden',timeMs:x.state.clock(),tid:42,mapVisible:false}));
 x.f.getter()();x.behavior(null);x.f.advance(100);x.f.getter()();x.f.tick();
 const kinds=x.events.filter(e=>['camera-unavailable','camera-sample'].includes(e.kind)).map(e=>e.kind);
 assert.deepEqual(kinds,['camera-unavailable','camera-sample']);assert.equal(x.f.stats.invocations.length,0);await x.stop();
});
test('native sample error is queued then terminal with no JS getter fallback',async()=>{
 const x=setup();x.prime();x.behavior(()=>{throw Error('native-read-failed');});
 const count=x.events.length;x.f.getter()();assert.equal(x.events.length,count,'hook never emits errors directly');
 x.f.drain();const result=await x.promise;assert.equal(result.reason,'native-sampler-error');
 assert.match(result.sampleFailure.message,/native-read-failed/);assert.equal(x.f.stats.invocations.length,0);
 assert.equal(x.events.filter(e=>e.kind==='sampler-error').length,1);assert.ok(x.f.listeners.every(l=>l.detached===1));
});
test('selected unavailable factory and install failure cannot use legacy continuous reads',async()=>{
 for(const options of [{missing:true},{installError:true}]){
  const x=setup(options);x.f.drain();const result=await x.promise;
  assert.notEqual(result.status,'complete');assert.equal(x.f.stats.invocations.length,0);assert.equal(x.calls.length,0);
 }
});
test('selected native metadata missing or failure is terminal before sampler or hooks',async()=>{
 for(const options of [{metadataMissing:true},{metadataFailure:true}]){
  const x=setup(options);x.f.drain();const result=await x.promise;
  assert.equal(result.status,'error');assert.match(result.message,/native-camera-metadata/);
  assert.equal(x.created,0);assert.equal(x.calls.length,0);assert.equal(x.f.listeners.length,0);
  assert.equal(x.f.stats.invocations.length,0);assert.equal(x.f.stats.lookups,0,'no legacy discovery fallback');
  assert.equal(x.f.stats.attach,x.f.stats.detach);assert.equal(x.f.timers.size,0);
 }
});
test('stop during native sample defers observer teardown and discards returned reading',async()=>{
 const x=setup();x.prime();x.behavior(()=>{x.f.probe.stop();assert.ok(x.f.listeners.every(l=>l.detached===0));
  return {x:1,y:0,z:0,yawDegrees:90,cameraId:12,timeMs:x.state.clock(),tid:42,mapVisible:true};});
 x.f.getter()();x.f.drain();assert.equal((await x.promise).status,'cancelled');
 assert.equal(x.events.filter(e=>e.kind==='camera-sample').length,0);assert.ok(x.f.listeners.every(l=>l.detached===1));assert.equal(x.f.handles.size,0);
});
test('nonpilot continuous and selected noncontinuous retain legacy sampler',async()=>{
 for(const options of [{selected:false},{continuous:false}]){
  const x=setup(options);x.prime();if(options.continuous!==false)x.f.getter()();
  assert.equal(x.created,0);assert.ok(x.f.stats.invocations.length>0);await x.stop();assert.equal(x.f.handles.size,0);
 }
});
