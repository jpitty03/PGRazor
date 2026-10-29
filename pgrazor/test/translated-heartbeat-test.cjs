"use strict";
const assert=require('node:assert/strict'),test=require('node:test'),fs=require('node:fs'),vm=require('node:vm');
const agent=fs.readFileSync(__dirname+'/../embedded-agent.js','utf8');
function actual(name){
 const start=agent.indexOf('function '+name+'(');assert.ok(start>=0,'actual '+name+' exists');
 const next=agent.indexOf('\nfunction ',start+1);return agent.slice(start,next<0?agent.length:next);
}
test('research heartbeat retains camera and interaction requests',()=>{
 const calls=[];const c={bootParameters:{cameraSample:true,interactionProbe:true},diagnosticTimer:1,interactionTimer:2,
  Java:{performNow:f=>f()},control:{renewLease(){calls.push('lease');},takeCameraSampleRequest:()=>true,
   takeInteractionStopRequest:()=>true,takeInteractionTraceRequest:()=>true},
  clearTimeout:id=>calls.push(id),setImmediate:f=>f(),runDiagnostic:()=>calls.push('camera'),
  stopInteraction:()=>calls.push('stop'),runInteraction:trace=>calls.push(trace?'trace':'inspect')};
 vm.runInNewContext(actual('heartbeat'),c);c.heartbeat();
 assert.deepEqual(calls,['lease',1,'camera','stop',2,'trace']);
 assert.equal(c.diagnosticTimer,null);assert.equal(c.interactionTimer,null);
});
test('research transport waits for a fresh snapshot and schedules research only once',()=>{
 let fresh=false,tick,now=1000;const calls=[];
 const control={renewLease:()=>calls.push('lease'),event:()=>calls.push('ready')};
 const bridge={control,tick:()=>fresh};
 const c={bootParameters:{compiledNativeTransport:true},javaControl:null,
  PGRazorNativeTransport:{create:()=>bridge},setInterval:f=>(tick=f,1),Date:{now:()=>now},
  scheduleDiagnostic:()=>calls.push('camera'),scheduleInteraction:()=>calls.push('interaction'),
  heartbeat:()=>calls.push('heartbeat'),bootLog:e=>{throw Error(e);}};
 vm.runInNewContext(actual('startGameplayTransport'),c);c.startGameplayTransport();
 tick();assert.deepEqual(calls,[]);fresh=true;tick();
 assert.deepEqual(calls,['ready','camera','interaction','heartbeat']);
 fresh=false;now+=500;tick();assert.equal(calls.length,4);
 fresh=true;tick();assert.deepEqual(calls,['ready','camera','interaction','heartbeat','heartbeat']);
});
test('retired gameplay cannot be loaded or scheduled by the research agent',()=>{
 assert.doesNotMatch(agent,/quickSummary|quickBall|quickCatchWait|PGRazorNativeCatchWait|syncTransferPolicy|syncBerryPolicy/);
});
