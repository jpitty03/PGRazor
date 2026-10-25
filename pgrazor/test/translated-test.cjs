const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const scope={};vm.runInNewContext(fs.readFileSync('pgrazor/runtime/translated.js','utf8'),scope);
const bridge=scope.PGRazorTranslated, storage={}, calls=[];let time=1000,pulse=false;
const io={read:n=>storage[n],write:(n,v)=>storage[n]=JSON.parse(JSON.stringify(v))};
const control=Object.fromEntries(bridge.readers.map(n=>[n,()=>true]));
for(const n of bridge.actions)control[n]=(...args)=>calls.push([n,args]);
for(const n of ['takeCameraSampleRequest','takeInteractionStopRequest','takeInteractionTraceRequest'])control[n]=()=>{const v=pulse;pulse=false;return v;};
const host=bridge.host({io,control,session:'one',now:()=>time}),client=bridge.client({io,session:'one',now:()=>time});
assert.equal(client.control.active(),false);
host.tick();assert.equal(client.tick(),true);assert.equal(client.control.active(),true);
client.control.setVgpReady(true);client.tick();host.tick();host.tick();assert.equal(calls.length,1);client.tick();
pulse=true;host.tick();client.tick();assert.equal(client.control.takeCameraSampleRequest(),true);assert.equal(client.control.takeCameraSampleRequest(),false);
time+=501;assert.equal(client.control.active(),false);assert.throws(()=>client.control.latitude());
host.tick();client.tick();assert.equal(client.control.active(),true);
storage.events={session:'wrong',seq:2,calls:[['setVgpReady',[false]]]};host.tick();assert.equal(calls.length,1);
storage.events={session:'one',seq:2,calls:[['unapproved',[]]]};host.tick();client.tick();assert.equal(client.control.active(),false);assert.match(storage.state.fault,/Invalid/);
console.log('Translated mailbox tests passed');
// Scanner must fail closed when the MuMu host heartbeat expires, and deliver
// discoveries once even if the host polls the same event batch twice.
{
 let clock=1000;const data={},found=[];
 const channel={read:n=>data[n],write:(n,v)=>data[n]=JSON.parse(JSON.stringify(v))};
 const api=Object.fromEntries(bridge.readers.map(n=>[n,()=>true]));
 for(const n of bridge.actions)api[n]=()=>{};
 for(const n of ['takeCameraSampleRequest','takeInteractionStopRequest','takeInteractionTraceRequest'])api[n]=()=>false;
 api.shinyScannerFound=(...args)=>found.push(args);
 const h=bridge.host({io:channel,control:api,session:'scanner',now:()=>clock});
 const c=bridge.client({io:channel,session:'scanner',now:()=>clock});
 assert.equal(c.control.shinyScannerAllowed(),false);
 h.tick();c.tick();assert.equal(c.control.shinyScannerAllowed(),true);
 c.control.shinyScannerFound('14219003938633576998',597,61,42.65,23.34);
 c.tick();h.tick();h.tick();assert.equal(found.length,1);
 assert.equal(found[0][0],'14219003938633576998');
 clock+=501;assert.equal(c.control.shinyScannerAllowed(),false);
}
// Guest ELF executable validation must reject data pages and altered mappings.
const bytes=Buffer.alloc(4096);bytes.writeUInt32LE(0x464c457f);bytes[4]=2;bytes[5]=1;bytes.writeUInt16LE(183,18);bytes.writeBigUInt64LE(64n,32);bytes.writeUInt16LE(56,54);bytes.writeUInt16LE(1,56);bytes.writeUInt32LE(1,64);bytes.writeUInt32LE(5,68);bytes.writeBigUInt64LE(512n,80);bytes.writeBigUInt64LE(256n,104);
class Pointer {constructor(n){this.n=n;}add(n){return new Pointer(this.n+n);}compare(p){return Math.sign(this.n-p.n);}toString(){return String(this.n);}readU8(){return bytes.readUInt8(this.n);}readU16(){return bytes.readUInt16LE(this.n);}readU32(){return bytes.readUInt32LE(this.n);}readU64(){return {toNumber:()=>Number(bytes.readBigUInt64LE(this.n))};}}
scope.Process={arch:'arm64'};bridge.installCodeValidation();
const mod={base:new Pointer(0),size:4096,path:'/lib/game.so'},range={base:new Pointer(0),size:4096,protection:'r--',file:{path:'/lib/game.so'}};
assert.equal(scope.PGRazorGuestCode(mod,new Pointer(512),32,range),true);
assert.equal(scope.PGRazorGuestCode(mod,new Pointer(256),32,range),false);
assert.equal(scope.PGRazorGuestCode(mod,new Pointer(760),16,range),false);
assert.equal(scope.PGRazorGuestCode(mod,new Pointer(512),32,{...range,protection:'rw-'}),false);
assert.equal(scope.PGRazorGuestCode(mod,new Pointer(512),32,{...range,file:{path:'/other'}}),false);
console.log('Guest executable segment tests passed');
