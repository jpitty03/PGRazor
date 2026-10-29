// PGRazor native migration: CONTINUOUS CAMERA SAMPLER controller (feature id 41).
//
// Thin adapter for pgrazor/native/camera_sampler.{h,cpp}. It owns exactly the continuous branch of
// diagnostics/camera-probe.js sample() (~303-448): the per-getter managed reads (Camera.get_main match,
// the WorldMap culling-mask gate, Transform.forward yaw, Object.GetInstanceID) and the map/yaw decision.
// The host (camera-probe.js) keeps everything else -- the Gum get_MainCamera observer, the invocation-thread
// / demand / depth gates, the throttle, the latest-only queue and the poll/emit -- and simply hands this
// adapter the proven owner tid, the RAW rig receiver + RAW returned camera captured in the getter, and the
// residual lease (already clamped by the host; the native layer re-clamps to 2500 ms). No hook is installed
// and NO managed call is made from JS here: rig/camera cross as raw pointers and only scalars come back.
//
// ABI: PGRazorNativeFeature(41,1,24) -- a version-1, 24-byte table (8-byte header + install/sample).
// install(config)->0 publishes the immutable 216-byte config (8-byte header + 25 pointers + expected_layer
// (13)/reserved0(0)). sample(request,out) reads the 40-byte request (version/size, tid, remaining_ms, raw
// rig, raw camera) and writes the 72-byte output (status, reason, camera_id, mask, layer, reserved, x/y/z,
// yaw, time_ms). It returns 1 sample / 0 unavailable (transient) / negative fault.
//
// This adapter maps the native output back to the legacy JS reading shapes: a 1 -> the success record
// {x,y,z,yawDegrees,cameraId,timeMs,tid,mapVisible:true,cameraCullingMask,worldMapLayer,mapGate}; a 0 -> the
// unavailable record {unavailable:true,reason,timeMs,tid,mapVisible:false} (plus the mask/layer/mapGate map
// facts only on map-hidden). A negative fault THROWS, so the host latches it exactly like the legacy
// sampleError (there is no fallback to JS managed reads once the native path is selected).
//
// create({...}) returns null ONLY when the native pilot/table is absent (the host keeps the inline JS
// sampler). A present pilot with a missing loader/table throws, and missing/inert metadata or a non-zero
// install throws -- the coordinator disables the native path rather than mixing a partial install with it.
(function(root){
 root.PGRazorNativeCameraSampler={create({state,found,module,Memory,NativeFunction,ptr}){
  if(typeof root.PGRazorNativeFeature!=='function'){if(root.PGRazorNativeCorePath)throw Error('native-camera-sampler-loader');return null;}
  const table=root.PGRazorNativeFeature(41,1,24);if(!table||table.isNull()){if(root.PGRazorNativeCorePath)throw Error('native-camera-sampler-table');return null;}

  const nn=x=>!!x&&(typeof x.isNull!=='function'||!x.isNull());
  const s=found&&found.sample;
  if(!s||!module)throw Error('native-camera-sampler-metadata');
  const classes=s.classes||{},methods=s.methods||{},fields=s.fields||{};

  const exp=n=>{const a=module.findExportByName('il2cpp_'+n);return a&&!a.isNull()?a:null;};
  // Flat pointer array in EXACT PgrCameraSamplerConfig order (camera_sampler.h). Every one is required; the
  // native layer re-checks the header and rejects any null, so a null here fails closed (throws).
  const pointers=[
   exp('runtime_invoke'),exp('object_get_class'),exp('class_is_assignable_from'),exp('field_get_value'),exp('object_unbox'),
   exp('gchandle_new'),exp('gchandle_get_target'),exp('gchandle_free'),exp('thread_current'),exp('string_new'),
   classes.Camera,classes.Transform,s.vector,s.rig,s.integer,s.string,
   methods.get_main,methods.get_transform,methods.get_forward,methods.NameToLayer,methods.get_cullingMask,methods.GetInstanceID,
   fields.x,fields.y,fields.z];
  if(pointers.length!==25)throw Error('native-camera-sampler-config-shape:'+pointers.length);
  // Strongly retain the config buffer for the process lifetime (install copies it, but keep a ref).
  const config=Memory.alloc(216);config.writeU32(1);config.add(4).writeU32(216);
  pointers.forEach((v,i)=>{
   if(!nn(v))throw Error('native-camera-sampler-config-null:'+i);
   config.add(8+i*8).writePointer(v);
  });
  config.add(208).writeS32(13);config.add(212).writeS32(0);

  const fn=(i,ret,args)=>new NativeFunction(table.add(8+i*8).readPointer(),ret,args,{exceptions:'propagate'});
  const installFn=fn(0,'int',['pointer']),sampleFn=fn(1,'int',['pointer','pointer']);

  const rc=installFn(config);if(rc!==0)throw Error('native-camera-sampler-install:'+rc);

  // reason code (0 refusals) -> legacy event reason text.
  const REASON={2:'off',3:'lease-expired',4:'owner-thread-unavailable',5:'non-map-rig',
   6:'camera-getter-returned-null',7:'camera-mismatch',8:'camera-transform-unavailable',9:'map-hidden'};
  // reason code (negative faults) -> fault label carried on the thrown error.
  const FAULT={10:'clock',11:'gc-handle',12:'managed-getter-exception',13:'runtime-class-mismatch',
   14:'world-map-layer-mismatch',15:'nonfinite-forward-vector',16:'vertical-forward-vector',17:'reentrant'};

  function sample(tid,rig,camera,remaining){
   const req=Memory.alloc(40);req.writeU32(1);req.add(4).writeU32(40);
   req.add(8).writeU64(tid);req.add(16).writeU64(Math.max(0,Math.floor(remaining)||0));
   req.add(24).writePointer(rig||ptr(0));req.add(32).writePointer(camera||ptr(0));
   const out=Memory.alloc(72);out.writeU32(1);out.add(4).writeU32(72);

   const status=sampleFn(req,out);
   // A negative fault latches on the host (consistent with the legacy sampleError); never a reading.
   if(status<0){
    const reason=FAULT[out.add(12).readU32()]||('native-sampler-error:'+status);
    const err=Error('native-camera-sampler-fault:'+reason);err.cameraProbeReason=reason;throw err;
   }
   // Well-formed output header + status/return agreement (fail closed on corruption).
   if((status!==0&&status!==1)||out.readU32()!==1||out.add(4).readU32()!==72||out.add(28).readS32()!==0||out.add(8).readS32()!==status){
    const err=Error('native-camera-sampler-output');err.cameraProbeReason='native-sampler-output';throw err;
   }
   const timeMs=out.add(64).readDouble();
   if(!Number.isFinite(timeMs)||timeMs<0||timeMs>Number.MAX_SAFE_INTEGER)throw Error('native-camera-sampler-time');
   if(status===0){
    const code=out.add(12).readU32(),reason=REASON[code]||('native:'+code);
    const record={unavailable:true,reason,timeMs,tid,mapVisible:false};
    if(code===9){ // map-hidden carries the facts that justified the invalidation
     record.cameraCullingMask=out.add(20).readU32();record.worldMapLayer=out.add(24).readS32();
     record.mapGate='world-map-layer-visible';
    }
    return record;
   }
   // status 1: a fresh world-map reading.
   const record={x:out.add(32).readDouble(),y:out.add(40).readDouble(),z:out.add(48).readDouble(),
    yawDegrees:out.add(56).readDouble(),cameraId:out.add(16).readS32(),timeMs,tid,mapVisible:true,
    cameraCullingMask:out.add(20).readU32(),worldMapLayer:out.add(24).readS32(),mapGate:'world-map-layer-visible'};
   if(![record.x,record.y,record.z,record.yawDegrees].every(Number.isFinite)||record.yawDegrees<0||record.yawDegrees>=360||Math.hypot(record.x,record.z)<0.000001||record.worldMapLayer!==13||(record.cameraCullingMask&8192)===0)throw Error('native-camera-sampler-reading');
   return record;
  }

  // Keep the config buffer referenced so it is never collected while the module is installed.
  return {sample,_config:config,_state:state};
 }};
})(globalThis);
