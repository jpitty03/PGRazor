// Original 0.1.6 patch 0005 algorithm, test oracle only; PvPoke data license in assets/pvp.
const data=require('../assets/pvp/data.json');const PVP_DATA={p:data.species,k:data.keys},PVP_CPM=data.cpm;
const PVP_LEAGUES=[['LL',500,'#d6517f'],['GL',1500,'#2b7bd6'],['UL',2500,'#c08a12']],PVP_CAP_INDEX=98,PVP_TOP=50,PVP_DIM='#6b7f80',pvpTables=new Map();
function pvpCp(e,a,d,s,cpm){return Math.floor((e[1]+a)*Math.sqrt(e[2]+d)*Math.sqrt(e[3]+s)*cpm*cpm/10);}
/** Ranks (Uint16, 0 = not eligible) and the level index each rank is reached at (Uint8), indexed a*256+d*16+s, for one species in one league. */
function pvpTable(index,limit){
 const key=index+':'+limit;let table=pvpTables.get(key);if(table){pvpTables.delete(key);pvpTables.set(key,table);return table;}
 const e=PVP_DATA.p[index],floor=e[4],start=limit>500?Math.max(0,Math.round((e[5]-1)*2)):0,rows=[];
 for(let s=15;s>=floor;s--)for(let d=15;d>=floor;d--)for(let a=15;a>=floor;a--){
  // Highest level index in [start, cap] whose CP fits (CP grows with level, so binary search).
  let lo=start,hi=PVP_CAP_INDEX;if(pvpCp(e,a,d,s,PVP_CPM[lo])>limit)continue;
  while(lo<hi){const mid=(lo+hi+1)>>1;if(pvpCp(e,a,d,s,PVP_CPM[mid])<=limit)lo=mid;else hi=mid-1;}
  const cpm=PVP_CPM[lo];rows.push([a*256+d*16+s,(e[1]+a)*cpm*(e[2]+d)*cpm*Math.floor((e[3]+s)*cpm),lo]);}
 rows.sort((x,y)=>y[1]-x[1]);table={rank:new Uint16Array(4096),level:new Uint8Array(4096)};
 for(let i=0;i<rows.length;i++){table.rank[rows[i][0]]=i+1;table.level[rows[i][0]]=rows[i][2];}
 pvpTables.set(key,table);if(pvpTables.size>192)pvpTables.delete(pvpTables.keys().next().value);return table;}
/** Level index (0 = level 1, half levels between) of a CP multiplier, or -1 when it matches none. */
function pvpLevelIndex(cpm){
 if(!(Number.isFinite(cpm)&&cpm>0))return -1;
 for(let i=0;i<=PVP_CAP_INDEX;i++)if(Math.abs(PVP_CPM[i]-cpm)<2e-5)return i;
 return -1;}
function pvpNumber(n){return String(n).replace(/\B(?=(\d{3})+(?!\d))/g,',');}
/**
 * Per league, the family members (species/form and its evolutions) that can still play it, best rank first:
 * the species must reach the CP limit at all, and its CP at the Pokemon's current multiplier must not be
 * over the limit (levels cannot go down). Unknown multiplier: no level filter.
 */
function pvpRanks(dex,form,a,d,s,cpm){
 if(typeof PVP_DATA==='undefined'||![a,d,s].every(v=>Number.isInteger(v)&&v>=0&&v<=15))return [];
 const start=PVP_DATA.k[dex+':'+form]!==undefined?PVP_DATA.k[dex+':'+form]:PVP_DATA.k[String(dex)];if(start===undefined)return [];
 const family=[],seen=new Set([start]),queue=[start];while(queue.length){const i=queue.shift();family.push(i);for(const n of PVP_DATA.p[i][6])if(!seen.has(n)){seen.add(n);queue.push(n);}}
 const known=Number.isFinite(cpm)&&cpm>0,out=[];
 for(const [tag,limit,color] of PVP_LEAGUES){const fits=[];
  for(const i of family){const e=PVP_DATA.p[i];if(a<e[4]||d<e[4]||s<e[4])continue;
   // A league only counts when the species can reach its CP limit (otherwise every IV just maxes out).
   if(pvpCp(e,15,15,15,PVP_CPM[PVP_CAP_INDEX])<=limit)continue;
   if(known&&pvpCp(e,a,d,s,cpm)>limit)continue;
   const t=pvpTable(i,limit),at=a*256+d*16+s,rank=t.rank[at];if(!rank)continue;
   fits.push({rank,name:e[0],level:t.level[at],cp:pvpCp(e,a,d,s,PVP_CPM[t.level[at]])});}
  fits.sort((x,y)=>x.rank-y.rank);out.push({tag,limit,color,fits});}
 return out;}
/**
 * Label lines: each top-50 member as "GL #3 Gallade · Lv 17 · CP 1499" in the league colour (at most 3 per league);
 * Great and Ultra League without a top-50 member show their best playable rank in grey. mark adds a check when the
 * Pokemon is already at that rank's level.
 */
function pvpLines(dex,form,a,d,s,cpm,mark){
 const now=mark?pvpLevelIndex(cpm):-1,lines=[];
 function line(color,tag,f){return '<color="'+color+'">'+tag+' #'+pvpNumber(f.rank)+'</color> '+f.name+' · Lv '+(f.level/2+1)+' · CP '+f.cp+(now>=0&&f.level===now?' ✓':'');}
 for(const league of pvpRanks(dex,form,a,d,s,cpm)){
  const top=league.fits.filter(f=>f.rank<=PVP_TOP).slice(0,3);
  if(top.length)for(const f of top)lines.push(line(league.color,league.tag,f));
  else if(league.fits.length&&league.limit>500)lines.push(line(PVP_DIM,league.tag,league.fits[0]).replace('</color> ',' ').replace(/$/,'</color>'));
 }
 return lines;}
function pvpCells(dex,form,a,d,s,cpm){
 return pvpRanks(dex,form,a,d,s,cpm).filter(l=>l.fits.length&&l.fits[0].rank<=PVP_TOP)
  .map(l=>'<color="'+l.color+'">'+l.tag+'#'+l.fits[0].rank+'</color>').join(' ');
}
module.exports={pvpRanks,pvpLines,pvpCells};
