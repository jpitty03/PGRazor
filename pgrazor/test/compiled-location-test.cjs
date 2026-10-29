const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const code=fs.readFileSync(__dirname+'/../runtime/compiled-location.js','utf8');

// Sentinel flag objects stand in for Frida Int64 wrappers: the adapter must forward them to the
// Java helper by identity, never coercing a 64-bit flag through a JS number.
const LOCATION_FLAG={tag:'location'},PRECISE_FLAG={tag:'precise'};
const STATUS_CLASS='com.nianticlabs.platform.niaadvsynccore.permission.PermissionStatus';
const IN_USE={value:'in-use'},ALWAYS={value:'always'};

function build(opts={}){
 const state={desired:true,disposed:false,stopped:false};
 const events=[],bootLogs=[],casts=[],accessed=[];
 const calls={locationPermission:[],permissionStatus:[],dismiss:[],shutdown:[],lifecycle:[]};
 const control={desiredEnabled:()=>state.desired,event:(k,d)=>events.push([k,d])};
 const helper={
  locationPermission(p){calls.locationPermission.push(p);return state.desired&&(p==='android.permission.ACCESS_FINE_LOCATION'||p==='android.permission.ACCESS_COARSE_LOCATION');},
  permissionStatus(flag){calls.permissionStatus.push(flag);if(!state.desired)return null;if(flag===LOCATION_FLAG)return IN_USE;if(flag===PRECISE_FLAG)return ALWAYS;return null;},
  dismissAdventureSync(activity,action){calls.dismiss.push({activity,action});},
  shutdown(){calls.shutdown.push('permissions');}
 };
 const deliveryThrows=opts.deliveryThrows||{};
 const delivery={
  start(o){calls.lifecycle.push(['helper-start',o]);if(deliveryThrows.start)throw Error('start-fault');},
  resume(o){calls.lifecycle.push(['helper-resume',o]);if(deliveryThrows.resume)throw Error('resume-fault');},
  pause(o){calls.lifecycle.push(['helper-pause',o]);if(deliveryThrows.pause)throw Error('pause-fault');},
  stop(o){calls.lifecycle.push(['helper-stop',o]);if(deliveryThrows.stop)throw Error('stop-fault');},
  shutdown(){calls.shutdown.push('delivery');}
 };
 const setter=opts.setterThrows||{};
 function method(fn,name){let impl;const m={call:(self,...a)=>fn(self,...a)};
  // A setter that rejects a non-null install lets a scenario force a partial-rollback mid-group;
  // the null assignment used to roll back always succeeds.
  Object.defineProperty(m,'implementation',{get:()=>impl,set:v=>{if(setter[name]&&v!==null)throw Error('impl-rejected');impl=v;}});
  return {overload(){return m;},m};}
 const unity=method(()=>'os-unity'),plugin=method(()=>'os-plugin'),status=method(()=>'os-status');
 const Service={invokeOnStart:method(self=>calls.lifecycle.push(['orig-start',self]),'invokeOnStart'),
  invokeOnStop:method(self=>calls.lifecycle.push(['orig-stop',self]),'invokeOnStop'),
  invokeOnPause:method(self=>calls.lifecycle.push(['orig-pause',self]),'invokeOnPause'),
  invokeOnResume:method(self=>calls.lifecycle.push(['orig-resume',self]),'invokeOnResume')};
 const classes={
  'dev.pgrazor.GameLocationPermissions':{create(){if(opts.permissionsCreateThrows)throw Error('perm-metadata');return helper;}},
  'dev.pgrazor.GameLocationDelivery':{create(){if(opts.deliveryCreateThrows)throw Error('delivery-metadata');return delivery;}},
  [STATUS_CLASS]:{__statusClass:true},
  'com.unity3d.player.UnityPermissions':{hasUserAuthorizedPermission:unity},
  'com.unity3d.plugin.UnityAndroidPermissions':{IsPermissionGranted:plugin},
  'com.nianticlabs.platform.niaadvsyncpermissions.internal.InternalPermissionManager':{permissionStatus:status},
  'com.nianticlabs.nia.contextservice.ContextService':Service,
  'com.nianticlabs.background.configuration.InitialDialog':{onCreate:method(self=>{self.created=true;})},
  'com.nianticlabs.background.configuration.ManualPermissionDialog':{onCreate:method(self=>{self.created=true;})},
  'com.nianticlabs.background.configuration.FinalDialog':{onCreate:method(self=>{self.created=true;})}
 };
 for(const name of opts.omit||[])delete classes[name];
 const Java={use(n){accessed.push(n);if(opts.throwOnUse===n)throw Error('use-missing:'+n);if(!(n in classes))throw Error('missing:'+n);return classes[n];},
  cast(o,t){casts.push([o,t]);return o;}};
 const ctx={};vm.createContext(ctx);vm.runInContext(code,ctx);
 const adapter=ctx.PGRazorCompiledLocation.create({Java,control,isDisposed:()=>state.disposed,bootLog:m=>bootLogs.push(m)});
 return {adapter,state,events,bootLogs,casts,accessed,calls,classes,unity,plugin,status,Service,helper,delivery};
}

// --- Permission gating -------------------------------------------------------------------
{
 const env=build();env.adapter.installPermissions();
 const {unity,plugin,status,state,calls,casts,events,classes}=env;
 // Exact fine/coarse are answered true; every other name defers to the original once.
 assert.equal(unity.m.implementation({},'android.permission.ACCESS_FINE_LOCATION'),true);
 assert.equal(unity.m.implementation({},'android.permission.ACCESS_COARSE_LOCATION'),true);
 assert.equal(unity.m.implementation({},'android.permission.CAMERA'),'os-unity');
 assert.equal(unity.m.implementation({},'android.permission.ACCESS_BACKGROUND_LOCATION'),'os-unity');
 // Plugin gate: a null Activity always defers to the original, without consulting the helper.
 let seen=calls.locationPermission.length;
 assert.equal(plugin.m.implementation(null,'android.permission.ACCESS_FINE_LOCATION'),'os-plugin');
 assert.equal(calls.locationPermission.length,seen);
 assert.equal(plugin.m.implementation({},'android.permission.ACCESS_FINE_LOCATION'),true);
 // Disabled preference: the helper returns false, so the original answers.
 state.desired=false;assert.equal(unity.m.implementation({},'android.permission.ACCESS_FINE_LOCATION'),'os-unity');state.desired=true;
 // Disposed: original answers and the helper is never consulted (short-circuit).
 state.disposed=true;seen=calls.locationPermission.length;
 assert.equal(unity.m.implementation({},'android.permission.ACCESS_FINE_LOCATION'),'os-unity');
 assert.equal(calls.locationPermission.length,seen);state.disposed=false;
 // permissionStatus: location/precise flags map to the helper values (cast to PermissionStatus).
 assert.equal(status.m.implementation(LOCATION_FLAG),IN_USE);
 assert.equal(status.m.implementation(PRECISE_FLAG),ALWAYS);
 assert.equal(calls.permissionStatus[0],LOCATION_FLAG,'64-bit flag forwarded by identity, no coercion');
 assert.ok(casts.some(c=>c[0]===IN_USE&&c[1]===classes[STATUS_CLASS]),'override cast to PermissionStatus');
 // Unrelated flag, disabled and disposed all defer to the original.
 assert.equal(status.m.implementation({tag:'other'}),'os-status');
 state.desired=false;assert.equal(status.m.implementation(LOCATION_FLAG),'os-status');state.desired=true;
 state.disposed=true;assert.equal(status.m.implementation(LOCATION_FLAG),'os-status');state.disposed=false;
 // The synthetic-location notice is emitted at most once.
 assert.equal(events.filter(e=>e[0]==='location-permission').length,1);
 // Optional dialogs: original onCreate runs first, then the helper dismiss with the paired action.
 const initial=classes['com.nianticlabs.background.configuration.InitialDialog'];
 const a1={};initial.onCreate.m.implementation.call(a1,{});
 assert.ok(a1.created);assert.equal(calls.dismiss.at(-1).activity,a1);assert.equal(calls.dismiss.at(-1).action,'onClickCancel');
 const fin=classes['com.nianticlabs.background.configuration.FinalDialog'];
 const a2={};fin.onCreate.m.implementation.call(a2,null);// null Bundle state preserved to the original
 assert.ok(a2.created);assert.equal(calls.dismiss.at(-1).action,'onClickClose');
 // The native location manager is never resolved: delivery owns no JS-side native method hook.
 assert.ok(!env.accessed.includes('com.nianticlabs.nia.location.NianticLocationManager'));
 // Repeated installPermissions is a no-op (no duplicate hooks / helper creation).
 const before=env.accessed.length;env.adapter.installPermissions();assert.equal(env.accessed.length,before);
}

// --- Permission helper metadata failure -> no hooks, error event ------------------------
{
 const env=build({permissionsCreateThrows:true});env.adapter.installPermissions();
 assert.equal(env.unity.m.implementation,undefined,'no gate installed when the helper cannot be built');
 assert.ok(env.events.some(e=>e[0]==='location-permission-error'));
}

// --- Partial required-group failure rolls back and shuts the helper down ----------------
{
 const env=build({throwOnUse:'com.unity3d.plugin.UnityAndroidPermissions'});env.adapter.installPermissions();
 // The UnityPermissions gate installed first, then the plugin resolution failed.
 assert.equal(env.unity.m.implementation,null,'first gate rolled back');
 assert.ok(env.calls.shutdown.includes('permissions'),'helper shut down on rollback');
 assert.ok(env.events.some(e=>e[0]==='location-permission-error'));
 // No dialog hooks were installed because the required group never completed.
 assert.equal(env.classes['com.nianticlabs.background.configuration.InitialDialog'].onCreate.m.implementation,undefined);
}

// --- Delivery lifecycle order, disposed gating and helper-fault isolation ----------------
{
 const env=build();env.adapter.installPermissions();env.adapter.installDelivery();
 const {Service,calls,state}=env;const svc={id:'service'};
 Service.invokeOnStart.m.implementation.call(svc);
 assert.deepEqual(calls.lifecycle.slice(-2),[['orig-start',svc],['helper-start',svc]],'start: original then helper');
 Service.invokeOnResume.m.implementation.call(svc);
 assert.deepEqual(calls.lifecycle.slice(-2),[['orig-resume',svc],['helper-resume',svc]],'resume: original then helper');
 Service.invokeOnStop.m.implementation.call(svc);
 assert.deepEqual(calls.lifecycle.slice(-2),[['helper-stop',svc],['orig-stop',svc]],'stop: helper then original');
 Service.invokeOnPause.m.implementation.call(svc);
 assert.deepEqual(calls.lifecycle.slice(-2),[['helper-pause',svc],['orig-pause',svc]],'pause: helper then original');
 // Disposed: original still dispatched, helper never captures the service.
 state.disposed=true;let n=calls.lifecycle.length;
 Service.invokeOnStart.m.implementation.call(svc);
 assert.deepEqual(calls.lifecycle.slice(n),[['orig-start',svc]]);state.disposed=false;
 // Repeated installDelivery does not reinstall.
 const before=env.accessed.length;env.adapter.installDelivery();assert.equal(env.accessed.length,before);
}
{
 // A helper exception on stop/pause must not prevent the game's own lifecycle teardown.
 const env=build({deliveryThrows:{stop:true,pause:true}});env.adapter.installDelivery();
 const {Service,calls}=env;const svc={id:'service'};
 Service.invokeOnStop.m.implementation.call(svc);
 assert.deepEqual(calls.lifecycle.slice(-2),[['helper-stop',svc],['orig-stop',svc]],'original stop runs despite helper fault');
 Service.invokeOnPause.m.implementation.call(svc);
 assert.deepEqual(calls.lifecycle.slice(-2),[['helper-pause',svc],['orig-pause',svc]],'original pause runs despite helper fault');
 assert.ok(env.bootLogs.some(m=>m.startsWith('location-delivery-stop')));
}

// --- Delivery install failures: rollback+shutdown, and no legacy fallback ----------------
{
 // The fourth hook (pause) is rejected mid-group; the three already installed roll back.
 const env=build({setterThrows:{invokeOnPause:true}});
 assert.throws(()=>env.adapter.installDelivery(),/impl-rejected/);
 assert.equal(env.Service.invokeOnStart.m.implementation,null,'partial delivery hooks rolled back');
 assert.equal(env.Service.invokeOnStop.m.implementation,null);
 assert.ok(env.calls.shutdown.includes('delivery'),'delivery helper shut down on rollback');
}
{
 const env=build({deliveryCreateThrows:true});
 assert.throws(()=>env.adapter.installDelivery(),/delivery-metadata/,'selected pilot throws, never legacy fallback');
 assert.equal(env.Service.invokeOnStart.m.implementation,undefined);
}

// --- Stop restores every hook and shuts both helpers down, idempotently ------------------
{
 const env=build();env.adapter.installPermissions();env.adapter.installDelivery();
 const {Service,unity,plugin,status,classes}=env;
 env.adapter.stop();
 for(const m of [unity.m,plugin.m,status.m,Service.invokeOnStart.m,Service.invokeOnStop.m,Service.invokeOnPause.m,Service.invokeOnResume.m,
  classes['com.nianticlabs.background.configuration.InitialDialog'].onCreate.m,
  classes['com.nianticlabs.background.configuration.FinalDialog'].onCreate.m])assert.equal(m.implementation,null);
 assert.ok(env.calls.shutdown.includes('delivery')&&env.calls.shutdown.includes('permissions'));
 env.adapter.stop();// idempotent
 // Install methods stay inert after stop.
 env.adapter.installPermissions();env.adapter.installDelivery();
 assert.equal(unity.m.implementation,null);assert.equal(Service.invokeOnStart.m.implementation,null);
}

console.log('PASS compiled location adapter: fine/coarse gating with disabled/disposed/null-activity fallback, 64-bit flag identity and PermissionStatus cast, optional dialog dismissal, required-group and delivery rollback, lifecycle order with helper-fault isolation, stop restore and idempotent shutdown, no native method hook');

// Coordinator regressions: cleanup also covers lookup failures and already captured callbacks.
{
 const env=build({throwOnUse:'com.nianticlabs.nia.contextservice.ContextService'});
 assert.throws(()=>env.adapter.installDelivery(),/use-missing/);
 assert.deepEqual(env.calls.shutdown,['delivery']);
}
{
 const env=build();env.adapter.installPermissions();env.adapter.installDelivery();
 const dialog=env.classes['com.nianticlabs.background.configuration.InitialDialog'].onCreate.m.implementation;
 const start=env.Service.invokeOnStart.m.implementation;
 env.state.disposed=true;const activity={};dialog.call(activity,null);
 assert.equal(activity.created,true);assert.equal(env.calls.dismiss.length,0);
 env.state.disposed=false;env.adapter.stop();
 dialog.call(activity,null);start.call({});
 assert.equal(env.calls.dismiss.length,0);
 assert.equal(env.calls.lifecycle.filter(x=>x[0]==='helper-start').length,0);
 assert.equal(env.calls.lifecycle.filter(x=>x[0]==='orig-start').length,1);
}
