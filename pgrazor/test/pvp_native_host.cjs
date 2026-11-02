// Execute the production C++ ranking compiled as WASM, compare to the original JS oracle.
const fs=require('fs'),assert=require('assert'),path=require('path');
const {pvpRanks,pvpLines,pvpCells}=require('./pvp_reference.cjs');const data=require('../assets/pvp/data.json');
(async()=>{
 const {instance}=await WebAssembly.instantiate(fs.readFileSync(process.argv[2]),{}),api=instance.exports;
 function text(p){const b=new Uint8Array(api.memory.buffer);let end=p;while(b[end])++end;return Buffer.from(b.subarray(p,end)).toString('utf8');}
 function name(index){return text(api.species_name(index));}
 function query(dex,form,a,d,s,m){const n=api.query(dex,form,a,d,s,m),p=api.result_address(),view=new DataView(api.memory.buffer),out=[];
  for(let i=0;i<n;i++){const at=p+i*20;out.push({league:view.getInt32(at,true),name:name(view.getInt32(at+4,true)),rank:view.getInt32(at+8,true),level:view.getInt32(at+12,true),cp:view.getInt32(at+16,true)});}return out;}
 function compare(args){const expected=pvpRanks(...args).flatMap((l,league)=>l.fits.map(f=>({league,name:f.name,rank:f.rank,level:f.level,cp:f.cp})));
  assert.deepStrictEqual(query(...args),expected,JSON.stringify(args));
  const [dex,form,a,d,s,m]=args,valid=[a,d,s].every(v=>Number.isInteger(v)&&v>=0&&v<=15),ranks=pvpCells(...args);
  const expectedCell=valid?'<color="#36b89e">'+Math.round((a+d+s)/45*100)+'% '+a+'/'+d+'/'+s+'</color>'+(ranks?' '+ranks:''):'';
  assert.strictEqual(text(api.cell_summary(...args)),expectedCell,'cell '+JSON.stringify(args));
  for(const owned of [false,true]){const expectedLines=pvpLines(...args,owned);assert.strictEqual(text(api.label(...args,owned?1:0)),expectedLines.length?'\n'+expectedLines.join('\n'):'',JSON.stringify(args));}}
 const level=n=>Math.fround(data.cpm[(n-1)*2]);
 for(const args of [[183,0,0,15,15,level(15)],[183,0,0,15,15,NaN],[66,0,10,12,14,level(20)],[149,0,12,13,11,level(30)],[178,0,0,13,15,level(26)],[484,0,14,13,15,level(20)],[280,0,0,15,15,level(1)],[26,50,0,15,15,level(10)],[99999,0,0,15,15,level(10)],[280,0,16,0,0,level(10)]])compare(args);
 assert.deepStrictEqual(pvpLines(183,0,0,15,15,level(15),false),['<color="#2b7bd6">GL #1</color> Azumarill · Lv 45.5 · CP 1499']);
 // Fixed-seed coverage crosses cache eviction, forms, IV floors, ties, level caps and CP filters.
 let seed=0x12345678;const next=()=>{seed=(Math.imul(seed,1664525)+1013904223)>>>0;return seed;};const keys=Object.keys(data.keys);
 const began=performance.now();for(let i=0;i<200;i++){const [dex,form=0]=keys[next()%keys.length].split(':').map(Number);compare([dex,form,next()%16,next()%16,next()%16,i%11?Math.fround(data.cpm[next()%99]):NaN]);}
 compare([183,0,0,15,15,level(15)]);
 assert.equal(api.cache_capacity(),192);
 const warmBuilds=api.cache_builds(),warmHits=api.cache_hits();
 for(let i=0;i<100;i++)query(183,0,0,15,15,level(15));
 assert.equal(api.cache_builds(),warmBuilds,'warm visible species must not rebuild ranking tables');
 assert.ok(api.cache_hits()>warmHits,'warm queries reuse tables');
 assert.ok(api.cache_evictions()>0,'bounded cache eviction remains covered');
 console.log('PASS PvP cache: '+api.cache_capacity()+' tables; '+api.cache_builds()+' builds, '+api.cache_hits()+' hits, '+api.cache_evictions()+' evictions; 100 warm queries without rebuild');
 console.log('PASS production C++ PvP: 211 oracle comparisons, forms/evolutions, CP and IV floors, stable ranking, cell summaries and cache eviction; '+Math.round(performance.now()-began)+' ms including JS oracle');
})().catch(e=>{console.error(e);process.exitCode=1;});
