'use strict';
const assert=require('node:assert/strict'),test=require('node:test'),fs=require('node:fs'),vm=require('node:vm');
const source=fs.readFileSync(__dirname+'/../embedded-agent.js','utf8');
function actual(name){const start=source.indexOf('function '+name+'(');assert(start>=0);
 const next=source.indexOf('\nfunction ',start+1),rpc=source.indexOf('\nrpc.exports',start);
 return source.slice(start,next<0?rpc:next);}
function fixture(java=false){
 const calls={created:0,legacy:0,diagnostic:0,interaction:0,heartbeat:0,renew:0,disconnect:0,events:[],errors:[],rollback:0};
 let fresh=false,clock=1000,createError=false,tickError=false;
 const intervals=[];
 const gameplay={};
 gameplay.event=(...args)=>calls.events.push(args);
 const lifecycle={renewLease(){calls.renew++;},disconnect(){calls.disconnect++;},connect(){},
  event(){},active:()=>true,latitude:()=>77,longitude:()=>88,speed:()=>3,bearing:()=>4,
  desiredEnabled:()=>false,observeRealCoordinate:(...args)=>calls.events.push(args)};
 const location={};for(const name of ['getLatitude','getLongitude','getSpeed','getBearing']){
  const method={call:()=>12};location[name]={overload:()=>method,method};}
 const javaBridge={perform:fn=>fn(),scheduleOnMainThread:fn=>fn(),use:name=>name==='dev.pgrazor.Control'?lifecycle:location};
 const context={bootParameters:{compiledNativeTransport:true,compiledJavaBridges:true,translatedRole:'android'},
  javaControl:java?lifecycle:null,control:null,translatedBridge:null,translatedTimer:null,disposed:false,
  hooks:[],locationAdapter:null,timer:null,Java:javaBridge,File:{readAllText:()=>''},Script:{evaluate:()=>javaBridge},
  PGRazorNativeTransport:{create(){calls.created++;if(createError)throw Error('create failure');return{control:gameplay,tick:()=>{if(tickError)throw Error('tick failure');return fresh;}};}},
  PGRazorTranslated:{installCodeValidation(){calls.validation=(calls.validation||0)+1;},client(){calls.legacy++;throw Error('legacy forbidden');},files(){calls.legacy++;throw Error('legacy forbidden');}},
  setInterval:(fn,ms)=>{intervals.push({fn,ms});return intervals.length;},Date:{now:()=>clock},
  scheduleDiagnostic:()=>calls.diagnostic++,scheduleInteraction:()=>calls.interaction++,
  heartbeat:()=>calls.heartbeat++,bootLog:error=>calls.errors.push(error),
  installGoogleBrowserLogin(){},rollback:()=>calls.rollback++};
 vm.createContext(context);vm.runInContext(actual('startGameplayTransport'),context);
 return{context,calls,intervals,gameplay,lifecycle,location,start:()=>context.startGameplayTransport(),
  tick:()=>intervals[0].fn(),fresh:v=>fresh=v,tickError:v=>tickError=v,clock:v=>clock=v,error:()=>createError=true};
}
test('compiled selection owns one 50ms timer and gates research heartbeat on freshness',()=>{
 const x=fixture();x.start();assert.equal(x.calls.created,1);assert.equal(x.calls.legacy,0);
 assert.deepEqual(x.intervals.map(x=>x.ms),[50]);x.tick();
 assert.equal(x.calls.diagnostic,0);assert.equal(x.calls.interaction,0);assert.equal(x.calls.heartbeat,0);
 x.fresh(true);x.tick();
 assert.equal(x.calls.diagnostic,1);assert.equal(x.calls.interaction,1);assert.equal(x.calls.heartbeat,1);
 assert.equal(x.gameplay.renewLease,undefined);assert.equal(x.gameplay.disconnect,undefined);
 x.tick();assert.equal(x.calls.diagnostic,1);assert.equal(x.calls.heartbeat,1);
 x.fresh(false);x.clock(1300);x.tick();assert.equal(x.calls.heartbeat,1);
 x.fresh(true);x.tick();assert.equal(x.calls.diagnostic,1);assert.equal(x.calls.interaction,1);assert.equal(x.calls.heartbeat,2);
});
test('transport tick exceptions report failure and a later fresh poll recovers research',()=>{
 const x=fixture();x.start();x.fresh(true);x.tickError(true);x.tick();
 assert.equal(x.calls.diagnostic,0);assert.equal(x.calls.heartbeat,0);
 assert.match(x.calls.errors[0],/translated-runtime-error: Error: tick failure/);
 x.tickError(false);x.tick();assert.equal(x.calls.diagnostic,1);assert.equal(x.calls.interaction,1);assert.equal(x.calls.heartbeat,1);
 assert.equal(x.calls.created,1);assert.equal(x.calls.legacy,0);
});
test('Java lifecycle stays Java-owned while gameplay methods use compiled snapshot',()=>{
 const x=fixture(true);x.start();assert.equal(x.context.control,x.gameplay);
 x.context.control.renewLease();x.context.control.disconnect();
 assert.equal(x.calls.renew,1);assert.equal(x.calls.disconnect,1);assert.notEqual(x.context.control,x.lifecycle);
});
test('create failure propagates without legacy fallback or timers',()=>{
 const x=fixture();x.error();assert.throws(x.start,/create failure/);assert.equal(x.calls.legacy,0);assert.equal(x.intervals.length,0);
});
test('actual Android start selects compiled transport and captures Java Location control',()=>{
 const x=fixture(true);vm.runInContext(actual('start'),x.context);x.context.start();
 assert.equal(x.calls.rollback,0);assert.equal(x.calls.created,1);assert.deepEqual(x.intervals.map(x=>x.ms),[50]);
 assert.equal(x.calls.diagnostic,0);assert.equal(x.context.control,x.gameplay);
 x.gameplay.latitude=()=>{throw Error('Location must retain Java owner');};
 assert.equal(x.location.getLatitude.method.implementation.call({}),77);
 x.lifecycle.active=()=>false;assert.equal(x.location.getLongitude.method.implementation.call({}),12);
 assert.deepEqual(x.calls.events.at(-1),[false,12]);
});

test('actual translated guest start uses the same compiled transport without Java lifecycle',()=>{
 const x=fixture();x.context.bootParameters.translatedRole='native';
 vm.runInContext(actual('start'),x.context);x.context.start();
 assert.equal(x.calls.validation,1);assert.equal(x.calls.created,1);assert.equal(x.calls.legacy,0);
 assert.deepEqual(x.intervals.map(x=>x.ms),[50]);assert.equal(x.gameplay.renewLease,undefined);
 x.fresh(true);x.tick();assert.equal(x.calls.diagnostic,1);assert.equal(x.calls.interaction,1);
});
