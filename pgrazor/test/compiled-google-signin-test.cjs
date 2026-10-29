const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const code=fs.readFileSync(__dirname+'/../runtime/compiled-google-signin.js','utf8');

const COMPANION='com.nianticlabs.platform.authgooglesignin.SignInManager$Companion';
const TYPES=['android.app.Activity','com.google.android.gms.auth.api.signin.GoogleSignInClient','kotlin.jvm.functions.Function1'];

function build(opts={}){
 const state={disposed:false};
 const bootLogs=[],accessed=[];
 // The helper owns the browser flow: a live signIn begins a browser, a shut-down (dead) helper
 // cancels the captured consumer without ever beginning one.
 const helper={live:true,browserBegins:0,shutdowns:0,signIns:[],
  signIn(activity,consumer){this.signIns.push({activity,consumer,live:this.live});if(this.live)this.browserBegins++;},
  shutdown(){this.shutdowns++;this.live=false;}};
 // The game sign-in method: tracks the exact overload types requested, how many times the original
 // was called, and lets a scenario reject the implementation setter to force a rollback path.
 function method(){let impl;const m={overloadTypes:null,originalCalls:0,call(self,...a){this.originalCalls++;return'original-auth';}};
  Object.defineProperty(m,'implementation',{get:()=>impl,set:v=>{
   if(v!==null&&opts.setterThrows)throw Error('impl-rejected:tok_'+Math.random());
   if(v===null&&opts.unhookThrows)throw Error('unhook-rejected:tok_'+Math.random());
   impl=v;}});
  return {signIn:{overload(...t){m.overloadTypes=t;return m;}},m};}
 const signInMethod=method();
 const classes={
  'dev.pgrazor.GameGoogleSignIn':{create(){if(opts.helperCreateThrows)throw Error('helper-metadata:tok_x');return helper;}},
  [COMPANION]:signInMethod};
 const Java={use(n){accessed.push(n);if(opts.throwOnUse===n)throw Error('use-missing:'+n+':tok_y');if(!(n in classes))throw Error('missing:'+n);return classes[n];}};
 const ctx={};vm.createContext(ctx);vm.runInContext(code,ctx);
 const adapter=ctx.PGRazorCompiledGoogleSignIn.create({Java,isDisposed:()=>state.disposed,bootLog:m=>bootLogs.push(m)});
 return {adapter,state,bootLogs,accessed,helper,signInMethod,m:signInMethod.m};
}

// --- Creation is inert: nothing is resolved or hooked until install() -------------------------
{
 const env=build();
 assert.equal(env.accessed.length,0,'no Java class resolved at creation');
 assert.equal(env.m.implementation,undefined,'no hook installed at creation');
}

// --- install resolves the exact overload, hooks, and bridges to the helper by (activity,consumer)
{
 const env=build();env.adapter.install();
 const {m,helper,accessed}=env;
 assert.deepEqual(m.overloadTypes,TYPES,'exact overload types');
 assert.ok(accessed.includes('dev.pgrazor.GameGoogleSignIn')&&accessed.includes(COMPANION));
 assert.equal(typeof m.implementation,'function','hook installed');
 // The hook forwards only (activity,consumer); the GoogleSignInClient is ignored and the original
 // is never called. The live helper begins a browser.
 const activity={a:1},client={c:1},consumer={fn:1};
 m.implementation.call({},activity,client,consumer);
 assert.equal(helper.signIns.length,1);
 assert.equal(helper.signIns[0].activity,activity);
 assert.equal(helper.signIns[0].consumer,consumer);
 assert.ok(!('client'in helper.signIns[0]),'client never forwarded');
 assert.equal(helper.browserBegins,1,'live helper begins the browser');
 assert.equal(m.originalCalls,0,'original Google sign-in flow never called');
}

// --- install is idempotent: a second call resolves nothing and keeps the single helper ---------
{
 const env=build();env.adapter.install();
 const resolved=env.accessed.length,impl=env.m.implementation;
 env.adapter.install();
 assert.equal(env.accessed.length,resolved,'no re-resolution on second install');
 assert.equal(env.m.implementation,impl,'hook unchanged on second install');
 assert.equal(env.helper.shutdowns,0,'no extra helper created/shut down');
}

// --- install is inert while disposed, and while stopped ---------------------------------------
{
 const env=build();env.state.disposed=true;env.adapter.install();
 assert.equal(env.accessed.length,0,'disposed install does nothing');
 assert.equal(env.m.implementation,undefined);
}
{
 const env=build();env.adapter.install();env.adapter.stop();env.adapter.install();
 assert.equal(env.m.implementation,null,'stopped install stays inert (hook not reinstalled)');
}

// --- Retained callback while stopped: shut helper down, forward, never begin a browser ---------
{
 const env=build();env.adapter.install();
 const retained=env.m.implementation;// captured before stop
 env.adapter.stop();// stop() shuts the helper down and unhooks
 assert.equal(env.helper.shutdowns,1,'stop shut the helper down');
 const activity={a:2},consumer={fn:2};
 retained.call({},activity,{},consumer);
 // Hook forwards to the dead helper; shutdown is idempotent and signIn cancels without a browser.
 assert.equal(env.helper.signIns.length,1);
 assert.equal(env.helper.signIns[0].live,false,'helper is dead when the retained consumer arrives');
 assert.equal(env.helper.browserBegins,0,'no browser begun for the retained consumer');
 assert.equal(env.m.originalCalls,0,'no original auth fallback');
}

// --- Retained callback while disposed behaves the same (shutdown then forward, no browser) -----
{
 const env=build();env.adapter.install();
 const retained=env.m.implementation;
 env.state.disposed=true;
 retained.call({},{a:3},{},{fn:3});
 assert.equal(env.helper.shutdowns,1,'disposed hook shuts the helper down');
 assert.equal(env.helper.browserBegins,0,'disposed hook begins no browser');
 assert.equal(env.m.originalCalls,0);
}

// --- Partial install: overload lookup fails -> helper shut down, no hook, throws, no payload ----
{
 const env=build({throwOnUse:COMPANION});
 assert.throws(()=>env.adapter.install(),/use-missing/,'lookup failure propagates, no legacy fallback');
 assert.equal(env.m.implementation,undefined,'no hook left installed');
 assert.equal(env.helper.shutdowns,1,'helper shut down on partial install');
 assert.ok(env.bootLogs.includes('google-signin-install-error'));
 assert.ok(env.bootLogs.every(l=>!/tok_|use-missing/.test(l)),'logs carry no raw error payload');
}

// --- Partial install: implementation setter rejected -> rollback to null, helper shut down ------
{
 const env=build({setterThrows:true});
 assert.throws(()=>env.adapter.install(),/impl-rejected/);
 assert.equal(env.m.implementation,null,'partial hook rolled back to null');
 assert.equal(env.helper.shutdowns,1,'helper shut down on setter failure');
 assert.deepEqual(env.bootLogs,['google-signin-install-error'],'only the constant phase is logged');
 assert.ok(env.bootLogs.every(l=>!/tok_|impl-rejected/.test(l)),'no credential-bearing payload logged');
}

// --- Initial helper-factory failure is not swallowed (and never shuts a nonexistent helper) -----
{
 const env=build({helperCreateThrows:true});
 assert.throws(()=>env.adapter.install(),/helper-metadata/,'initial install failure propagates');
 assert.equal(env.helper.shutdowns,0,'no helper existed to shut down');
 assert.equal(env.m.implementation,undefined);
}

// --- stop marks stopped before cleanup, shuts the helper down even when unhooking throws --------
{
 const env=build({unhookThrows:true});env.adapter.install();
 env.adapter.stop();
 assert.equal(env.helper.shutdowns,1,'helper shut down despite the unhook throwing');
 assert.ok(env.bootLogs.includes('google-signin-stop-error'));
 assert.ok(env.bootLogs.every(l=>!/tok_|unhook-rejected/.test(l)),'stop error carries no raw payload');
 // Idempotent: a second stop does nothing further, and install stays inert afterwards.
 env.adapter.stop();
 assert.equal(env.helper.shutdowns,1,'second stop does not re-shutdown');
}

// --- stop restores the hook and shuts the helper down, idempotently ----------------------------
{
 const env=build();env.adapter.install();
 env.adapter.stop();
 assert.equal(env.m.implementation,null,'hook restored');
 assert.equal(env.helper.shutdowns,1);
 env.adapter.stop();// idempotent, helper already released
 assert.equal(env.helper.shutdowns,1);
 env.adapter.install();// stays inert after stop
 assert.equal(env.m.implementation,null);
}

{
 const env=build();env.adapter.install();
 env.helper.signIn=()=>{throw Error('private_token_payload');};
 assert.throws(()=>env.m.implementation({},null,{}),/private_token_payload/);
 assert.equal(env.bootLogs.at(-1),'google-signin-dispatch-error');
 assert.equal(env.m.originalCalls,0);
 assert.ok(env.bootLogs.every(value=>!value.includes('private_token_payload')));
}
console.log('PASS compiled google sign-in adapter: inert creation, exact overload types with (activity,consumer) bridge and ignored client, install-once idempotence, disposed/stopped inertness, retained-callback shutdown-then-forward with no browser, overload/setter partial-install rollback with helper shutdown, un-swallowed initial failure, stop cleanup when unhook throws, constant-only error phases with no credential payload, and no original auth fallback');
