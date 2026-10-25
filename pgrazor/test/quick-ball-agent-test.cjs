const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm'),path=require('node:path');
const source=require('./script-bundle.cjs')('embedded-agent.js');
function fixture(fault={}) {
 const timers=new Map(),events=[],methods={},order=[],features={},outcomes=[];let id=0,beat,foreground=true,connected=true;
 const control={connect(){connected=true;},renewLease(){},active:()=>false,embedded(){},event(...e){events.push(e);},
  quickBallOutcome(o){if(fault.outcomeThrows)throw Error('toast failed');outcomes.push(o);return true;}};
 control.setBerryReady=()=>{};control.setThrowArReady=()=>{};control.setThrowReady=()=>{};control.setMapTapReady=()=>{};control.throwAllowed=()=>false;control.mapTapAllowed=()=>false;
 const originals={setTransferKeepAll(){},setTransferKeepSpecies(){},selectBerryPriority(){},setAutoBerry(){},setEncounterBerryEnabled(){},setMapTapMode(){},setThrowGrade(){},setThrowCurve(){},suspendForBackground(){foreground=false;return 'background';},disconnect(){connected=false;return 'disconnect';}};
 for(const name of ['Summary','Ball']) {
  const f=features[name]={ready:false,enabled:false,configured:false,permissions:[],options:null,stopCount:0,failRevoke:false};
  control['configureQuick'+name]=v=>{f.configured=v;f.ready=false;f.enabled=false;};
  control['quick'+name+'Allowed']=()=>f.ready&&f.enabled&&foreground&&connected;
  originals['setQuick'+name+'Enabled']=v=>{f.enabled=v&&f.ready;return 'enabled';};
  originals['setQuick'+name+'Ready']=v=>{if(name==='Ball'&&fault.ballStatusThrows)throw Error('ball status bridge failed');f.ready=v&&f.configured;if(!f.ready)f.enabled=false;return 'ready';};
  f.api={start(o){f.options=o;return new Promise(resolve=>f.finish=resolve);},setAllowed(v){
   f.permissions.push(v);order.push(name+':'+v);if(!v&&f.failRevoke)throw Error('revocation failed');
  },stop(){f.stopCount++;order.push(name+':stop');if(name==='Ball')return new Promise(resolve=>f.resolveStop=()=>{f.finish({status:'cancelled'});resolve({status:'cancelled'});});f.finish({status:'cancelled'});return Promise.resolve();}};
 }
 for(const [name,fn] of Object.entries(originals)) {
  let implementation=null;
  const method={call(receiver,...args){order.push('original:'+name);return fn.apply(receiver,args);}};
  Object.defineProperty(method,'implementation',{get:()=>implementation,set(v){if(v&&fault.install===name)throw Error('bad hook');implementation=v;}});
  methods[name]=method;
  const wrapped=function(...args){return implementation?implementation.apply(this,args):method.call(this,...args);};
  wrapped.overload=(...types)=>{assert.deepEqual(types,['setMapTapMode','setThrowGrade','selectBerryPriority','setAutoBerry'].includes(name)?['int']:name==='setTransferKeepSpecies'?['java.lang.String']:['setThrowCurve','setTransferKeepAll','setEncounterBerryEnabled'].includes(name)?['boolean']:name.includes('Enabled')?['boolean']:name.includes('Ready')?['boolean','java.lang.String']:[]);return method;};
  control[name]=wrapped;
 }
 const location={};for(const name of ['getLatitude','getLongitude','getSpeed','getBearing']){methods[name]={implementation:null,call:()=>1};location[name]={overload:()=>methods[name]};}
 const java={perform:f=>f(),performNow:f=>f(),scheduleOnMainThread:f=>f(),use:n=>n==='dev.pgrazor.Control'?control:location};
 const context={rpc:{exports:{}},console:{log(){}},File:{readAllText:()=>''},Script:{evaluate(name){
  if(name.includes('quick-ball')){if(fault.asset)throw Error('bad asset');return features.Ball.api;}
  if(name.includes('quick-summary')||name.includes('quick-close'))return features.Summary.api;return java;
 }},setImmediate:f=>f(),setInterval:f=>{beat=f;return 10;},clearInterval(){},setTimeout(f,ms){timers.set(++id,{f,ms});return id;},clearTimeout:id=>timers.delete(id)};
 vm.runInNewContext(source,context);
 return {control,context,features,methods,events,timers,order,outcomes,fault,beat:()=>beat(),init(both=false,close=false){context.rpc.exports.init('early',{quickBall:true,quickBallPath:'ball',...(both?{quickSummary:true,quickSummaryPath:'summary',quickClose:close}:{})});},
  fire(ms=1500){for(const [id,t] of [...timers]){assert.equal(t.ms,ms);timers.delete(id);t.f();}},
  ready(){features.Ball.options.onEvent({kind:'metadata-ready'});},enable(){control.setQuickBallEnabled(true);},
  async dispose(){const p=context.rpc.exports.dispose();if(features.Ball.resolveStop)features.Ball.resolveStop();await p;}};
}
async function main(){
 for(const lifecycle of ['suspendForBackground','disconnect']) {
  const c=fixture();c.init(true,true);c.fire();assert.equal(c.features.Ball.options,null,'Ball hooks wait for summary metadata');c.features.Summary.options.onEvent({kind:'metadata-ready'});c.fire(100);c.ready();
  c.enable();c.control.setQuickSummaryEnabled(true);c.beat();
  assert.equal(c.features.Summary.permissions.at(-1),true);assert.equal(c.features.Ball.permissions.at(-1),true);
  c.order.length=0;c.control[lifecycle]();assert.deepEqual(c.order,['Summary:false','Ball:false','original:'+lifecycle]);
  await c.dispose();
 }
 const f=fixture();f.init();assert(f.features.Ball.configured);assert(!f.features.Ball.ready&&!f.features.Ball.enabled);assert.equal(f.timers.size,1);
 f.fire();f.beat();assert.equal(f.features.Ball.permissions.at(-1),false);
 f.ready();assert(f.features.Ball.ready);assert(!f.features.Ball.enabled,'Metadata support must not auto-enable');
 f.enable();assert(f.features.Ball.enabled,'No fresh catch baseline is required');f.beat();assert.equal(f.features.Ball.permissions.at(-1),true);
 f.order.length=0;assert.equal(f.control.setQuickBallEnabled(false),'enabled');assert.deepEqual(f.order,['Ball:false','original:setQuickBallEnabled']);
 f.enable();f.beat();f.order.length=0;f.control.suspendForBackground();assert.deepEqual(f.order,['Ball:false','original:suspendForBackground']);
 f.beat();assert.equal(f.features.Ball.permissions.at(-1),false);await f.dispose();assert(Object.values(f.methods).every(m=>m.implementation===null));
 const early=fixture();early.init();await early.dispose();assert.equal(early.timers.size,0);assert.equal(early.features.Ball.stopCount,0);
 const both=fixture();both.init(true);both.fire();both.ready();both.features.Summary.options.onEvent({kind:'metadata-ready'});
 both.enable();both.control.setQuickSummaryEnabled(true);both.beat();assert.equal(both.features.Ball.permissions.at(-1),true);assert.equal(both.features.Summary.permissions.at(-1),true);
 both.order.length=0;both.control.disconnect();assert.deepEqual(both.order,['Summary:false','Ball:false','original:disconnect'],'Both permissions revoke before shared original lifecycle');
 await both.dispose();assert(Object.values(both.methods).every(m=>m.implementation===null));
 const partial=fixture({install:'setQuickBallReady'});partial.init(true);assert.equal(partial.timers.size,1,'Summary startup remains scheduled after optional ball hook failure');
 assert.equal(partial.methods.setQuickBallEnabled.implementation,null);assert.equal(typeof partial.methods.suspendForBackground.implementation,'function');
 partial.fire();partial.features.Summary.options.onEvent({kind:'metadata-ready'});partial.control.setQuickSummaryEnabled(true);partial.beat();assert.equal(partial.features.Summary.permissions.at(-1),true);
 partial.order.length=0;partial.control.suspendForBackground();assert.deepEqual(partial.order,['Summary:false','original:suspendForBackground']);await partial.dispose();
 const broken=fixture({asset:true});broken.init();broken.fire();assert(!broken.features.Ball.ready);assert.equal(typeof broken.methods.getLatitude.implementation,'function');await broken.dispose();
 const revocation=fixture();revocation.init();revocation.fire();revocation.ready();revocation.enable();revocation.beat();revocation.features.Ball.failRevoke=true;
 assert.equal(revocation.control.suspendForBackground(),'background');assert.equal(revocation.features.Ball.stopCount,1);assert(!revocation.features.Ball.ready);revocation.beat();
 revocation.features.Ball.options.onEvent({kind:'outcome-confirmed',outcome:'caught'});assert.equal(revocation.outcomes.length,0,'Failed feature forwards no outcome');await revocation.dispose();
 const toast=fixture();toast.init();toast.fire();toast.ready();toast.enable();toast.beat();const outcome=e=>toast.features.Ball.options.onEvent(e);
 for(const kind of ['trace-identity','trace-state','trace-end','trace-unavailable'])outcome({kind});
 assert.equal(toast.events.filter(e=>e[0]==='quick-ball-trace').length,4);
 assert(!toast.events.some(e=>e[0]==='quick-ball'&&e[1].includes('"trace-')),'Trace records stay out of the speed report grammar');
 assert(toast.features.Ball.ready,'Optional telemetry failure does not revoke acceleration');
 assert.equal(toast.outcomes.length,0,'Telemetry never becomes a toast');
 outcome({kind:'outcome-confirmed',outcome:'caught',status:1,statusName:'CatchSuccess',tid:7});outcome({kind:'outcome-confirmed',outcome:'broke-free'});outcome({kind:'outcome-confirmed',outcome:'fled'});
 assert.deepEqual(toast.outcomes,['caught','broke-free','fled']);
 for(const bad of [{kind:'outcome-suppressed',statusName:'CatchMissed',reason:'ambiguous-status'},{kind:'outcome-suppressed',outcome:'caught'},{kind:'outcome-confirmed',outcome:'Caught'},
  {kind:'outcome-confirmed',outcome:'missed'},{kind:'outcome-confirmed'},{kind:'outcome-confirmed',outcome:{toString:()=>'caught'}},{kind:'outcome-unavailable',reason:'gc-handle-new'}])outcome(bad);
 assert.deepEqual(toast.outcomes,['caught','broke-free','fled'],'Only whitelisted confirmed outcomes reach Java');
 assert.equal(toast.events.filter(e=>e[0]==='quick-ball-outcome').length,10,'Every native outcome record is logged');
 assert(!toast.events.some(e=>e[0]==='quick-ball'&&e[1].includes('"outcome-')),'Outcome records stay out of the [quick-ball] evidence grammar');
 toast.fault.outcomeThrows=true;outcome({kind:'outcome-confirmed',outcome:'caught'});toast.fault.outcomeThrows=false;
 toast.beat();assert.equal(toast.features.Ball.permissions.at(-1),true,'Toast failure keeps acceleration permission');assert(toast.features.Ball.ready);
 assert.equal(typeof toast.methods.getLatitude.implementation,'function','Toast failure leaves location hooks installed');
 outcome({kind:'outcome-confirmed',outcome:'fled'});assert.deepEqual(toast.outcomes,['caught','broke-free','fled','fled']);
 const toastDisposed=toast.context.rpc.exports.dispose();outcome({kind:'outcome-confirmed',outcome:'caught'});toast.features.Ball.resolveStop();await toastDisposed;
 assert.equal(toast.outcomes.length,4,'No outcome is forwarded after dispose');
 const cleanup=fixture();cleanup.init();cleanup.fire();cleanup.ready();cleanup.enable();cleanup.beat();let completed=false;
 const disposing=cleanup.context.rpc.exports.dispose().then(()=>completed=true);await Promise.resolve();assert(!completed,'Disposal must await Unity-thread restoration');
 assert.equal(cleanup.methods.getLatitude.implementation,null,'Other hooks clean up while ball restoration waits');
 cleanup.features.Ball.options.onEvent({kind:'cleanup-pending'});assert(cleanup.events.some(e=>e[0]==='quick-ball'&&e[1].includes('cleanup-pending')));
 cleanup.features.Ball.options.onEvent({kind:'metadata-ready'});assert(!cleanup.features.Ball.ready,'Late metadata cannot restore readiness after dispose');
 cleanup.features.Ball.resolveStop();await disposing;assert(completed);
 const status=fixture();status.init(true);status.fire();status.ready();status.fault.ballStatusThrows=true;
 let pending,done=false;
 assert.doesNotThrow(()=>{pending=status.context.rpc.exports.dispose().then(()=>done=true);});
 await Promise.resolve();assert(!done,'Status failure cannot skip native cleanup wait');
 assert(status.order.includes('original:disconnect'),'Optional status failure cannot skip disconnect');
 assert.equal(status.features.Summary.stopCount,1);assert.equal(status.features.Ball.stopCount,1);
 status.features.Ball.resolveStop();await pending;assert(done);
 console.log('PASS: quick-ball default OFF, metadata readiness, immediate revoke, shared lifecycle coexistence, isolated failure, awaited native restoration, and whitelisted outcome forwarding');
 const scanner=fixture(), forwarded=[];
 scanner.control.setShinyScannerReady=(...a)=>forwarded.push(['ready',...a]);
 scanner.control.setShinyScannerStatus=(...a)=>forwarded.push(['status',...a]);
 scanner.control.shinyScannerFound=(...a)=>forwarded.push(['found',...a]);
 scanner.init();scanner.fire();scanner.ready();
 const scanEvent=scanner.features.Ball.options.onEvent;
 scanEvent({kind:'shiny-ready',available:true});
 scanEvent({kind:'shiny-status',message:'Scanning'});
 scanEvent({kind:'shiny-found',encounterId:'14219003938633576998',species:597,cp:61,latitude:42.65,longitude:23.34});
 assert.deepEqual(forwarded,[['ready',true,'Ready'],['status','Scanning'],['found','14219003938633576998',597,61,42.65,23.34]],'Ball module scanner events reach Java controls');
}
main().catch(e=>{console.error(e);process.exitCode=1;});
