const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const code=fs.readFileSync(__dirname+'/../embedded-agent.js','utf8');
function fixture(enabled=true,pilot=true) {
 const events=[],seen=[];
 const c={rpc:{},console:{log(){}},Java:{use(){throw Error('legacy Java access');}},
  PGRazorNativeCorePath:pilot?'selected':'',events,seen};
 vm.createContext(c);vm.runInContext(code,c);
 vm.runInContext(`bootParameters={googleBrowserLogin:${enabled}};control={event:(...args)=>events.push(args)};`,c);
 return {c,events,seen,run:s=>vm.runInContext(s,c)};
}
{
 const f=fixture(false);f.run('installGoogleBrowserLogin()');assert.deepEqual(f.events,[]);
}
{
 const f=fixture();assert.throws(()=>f.run('installGoogleBrowserLogin()'),/adapter-missing/);
 f.c.PGRazorCompiledGoogleSignIn={create:()=>null};
 assert.throws(()=>f.run('installGoogleBrowserLogin()'),/adapter-unavailable/);
}
{
 const f=fixture();let stops=0,installs=0,deps;
 f.c.PGRazorCompiledGoogleSignIn={create:d=>{deps=d;return {install(){installs++;},stop(){stops++;}};}};
 f.run('installGoogleBrowserLogin()');assert.equal(installs,1);assert.equal(deps.Java,f.c.Java);
 assert.equal(deps.isDisposed(),false);f.run('disposed=true');assert.equal(deps.isDisposed(),true);
 assert.equal(f.events[0][1],'Compiled browser sign-in hook ready');
 f.run('rollback();rollback()');assert.equal(stops,1);
}
{
 const f=fixture();let stops=0;
 f.c.PGRazorCompiledGoogleSignIn={create:()=>({install(){throw Error('factory install failed');},stop(){stops++;}})};
 assert.throws(()=>f.run('installGoogleBrowserLogin()'),/factory install failed/);
 f.run('rollback()');assert.equal(stops,1);assert.deepEqual(f.events,[]);
}
{
 const f=fixture(true,false);assert.throws(()=>f.run('installGoogleBrowserLogin()'),/legacy Java access/);
}
console.log('PASS compiled Google handoff host: pilot selection, disabled inert, no selected legacy fallback, rollback cleanup');
