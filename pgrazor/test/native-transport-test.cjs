// Transitional scalar-marshalling boundary only. Feature52 native/JNI behavior
// needs its actual-library and Android fixtures; this VM installs no real hooks.
'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const source=fs.readFileSync(__dirname+'/../runtime/native-transport.js','utf8');
const java=fs.readFileSync(__dirname+'/../src/dev/pgrazor/NativeRuntimeTransport.java','utf8');
const legacy=fs.readFileSync(__dirname+'/../runtime/translated.js','utf8');
const javaArray=name=>{const found=java.match(new RegExp('private static final String\\[\\] '+name+' = \\{([\\s\\S]*?)\\};'));assert.ok(found,name);return JSON.parse('['+found[1]+']');};
function fixture(){
 let address=65536;const blocks=[],functions=new Map(),calls=[],emissions=[];
 let emitCode=0,sampleCode=0,fresh=1,fault=0,commandCalls=0;
 const errors=new Map(),boolValues=new Map();
 class P {
  constructor(n){this.n=n;}add(n){return new P(this.n+n);}isNull(){return !this.n;}
  block(size){const b=blocks.find(x=>this.n>=x.n&&this.n+size<=x.n+x.data.length);assert.ok(b,'fixture memory bounds');return [b.data,this.n-b.n];}
  writeU32(v){const [b,o]=this.block(4);b.writeUInt32LE(v>>>0,o);}readU32(){const [b,o]=this.block(4);return b.readUInt32LE(o);}
  writeS32(v){const [b,o]=this.block(4);b.writeInt32LE(v,o);}readS32(){const [b,o]=this.block(4);return b.readInt32LE(o);}
  writeS64(v){const [b,o]=this.block(8);b.writeBigInt64LE(BigInt(v),o);}readS64(){const [b,o]=this.block(8);return b.readBigInt64LE(o);}
  writePointer(p){const [b,o]=this.block(8);b.writeBigUInt64LE(BigInt(p.n),o);}readPointer(){const [b,o]=this.block(8);return new P(Number(b.readBigUInt64LE(o)));}
  writeDouble(v){const [b,o]=this.block(8);b.writeDoubleLE(v,o);}readDouble(){const [b,o]=this.block(8);return b.readDoubleLE(o);}
  writeBytes(bytes){const [b,o]=this.block(bytes.length);Buffer.from(bytes).copy(b,o);}
  writeByteArray(bytes){const raw=Buffer.from(bytes);this.writeBytes(raw);return this;}
  readByteArray(size){const raw=this.readBytes(size);return raw.buffer.slice(raw.byteOffset,raw.byteOffset+raw.byteLength);}
  readBytes(size){const [b,o]=this.block(size);return Buffer.from(b.subarray(o,o+size));}
  readUtf8String(size){const raw=this.readBytes(size);if(raw.includes(0))throw Error('Frida g_utf8_validate rejects embedded NUL with explicit length');return raw.toString('utf8');}
 }
 const ptr=n=>new P(n),alloc=size=>{const n=address;address+=Math.max(size,1)+16;blocks.push({n,data:Buffer.alloc(size)});return ptr(n);};
 const text=(out,len,value)=>{const bytes=Buffer.from(value,'utf8');out.writeBytes(bytes);len.writeU32(bytes.length);};
 const status=(kind,id)=>errors.get(kind+':'+id)||0;
 const table=alloc(96);
 const install=(index,work)=>{const p=ptr(1000+index);functions.set(p.n,work);table.add(8+index*8).writePointer(p);};
 install(0,()=>assert.fail('Java owns native install'));
 install(1,()=>assert.fail('Java owns native exchange'));
 install(2,(opcode,scalars,ns,strings,nt)=>{
  calls.push('emit');const scalarBytes=scalars.readBytes(ns*8),stringArgs=[];
  for(let i=0;i<nt;i++){const slot=strings.add(i*16),pointer=slot.readPointer(),length=slot.add(8).readU32();assert.equal(slot.add(12).readU32(),0);
   stringArgs.push({length,bytes:pointer.readBytes(length)});}
  emissions.push({opcode,ns,nt,scalarBytes,stringArgs});return emitCode;
 });
 install(3,(id,out)=>{calls.push('bool:'+id);out.writeS32(boolValues.has(id)?boolValues.get(id):id%2);return status('bool',id);});
 install(4,(id,out)=>{calls.push('int:'+id);out.writeS32(-100-id);return status('int',id);});
 install(5,(id,out,present)=>{calls.push('double:'+id);out.writeDouble(id?-0:12.5);present.writeS32(id?0:1);return status('double',id);});
 install(6,(id,out,cap,len)=>{calls.push('string:'+id);assert.equal(cap,1048576);text(out,len,'reader-'+id+'\0\u96ea\ud83d\ude42');return status('string',id);});
 install(7,(cmd,cap,len,token,cap2,len2)=>{calls.push('command');assert.equal(cap,1048576);assert.equal(cap2,1048576);commandCalls++;
  text(cmd,len,'command-'+commandCalls+'\0\u96ea');text(token,len2,'18446744073709551615');return status('command',0);});
 install(8,(id,out)=>{calls.push('pulse:'+id);out.writeS32(id===1?1:0);return status('pulse',id);});
 install(9,out=>{calls.push('sample');out.add(76).writeU32(1-fresh);out.add(80).writeU32(fresh);out.add(84).writeU32(fault);return sampleCode;});
 install(10,()=>assert.fail('Guest local stop must not invoke Java-owned native global stop'));
 const forbidden=()=>assert.fail('scalar adapter must not use JSON/files/timers');
 const context={PGRazorNativeFeature(id,version,size){assert.deepEqual([id,version,size],[52,1,96]);return table;},
  NativeFunction:function(p,ret,args,options){assert.equal(options&&options.scheduling,'exclusive','shared scalar/text scratch requires callback-free native call to retain JS lock');assert.equal(options.exceptions,'propagate');const call=functions.get(p.n);assert.equal(typeof call,'function');return call;},
  Memory:{alloc,allocUtf8String(value){const nul=value.indexOf('\0'),cString=nul<0?value:value.slice(0,nul);const bytes=Buffer.from(cString+'\0','utf8'),p=alloc(bytes.length);p.writeBytes(bytes);return p;}},
  JSON:{parse:forbidden,stringify:forbidden},File:{readAllText:forbidden,writeAllText:forbidden},setInterval:forbidden,setTimeout:forbidden};
 vm.runInNewContext(source,context);
 return {bridge:context.PGRazorNativeTransport.create(),schema:context.PGRazorNativeTransport.schema,calls,emissions,errors,boolValues,
  emitCode:value=>emitCode=value,sample:(isFresh,isFault,rc=0)=>{fresh=isFresh;fault=isFault;sampleCode=rc;}};
}
const f=fixture(),{control}=f.bridge;
for(const group of ['BOOL_READERS','INT_READERS','DOUBLE_READERS','STRING_READERS','PULSE_READERS','ACTIONS'])assert.deepEqual(Array.from(f.schema[group]),javaArray(group),group+' exact Java wire order');
const names=[...f.schema.BOOL_READERS,...f.schema.INT_READERS,...f.schema.DOUBLE_READERS,...f.schema.STRING_READERS,'accountCommandSnapshot'];
const oldReaders=legacy.match(/const readers='([^']+)'/)[1].split(' ').concat('accountCommandSnapshot');
assert.equal(names.length,57);assert.deepEqual([...names].sort(),oldReaders.sort());
assert.deepEqual(Array.from(f.schema.PULSE_READERS),legacy.match(/const pulses='([^']+)'/)[1].split(' '));
assert.deepEqual(Array.from(f.schema.ACTIONS,s=>s.split(':')[0]),legacy.match(/const actions='([^']+)'/)[1].split(' '));
assert.equal(f.schema.PULSE_READERS.length,3);assert.equal(f.schema.ACTIONS.length,59);
for(const name of [...names,...f.schema.PULSE_READERS,...Array.from(f.schema.ACTIONS,s=>s.split(':')[0])])assert.equal(typeof control[name],'function',name);
f.schema.BOOL_READERS.forEach((name,id)=>assert.equal(control[name](),!!(id%2)));
f.schema.INT_READERS.forEach((name,id)=>assert.equal(control[name](),-100-id));
assert.equal(control.latitude(),12.5);assert.ok(Number.isNaN(control.longitude()),'native has_value0 maps unknown coordinate to NaN');
f.schema.STRING_READERS.forEach((name,id)=>assert.equal(control[name](),'reader-'+id+'\0\u96ea\ud83d\ude42'));
let before=f.calls.length;assert.deepEqual(Array.from(control.accountCommandSnapshot()),['command-1\0\u96ea','18446744073709551615']);
assert.deepEqual(f.calls.slice(before),['command'],'command/token pair is one atomic native call');
f.schema.PULSE_READERS.forEach((name,id)=>assert.equal(control[name](),id===1));
console.log('PASS native transport all57 readers/3 pulses/59 schema entries, exact atomic command strings');

// Exercise EVERY action, preserving scalar order/bits and length-prefixed UTF8.
const valid=type=>type==='String'?'value\0\u96ea\ud83d\ude42':type==='boolean'?true:type==='double'?-123.25:type==='long'?Number.MAX_SAFE_INTEGER:-2147483648;
f.schema.ACTIONS.forEach((spec,opcode)=>{
 const [name,signature]=spec.split(':'),types=signature?signature.split(','):[],args=types.map(valid);
 const n=f.emissions.length;control[name](...args);assert.equal(f.emissions.length,n+1);
 const record=f.emissions.at(-1);assert.equal(record.opcode,opcode);assert.equal(record.ns,types.filter(t=>t!=='String').length);assert.equal(record.nt,types.filter(t=>t==='String').length);
 let scalar=0,string=0;
 types.forEach((type,id)=>{const value=args[id];if(type==='String'){
  const bytes=Buffer.from(value,'utf8');assert.equal(record.stringArgs[string].length,bytes.length);assert.deepEqual(record.stringArgs[string++].bytes,bytes);
 }else{if(type==='double')assert.equal(record.scalarBytes.readDoubleLE(scalar*8),value);else assert.equal(record.scalarBytes.readBigInt64LE(scalar*8),BigInt(type==='boolean'?Number(value):value));scalar++;}});
});
control.setEncounterBerry(false,true);assert.equal(f.emissions.at(-1).scalarBytes.readBigInt64LE(0),0n);
control.updateCameraHeadingAt(0,2147483647,false,-Number.MAX_SAFE_INTEGER);assert.equal(f.emissions.at(-1).scalarBytes.readBigInt64LE(24),-9007199254740991n);
control.setSpawnRange(-0,1.25);assert.ok(Object.is(f.emissions.at(-1).scalarBytes.readDoubleLE(0),-0));
console.log('PASS native transport complete action scalar/UTF8/NUL packing and safe64bit limits');

let invalidCase=0;
for(const [name,args] of [
 ['setSpawnReady',[1]],['setSpawnReady',['true']],['setSpawnReady',[]],['setSpawnReady',[true,false]],
 ['event',[null,'ok']],['event',[{},'ok']],['event',['ok',false]],['event',['\ud800','ok']],['event',['ok','\udc00']],
 ['event',['x'.repeat(1048577),'ok']],['event',['\u96ea'.repeat(349526),'ok']],
 ['setSpawnRange',[NaN,1]],['setSpawnRange',[Infinity,1]],['setSpawnRange',['1',2]],
 ['applyMapTap',[1,2,2147483648,0,4]],['applyMapTap',[1,2,-2147483649,0,4]],['applyMapTap',[1,2,1.5,0,4]],
 ['updateCameraHeadingAt',[1,2,true,9007199254740992]],['updateCameraHeadingAt',[1,2,true,1n]],
 ['updateCameraHeadingAt',[1,2,true,'9007199254740991']],
]){const n=f.emissions.length;assert.throws(()=>control[name](...args),undefined,'invalid case '+(invalidCase++)+' '+name);assert.equal(f.emissions.length,n,'invalid '+name+' never reaches native emit');}
for(const code of [-13,-20,-16]){f.emitCode(code);const n=f.emissions.length;assert.throws(()=>control.accountSessionSaved(),new RegExp('rejected:accountSessionSaved:'+code));assert.equal(f.emissions.length,n+1,'one explicit failed emit, no JS retry queue');}
f.emitCode(0);
f.errors.set('int:0',-19);assert.throws(()=>control.battleOptions(),e=>e.code==='PGRAZOR_TRANSLATED_SNAPSHOT_UNAVAILABLE'&&e.message.includes('-19'));f.errors.clear();
f.boolValues.set(3,0);assert.equal(control.active(),false,'native guarded stale false passes through without a JS lease');
f.errors.set('bool:3',-19);assert.throws(()=>control.active(),e=>e.code==='PGRAZOR_TRANSLATED_SNAPSHOT_UNAVAILABLE');f.errors.clear();
f.sample(1,0);assert.equal(f.bridge.fresh(),true);assert.equal(f.bridge.tick(),true);
f.sample(0,0);assert.equal(f.bridge.fresh(),false,'fresh is offset80, not bound offset76');
f.sample(1,1);assert.equal(f.bridge.fresh(),false,'fault offset84 disarms fresh');
f.sample(1,0,-6);assert.equal(f.bridge.fresh(),false);f.sample(1,0);
console.log('PASS native transport invalid-type preflight, explicit backpressure, stale code and sample80/84');
before=f.calls.length;control.connect();control.disconnect();control.renewLease();assert.equal(f.calls.length,before);f.bridge.stop();f.bridge.stop();assert.equal(f.bridge.fresh(),false);assert.equal(f.bridge.tick(),false);
assert.equal(control.takeCameraSampleRequest(),false);control.event('ignored','after local stop');control.accountSessionSaved();
assert.equal(f.calls.length,before,'local stop makes no native global stop/sample/pulse/emit calls');
console.log('PASS native transport local-only idempotent stop; no JSON/files/timers or global transport shutdown');
