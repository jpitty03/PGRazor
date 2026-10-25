// Native shiny scanner: map-only encounter requests that read the returned
// Pokemon's shiny status with NO capture, berry, spin, transfer or encounter-UI
// RPC. Bundled before diagnostics/quick-ball.js so the scanner stays an
// extracted, independently testable module while quick-ball only resolves the
// metadata, drives one Unity-thread tick and exposes the API.
//
// Verified basis (.artifacts/shiny-scanner): a map-only SendEncounterRequest
// returns a matching Holoholo.Rpc.EncounterOutProto whose PokemonProto exposes
// get_IsShinyPokemon. Ferroseed read true and Gulpin read false, each confirmed
// by a later manual encounter; the getter is readable on the validated response
// before EncounterNameplate.SetPokemonUI runs.
//
// Safety invariants:
//  - one in-flight request at a time, >=3s between requests, 20s per-request timeout;
//  - only visible exact WildMapPokemon within 80m of finite supplied coordinates;
//  - a bounded 30-minute session cache suppresses duplicate requests INCLUDING
//    failures; an expired entry may be rescanned only after that 30 minutes;
//  - classification fails closed to Unknown on any missing/mismatched/unreadable
//    field or native error; a non-shiny verdict is trusted only after the correct
//    response class, matching encounter identity and a fully populated proto;
//  - every native read happens on the verified Unity thread (never the JS timer),
//    pending GC roots are freed on that thread, and runtime cache clears on stop;
//  - a scanner fault disables ONLY the scanner, never quick-catch speed or IV.
(function(root){
 const features=root.PGRazorFeatures||(root.PGRazorFeatures={});
 const REQUEST_GAP_MS=3000,TIMEOUT_MS=20000,RANGE_M=80,CACHE_MS=1800000,CACHE_MAX=512,MANUAL_MS=120000,MAX_OBJECTS=2048,EARTH_M=6371000;

 // ---- Pure logic (no native): coordinates, identity/classification, cache,
 // scheduling. Exported for unit testing and reused by the installed scanner so
 // the decision rules have a single source of truth. ----
 function validCoords(lat,lon){return Number.isFinite(lat)&&Number.isFinite(lon)&&Math.abs(lat)<=90&&Math.abs(lon)<=180;}
 function distanceMeters(aLat,aLon,bLat,bLon){const r=Math.PI/180,x=(bLat-aLat)*r,y=(bLon-aLon)*r,v=Math.sin(x/2)**2+Math.cos(aLat*r)*Math.cos(bLat*r)*Math.sin(y/2)**2;return EARTH_M*2*Math.asin(Math.sqrt(Math.min(1,v)));}
 function inRange(aLat,aLon,bLat,bLon){if(!validCoords(aLat,aLon)||!validCoords(bLat,bLon))return false;const d=distanceMeters(aLat,aLon,bLat,bLon);return Number.isFinite(d)&&d<=RANGE_M;}
 // A false shiny getter is trusted only once the response class, encounter
 // identity and a fully populated Pokemon proto all check out; anything missing,
 // mismatched, unreadable or errored stays Unknown and is never called non-shiny.
 function classify(r){
  if(!r||r.responseOk!==true)return{status:'unknown',reason:'response-class'};
  if(r.success!==true)return{status:'unknown',reason:'response-status'};
  if(r.idMatch!==true)return{status:'unknown',reason:'encounter-id-mismatch'};
  if(r.cpValid!==true||r.speciesValid!==true)return{status:'unknown',reason:'stats-not-populated'};
  if(typeof r.shiny!=='boolean')return{status:'unknown',reason:'shiny-unreadable'};
  return{status:r.shiny?'shiny':'non-shiny',reason:'validated'};
 }
 function requestReady(st,now){return !st.pending&&Number.isFinite(now)&&now>=st.next;}
 function requestExpired(pending,now){return !!pending&&Number.isFinite(now)&&now-pending.at>=TIMEOUT_MS;}
 // Bounded session cache keyed by encounter id. An entry suppresses re-requests
 // (including failures) until it expires 30 minutes later; past the size bound
 // the oldest entry is evicted (Map preserves insertion order, re-put refreshes).
 function cacheHas(cache,id,now){const e=cache.get(id);if(!e)return false;if(Number.isFinite(now)&&now>=e.expires){cache.delete(id);return false;}return true;}
 function cachePut(cache,id,status,now){cachePrune(cache,now);if(!cache.has(id)&&cache.size>=CACHE_MAX)return cache;cache.set(id,{status,expires:now+CACHE_MS});return cache;}
 function cachePrune(cache,now){for(const [k,e] of cache)if(Number.isFinite(now)&&now>=e.expires)cache.delete(k);}
 const pure={validCoords,distanceMeters,inRange,classify,requestReady,requestExpired,cacheHas,cachePut,cachePrune};

 features.shinyScanner=function({s,p,event,Interceptor,unity,exact,hold,target,invoke,api,Memory,ptr,alive,bool,field,LIMITS}){
  const sc=p&&p.shinyScanner,iv=p&&p.iv;
  const st={off:false,enabled:false,latitude:null,longitude:null,until:0,next:0,pending:null,manual:null,cache:new Map(),status:''};
  function status(message){if(message!==st.status){st.status=message;event(s,'shiny-status',{message});}}
  function drop(){const r=st.pending;if(!r)return;st.pending=null;if(r.promise)try{api.gchandle_free(r.promise);}catch(_){}try{api.gchandle_free(r.map);}catch(_){}}
  // Any scanner-level failure disables only the scanner and releases its roots.
  function failScanner(e){if(st.off)return;st.off=true;drop();st.cache.clear();const reason='Scanner unavailable: '+String(e&&e.message||e).slice(0,120);status(reason);event(s,'shiny-ready',{available:false,reason});}
  function readId(o){const b=Memory.alloc(8);api.field_get_value(o,iv.wildId,b);return b.readU64().toString();}
  function readDouble(o,f){const b=Memory.alloc(8);api.field_get_value(o,f,b);return b.readDouble();}
  function manualExcluded(id,now){return !!st.manual&&st.manual.id===id&&now-st.manual.at<MANUAL_MS;}

  // Read the response attached to this exact map object and classify it. Validate
  // identity and population before trusting the shiny getter; a native error here
  // caches the target as a failure (no immediate rescan) but never faults the scanner.
  function classifyMap(map,id,now){
   const roots=[],own=o=>{if(o.isNull())return null;const h=hold(o);roots.push(h);return target(h);};
   let species=null,cp=null,lat=null,lon=null,verdict;
   try{
    const response=own(field(map,iv.mapResponse,'pointer'));
    const responseOk=!!response&&exact(response,iv.encounterOut);
    let idMatch=false,cpValid=false,speciesValid=false,shiny=null;
    const success=responseOk&&field(response,sc.status,'int')===sc.success;
    if(success){
     const rwild=own(field(response,iv.responseWild,'pointer'));
     if(rwild&&exact(rwild,iv.wildProto)){
      idMatch=readId(rwild)===id;lat=readDouble(rwild,iv.wildLatitude);lon=readDouble(rwild,iv.wildLongitude);
      const pokemon=own(field(rwild,iv.wildPokemon,'pointer'));
      if(pokemon&&exact(pokemon,iv.pokemon)){
       cp=field(pokemon,iv.values.cp_,'int');cpValid=Number.isInteger(cp)&&cp>0;
       species=field(pokemon,sc.species,'int');speciesValid=Number.isInteger(species)&&species>0;
       shiny=bool(sc.shiny.method,pokemon,null);
      }
     }
    }
    verdict=classify({responseOk,success,idMatch,cpValid,speciesValid,shiny});
   }catch(e){cachePut(st.cache,id,'error',now);drop();status('Encounter check failed');return;}
   finally{for(const h of roots)try{api.gchandle_free(h);}catch(_){}}
   cachePut(st.cache,id,verdict.status,now);
   event(s,'shiny-checked',{encounterId:id,status:verdict.status,reason:verdict.reason,species,cp});
   if(verdict.status==='shiny'&&validCoords(lat,lon)){event(s,'shiny-found',{encounterId:id,species,cp,latitude:lat,longitude:lon});status('Shiny found');}
   else if(verdict.status==='non-shiny')status('Checked: not shiny');
   else status('Encounter data incomplete');
   drop();
  }

  // Enumerate visible wild map Pokemon, pick the first eligible target and either
  // classify its already-cached response or issue one fresh map-only request.
  function scanOnce(now){
   st.next=now+REQUEST_GAP_MS;cachePrune(st.cache,now);
   if(st.cache.size>=CACHE_MAX){status('Scan cache full; waiting for expiry');return;}
   const roots=[],own=o=>{const h=hold(o);roots.push(h);return target(h);};
   try{
    const typeObj=own(sc.typeObject(api.class_get_type(iv.wildMap))),args=Memory.alloc(8);args.writePointer(typeObj);
    const array=own(invoke(sc.findAll,ptr(0),args)),count=array.add(24).readU64().toNumber();
    if(!Number.isFinite(count)||count<0||count>MAX_OBJECTS)throw Error('map-count');
    let nearby=0,chosen=null;
    for(let i=0;i<count&&!chosen;i++){
     const map=array.add(32+i*8).readPointer();if(!exact(map,iv.wildMap))continue;
     const mapHandle=hold(map);roots.push(mapHandle);if(!alive(mapHandle)||!bool(sc.visible.method,map))continue;
     const wild=own(field(map,iv.mapWild,'pointer'));if(!exact(wild,iv.wildProto))continue;
     const lat=readDouble(wild,iv.wildLatitude),lon=readDouble(wild,iv.wildLongitude);
     if(!inRange(st.latitude,st.longitude,lat,lon))continue;nearby++;
     const id=readId(wild);if(manualExcluded(id,now)||cacheHas(st.cache,id,now))continue;
     chosen={map,id,cached:!field(map,iv.mapResponse,'pointer').isNull()};
    }
    if(!chosen){status(nearby?'Nearby targets already checked':'No wild Pokemon within range');return;}
    // A cached valid response classifies without a new request.
    if(chosen.cached){classifyMap(chosen.map,chosen.id,now);return;}
    // Keep the map and request promise rooted while the response is pending.
    const mh=hold(chosen.map);let ph=null;
    // Reserve before sending: OFF/background/despawn cannot make this target
    // immediately eligible for another request while the first completes.
    cachePut(st.cache,chosen.id,'requested',now);
    try{const promise=invoke(sc.request.method,chosen.map);if(promise.isNull())throw Error('request-null');ph=hold(promise);}
    catch(e){try{api.gchandle_free(mh);}catch(_){}status('Encounter request failed');return;}
    st.pending={map:mh,promise:ph,id:chosen.id,at:now};event(s,'shiny-request',{encounterId:chosen.id});status('Scanning nearby Pokemon');
   }finally{for(const h of roots)try{api.gchandle_free(h);}catch(_){}}
  }

  // Runs only from quick-ball's verified Unity-thread cleanup tick.
  function tick(now){
   if(st.off)return;
   // Stop releases roots and clears the runtime cache without needing a clock.
   if(s.stopping){drop();st.cache.clear();return;}
   if(now===null||now===undefined)return;
   try{
    const ready=st.enabled&&st.latitude!==null&&now<st.until;
    // Disabling, a lost lease or missing coordinates drops pending roots safely here.
    if(!ready){if(st.pending)drop();status(!st.enabled?'Off':st.latitude===null?'Waiting for valid location':'Waiting for foreground map');return;}
    if(st.pending){
     const r=st.pending;
     // Stale map, despawn, cancellation and the manual-encounter race all drop the request.
     if(!alive(r.map)||manualExcluded(r.id,now)){drop();return;}
     const map=target(r.map),wild=field(map,iv.mapWild,'pointer');
     if(!bool(sc.visible.method,map)||!exact(wild,iv.wildProto)||readId(wild)!==r.id||!inRange(st.latitude,st.longitude,readDouble(wild,iv.wildLatitude),readDouble(wild,iv.wildLongitude))){drop();return;}
     if(requestExpired(r,now)){cachePut(st.cache,r.id,'error',now);drop();st.next=now+REQUEST_GAP_MS;status('Request timed out');return;}
     if(field(target(r.map),iv.mapResponse,'pointer').isNull())return; // response not populated yet
     classifyMap(target(r.map),r.id,now);return;
    }
    if(requestReady(st,now))scanOnce(now);
   }catch(e){failScanner(e);}
  }

  // Renewed foreground lease (same LIMITS lease). The coordinator folds the
  // map-visible AND VGP-off guard into `allowed`; coordinates must be finite.
  function setOptions(enabled,allowed,latitude,longitude){
   if(st.off)return false;st.enabled=enabled===true;
   if(!validCoords(latitude,longitude)){st.latitude=null;st.longitude=null;st.until=0;return false;}
   st.latitude=latitude;st.longitude=longitude;
   try{st.until=allowed===true?s.now()+LIMITS.leaseMs:0;}catch(_){st.until=0;return false;}
   return allowed===true;
  }

  function install(){
   if(!sc||!sc.available){st.off=true;event(s,'shiny-ready',{available:false,reason:(sc&&sc.reason)||'metadata-missing'});return;}
   event(s,'shiny-ready',{available:true});
   // Respect a manual encounter: note the wild target the nameplate opens so the
   // scanner never claims or re-requests it while the user is interacting.
   if(iv&&iv.available&&iv.setup&&iv.setup.address)s.listeners.push(Interceptor.attach(iv.setup.address,{onEnter(a){
    if(st.off||s.stopping||!unity(this.threadId))return;
    try{if(!exact(a[1],iv.wildMap))return;const wild=field(a[1],iv.mapWild,'pointer');if(!exact(wild,iv.wildProto))return;
     st.manual={id:readId(wild),at:s.now()};if(st.pending&&st.pending.id===st.manual.id)drop();}catch(_){}
   }}));
  }

  function hasRoots(){return !!st.pending;}
  return {install,tick,setOptions,hasRoots,state:st,pure,limits:{REQUEST_GAP_MS,TIMEOUT_MS,RANGE_M,CACHE_MS,CACHE_MAX,MANUAL_MS,leaseMs:LIMITS&&LIMITS.leaseMs}};
 };
 features.shinyScanner.pure=pure;
})(globalThis);
