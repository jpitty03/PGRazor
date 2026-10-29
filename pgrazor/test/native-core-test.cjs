const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const source=fs.readFileSync(require('node:path').join(__dirname,'../runtime/native-core.js'),'utf8');
function fixture(path){
 const memory=new Map(),calls=[],functions=new Map();let next=1000;
 class Pointer{
  constructor(address){this.address=address;}
  add(n){return new Pointer(this.address+n);}
  isNull(){return !this.address;}
  readU32(){return memory.get(this.address);}
  readPointer(){return memory.get(this.address)||new Pointer(0);}
  readDouble(){return memory.get(this.address)||0;}
  readU64(){return {toString:()=>String(memory.get(this.address)||0)};}
 }
 const alloc=()=>new Pointer(next+=100);
 const callable=fn=>{const p=alloc();functions.set(p.address,fn);return p;};
 function table(version,size,fns=[]){const p=alloc();memory.set(p.address,version);memory.set(p.address+4,size);fns.forEach((fn,i)=>memory.set(p.address+8+i*8,callable(fn)));return p;}
 const feature=table(1,24),v1=table(1,40,[()=>0,()=>0,()=>0,()=>0]);
 const v2=table(2,48,[()=>0,()=>0,()=>0,()=>0,(id,v)=>id===3&&v===1?feature:new Pointer(0)]);
 const factory=callable(v=>{calls.push(['version',v]);return v===1?v1:v===2?v2:new Pointer(0);});
 const exports={dlopen:callable(()=>{calls.push(['load']);return alloc();}),dlsym:callable(()=>factory)};
 const context=vm.createContext({PGRazorNativeCorePath:path,Process:{pointerSize:8},Module:{getGlobalExportByName:n=>exports[n]},
  Memory:{alloc,allocUtf8String:alloc},NativeFunction:function(p){assert.ok(functions.has(p.address));return functions.get(p.address);}});
 vm.runInContext(source,context);return {context,calls,feature,memory,v2};
}
let f=fixture(null);assert.equal(f.context.PGRazorNativeCore(),null);assert.equal(f.context.PGRazorNativeFeature(3,1,24),null);assert.equal(f.calls.length,0);
f=fixture('/test/native.so');assert.equal(f.context.PGRazorNativeFeature(3,1,24),f.feature);
assert.equal(f.context.PGRazorNativeCore().install(null),0);
vm.runInContext(source,f.context); // both bundles may include the loader in the same realm
assert.equal(f.context.PGRazorNativeFeature(3,1,24),f.feature);
assert.equal(f.calls.filter(c=>c[0]==='load').length,1);
assert.throws(()=>f.context.PGRazorNativeFeature(3,1,32),/ABI mismatch/);
assert.throws(()=>f.context.PGRazorNativeFeature(99,1,24),/ABI mismatch/);
f.memory.set(f.v2.address+4,40);assert.throws(()=>f.context.PGRazorNativeFeature(3,1,24),/ABI mismatch/);
console.log('PASS native feature discovery: inert default, shared load, v1 compatibility and ABI rejection');
