// Feature 59 (API 1): every camera-probe discover() metadata decision/descriptor is COMPILED.
// This adapter only marshals the validated module/export boundary (the 31 feature47 il2cpp metadata
// exports + the PgrCameraMetaExtra export/presence block + the real module range + the attached
// thread accessor + domain/mode flags) and maps the fixed-versioned output handles and status codes
// back into the exact discover() return shape ({execute, events, vectorFields, sample}, {retry}, or
// {unavailable}). It performs NO metadata validation and NO business logic: a negative native status
// throws with no fallback.
(function(root){
 'use strict';
 // feature47's 31 il2cpp metadata exports in fixed PgrMetaExports slot order.
 // Slots with bit -2 are unused feature47-only dependencies, optional without a presence bit.
 // Slots with bit >= 0 are camera-optional (vector-field, sampleCamera, or continuous gates) and are
 // resolved via opt() so native resolve() can classify their absence with the exact camera-probe reason.
 const META_EXPORTS=[
  ['domain_get',-1],
  ['domain_get_assemblies',-1],
  ['assembly_get_image',-1],
  ['class_from_name',-1],
  ['class_get_type',-2],
  ['type_get_name',-1],
  ['free',-1],
  ['class_get_field_from_name',0],
  ['field_get_parent',2],
  ['field_get_flags',3],
  ['field_get_offset',-2],
  ['field_get_type',1],
  ['class_from_type',-2],
  ['class_is_valuetype',13],
  ['class_is_enum',-2],
  ['class_enum_basetype',-2],
  ['class_value_size',14],
  ['class_get_method_from_name',-1],
  ['method_get_class',-1],
  ['method_get_name',-1],
  ['method_get_flags',-1],
  ['method_get_param_count',-1],
  ['method_get_param',18],
  ['method_get_return_type',-1],
  ['method_is_generic',16],
  ['method_is_inflated',17],
  ['debug_get_method_info',15],
  ['class_is_assignable_from',12],
  ['class_get_methods',-2],
  ['class_get_nested_types',-2],
  ['class_get_declaring_type',-2]
 ];
 // Optional exports in PgrCameraMetaExtra: [name, offsetInExtra, bitIndex].
 const EXTRA_OPT_EXPORTS=[
  ['get_corlib',16,11],
  ['runtime_invoke',24,4],
  ['object_get_class',32,5],
  ['object_unbox',40,6],
  ['gchandle_new',48,7],
  ['gchandle_get_target',56,8],
  ['gchandle_free',64,9],
  ['field_get_value',72,10],
  ['string_new',80,19]
 ];
 // NativeFunction signatures bound onto `api` when sampleCamera / continuous succeed.
 const SAMPLE_API_SIGS=[
  ['runtime_invoke','pointer',['pointer','pointer','pointer','pointer']],
  ['object_get_class','pointer',['pointer']],
  ['object_unbox','pointer',['pointer']],
  ['gchandle_new','pointer',['pointer','bool']],
  ['gchandle_get_target','pointer',['pointer']],
  ['gchandle_free','void',['pointer']],
  ['field_get_value','void',['pointer','pointer','pointer']],
  ['get_corlib','pointer',[]],
  ['class_is_assignable_from','bool',['pointer','pointer']],
  ['class_is_valuetype','bool',['pointer']],
  ['class_value_size','int',['pointer','pointer']]
 ];
 const MAP_API_SIGS=[
  ['debug_get_method_info','bool',['pointer','pointer']],
  ['method_is_generic','bool',['pointer']],
  ['method_is_inflated','bool',['pointer']],
  ['method_get_param','pointer',['pointer','uint32']],
  ['string_new','pointer',['pointer']]
 ];
 // Positional schema tables matching camera_metadata.h declaration order.
 const TARGET_ROWS=[
  ['Camera','get_main','UnityEngine.Camera',true,0,0],
  ['Component','get_transform','UnityEngine.Transform',false,1,1],
  ['Transform','get_forward','UnityEngine.Vector3',false,2,2],
  ['UnitySynchronizationContext','ExecuteTasks','System.Void',true,3,3],
  ['Object','GetInstanceID','System.Int32',false,4,4]
 ];
 const VECTOR_STATUS_NAMES=[null,'exports-unavailable','class-unavailable','signature-mismatch','verified'];
 const VECTOR_FIELD_KEYS=['x','y','z'];
 const CONFIG_BYTES=112,EXTRA_BYTES=112,OUTPUT_BYTES=368,REASON_MAX=120;

 function discover({host}){
  if(!root.PGRazorNativeCorePath)return null;
  if(typeof root.PGRazorNativeFeature!=='function')throw Error('native-camera-metadata-loader');
  const table=root.PGRazorNativeFeature(59,1,24);
  if(!table||table.isNull())throw Error('native-camera-metadata-table');
  if(table.add(8).readU32()!==CONFIG_BYTES||table.add(12).readU32()!==OUTPUT_BYTES)throw Error('native-camera-metadata-layout');
  const {Memory,NativeFunction,Process,module,api,domain,sampleCamera,continuous,ptr}=host;
  const exp=name=>{const p=module.findExportByName('il2cpp_'+name);if(!p||p.isNull())throw Error('native-camera-metadata-export:'+name);host.executable(module,p,4);return p;};
  const opt=name=>{const p=module.findExportByName('il2cpp_'+name);if(!p||p.isNull())return null;host.executable(module,p,4);return p;};

  const resolvedPtrs={};
  let present=0;
  const exports=Memory.alloc(META_EXPORTS.length*8);
  META_EXPORTS.forEach(([name,bit],i)=>{
   const p=bit===-1?exp(name):opt(name);
   if(p){
    resolvedPtrs[name]=p;
    if(bit>=0)present|=(1<<bit);
   }
   exports.add(i*8).writePointer(p||ptr(0));
  });

  const extra=Memory.alloc(EXTRA_BYTES);
  extra.writeU32(1);extra.add(4).writeU32(EXTRA_BYTES);
  extra.add(8).writePointer(exp('image_get_name'));
  EXTRA_OPT_EXPORTS.forEach(([name,off,bit])=>{
   const p=opt(name);
   if(p){
    resolvedPtrs[name]=p;
    present|=(1<<bit);
   }
   extra.add(off).writePointer(p||ptr(0));
  });
  extra.add(88).writePointer(ptr(0));
  extra.add(96).writePointer(ptr(0));
  extra.add(104).writeU32(present>>>0);
  extra.add(108).writeU32(0);

  const config=Memory.alloc(CONFIG_BYTES),out=Memory.alloc(OUTPUT_BYTES);
  config.writeU32(1);config.add(4).writeU32(CONFIG_BYTES);
  config.add(8).writeU32(1);config.add(12).writeU32(64);
  config.add(16).writePointer(exports);config.add(24).writePointer(module.base);config.add(32).writeU64(module.size);
  config.add(40).writePointer(ptr(0));config.add(48).writePointer(ptr(0));config.add(56).writePointer(ptr(0));
  config.add(64).writeU32(0);config.add(68).writeU32(0);
  config.add(72).writePointer(exp('thread_current'));
  config.add(80).writeU64(Process.getCurrentThreadId());
  config.add(88).writePointer(domain||ptr(0));
  config.add(96).writePointer(extra);
  config.add(104).writeU32(sampleCamera?1:0);
  config.add(108).writeU32(continuous?1:0);

  const resolveFn=new NativeFunction(table.add(16).readPointer(),'int',['pointer','pointer'],{exceptions:'propagate'});
  const status=resolveFn(config,out);
  if(status<0||(status!==0&&status!==1&&status!==2))throw Error('native-camera-metadata-resolve:'+status);
  if(out.readU32()!==1||out.add(4).readU32()!==OUTPUT_BYTES||(out.add(8).readU32()|0)!==status)throw Error('native-camera-metadata-output');

  const readReason=p=>{let s='';for(let i=0;i<REASON_MAX;i++){const b=p.add(i).readU8();if(!b)break;s+=String.fromCharCode(b);}return s;};
  const reasonText=readReason(out.add(248));
  if(status===1){
   if(!reasonText)throw Error('native-camera-metadata-output');
   return {retry:reasonText};
  }
  if(status===2){
   if(!reasonText)throw Error('native-camera-metadata-output');
   return {unavailable:reasonText};
  }

  const vfCode=out.add(12).readU32();
  const vectorFields=VECTOR_STATUS_NAMES[vfCode];
  const targetCount=out.add(16).readU32();
  const expectedTargets=sampleCamera?5:4;
  if(!vectorFields||targetCount!==expectedTargets)throw Error('native-camera-metadata-output');

  const require=(name,v)=>{if(!v||v.isNull())throw Error('native-camera-metadata-null:'+name);return v;};
  const execute=require('execute',out.add(48).readPointer());
  const debugAddress=out.add(56).readPointer();
  const getMainCameraAddress=out.add(64).readPointer();

  const classHandles=[];
  for(let i=0;i<11;i++)classHandles.push(out.add(72+i*8).readPointer());
  const fieldHandles=[];
  for(let i=0;i<3;i++)fieldHandles.push(out.add(160+i*8).readPointer());
  const methodHandles=[];
  for(let i=0;i<8;i++)methodHandles.push(out.add(184+i*8).readPointer());

  const events=[],methods={},classes={};
  for(let i=0;i<targetCount;i++){
   const [klassName,methodName,returnType,isStatic,cIdx,mIdx]=TARGET_ROWS[i];
   classes[klassName]=require('class:'+klassName,classHandles[cIdx]);
   methods[methodName]=require('method:'+methodName,methodHandles[mIdx]);
   events.push({className:'UnityEngine.'+klassName,name:methodName,parameterCount:0,returnType,isStatic});
  }

  const fields={};
  if(vfCode===4){
   VECTOR_FIELD_KEYS.forEach((k,i)=>{fields[k]=require('field:'+k,fieldHandles[i]);});
  }

  let sample=null;
  if(sampleCamera){
   SAMPLE_API_SIGS.forEach(([name,ret,args])=>{
    const p=require('export:'+name,resolvedPtrs[name]);
    api[name]=new NativeFunction(p,ret,args);
   });
   const vector=require('class:Vector3',classHandles[5]);
   const integer=require('class:Int32',classHandles[6]);
   sample={api,methods,classes,vector,integer,fields,exception:Memory.alloc(8),float:Memory.alloc(4)};
   if(continuous){
    MAP_API_SIGS.forEach(([name,ret,args])=>{
     const p=require('export:'+name,resolvedPtrs[name]);
     api[name]=new NativeFunction(p,ret,args);
    });
    sample.debugAddress=require('debugAddress',debugAddress);
    sample.getMainCameraAddress=require('getMainCameraAddress',getMainCameraAddress);
    methods.get_MainCamera=require('method:get_MainCamera',methodHandles[5]);
    methods.get_cullingMask=require('method:get_cullingMask',methodHandles[6]);
    methods.NameToLayer=require('method:NameToLayer',methodHandles[7]);
    sample.rig=require('class:MainCameraRig',classHandles[7]);
    sample.string=require('class:String',classHandles[9]);
    sample.worldMapName=Memory.allocUtf8String('WorldMap');
   }
  }

  return {execute,events,vectorFields,sample};
 }

 root.PGRazorNativeCameraMetadata={discover,resolve:discover};
})(typeof globalThis!=='undefined'?globalThis:this);
