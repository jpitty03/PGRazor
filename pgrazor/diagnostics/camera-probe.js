// Default: passive Unity invocation-thread observation. sampleCamera:true opts
// into bounded read-only getters on an observed Unity execution point.
// Loading defines an API and performs no native calls.
(function(root) {
    'use strict';
    const LIMITS=Object.freeze({attempts:60,retryMs:1000,prepareMs:60000,assemblies:128,
        scanMs:3000,nameBytes:256,observeMs:12000,pollMs:250,threads:8,calls:200000,
        sampleMs:10000,sampleIntervalMs:500,samples:20});
    const TARGETS=[['Camera','get_main','UnityEngine.Camera',true],
        ['Component','get_transform','UnityEngine.Transform',false],
        ['Transform','get_forward','UnityEngine.Vector3',false],
        ['UnitySynchronizationContext','ExecuteTasks','System.Void',true]];
    const SIGNATURES={domain_get:['pointer',[]],domain_get_assemblies:['pointer',['pointer','pointer']],
        assembly_get_image:['pointer',['pointer']],image_get_name:['pointer',['pointer']],
        class_from_name:['pointer',['pointer','pointer','pointer']],
        class_get_method_from_name:['pointer',['pointer','pointer','int']],
        method_get_class:['pointer',['pointer']],method_get_name:['pointer',['pointer']],
        method_get_param_count:['uint32',['pointer']],method_get_flags:['uint32',['pointer','pointer']],
        method_get_return_type:['pointer',['pointer']],type_get_name:['pointer',['pointer']],free:['void',['pointer']],
        thread_current:['pointer',[]],thread_attach:['pointer',['pointer']],thread_detach:['void',['pointer']]};
    let running=null;
    // One continuous filter per script/process. Keep code and callback ownership
    // until script teardown, even after detaching an invocation still in flight.
    const nativeFilters=[];
    // Shipped ARM64 debug-info ABI, verified by the bounded scene probe.
    const DEBUG_INFO_WORDS=[0x90051008,0xf9474508,0xb4000148,0x90051009,0xf9474929,
        0xb40000e9,0xf940040a,0xeb0a011f,0x2a1f03e8,0x54000128,0xeb0a013f,
        0x540000e3,0xb40000a1,0xf9400408,0xb900083f,0xf900083f,0xf9000028,
        0x52800028,0x2a0803e0,0xd65f03c0];
    const FILTER_SOURCE=`
#include <gum/guminterceptor.h>
#include <glib.h>
extern gpointer execute_method;
extern int enter_match(unsigned int tid);
extern void leave_match(unsigned int tid);
extern int pgrazor_clock_gettime(int clock_id, void *result);
/* Android ARM64 uses two signed 64-bit longs for struct timespec. */
typedef struct { long seconds; long nanoseconds; } PGRazorTimespec;
typedef char PGRazorTimespecSizeCheck[(sizeof(long)==8 && sizeof(PGRazorTimespec)==16)?1:-1];
typedef struct { int accepted; } Match;
void on_enter(GumInvocationContext *ic) {
    Match *m=GUM_IC_GET_INVOCATION_DATA(ic,Match);
    m->accepted=0;
    if(gum_invocation_context_get_nth_argument(ic,0)==execute_method)
        m->accepted=enter_match(gum_invocation_context_get_thread_id(ic));
}
void on_leave(GumInvocationContext *ic) {
    Match *m=GUM_IC_GET_INVOCATION_DATA(ic,Match);
    if(m->accepted)leave_match(gum_invocation_context_get_thread_id(ic));
}
double monotonic_ms(void) {
    PGRazorTimespec time;
    /* CLOCK_BOOTTIME includes suspend, matching Android elapsedRealtime(). */
    if(pgrazor_clock_gettime(7,&time)!=0 || time.seconds<0 || time.nanoseconds<0 || time.nanoseconds>=1000000000)
        return -1;
    return (double)time.seconds*1000.0+(double)time.nanoseconds/1000000.0;
}
`;
    function text(p) {
        if(p.isNull())throw Error('Null metadata string');
        let n=0;while(n<LIMITS.nameBytes && p.add(n).readU8()!==0)n++;
        if(n===LIMITS.nameBytes)throw Error('Metadata string exceeds bound');
        return n?p.readUtf8String(n):'';
    }
    function bind(module) {
        const api={};
        for(const key of Object.keys(SIGNATURES)) {
            const address=module.findExportByName('il2cpp_'+key);
            if(!address || address.isNull())throw Error('Missing export il2cpp_'+key);
            api[key]=new NativeFunction(address,...SIGNATURES[key]);
        }
        return api;
    }
    function typeName(api,type) {
        if(type.isNull())throw Error('Null metadata type');
        const name=api.type_get_name(type);
        if(name.isNull())throw Error('Null allocated type name');
        try{return text(name);}finally{api.free(name);}
    }
    function pointerHandleAbi(module) {
        // Narrow fingerprint of the shipped 6000.3.22f1 ARM64 implementation.
        // No GC handle function is called until this pointer ABI is verified.
        // See build/feature-research/GC-HANDLE-ABI.txt; unknown code fails closed.
        function readableCode(address,length) {
            if(address.compare(module.base)<0 || address.add(length).compare(module.base.add(module.size))>0)return false;
            const range=Process.findRangeByAddress(address);
            return range!==null && range.protection.includes('r') && (range.protection.includes('x')||(typeof PGRazorGuestCode==='function'&&PGRazorGuestCode(module,address,length,range))) &&
                address.add(length).compare(range.base.add(range.size))<=0;
        }
        function words(address,expected) {
            return readableCode(address,expected.length*4) && expected.every((word,i)=>address.add(i*4).readU32()===word);
        }
        function branch(address) {
            if(!readableCode(address,4))return null;
            const word=address.readU32();
            if((word>>>26)!==5)return null; // AArch64 B imm26, never BL or an indirect target.
            let immediate=word&0x03ffffff;if(immediate&0x02000000)immediate-=0x04000000;
            const target=address.add(immediate*4);
            return readableCode(target,4)?target:null;
        }
        try {
            const create=module.findExportByName('il2cpp_gchandle_new'),get=module.findExportByName('il2cpp_gchandle_get_target'),free=module.findExportByName('il2cpp_gchandle_free');
            if(!create || !get || !free || create.isNull() || get.isNull() || free.isNull())return false;
            if(!words(create,[0xaa0003e8,0x52800049,0x7200003f,0x1a890520,0xaa0803e1]))return false;
            const createBody=branch(create.add(20)),getBody=branch(get),freeBody=branch(free);
            return createBody!==null && getBody!==null && freeBody!==null &&
                words(createBody.add(0x384),[0x9a953120]) && // csel x0,x9,x21,lo: full-width return
                words(getBody,[0xf81b0ffe]) && words(getBody.add(20),[0x9273c817,0x394082e8]) &&
                words(freeBody,[0xf81b0ffe]) && words(freeBody.add(24),[0x9273c816,0x394082c8]);
        }catch(_){return false;}
    }
    function emit(state,kind,fields) {
        const event=Object.assign({time:new Date().toISOString(),kind},fields||{});
        if(!(state.continuous && kind==='camera-sample'))
            try{console.log('[pgrazor-camera-probe] '+JSON.stringify(event));}catch(_){}
        if(typeof state.onEvent==='function')try{state.onEvent(event);}catch(_){}
    }
    function executable(module,address,size) {
        if(!address || address.isNull() || !address.and(3).isNull() || address.compare(module.base)<0 || address.add(size).compare(module.base.add(module.size))>0)throw Error('invalid-code-range');
        const range=Process.findRangeByAddress(address);
        if(!range || range.protection.indexOf('r')<0 || !(range.protection.includes('x')||(typeof PGRazorGuestCode==='function'&&PGRazorGuestCode(module,address,size,range))) || address.add(size).compare(range.base.add(range.size))<=0===false)throw Error('code-not-rx');
    }
    function discover(module,api,domain,sampleCamera,continuous) {
        let owned=null;
        try {
            if(api.thread_current().isNull()) {
                owned=api.thread_attach(domain);
                if(owned.isNull())throw Error('IL2CPP thread attachment returned null');
            }
            if(root.PGRazorNativeCorePath) {
                if(!root.PGRazorNativeCameraMetadata || typeof root.PGRazorNativeCameraMetadata.discover!=='function')
                    throw Error('native-camera-metadata-missing');
                return root.PGRazorNativeCameraMetadata.discover({host:{Memory,NativeFunction,Process,module,api,domain,sampleCamera,continuous,ptr,executable}});
            }
            const count=Memory.alloc(8);count.writeU64(0);
            const assemblies=api.domain_get_assemblies(domain,count),total=count.readU64();
            if(total.compare(0)===0)return {retry:'assemblies-not-ready'};
            if(assemblies.isNull())throw Error('Null assembly list');
            const limit=total.compare(LIMITS.assemblies)>0?LIMITS.assemblies:total.toNumber();
            const deadline=Date.now()+LIMITS.scanMs;
            let image=null,gameImage=null;
            for(let i=0;i<limit;i++) {
                if(Date.now()>=deadline)return {unavailable:'metadata-scan-budget'};
                const assembly=assemblies.add(i*8).readPointer();if(assembly.isNull())continue;
                const candidate=api.assembly_get_image(assembly);if(candidate.isNull())continue;
                const name=text(api.image_get_name(candidate));
                if(name==='UnityEngine.CoreModule.dll')image=candidate;
                if(continuous && name==='holo-game.dll')gameImage=candidate;
                if(image && (!continuous || gameImage))break;
            }
            if(!image)return {retry:'core-module-not-found-within-bound'};
            if(continuous && !gameImage)return {retry:'holo-game-not-found-within-bound'};
            const ns=Memory.allocUtf8String('UnityEngine'),events=[],methods={},classes={};let execute=null;
            const targets=sampleCamera?TARGETS.concat([['Object','GetInstanceID','System.Int32',false]]):TARGETS;
            for(const [klassName,methodName,returns,isStatic] of targets) {
                const klass=api.class_from_name(image,ns,Memory.allocUtf8String(klassName));
                if(klass.isNull())return {unavailable:'missing-class:'+klassName};
                const method=api.class_get_method_from_name(klass,Memory.allocUtf8String(methodName),0);
                if(method.isNull())return {unavailable:'missing-method:'+klassName+'.'+methodName};
                const impl=Memory.alloc(4);impl.writeU32(0);
                const flags=api.method_get_flags(method,impl),actualType=typeName(api,api.method_get_return_type(method));
                if(!api.method_get_class(method).equals(klass) || text(api.method_get_name(method))!==methodName ||
                    api.method_get_param_count(method)!==0 || Boolean(flags&0x10)!==isStatic || actualType!==returns)
                    return {unavailable:'signature-mismatch:'+klassName+'.'+methodName};
                events.push({className:'UnityEngine.'+klassName,name:methodName,parameterCount:0,returnType:actualType,isStatic});
                methods[methodName]=method;classes[klassName]=klass;
                if(methodName==='ExecuteTasks')execute=method;
            }
            // Vector field checks are preparatory only and never read object values.
            const optional={class_get_field_from_name:['pointer',['pointer','pointer']],field_get_type:['pointer',['pointer']],
                field_get_parent:['pointer',['pointer']],field_get_flags:['int',['pointer']]};
            const fieldApi={},fields={};let fieldStatus='exports-unavailable',vector=null;
            for(const key of Object.keys(optional)) {
                const address=module.findExportByName('il2cpp_'+key);
                if(!address || address.isNull())break;
                fieldApi[key]=new NativeFunction(address,...optional[key]);
            }
            if(Object.keys(fieldApi).length===Object.keys(optional).length) {
                vector=api.class_from_name(image,ns,Memory.allocUtf8String('Vector3'));
                fieldStatus='verified';
                if(vector.isNull())fieldStatus='class-unavailable';
                else for(const key of ['x','y','z']) {
                    const field=fieldApi.class_get_field_from_name(vector,Memory.allocUtf8String(key));
                    if(field.isNull() || !fieldApi.field_get_parent(field).equals(vector) || (fieldApi.field_get_flags(field)&0x10) ||
                        typeName(api,fieldApi.field_get_type(field))!=='System.Single'){fieldStatus='signature-mismatch';break;}
                    fields[key]=field;
                }
            }
            let sample=null;
            if(sampleCamera) {
                if(fieldStatus!=='verified')return {unavailable:'vector-fields-'+fieldStatus};
                if(!pointerHandleAbi(module))return {unavailable:'gc-handle-abi-unverified'};
                const extra={runtime_invoke:['pointer',['pointer','pointer','pointer','pointer']],
                    object_get_class:['pointer',['pointer']],object_unbox:['pointer',['pointer']],
                    gchandle_new:['pointer',['pointer','bool']],gchandle_get_target:['pointer',['pointer']],gchandle_free:['void',['pointer']],
                    field_get_value:['void',['pointer','pointer','pointer']],get_corlib:['pointer',[]],
                    class_is_assignable_from:['bool',['pointer','pointer']],class_is_valuetype:['bool',['pointer']],
                    class_value_size:['int',['pointer','pointer']]};
                for(const key of Object.keys(extra)) {
                    const address=module.findExportByName('il2cpp_'+key);
                    if(!address || address.isNull())return {unavailable:'sampling-export-missing:'+key};
                    api[key]=new NativeFunction(address,...extra[key]);
                }
                const corlib=api.get_corlib();if(corlib.isNull())return {unavailable:'corlib-unavailable'};
                const integer=api.class_from_name(corlib,Memory.allocUtf8String('System'),Memory.allocUtf8String('Int32'));
                const alignment=Memory.alloc(4);alignment.writeU32(0);
                if(integer.isNull() || !api.class_is_valuetype(integer) || api.class_value_size(integer,alignment)!==4 ||
                    !api.class_is_valuetype(vector))return {unavailable:'boxed-value-type-mismatch'};
                sample={api,methods,classes,vector,integer,fields,exception:Memory.alloc(8),float:Memory.alloc(4)};
                if(continuous) {
                    const rig=api.class_from_name(gameImage,Memory.allocUtf8String('Niantic.Holoholo.CameraRig'),Memory.allocUtf8String('MainCameraRig'));
                    const stack=api.class_from_name(gameImage,Memory.allocUtf8String('Niantic.Holoholo.Cameras'),Memory.allocUtf8String('CameraStack'));
                    const string=api.class_from_name(corlib,Memory.allocUtf8String('System'),Memory.allocUtf8String('String'));
                    const layerMask=api.class_from_name(image,ns,Memory.allocUtf8String('LayerMask'));
                    if(rig.isNull() || stack.isNull() || string.isNull() || layerMask.isNull() || !api.class_is_assignable_from(stack,rig) || api.class_is_valuetype(string))
                        return {unavailable:'map-gate-class-mismatch'};
                    for(const [key,result,args] of [
                        ['debug_get_method_info','bool',['pointer','pointer']],
                        ['method_is_generic','bool',['pointer']],['method_is_inflated','bool',['pointer']],
                        ['method_get_param','pointer',['pointer','uint32']],['string_new','pointer',['pointer']]]) {
                        const address=module.findExportByName('il2cpp_'+key);
                        if(!address || address.isNull())return {unavailable:'map-gate-export-missing:'+key};
                        api[key]=new NativeFunction(address,result,args);
                        if(key==='debug_get_method_info')sample.debugAddress=address;
                    }
                    function executable(address,length) {
                        if(address.isNull() || address.compare(module.base)<0 || address.add(length).compare(module.base.add(module.size))>0)return false;
                        const range=Process.findRangeByAddress(address);
                        return !!range && range.protection.includes('r') && (range.protection.includes('x')||(typeof PGRazorGuestCode==='function'&&PGRazorGuestCode(module,address,length,range))) && address.add(length).compare(range.base.add(range.size))<=0;
                    }
                    if(!executable(sample.debugAddress,80) || DEBUG_INFO_WORDS.some((word,i)=>sample.debugAddress.add(i*4).readU32()!==word))
                        return {unavailable:'map-debug-info-abi-unverified'};
                    const resolved=[];
                    for(const [owner,key,returns,rva] of [[stack,'get_MainCamera','UnityEngine.Camera',0xae09554]]) {
                        const method=api.class_get_method_from_name(owner,Memory.allocUtf8String(key),0);
                        if(method.isNull())return {unavailable:'map-gate-method-missing:'+key};
                        const impl=Memory.alloc(4);impl.writeU32(0);
                        if(!api.method_get_class(method).equals(owner) || text(api.method_get_name(method))!==key ||
                            api.method_get_param_count(method)!==0 || (api.method_get_flags(method,impl)&0x10) ||
                            typeName(api,api.method_get_return_type(method))!==returns || api.method_is_generic(method) || api.method_is_inflated(method))
                            return {unavailable:'map-gate-signature-mismatch:'+key};
                        const out=Memory.alloc(24);out.writeByteArray(new Uint8Array(24));
                        if(!api.debug_get_method_info(method,out))return {unavailable:'map-gate-code-unavailable:'+key};
                        const address=out.readPointer();
                        if(!executable(address,8) || !address.and(3).isNull() || !address.equals(module.base.add(rva)) || resolved.some(p=>p.equals(address)))
                            return {unavailable:'map-gate-code-mismatch:'+key};
                        resolved.push(address);methods[key]=method;
                        if(key==='get_MainCamera') {
                            if(address.readU32()!==0xf9400c00 || address.add(4).readU32()!==0xd65f03c0)
                                return {unavailable:'map-camera-getter-code-unverified'};
                            sample.getMainCameraAddress=address;
                        }
                    }
                    for(const [owner,key,isStatic,parameters] of [[classes.Camera,'get_cullingMask',false,[]],[layerMask,'NameToLayer',true,['System.String']]]) {
                        const method=api.class_get_method_from_name(owner,Memory.allocUtf8String(key),parameters.length);
                        if(method.isNull())return {unavailable:'map-gate-method-missing:'+key};
                        const impl=Memory.alloc(4);impl.writeU32(0);
                        if(!api.method_get_class(method).equals(owner) || text(api.method_get_name(method))!==key ||
                            api.method_get_param_count(method)!==parameters.length || Boolean(api.method_get_flags(method,impl)&0x10)!==isStatic ||
                            typeName(api,api.method_get_return_type(method))!=='System.Int32' || api.method_is_generic(method) || api.method_is_inflated(method) ||
                            parameters.some((type,index)=>typeName(api,api.method_get_param(method,index))!==type))
                            return {unavailable:'map-gate-signature-mismatch:'+key};
                        methods[key]=method;
                    }
                    sample.rig=rig;sample.string=string;sample.worldMapName=Memory.allocUtf8String('WorldMap');
                }
            }
            return {execute,events,vectorFields:fieldStatus,sample};
        } finally {if(owned!==null && !owned.isNull())api.thread_detach(owned);}
    }
    function start(options) {
        if(!options || options.enabled!==true)return Promise.resolve({status:'disabled'});
        if(running)return Promise.resolve({status:'already-running'});
        if(options.continuous===true && options.sampleCamera!==true)return Promise.resolve({status:'unavailable',reason:'continuous-requires-camera-sampling'});
        if(options.continuous===true && nativeFilters.length)return Promise.resolve({status:'unavailable',reason:'continuous-restart-required'});
        const completion=new Promise(resolve=>{
            const state={active:true,timer:null,onEvent:options.onEvent,listeners:[],depth:new Map(),threads:new Map(),
                calls:0,rawMatches:0,matches:0,overflow:false,attempts:0,deadline:Date.now()+LIMITS.prepareMs,finish:null,
                sampleCamera:options.sampleCamera===true,sampling:false,sampleError:null,sampleQueue:[],sampleCount:0,
                sampleStartedAt:null,lastSampleAt:null,sampleDone:false,sampleTid:null,stopping:false,pendingFinish:null,
                sampleAttempts:0,sampleFailure:null,continuous:options.continuous===true,
                sampleRequested:false,clock:()=>Date.now(),lastClock:null,pendingUnavailable:null,unavailableReported:false,getterDepth:0,worldMapLayer:null};
            running=state;
            state.finish=result=>{
                if(!state.active)return;
                // Cooperative NativeFunction calls may release the JS lock. Do not
                // announce completion or release listeners before owned GC handles
                // have been released by a currently executing sample's finally.
                if(state.sampling) {
                    state.stopping=true;state.pendingFinish=state.pendingFinish||result;
                    if(state.timer!==null)clearTimeout(state.timer);
                    return;
                }
                state.active=false;
                if(state.timer!==null)clearTimeout(state.timer);
                const errors=[];
                for(const listener of state.listeners)try{listener.detach();}catch(e){errors.push(String(e));}
                state.listeners=[];state.depth.clear();running=null;
                if(errors.length)result={status:'error',reason:'listener-cleanup-failed',errors};
                // stop() can be called from application code; terminal sink stays on a timer.
                setTimeout(()=>{emit(state,'finished',result);resolve(result);},0);
            };
            function observe(module,found) {
                state.nativeSampler=null;
                if(state.continuous&&root.PGRazorNativeCorePath){
                    try{
                        if(!root.PGRazorNativeCameraSampler)throw Error('native-camera-sampler-missing');
                        state.nativeSampler=root.PGRazorNativeCameraSampler.create({state,found,module,Memory,NativeFunction,ptr});
                        if(!state.nativeSampler)throw Error('native-camera-sampler-unavailable');
                        emit(state,'native-camera-sampler-ready',{backend:'native'});
                    }catch(error){state.finish({status:'unavailable',reason:'native-camera-sampler-install:'+String(error)});return;}
                }
                function clock() {
                    let now;
                    try{now=state.clock();}catch(error){state.sampleError='boot-clock-read-failed';return null;}
                    if(state.continuous && (!Number.isFinite(now) || now<0 || now>Number.MAX_SAFE_INTEGER ||
                            (state.lastClock!==null && now<state.lastClock))) {
                        state.sampleError='invalid-boot-clock';return null;
                    }
                    state.lastClock=now;return now;
                }
                function sample(tid,rigReceiver,returnedCamera) {
                    if(!state.sampleCamera || state.stopping || state.sampling || state.sampleError || state.sampleDone || state.overflow ||
                        state.threads.size!==1 || state.matches<3 || state.depth.size!==0)return;
                    if(state.continuous && !state.sampleRequested)return;
                    if(state.continuous && (!state.threads.has(tid) || !rigReceiver || !returnedCamera))return;
                    const now=clock();if(now===null)return;
                    if(!state.continuous && state.sampleStartedAt!==null && (now-state.sampleStartedAt>=LIMITS.sampleMs || state.sampleCount>=LIMITS.samples)) {state.sampleDone=true;return;}
                    if(state.lastSampleAt!==null && now-state.lastSampleAt<(state.continuous?100:LIMITS.sampleIntervalMs))return;
                    const s=found.sample,api=s.api,handles=[];
                    state.sampling=true;state.sampleTid=tid;state.sampleAttempts++;
                    let stage='sample-start', record=null;
                    function captureFailure(error) {
                        if(state.sampleFailure)return;
                        let name='Error',message='No error message';
                        try{if(error && typeof error.name==='string')name=error.name.slice(0,64);}catch(_){}
                        try {
                            if(error && typeof error.message==='string')message=error.message.slice(0,256);
                            else if(error && typeof error.cameraProbeReason==='string')message=error.cameraProbeReason.slice(0,256);
                            else if(typeof error==='string')message=error.slice(0,256);
                        }catch(_){}
                        state.sampleFailure={stage,errorName:name,message,tid,sampleAttempt:state.sampleAttempts};
                    }
                    function fail(reason){throw {cameraProbeReason:reason};}
                    function current() {
                        const previous=stage;stage=previous+':thread-check';
                        if(!state.active || state.stopping || state.sampleError)fail('sampling-cancelled');
                        if(Process.getCurrentThreadId()!==tid)fail('sampling-thread-changed');
                        stage=previous+':thread-current';
                        if(api.thread_current().isNull())fail('unity-thread-not-attached');
                        stage=previous;
                    }
                    function target(handle) {
                        const previous=stage;stage=previous+':gchandle-get-target';
                        const object=api.gchandle_get_target(handle);if(object.isNull())fail('gc-handle-target-missing');stage=previous;return object;
                    }
                    function retain(object,expected,subclass,label) {
                        stage=label+':null-check';
                        if(object.isNull())fail('camera-getter-returned-null');
                        stage=label+':gchandle-new';
                        // Frida's native bool argument uses an integer FFI value.
                        const handle=api.gchandle_new(object,0);if(handle.isNull())fail('gc-handle-allocation-failed');
                        handles.push(handle);
                        stage=label+':object-get-class';
                        const actual=api.object_get_class(target(handle));
                        stage=label+':class-check';
                        if(actual.isNull())fail('runtime-class-mismatch');
                        if(!(subclass?api.class_is_assignable_from(expected,actual):actual.equals(expected))) {
                            if(state.continuous && label==='MainCameraRig')throw {cameraUnavailable:'non-map-rig'};
                            fail('runtime-class-mismatch');
                        }
                        return handle;
                    }
                    function invoke(method,handle,label,parameters) {
                        stage=label+':invoke-preparation';current();stage=label+':exception-reset';s.exception.writePointer(ptr(0));
                        stage=label+':receiver';
                        const object=handle===null?ptr(0):target(handle);
                        stage=label+':runtime-invoke';
                        const result=api.runtime_invoke(method,object,parameters||ptr(0),s.exception);
                        current();
                        stage=label+':exception-read';
                        if(!s.exception.readPointer().isNull())fail('managed-getter-exception');
                        return result;
                    }
                    function integerValue(handle,label) {
                        stage=label+':object-unbox';
                        const value=api.object_unbox(target(handle));if(value.isNull())fail('boxed-integer-unavailable');
                        stage=label+':value-read';return value.readS32();
                    }
                    try {
                        if(state.sampleStartedAt===null)state.sampleStartedAt=now;
                        state.lastSampleAt=now;
                        if(state.nativeSampler){
                            stage='native-camera-sample';
                            record=state.nativeSampler.sample(tid,rigReceiver,returnedCamera,250);
                            if(record&&!record.unavailable){state.sampleCount++;record.sampleIndex=state.sampleCount;}
                        }else{
                        current();
                        let naturalCamera=null;
                        if(state.continuous) {
                            const rig=retain(rigReceiver,s.rig,false,'MainCameraRig');
                            if(returnedCamera.isNull())throw {cameraUnavailable:'camera-getter-returned-null'};
                            naturalCamera=retain(returnedCamera,s.classes.Camera,false,'NaturalCamera');
                        }
                        const cameraObject=invoke(s.methods.get_main,null,'Camera.get_main');
                        if(state.continuous && cameraObject.isNull())throw {cameraUnavailable:'camera-getter-returned-null'};
                        const camera=retain(cameraObject,s.classes.Camera,false,'Camera');
                        if(state.continuous && !target(naturalCamera).equals(target(camera)))throw {cameraUnavailable:'camera-mismatch'};
                        let mapFacts=null;
                        if(state.continuous) {
                            if(state.worldMapLayer===null) {
                                stage='WorldMap:string-new';current();
                                const name=retain(api.string_new(s.worldMapName),s.string,false,'String');
                                const parameters=Memory.alloc(Process.pointerSize);parameters.writePointer(target(name));
                                let result;
                                try{result=invoke(s.methods.NameToLayer,null,'LayerMask.NameToLayer',parameters);}
                                finally{parameters.writePointer(ptr(0));}
                                const layer=retain(result,s.integer,false,'LayerIndex');
                                const index=integerValue(layer,'LayerIndex');
                                if(index!==13)fail('world-map-layer-mismatch');
                                state.worldMapLayer=index;
                            }
                            const mask=retain(invoke(s.methods.get_cullingMask,camera,'Camera.get_cullingMask'),s.integer,false,'CullingMask');
                            const bits=integerValue(mask,'CullingMask')>>>0;
                            mapFacts={cameraCullingMask:bits,worldMapLayer:state.worldMapLayer,mapGate:'world-map-layer-visible'};
                            if((bits&(1<<state.worldMapLayer))===0)
                                throw {cameraUnavailable:'map-hidden',fields:Object.assign({mapVisible:false},mapFacts)};
                        }
                        const transformObject=invoke(s.methods.get_transform,camera,'Component.get_transform');
                        if(state.continuous && transformObject.isNull())throw {cameraUnavailable:'camera-transform-unavailable'};
                        const transform=retain(transformObject,s.classes.Transform,true,'Transform');
                        const vector=retain(invoke(s.methods.get_forward,transform,'Transform.get_forward'),s.vector,false,'Vector3');
                        const values={};
                        for(const axis of ['x','y','z']) {
                            stage='Vector3.'+axis+':field-get-value';current();api.field_get_value(target(vector),s.fields[axis],s.float);
                            stage='Vector3.'+axis+':float-read';
                            const value=s.float.readFloat();if(!Number.isFinite(value))fail('nonfinite-forward-vector');values[axis]=value;
                        }
                        if(Math.hypot(values.x,values.z)<0.000001)fail('vertical-forward-vector');
                        const yawDegrees=(Math.atan2(values.x,values.z)*180/Math.PI+360)%360;
                        const identity=retain(invoke(s.methods.GetInstanceID,camera,'Object.GetInstanceID'),s.integer,false,'Int32');
                        stage='Int32:object-unbox';
                        const unboxed=api.object_unbox(target(identity));if(unboxed.isNull())fail('boxed-identity-unavailable');
                        stage='Int32:value-read';
                        const cameraId=unboxed.readS32();current();
                        state.sampleCount++;
                        record={sampleIndex:state.sampleCount,timeMs:now,tid,cameraId,x:values.x,y:values.y,z:values.z,yawDegrees};
                        if(state.continuous)Object.assign(record,{mapVisible:true},mapFacts);
                        if(!state.continuous && state.sampleCount>=LIMITS.samples)state.sampleDone=true;
                        }
                    }catch(error){
                        if(state.continuous && error && typeof error.cameraUnavailable==='string')
                            record=Object.assign({unavailable:true,reason:error.cameraUnavailable,timeMs:now,tid},error.fields||{});
                        else {captureFailure(error);if(!state.sampleError)state.sampleError=(error && error.cameraProbeReason)||'native-sampler-error';}
                    }
                    finally {
                        for(let i=handles.length-1;i>=0;i--)try{stage='cleanup:gchandle-free';api.gchandle_free(handles[i]);}catch(error){captureFailure(error);state.sampleError='gc-handle-cleanup-failed';}
                        if(record && !state.sampleError && !state.stopping && state.active) {
                            // Publish only after every temporary handle was released.
                            // A stalled consumer never creates an unbounded queue.
                            if(state.continuous)state.sampleQueue.length=0;
                            // Preserve an invalidation even if a newer good sample
                            // replaces the latest-reading slot before the next poll.
                            if(record.unavailable)state.pendingUnavailable=record;
                            else state.sampleQueue.push(record);
                        }
                        state.sampling=false;
                        if(state.pendingFinish) {
                            const result=state.pendingFinish;state.pendingFinish=null;
                            state.timer=setTimeout(()=>state.finish(result),0);
                        }
                    }
                }
                const addresses=new Set(),exports=[];
                function enterMatch(tid) {
                    if(!state.active || state.stopping || state.overflow || state.sampleError)return 0;
                    if(state.sampling){if(tid!==state.sampleTid)state.sampleError='multiple-matching-threads';return 0;}
                    if(!state.threads.has(tid) && state.threads.size>=LIMITS.threads){state.overflow=true;return 0;}
                    state.rawMatches++;
                    const depth=state.depth.get(tid)||0;state.depth.set(tid,depth+1);
                    if(depth===0){state.matches++;state.threads.set(tid,(state.threads.get(tid)||0)+1);}
                    if(state.sampleCamera && state.threads.size>1)state.sampleError='multiple-matching-threads';
                    return 1;
                }
                function leaveMatch(tid) {
                    if(!state.active)return;
                    const depth=(state.depth.get(tid)||1)-1;
                    if(depth===0)state.depth.delete(tid);else state.depth.set(tid,depth);
                    if(depth===0 && !state.continuous)sample(tid);
                }
                let filter=null;
                if(state.continuous) {
                    const libc=Process.findModuleByName('libc.so');
                    const clockAddress=libc && libc.findExportByName('clock_gettime');
                    if(!clockAddress || clockAddress.isNull())throw Error('CLOCK_BOOTTIME export unavailable');
                    const slot=Memory.alloc(Process.pointerSize);slot.writePointer(found.execute);
                    const enter=new NativeCallback(tid=>{state.calls++;return enterMatch(tid);},'int',['uint32']);
                    const leave=new NativeCallback(leaveMatch,'void',['uint32']);
                    filter=new CModule(FILTER_SOURCE,{execute_method:slot,enter_match:enter,leave_match:leave,pgrazor_clock_gettime:clockAddress});
                    nativeFilters.push({filter,slot,enter,leave});
                    // This bounded libc clock read cannot re-enter JavaScript.
                    // Keep its tiny read/update section atomic against stop/poll.
                    state.clock=new NativeFunction(filter.monotonic_ms,'double',[],{scheduling:'exclusive'});
                }
                for(const name of ['il2cpp_runtime_invoke','il2cpp_runtime_invoke_convert_args']) {
                    const address=module.findExportByName(name);if(!address || address.isNull())continue;
                    if(addresses.has(address.toString()))continue;addresses.add(address.toString());
                    const callbacks=filter?{onEnter:filter.on_enter,onLeave:filter.on_leave}:{
                        onEnter(args) {
                            this.pgrazorMatch=false;
                            if(!state.active || state.stopping || state.overflow || state.sampleError)return;
                            if(state.sampling){if(args[0].equals(found.execute) && this.threadId!==state.sampleTid)state.sampleError='multiple-matching-threads';return;}
                            if(++state.calls>LIMITS.calls){state.overflow=true;return;}
                            if(!args[0].equals(found.execute))return;
                            this.pgrazorTid=this.threadId;this.pgrazorMatch=enterMatch(this.threadId)===1;
                        },
                        onLeave() {
                            if(!state.active || !this.pgrazorMatch)return;
                            leaveMatch(this.pgrazorTid);
                        }
                    };
                    state.listeners.push(Interceptor.attach(address,callbacks));exports.push(name);
                }
                if(!state.listeners.length){state.finish({status:'unavailable',reason:'invocation-exports-missing'});return;}
                if(state.continuous)state.listeners.push(Interceptor.attach(found.sample.getMainCameraAddress,{
                    onEnter(args) {
                        this.pgrazorRig=null;
                        if(!state.active || state.stopping || state.sampling || state.sampleError || state.overflow ||
                            state.getterDepth || state.depth.size || state.matches<3 || state.threads.size!==1 || !state.threads.has(this.threadId))return;
                        // Receiver lives only in this verified leaf getter invocation.
                        state.getterDepth++;this.pgrazorRig=args[0];this.pgrazorTid=this.threadId;
                    },
                    onLeave(value) {
                        const receiver=this.pgrazorRig;this.pgrazorRig=null;
                        if(receiver===null)return;
                        state.getterDepth--;
                        if(!state.active || state.stopping || state.sampling || state.sampleError || state.depth.size)return;
                        sample(this.pgrazorTid,receiver,value);
                    }
                }));
                state.observedAt=Date.now();
                function poll() {
                    if(!state.active || state.stopping)return;
                    const elapsed=Date.now()-state.observedAt;
                    if(state.continuous) {
                        try{state.sampleRequested=typeof options.shouldSample==='function' && options.shouldSample()===true;}
                        catch(error){state.sampleError='sampling-request-failed';}
                    }
                    const queued=state.sampleQueue.splice(0);
                    let unavailable=state.pendingUnavailable;state.pendingUnavailable=null;
                    if(state.sampleError){queued.length=0;unavailable=null;}
                    if(unavailable && !state.unavailableReported) {
                        state.unavailableReported=true;emit(state,'camera-unavailable',unavailable);
                    }
                    for(const record of queued){
                        if(!state.active)return;
                        const deliveredAt=clock();if(deliveredAt===null)break;
                        record.ageMillis=state.continuous ? deliveredAt-record.timeMs : Math.max(0,deliveredAt-record.timeMs);
                        state.unavailableReported=false;emit(state,'camera-sample',record);
                    }
                    if(!state.active)return;
                    if(state.continuous && !state.overflow && !state.sampleError) {
                        state.timer=setTimeout(poll,100);return;
                    }
                    if(!state.continuous && state.sampleStartedAt!==null && Date.now()-state.sampleStartedAt>=LIMITS.sampleMs)state.sampleDone=true;
                    if(elapsed<LIMITS.observeMs && !state.overflow && !state.sampleError && !state.sampleDone){state.timer=setTimeout(poll,LIMITS.pollMs);return;}
                    const summary={observedMs:elapsed,calls:state.calls,rawMatches:state.rawMatches,matches:state.matches,
                        threads:Array.from(state.threads,([tid,count])=>({tid,count})),activeMatches:state.depth.size,
                        sampleCamera:state.sampleCamera,samples:state.sampleCount,sampleFailure:state.sampleFailure};
                    if(state.sampleFailure)emit(state,'sampler-error',state.sampleFailure);
                    emit(state,'thread-summary',summary);
                    let reason=null;
                    if(state.sampleError)reason=state.sampleError;
                    else if(state.overflow)reason='observation-budget';
                    else if(!state.matches)reason='execute-tasks-not-observed';
                    else if(state.threads.size!==1)reason='multiple-matching-threads';
                    else if(state.matches<3)reason='insufficient-matching-calls';
                    else if(state.depth.size)reason='invocation-still-active';
                    else if(state.sampleCamera && !state.sampleCount)reason='no-camera-samples';
                    state.finish(Object.assign({status:reason?'unavailable':'complete',reason,
                        mainThreadCandidateTid:reason?null:summary.threads[0].tid},summary));
                }
                state.timer=setTimeout(poll,state.continuous?100:LIMITS.pollMs);
                for(const signature of found.events){if(!state.active)return;emit(state,'signature',signature);}
                if(state.active)emit(state,'ready',{stage:state.continuous?'continuous-camera-sampling':state.sampleCamera?'bounded-camera-sampling':'passive-thread-observation',observeMs:state.continuous?null:LIMITS.observeMs,
                    vectorFields:found.vectorFields,exports});
            }
            function attempt() {
                if(!state.active)return;state.timer=null;state.attempts++;
                try {
                    if(Process.arch!=='arm64' || Process.pointerSize!==8){state.finish({status:'skipped',reason:'native-arm64-required'});return;}
                    const module=Process.findModuleByName('libil2cpp.so');let found;
                    if(!module)found={retry:'library-not-loaded'};
                    else {
                        const elf=new Uint8Array(module.base.readByteArray(20));
                        if(elf[0]!==127 || elf[1]!==69 || elf[2]!==76 || elf[3]!==70 || elf[4]!==2 || elf[5]!==1 || elf[18]!==183 || elf[19]!==0){state.finish({status:'skipped',reason:'library-architecture-mismatch'});return;}
                        const api=bind(module),domain=api.domain_get();found=domain.isNull()?{retry:'domain-not-ready'}:discover(module,api,domain,state.sampleCamera,state.continuous);
                    }
                    if(found.retry) {
                        if(!state.continuous && (state.attempts>=LIMITS.attempts || Date.now()+LIMITS.retryMs>state.deadline))state.finish({status:'unavailable',reason:found.retry,attempts:state.attempts});
                        else state.timer=setTimeout(attempt,LIMITS.retryMs);
                    } else if(found.unavailable)state.finish({status:'unavailable',reason:found.unavailable});
                    else observe(module,found);
                }catch(error){state.finish({status:'error',message:String(error)});}
            }
            state.timer=setTimeout(attempt,0);
        });
        if(running)running.completion=completion;
        return completion;
    }
    root.PGRazorCameraProbe=Object.freeze({start,stop(){
        if(!running)return Promise.resolve();
        const completion=running.completion;
        running.finish({status:'cancelled'});
        return completion;
    },limits:LIMITS});
})(globalThis);
globalThis.PGRazorCameraProbe;
