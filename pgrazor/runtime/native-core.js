// Transitional adapter: native gameplay stays in the ARM runtime on MuMu.
// Metadata validation and the existing host/guest settings bridge remain intact.
(function(root){
 if(root.PGRazorNativeCore&&root.PGRazorNativeFeature)return;
 let cached=null;
 let loaded=null;
 function load(){
  if(loaded)return loaded;
  if(!root.PGRazorNativeCorePath)return null;
  const trace=root.PGRazorNativeDiagnostic||(()=>{});
  trace('native-core: load begin');
  // Use the guest linker's handle directly. Frida's Module wrapper introspection
  // can stall under MuMu after a new ARM library has loaded.
  const open=new NativeFunction(Module.getGlobalExportByName('dlopen'),'pointer',['pointer','int']);
  const symbol=new NativeFunction(Module.getGlobalExportByName('dlsym'),'pointer',['pointer','pointer']);
  const library=open(Memory.allocUtf8String(root.PGRazorNativeCorePath),2);
  if(library.isNull())throw Error('Native core dlopen failed');
  trace('native-core: load complete');
  const address=symbol(library,Memory.allocUtf8String('pgrazor_native_api'));
  if(address.isNull())throw Error('Native core factory missing');
  trace('native-core: factory '+address);
  const get=new NativeFunction(address,'pointer',['uint']);
  loaded={library,get,trace};
  return loaded;
 }
 function checked(api,version,size){
  if(Process.pointerSize!==8||api.isNull()||api.readU32()!==version||api.add(4).readU32()!==size)
   throw Error('Native core ABI mismatch');
  return api;
 }
 root.PGRazorNativeFeature=function(id,version,size){
  const state=load();if(!state)return null;
  const api=checked(state.get(2),2,48);
  const query=new NativeFunction(api.add(40).readPointer(),'pointer',['uint','uint'],{exceptions:'propagate'});
  return checked(query(id,version),version,size);
 };
 root.PGRazorNativeCore=function(){
  if(cached)return cached;
  const state=load();if(!state)return null;
  const {library,get,trace}=state;
  trace('native-core: factory call begin');
  const api=get(1);
  trace('native-core: API resolved');
  checked(api,1,40);
  const fn=(index,result,args)=>new NativeFunction(api.add(8+index*8).readPointer(),result,args,{exceptions:'propagate'});
  const install=fn(0,'int',['pointer']),update=fn(1,'int',['int','int','uint64']);
  const sample=fn(2,'int',['pointer']),selftest=fn(3,'int',[]);
  if(root.PGRazorNativeCoreTest===true){
   trace('native-core: hook selftest begin');
   const result=selftest();
   trace('native-core: hook selftest result '+result);
   if(result!==0)throw Error('Native hook selftest failed: '+result);
  }
  const snapshot=Memory.alloc(24);
  cached={library,install(address){trace('native-core: spawn install begin');const result=install(address);trace('native-core: spawn install result '+result);return result;},update,sample(){
   if(sample(snapshot)!==0)return null;
   return {original:snapshot.readDouble(),effective:snapshot.add(8).readDouble(),count:snapshot.add(16).readU64().toString()};
  }};
  return cached;
 };
})(globalThis);
