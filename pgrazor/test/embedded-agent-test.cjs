const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const source=require('./script-bundle.cjs')('embedded-agent.js');
function fixture(failAt, surveyFails=false, continuous=false) {
    const scheduled=[],events=[],methods={},timeouts=new Map(),cameraStatus=[]; let active=false,timerStarted=false,surveyStarts=0,surveyStops=0,beat,request=false,configured=false,busy=false;
    let steeringConfigured=false, requested=true, probeOptions, finishProbe, probeCompletion;
    const headings=[],invalidations=[];
    const control={active:()=>active,latitude:()=>10,longitude:()=>20,speed:()=>1,bearing:()=>90,
        configureCameraSteering(v){steeringConfigured=v;},cameraSteeringRequested(){return requested;},buddySamplingRequested(){return false;},
        updateCameraHeadingAt(...args){headings.push(args);},invalidateCameraHeading(reason){invalidations.push(reason);},
        connect(){active=false;},event:(...x)=>events.push(x),renewLease(){},embedded(){active=false;},disconnect(){active=false;},
        configureCameraDiagnostics(v){configured=v;},setCameraDiagnosticRunning(v){busy=v;},cameraDiagnostic(s){cameraStatus.push(s);},takeCameraSampleRequest(){const r=request;request=false;return r;}};
    for(const name of ['getLatitude','getLongitude','getSpeed','getBearing']) methods[name]={implementation:null,call(obj){if(obj.error)throw obj.error;return obj[name];}};
    const location=Object.fromEntries(Object.keys(methods).map(name=>[name,{overload(){if(name===failAt)throw Error('Synthetic method failure');return methods[name];}}]));
    const java={perform:f=>f(),performNow:f=>f(),scheduleOnMainThread:f=>f(),use:name=>name==='dev.pgrazor.Control'?control:location};
    function File() { this.write=()=>{}; this.flush=()=>{}; this.close=()=>{}; }
    File.readAllText=()=>'';
    const survey={start(options){surveyStarts++;probeOptions=options;if(continuous){probeCompletion=new Promise(resolve=>{finishProbe=resolve;});return probeCompletion;}if(options.sampleCamera)options.onEvent({kind:'camera-sample',yawDegrees:123.456,cameraId:42,sampleIndex:1});options.onEvent({kind:'finished',status:'complete'});return Promise.resolve();},stop(){surveyStops++;return probeCompletion;}};
    const context={console:{log(){}},File,Script:{evaluate(name){if(name.includes('il2cpp')||name.includes('camera-probe')||name.includes('scene-probe')){if(surveyFails)throw Error('Survey load failure');return survey;}return java;}},rpc:{exports:{}},setImmediate:f=>scheduled.push(f),setInterval(fn){beat=fn;timerStarted=true;return 1;},clearInterval(){timerStarted=false;},setTimeout(fn,ms){assert([1500,20000].includes(ms));timeouts.set(1,fn);return 1;},clearTimeout(id){timeouts.delete(id);}};
    vm.runInNewContext(source,context);
    return {context,methods,events,scheduled,timeouts,cameraStatus,headings,invalidations,
        steeringConfigured:()=>steeringConfigured,probeOptions:()=>probeOptions,setRequested(v){requested=v;},finishProbe(){finishProbe();},
        configured:()=>configured,busy:()=>busy,request(){request=true;beat();},surveyStarts:()=>surveyStarts,surveyStops:()=>surveyStops,enable(){active=true;},timer:()=>timerStarted};
}
const f=fixture();
assert.equal(f.context.rpc.exports.init('early',{bridgePath:'fixture',startupLog:'fixture'}),undefined);
assert.equal(f.methods.getLatitude.implementation,null,'init must return before Java work');
assert.equal(f.scheduled.length,1); f.scheduled[0]();
assert(f.events.some(e=>e[0]==='embedded-ready')); assert(f.timer());
assert.match(f.events.find(e=>e[0]==='embedded-ready')[1],/^Frida Java Location getters installed;/);
const compiledLocation=fixture();
compiledLocation.context.rpc.exports.init('early',{compiledLocationGetters:true});compiledLocation.scheduled[0]();
assert.match(compiledLocation.events.find(e=>e[0]==='embedded-ready')[1],/^Compiled ART Location getters initialized;/);
for(const method of Object.values(compiledLocation.methods))assert.equal(method.implementation,null);
compiledLocation.context.rpc.exports.dispose();
assert.equal(f.timeouts.size,0,'Default build must not schedule metadata calls');
const original={getLatitude:12.5,getLongitude:45.25,getSpeed:2,getBearing:30};
for(const name of Object.keys(f.methods)) assert.equal(f.methods[name].implementation.call(original),original[name]);
const error=Error('Original getter throws'); assert.throws(()=>f.methods.getLatitude.implementation.call({error}),e=>e===error);
f.enable(); assert.equal(f.methods.getLatitude.implementation.call(original),10); assert.equal(f.methods.getLongitude.implementation.call(original),20);
f.context.rpc.exports.dispose(); assert(!f.timer()); for(const method of Object.values(f.methods)) assert.equal(method.implementation,null);
const bad=fixture('getSpeed'); bad.context.rpc.exports.init('early',{}); bad.scheduled[0]();
assert(bad.events.some(e=>e[0]==='embedded-error')); assert(!bad.timer()); for(const method of Object.values(bad.methods)) assert.equal(method.implementation,null);
for(const fails of [false,true]) {
    const diagnostic=fixture(undefined,fails);
    diagnostic.context.rpc.exports.init('early',{surveyPath:'survey'}); diagnostic.scheduled[0]();
    assert.equal(diagnostic.surveyStarts(),0,'Survey must wait until startup has completed');
    diagnostic.timeouts.get(1)();
    assert(diagnostic.events.some(e=>e[0]===(fails?'il2cpp-survey-error':'il2cpp-survey')));
    assert(diagnostic.timer(),'Diagnostics failure must not disable location adapter');
    assert.equal(typeof diagnostic.methods.getLatitude.implementation,'function');
    diagnostic.context.rpc.exports.dispose();
    if(!fails)assert.equal(diagnostic.surveyStops(),1);
}
const cancelled=fixture();cancelled.context.rpc.exports.init('early',{surveyPath:'survey'});cancelled.scheduled[0]();
const delayed=cancelled.timeouts.get(1);cancelled.context.rpc.exports.dispose();delayed();
assert.equal(cancelled.timeouts.size,0);assert.equal(cancelled.surveyStarts(),0);
const early=fixture();early.context.rpc.exports.init('early',{});early.context.rpc.exports.dispose();early.scheduled[0]();assert(!early.timer());
const camera=fixture();camera.context.rpc.exports.init('early',{cameraProbePath:'camera'});camera.scheduled[0]();
assert.equal(camera.surveyStarts(),0);camera.timeouts.get(1)();
assert(camera.events.some(e=>e[0]==='camera-probe'));
camera.context.rpc.exports.dispose();assert.equal(camera.surveyStops(),1);
const sampling=fixture();sampling.context.rpc.exports.init('early',{cameraProbePath:'camera',cameraSample:true});sampling.scheduled[0]();
assert(sampling.configured());sampling.request();assert.equal(sampling.timeouts.size,0,'Manual request cancels scheduled automatic probe');
sampling.scheduled[1]();assert.equal(sampling.surveyStarts(),1);assert(sampling.cameraStatus.some(s=>s.includes('123.5')&&s.includes('Camera 42')));
assert(sampling.busy(),'Busy stays true through the diagnostic promise');
sampling.request();sampling.scheduled[2]();assert.equal(sampling.surveyStarts(),1,'Overlapping diagnostics rejected');sampling.context.rpc.exports.dispose();assert(!sampling.busy());
const completed=fixture();completed.context.rpc.exports.init('early',{cameraProbePath:'camera',cameraSample:true});completed.scheduled[0]();completed.timeouts.get(1)();assert(completed.busy());
const broken=fixture(undefined,true);broken.context.rpc.exports.init('early',{cameraProbePath:'camera',cameraSample:true});broken.scheduled[0]();broken.timeouts.get(1)();assert(!broken.busy(),'Load failure releases diagnostic button');broken.context.rpc.exports.dispose();
const scene=fixture();scene.context.rpc.exports.init('early',{sceneProbePath:'scene',sceneObserve:true});scene.scheduled[0]();scene.timeouts.get(1)();assert(scene.events.some(e=>e[0]==='scene-probe'));assert(scene.probeOptions().observeRuntime);scene.context.rpc.exports.dispose();
const steering=fixture(undefined,false,true);
steering.context.rpc.exports.init('early',{cameraProbePath:'camera',cameraSteering:true});steering.scheduled[0]();
assert(steering.steeringConfigured());assert(!steering.configured());steering.timeouts.get(1)();
const opts=steering.probeOptions();assert(opts.continuous);assert(opts.sampleCamera);assert(opts.shouldSample());
steering.setRequested(false);assert(!opts.shouldSample());
const reading={kind:'camera-sample',yawDegrees:270,cameraId:42,ageMillis:105.9,timeMs:1000,mapVisible:true};
opts.onEvent(reading);assert.deepEqual(steering.headings[0],[270,42,true,1000],'Pass absolute boot timestamp; Java measures age at delivery');
opts.onEvent({...reading,timeMs:1100,mapVisible:false});assert.equal(steering.headings[1][2],false);
opts.onEvent({...reading,timeMs:1200,mapVisible:undefined});assert.equal(steering.headings[2][2],false,'Missing scene evidence cannot enable steering');
assert.equal(steering.events.filter(e=>e[0]==='camera-heading').length,1,'Throttle logs, not heading delivery');
for(const invalid of [{timeMs:NaN},{timeMs:-1},{yawDegrees:Infinity},{cameraId:0},{cameraId:1.5}])opts.onEvent({...reading,...invalid});
assert.equal(steering.headings.length,3);assert.equal(steering.invalidations.length,5);
opts.onEvent({kind:'camera-unavailable'});assert(steering.invalidations.includes('Camera unavailable'));
steering.finishProbe();
setImmediate(async()=>{
    assert(steering.invalidations.includes('Camera reader stopped'));
    steering.context.rpc.exports.dispose();assert(steering.invalidations.includes('Adapter disposed'));assert(!opts.shouldSample());
    opts.onEvent(reading);assert.equal(steering.headings.length,3,'Disposed reader cannot refresh heading');
    const pending=fixture(undefined,false,true);
    pending.context.rpc.exports.init('early',{cameraProbePath:'camera',cameraSteering:true});pending.scheduled[0]();pending.timeouts.get(1)();
    let disposed=false;const disposal=pending.context.rpc.exports.dispose().then(()=>{disposed=true;});
    await Promise.resolve();assert(!disposed,'RPC dispose must wait for native sampler cleanup');
    pending.finishProbe();await disposal;assert(disposed);
    assert(!completed.busy(),'Completion releases the diagnostic button');completed.context.rpc.exports.dispose();
    console.log('PASS: deferred startup, originals/exceptions, enabled values, diagnostic busy lifecycle, dispose and partial-failure cleanup');
});
