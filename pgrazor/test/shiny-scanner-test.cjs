// Unit tests for the extracted shiny scanner's pure scheduling, identity and
// failure logic, plus the Unity-thread root-release lifecycle. Loads the real
// module source (the one bundled before quick-ball.js); native enumeration and
// the live request/response path are exercised on device, not here.
const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm'),path=require('node:path');
const src=fs.readFileSync(path.join(__dirname,'../runtime/features/shinyScanner.js'),'utf8');
const ctx={};vm.createContext(ctx);vm.runInContext(src,ctx);
const factory=ctx.PGRazorFeatures.shinyScanner;
assert.equal(typeof factory,'function','module registers PGRazorFeatures.shinyScanner');
const pure=factory.pure;assert.ok(pure,'pure helpers exported for testing');

// A builder with inert stubs. Any native call that the tested path should NOT
// reach throws, proving the scheduling/lifecycle logic stays off the wire.
function build(opts={}){
 const events=[],freed=[],attached=[];let clock=opts.clock??100000;
 const s={now:()=>clock,stopping:false,listeners:[]};
 const api={gchandle_free:h=>freed.push(h),field_get_value:()=>{throw Error('no native field read in this test');},class_get_type:()=>{throw Error('no class_get_type');}};
 const sc=opts.scUnavailable?{available:false,reason:opts.reason||'metadata-x'}
  :{available:true,species:{},shiny:{method:{}},visible:{method:{}},request:{method:{}},findAll:{},typeObject:()=>{throw Error('no typeObject');}};
 const iv={available:true,setup:{address:{}},wildMap:{},mapWild:{},wildProto:{},mapResponse:{},responseWild:{},wildPokemon:{},pokemon:{},encounterOut:{},wildId:{},wildLatitude:{},wildLongitude:{},values:{cp_:{}}};
 const deps={s,p:{shinyScanner:sc,iv},event:(_s,k,v)=>events.push(Object.assign({k},v)),
  Interceptor:{attach:(addr,cb)=>{const h={addr,cb,detach(){}};attached.push(h);return h;}},
  unity:()=>true,exact:()=>true,hold:o=>o,target:o=>o,invoke:()=>{throw Error('no invoke');},api,
  Memory:{alloc:()=>({})},ptr:()=>({}),alive:()=>true,bool:()=>{throw Error('no bool');},field:()=>{throw Error('no field');},LIMITS:{leaseMs:2500}};
 return {mod:factory(deps),events,freed,attached,s,setClock:n=>clock=n};
}

// ---- validCoords ----
assert.equal(pure.validCoords(37.5,-122.1),true);
assert.equal(pure.validCoords(0,0),true);
for(const [lat,lon] of [[NaN,0],[0,Infinity],[91,0],[-90.1,0],[0,180.1],[0,-181],['37',0]])
 assert.equal(pure.validCoords(lat,lon),false,`rejects ${lat},${lon}`);

// ---- distance / inRange (80m) ----
assert.equal(pure.distanceMeters(0,0,0,0),0);
assert.equal(pure.inRange(0,0,0,0),true);
assert.ok(pure.distanceMeters(0,0,0,0.0005)<80,'~55m is inside 80m');
assert.ok(pure.distanceMeters(0,0,0,0.001)>80,'~111m is outside 80m');
assert.equal(pure.inRange(0,0,0,0.0005),true);
assert.equal(pure.inRange(0,0,0,0.001),false);
assert.equal(pure.inRange(NaN,0,0,0),false,'invalid origin is never in range');

// ---- classify: identity + fail-closed ----
assert.deepEqual(JSON.parse(JSON.stringify(pure.classify({responseOk:true,success:true,idMatch:true,cpValid:true,speciesValid:true,shiny:true}))),{status:'shiny',reason:'validated'});
assert.deepEqual(JSON.parse(JSON.stringify(pure.classify({responseOk:true,success:true,idMatch:true,cpValid:true,speciesValid:true,shiny:false}))),{status:'non-shiny',reason:'validated'});
assert.equal(pure.classify({responseOk:false,idMatch:true,cpValid:true,speciesValid:true,shiny:false}).status,'unknown');
assert.equal(pure.classify({responseOk:true,success:true,idMatch:false,cpValid:true,speciesValid:true,shiny:false}).reason,'encounter-id-mismatch');
assert.equal(pure.classify({responseOk:true,success:true,idMatch:true,cpValid:false,speciesValid:true,shiny:false}).reason,'stats-not-populated');
assert.equal(pure.classify({responseOk:true,success:true,idMatch:true,cpValid:true,speciesValid:false,shiny:true}).reason,'stats-not-populated');
assert.equal(pure.classify({responseOk:true,success:true,idMatch:true,cpValid:true,speciesValid:true,shiny:null}).reason,'shiny-unreadable');
assert.equal(pure.classify({responseOk:true,success:true,idMatch:true,cpValid:true,speciesValid:true,shiny:undefined}).status,'unknown','unreadable shiny is never non-shiny');
assert.equal(pure.classify(null).status,'unknown');
assert.equal(pure.classify({responseOk:true,success:false,idMatch:true,cpValid:true,speciesValid:true,shiny:false}).status,'unknown','failed encounter cannot be classified as non-shiny');

// ---- cache: dedup (incl. failures), 30-minute expiry, bound ----
{
 const cache=new Map(),t=1000;
 pure.cachePut(cache,'a','non-shiny',t);
 assert.equal(pure.cacheHas(cache,'a',t),true,'fresh entry suppresses rescan');
 assert.equal(pure.cacheHas(cache,'a',t+1799999),true,'still within 30 minutes');
 assert.equal(pure.cacheHas(cache,'a',t+1800000),false,'expires exactly at 30 minutes');
 assert.equal(cache.has('a'),false,'cacheHas prunes the expired entry');
 pure.cachePut(cache,'b','error',t);
 assert.equal(pure.cacheHas(cache,'b',t),true,'failures are cached to block duplicate requests');
 pure.cachePut(cache,'c','shiny',t);pure.cachePrune(cache,t+1800001);
 assert.equal(cache.size,0,'prune clears all expired');
 // bound: oldest evicted past the cap
 const big=new Map();for(let i=0;i<520;i++)pure.cachePut(big,'k'+i,'error',t);
 assert.equal(big.size,512,'cache is bounded');
 assert.equal(big.has('k0'),true,'unexpired entries are retained');
 assert.equal(big.has('k519'),false,'new entries wait when full');
}

// ---- scheduling: one pending + >=3s spacing + 20s timeout ----
assert.equal(pure.requestReady({pending:null,next:1000},1000),true,'ready at the scheduled time');
assert.equal(pure.requestReady({pending:null,next:1000},999),false,'spacing not yet elapsed');
assert.equal(pure.requestReady({pending:{},next:0},100000),false,'never two in flight');
assert.equal(pure.requestExpired({at:1000},21000),true,'20s timeout');
assert.equal(pure.requestExpired({at:1000},20999),false,'under 20s is not expired');
assert.equal(pure.requestExpired(null,99999),false);

// ---- setOptions: lease renewal, coord validation ----
{
 const {mod}=build({clock:50000});
 assert.equal(mod.setOptions(true,true,37.5,-122.1),true,'enabled+allowed+valid coords accepted');
 assert.equal(mod.state.until,52500,'renews same LIMITS lease');
 assert.equal(mod.state.enabled,true);
 assert.deepEqual([mod.state.latitude,mod.state.longitude],[37.5,-122.1]);
 assert.equal(mod.setOptions(true,false,37.5,-122.1),false,'no lease without allowed');
 assert.equal(mod.state.until,0);
 assert.equal(mod.setOptions(true,true,NaN,0),false,'invalid coords rejected');
 assert.equal(mod.state.until,0);assert.equal(mod.state.latitude,null);
 assert.equal(mod.setOptions(false,true,37.5,-122.1),true,'disabled still reports lease acceptance');
 assert.equal(mod.state.enabled,false);
}

// ---- stop: drop pending roots on the Unity thread + clear cache, no clock needed ----
{
 const {mod,freed,s}=build();
 mod.state.pending={map:'MAP',promise:'PROMISE',id:'7',at:1};
 pure.cachePut(mod.state.cache,'x','shiny',1);
 s.stopping=true;
 mod.tick(null); // no clock available
 assert.deepEqual(freed.sort(),['MAP','PROMISE'],'both roots freed');
 assert.equal(mod.hasRoots(),false,'no roots remain so stop can finish');
 assert.equal(mod.state.cache.size,0,'runtime cache cleared on stop');
}

// ---- disabling / lost lease: drop roots safely without touching the wire ----
{
 const {mod,freed,events}=build();
 mod.state.enabled=false;
 mod.state.pending={map:'M',promise:'P',id:'9',at:1};
 mod.tick(100000);
 assert.deepEqual(freed.sort(),['M','P'],'disabling drops roots');
 assert.equal(mod.hasRoots(),false);
 assert.equal(events.at(-1).k,'shiny-status');
 assert.equal(events.at(-1).message,'Off');
}

// ---- metadata unavailable disables only the scanner ----
{
 const {mod,events,attached}=build({scUnavailable:true,reason:'scanner-find-all'});
 mod.install();
 assert.equal(events.at(-1).k,'shiny-ready');
 assert.deepEqual([events.at(-1).available,events.at(-1).reason],[false,'scanner-find-all']);
 assert.equal(attached.length,0,'no hooks attached when unavailable');
 assert.equal(mod.state.off,true);
 assert.equal(mod.setOptions(true,true,37,-122),false,'setOptions is inert when off');
 mod.tick(100000); // inert, no native, no throw
 assert.equal(mod.hasRoots(),false);
}

// ---- available install: readiness event + one manual-encounter hook ----
{
 const {mod,events,attached}=build();
 mod.install();
 assert.equal(events.at(-1).k,'shiny-ready');
 assert.equal(events.at(-1).available,true);
 assert.equal(attached.length,1,'manual-encounter exclusion hook attached');
}

console.log('PASS shiny-scanner: coords/range, identity + fail-closed classification, bounded 30-min dedup cache, one-pending/3s-spacing/20s-timeout scheduling, lease renewal, Unity-thread root release on stop/disable, scanner-only metadata failure');
