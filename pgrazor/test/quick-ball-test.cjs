
const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm'),path=require('node:path');
const source=require('./script-bundle.cjs')('quick-ball.js');
const BALL='Niantic.Holoholo.Encounter.Pokeball',CAP='Niantic.Holoholo.Encounter.EncounterCaptureState',RESP='Holoholo.Rpc.CatchPokemonOutProto',EN=RESP+'.Types.Status',AN='UnityEngine.Animator',OB='UnityEngine.Object',SYNC='UnityEngine.UnitySynchronizationContext';
const NAMEPLATE='Niantic.Holoholo.Encounter.EncounterNameplate',PK='Holoholo.Rpc.PokemonProto',MAPPK='Niantic.Holoholo.Map.IMapPokemon',LEVELS='Niantic.Holoholo.GameMasterDataExt_PlayerLevelSettings',UITEXT='UnityEngine.UI.Text';
const WILDMAP='Niantic.Holoholo.Map.WildMapPokemon',WILDPROTO='Holoholo.Rpc.WildPokemonProto',ENCOUNTEROUT='Holoholo.Rpc.EncounterOutProto',TAPPABLE='Holoholo.Rpc.TappableEncounterProto';
const GROUND='Niantic.Holoholo.Map.GroundTapHandler',MAPVIEW='Niantic.Holoholo.Map.MapViewHandler',LATLNG='Niantic.LatLng',VEC3='UnityEngine.Vector3';
const THROW='Niantic.Holoholo.Encounter.PokeballThrow',ENCOUNTER='Niantic.Holoholo.Encounter.EncounterState';
const AINFO='UnityEngine.AnimatorStateInfo',RAC='UnityEngine.RuntimeAnimatorController',AOC='UnityEngine.AnimatorOverrideController',I32='System.Int32',STR='System.String';
const H={Closed:57362642,Drop:3492260380,Bounce:2240868719,ShakeWait:1281384737,Shakes3:824257959,Caught:2003394077,End:951154001,Wait:3722194139,NumShakes:3410899964};
const BALL_HASHES=[['dsmh',3535843008],['dsmi',57362642],['dsmj',1372578019],['dsmk',71445658],['dsml',3492260380],['dsmm',3049724742],['dsmn',1232599366],['dsmo',1808254291],['dsmp',951154001],['dsmq',596303718],['dsmr',3722194139],['dsms',382342365],['dsmt',137525990]];
const INFO=[['m_Name',I32],['m_Path',I32],['m_FullPath',I32],['m_NormalizedTime','System.Single'],['m_Length','System.Single'],['m_Speed','System.Single'],['m_SpeedMultiplier','System.Single'],['m_Tag',I32],['m_Loop',I32]];
const BASE=0x10000000,DEBUG=[0x90051008,0xf9474508,0xb4000148,0x90051009,0xf9474929,0xb40000e9,0xf940040a,0xeb0a011f,0x2a1f03e8,0x54000128,0xeb0a013f,0x540000e3,0xb40000a1,0xf9400408,0xb900083f,0xf900083f,0xf9000028,0x52800028,0x2a0803e0,0xd65f03c0];
const code=new Map([[0xad86a48,'ffc301d1fe0b00f9fc6f02a9fa6703a9f85f04a9f65705a9f44f06a9541a02b0'],[0xa85f504,'ff4301d1fe1300f9f65703a9f44f04a9754302d0f303012af40300aaa8de7239'],[0xa85eed4,'ff4301d1e81300fdfe5703a9f44f04a90840201e744302f0f30300aa88d67239'],[0xad84364,'ff8303d1fd7b08a9fc6f09a9fa670aa9f85f0ba9f6570ca9f44f0da9541a02f0'],[0x9e87568,'ea0f1cfce923016dfe5702a9f44f03a94840201e2940201e149202f00a40201e'],[0x9e87638,'e923bc6dfe0b00f9f65702a9f44f03a92840201e0940201e159202f0a8026539'],[0x9e8307c,'ff0302d1e923036dfe2300f9f85f05a9f65706a9f44f07a9349202f0f30300aa'],[0xad93280,'080441f9c80000b4002140f9031540f9427c4092040d40f980001fd6c0035fd6'],[0xad5fbdc,'fe5fbda9f65701a9f44f02a9961b0290f50302aaf40301aac8026539f30300aa'],[0xacf104c,'fd7bbaa9fc6f01a9fa6702a9f85f03a9f65704a9f44f05a9f51e02d0f40301aa'],[0xad58170,'fe0f1df8f65701a9f44f02a9b41b02f0f30300aa8886623928040037e0ef01d0'],[0xd954a40,'fe57bea9f44f01a974bc00d0f30300aa8866793908010037809700d000800c91'],[0xd954b48,'e80f1dfcfe5701a9f44f02a90840201e74bc00d0f30300aa886a793908010037'],[0xda6bba0,'fe0f1df8f65701a9f44f02a9f4b30090f30300aa882e673988020036f48800d0'],[0xacf0a8c,'fd7bbaa9fc6f01a9fa6702a9f85f03a9f65704a9f44f05a9f51e02f0f40301aa'],[0xad691c8,'e00314aae9bbff97c82c02b0f60313aa']]);
function fixture(options={}){
 let ms=10000,tid=77,callback=false,nextTimer=0,nextObject=0,alloc=0;const timers=new Map(),events=[],hooks=[],calls=[],handles=new Map(),writes=[],paramReads=[],stats={ownedAttach:0,ownedDetach:0,freeNames:0,names:0,staticReads:0};
 const mutable={restoreFail:false,applyFail:false,getFail:false,destroyed:false,throwAfterApply:false,clockFail:false,failHoldOf:null,stateFail:false,nameFail:false};
 class P{
  constructor(v,offset=0){this.v=v;this.offset=offset;}
  isNull(){return this.v===0;}equals(p){return !!p&&this.v===p.v&&this.offset===p.offset;}
  add(n){return typeof this.v==='number'?new P(this.v+n):new P(this.v,this.offset+n);}
  compare(p){return Math.sign(this.v-p.v);}and(n){return new P(this.v&n);}toInt32(){return this.v|0;}
  toString(){return typeof this.v==='number'?'0x'+this.v.toString(16):typeof this.v==='string'?this.v:'obj'+(this.v.id||(this.v.id=++nextObject));}
  writePointer(p){this.v.pointers??=new Map();this.v.pointers.set(this.offset,p);}
  readPointer(){if(this.v==='assemblies')return new P('assembly');return this.v.pointers?.get(this.offset)||new P(0);}
  writeU64(n){this.v.scalar=n;}readU64(){const n=this.v.scalar;return{compare:x=>Math.sign(n-(typeof x==='number'?x:x.toNumber())),toNumber:()=>n};}
  writeU32(n){this.v.scalar=n;}readU32(){if(typeof this.v==='number')return words.get(this.v)||0;return this.v.scalar||0;}
  writeS32(n){this.v.scalar=n;}readS32(){return this.v.scalar||0;}
  writeFloat(n){this.v.scalar=Math.fround(n);}readFloat(){return this.v.floats?.[this.offset/4]??this.v.scalar;}readDouble(){return this.v.doubles?.[this.offset/8]??this.v.scalar;}
  readU8(){return typeof this.v==='string'?(this.v.charCodeAt(this.offset)||0):this.v.scalar||0;}
  readUtf8String(n){return this.v.slice(this.offset,this.offset+n);}readUtf16String(n){return this.v.utf16.slice(this.offset,this.offset+n);}
  readS64(){return{toNumber:()=>this.offset===0?Math.floor(ms/1000):(ms%1000)*1e6};}
  writeByteArray(a){this.v.pointers=new Map();}
  readByteArray(n){const hex=code.get(this.v-BASE);assert(hex,'unknown code '+this.toString());const b=Buffer.from(hex,'hex');assert.equal(n,b.length);if(options.badCode)b[0]^=1;return Uint8Array.from(b).buffer;}
 }
 const nil=()=>new P(0),mem=n=>new P({size:n,id:++nextObject}),str=s=>new P(s),words=new Map();
 DEBUG.forEach((x,i)=>words.set(BASE+0x1000+i*4,options.badDebug?0:x));
 const exports={gchandle_new:BASE+0x2000,gchandle_get_target:BASE+0x2100,gchandle_free:BASE+0x2200,debug_get_method_info:BASE+0x1000};
 [0xaa0003e8,0x52800049,0x7200003f,0x1a890520,0xaa0803e1].forEach((x,i)=>words.set(exports.gchandle_new+i*4,x));
 const branch=(from,to)=>words.set(from,(0x14000000|((to-from)/4&0x3ffffff))>>>0);
 branch(exports.gchandle_new+20,BASE+0x3000);words.set(BASE+0x3384,options.badGc?0:0x9a953120);
 branch(exports.gchandle_get_target,BASE+0x4000);words.set(BASE+0x4000,0xf81b0ffe);words.set(BASE+0x4014,0x9273c817);words.set(BASE+0x4018,0x394082e8);
 branch(exports.gchandle_free,BASE+0x5000);words.set(BASE+0x5000,0xf81b0ffe);words.set(BASE+0x5018,0x9273c816);words.set(BASE+0x501c,0x394082c8);
 // Trace classes exist only with options.trace, so default fixtures prove the trace is optional.
 const classes=[BALL,CAP,RESP,AN,OB,SYNC,'System.Single','System.Boolean'];if(options.trace)for(const k of[AINFO,RAC,AOC,I32,STR])if(k!==options.omitClass)classes.push(k);
 if(options.throws)classes.push(THROW,ENCOUNTER);if(options.map)classes.push(GROUND,MAPVIEW,LATLNG,VEC3);
 if(options.iv)for(const k of [WILDMAP,WILDPROTO,ENCOUNTEROUT,TAPPABLE,NAMEPLATE,PK,MAPPK,LEVELS,UITEXT,'UnityEngine.TextAnchor','UnityEngine.HorizontalWrapMode',I32,STR])if(!classes.includes(k))classes.push(k);
 const fields=new Map();for(const[owner,name,type,offset]of[[BALL,'animator',AN,176],[BALL,'<HoldingBreadBall>k__BackingField','System.Boolean',576],[RESP,'Status',EN,16]])fields.set(owner+'::'+name,{owner,name,type,offset,flags:1});
 if(options.throws)for(const[name,type,offset]of [['BallType','Holoholo.Rpc.Item',16],['ReticleSize','System.Single',20],['HitBullseye','System.Boolean',24],['Spinning','System.Boolean',25],['Missed','System.Boolean',26]])fields.set(THROW+'::'+name,{owner:THROW,name,type,offset,flags:1});
 if(options.map)for(const [owner,name,type,offset] of [[LATLNG,'Latitude','System.Double',16],[LATLNG,'Longitude','System.Double',24],[VEC3,'x','System.Single',16],[VEC3,'y','System.Single',20],[VEC3,'z','System.Single',24]])fields.set(owner+'::'+name,{owner,name,type,offset,flags:1});
 if(options.iv){
 for(const[owner,name,type,offset]of [[WILDPROTO,'Latitude','System.Double',0x20],[WILDPROTO,'Longitude','System.Double',0x28],[WILDMAP,'eluo',WILDPROTO,0x270],[WILDMAP,'elup',ENCOUNTEROUT,0x278],[WILDMAP,'elus',TAPPABLE,0x290],[ENCOUNTEROUT,'Pokemon',WILDPROTO,0x10],[WILDPROTO,'Pokemon',PK,0x38],[WILDPROTO,'EncounterId','System.UInt64',0x10],[TAPPABLE,'Pokemon',PK,0x18],[NAMEPLATE,'cpText',UITEXT,128],[NAMEPLATE,'dsqf','IGameMasterData',104],...[['cp_',44],['individualAttack_',112],['individualDefense_',116],['individualStamina_',120]].map(([n,o])=>[PK,n,I32,o]),[PK,'cpMultiplier_','System.Single',124],[PK,'additionalCpMultiplier_','System.Single',172]])fields.set(owner+'::'+name,{owner,name,type,offset,flags:1});
 for(const[owner,name,value]of [['UnityEngine.TextAnchor','MiddleCenter',4],['UnityEngine.HorizontalWrapMode','Overflow',1]])fields.set(owner+'::'+name,{owner,name,type:owner,offset:0,flags:0x8056,value});
 }
 INFO.forEach(([name,type],i)=>fields.set(AINFO+'::'+name,{owner:AINFO,name,type,offset:options.infoOffsets?.[i]??0x10+i*4,flags:1}));
 BALL_HASHES.forEach(([name],i)=>fields.set(BALL+'::'+name,{owner:BALL,name,type:I32,offset:0x340+i*4,flags:1}));
 fields.set(BALL+'::dsjv',{owner:BALL,name:'dsjv',type:I32,offset:4,flags:options.staticFlags??0x11,value:options.numShakesHash??H.NumShakes});
 // Deliberately not the shipped 0..5 values: behavior must map literals by name.
 const literals={CatchError:40,CatchSuccess:options.successValue??42,CatchEscape:43,CatchFlee:44,CatchMissed:45,CatchItemReplacement:46,...options.literals};
 for(const[name,value]of Object.entries(literals))if(name!==options.missingLiteral)fields.set(EN+'::'+name,{owner:EN,name,type:options.badLiteralType===name?'System.Int32':EN,offset:0,flags:options.nonLiteral===name?0x8016:0x8056,value});
 // GetBool/GetInteger(String) are listed before the (Int32) overloads on purpose.
 const methods=[...(options.iv?[
 {name:'SetPokemonUI',owner:NAMEPLATE,ret:'System.Void',params:[MAPPK,'Niantic.Rpc.GameplayWeatherProto.Types.WeatherCondition'],rva:0xad84364},
 {name:'budn',owner:NAMEPLATE,ret:PK,params:[],rva:0xad86a48},
 {name:'GetPokemonLevelOfCpMultiplier',owner:LEVELS,ret:I32,params:['IGameMasterData','System.Single'],static:true,rva:0xa85eed4},
 {name:'CpMultiplierAtLevel',owner:LEVELS,ret:'System.Single',params:['IGameMasterData',I32],static:true,rva:0xa85f504},
 ...[['get_text',STR,[]],['get_supportRichText','System.Boolean',[]],['get_resizeTextForBestFit','System.Boolean',[]],['get_horizontalOverflow','UnityEngine.HorizontalWrapMode',[]],['get_alignment','UnityEngine.TextAnchor',[]],['set_supportRichText','System.Void',['System.Boolean']],['set_text','System.Void',[STR]],['get_fontSize',I32,[]],['set_fontSize','System.Void',[I32]],['set_resizeTextForBestFit','System.Void',['System.Boolean']],['set_horizontalOverflow','System.Void',['UnityEngine.HorizontalWrapMode']],['set_alignment','System.Void',['UnityEngine.TextAnchor']]].map(([name,ret,params])=>({name,owner:UITEXT,ret,params}))]:[]),...(options.map?[{name:'OnTap',owner:GROUND,ret:'System.Void',params:['System.Object','System.EventArgs'],rva:0x9e8307c},{name:'GetGroundPosition',owner:MAPVIEW,ret:'System.Boolean',params:['UnityEngine.Vector2',VEC3+'&'],rva:0x9e87638},{name:'GetWorldLocation',owner:MAPVIEW,ret:LATLNG,params:[VEC3],rva:0x9e87568}]:[]),...(options.throws?[{name:'btyl',owner:BALL,ret:'System.Void',params:[THROW],rva:0xad5fbdc},{name:'NotifyPokeballHit',owner:ENCOUNTER,ret:'System.Void',params:[THROW],rva:0xad93280}]:[]),{name:'btdf',owner:CAP,ret:'System.Void',params:[RESP],rva:0xacf104c},{name:'btxu',owner:BALL,ret:RESP,params:[],rva:0xad58170},{name:'get_speed',owner:AN,ret:'System.Single',params:[],rva:0xd954a40},{name:'set_speed',owner:AN,ret:'System.Void',params:['System.Single'],rva:0xd954b48},{name:'op_Implicit',owner:OB,ret:'System.Boolean',params:[OB],rva:0xda6bba0,static:true},{name:'ExecuteTasks',owner:SYNC,ret:'System.Void',params:[],static:true},{name:'btdd',owner:CAP,ret:'System.Void',params:[RESP],rva:0xacf0a8c},
  {name:'GetCurrentAnimatorStateInfo',owner:AN,ret:AINFO,params:[I32]},{name:'GetNextAnimatorStateInfo',owner:AN,ret:AINFO,params:[I32]},{name:'IsInTransition',owner:AN,ret:'System.Boolean',params:[I32]},
  {name:'get_runtimeAnimatorController',owner:AN,ret:RAC,params:[]},{name:'get_layerCount',owner:AN,ret:I32,params:[]},{name:'get_parameterCount',owner:AN,ret:I32,params:[]},{name:'get_name',owner:OB,ret:STR,params:[]},
  {name:'GetBool',owner:AN,ret:'System.Boolean',params:[STR]},{name:'GetBool',owner:AN,ret:'System.Boolean',params:[I32]},{name:'GetInteger',owner:AN,ret:I32,params:[STR]},
  ...(options.noIntOverload?[]:[{name:'GetInteger',owner:AN,ret:I32,params:[I32]}]),...(options.dupOverload?[{name:'GetBool',owner:AN,ret:'System.Boolean',params:[I32]}]:[])];
 const obj=(klass,fields={})=>new P({klass,fields,id:++nextObject});
 const api={object_get_virtual_method:(o,m)=>m,string_new:p=>{const b=obj(STR);b.v.text=p.v;return b;},domain_get:()=>options.noDomain?nil():str('domain'),domain_get_assemblies:(d,c)=>{c.v.scalar=options.manyAssemblies?300:1;return str('assemblies');},assembly_get_image:()=>str('image'),
  class_from_name:(i,ns,n)=>classes.includes(ns.v+'.'+n.v)&&!options.missingClass?str(ns.v+'.'+n.v):nil(),class_get_type:k=>k,
  class_get_field_from_name:(k,n)=>{const f=fields.get(k.v+'::'+n.v);return f?new P(f):nil();},field_get_parent:f=>str(f.v.owner),field_get_type:f=>str(options.badField===f.v.name?'System.Object':f.v.type),field_get_flags:f=>f.v.flags,field_get_offset:f=>f.v.offset+(options.badOffset?4:0),
  type_is_byref:t=>t.v.endsWith('&'),class_is_assignable_from:(base,derived)=>base.v===derived.v||base.v===MAPPK&&derived.v===WILDMAP||base.v===ENCOUNTER&&derived.v==='WildEncounter',class_from_type:t=>t,class_is_enum:()=>1,class_enum_basetype:()=>str(options.enum64?'System.Int64':'System.Int32'),class_is_valuetype:k=>options.infoRef&&k.v===AINFO?0:1,
  class_value_size:(k,out)=>{const[n,a]=k.v===LATLNG?[16,8]:k.v===VEC3?[12,4]:k.v===THROW?[options.badThrowSize?16:12,4]:k.v==='System.Boolean'?[1,1]:k.v===AINFO?[options.infoSize??36,options.infoAlign??4]:[4,4];out.v.scalar=a;return n;},
  field_static_get_value:(f,out)=>{if(f.v.owner===BALL){assert(callback,'Pokeball statics are read only on the Unity callback');stats.staticReads++;}out.v.scalar=f.v.value;},
  class_get_method_from_name:(k,n,count)=>{const m=methods.find(m=>m.name===n.v&&m.owner===k.v&&m.params.length===count);return m?new P({...m}):nil();},
  class_get_methods:(k,iter)=>{const list=methods.filter(m=>m.owner===k.v),i=iter.v.idx||0;iter.v.idx=i+1;return i<list.length?new P({...list[i]}):nil();},
  string_length:s=>s.v.text.length,string_chars:s=>new P({utf16:s.v.text}),method_get_class:m=>str(m.v.owner),method_get_name:m=>str(m.v.name),method_get_flags:m=>m.v.static?16:6,method_get_param_count:m=>m.v.params.length,method_get_param:(m,i)=>str(options.badParam?'System.Object':m.v.params[i]),method_get_return_type:m=>str(options.badReturn?'System.Int32':m.v.ret),method_is_generic:()=>options.generic?1:0,method_is_inflated:()=>0,
  type_get_name:t=>{stats.names++;return t;},free:()=>stats.freeNames++,thread_current:()=>callback?str('current'):nil(),thread_attach:()=>{stats.ownedAttach++;return str('owned');},thread_detach:p=>{assert.equal(p.v,'owned');stats.ownedDetach++;},
  debug_get_method_info:(m,out)=>{out.writePointer(new P(BASE+m.v.rva));return 1;},object_get_class:o=>str(o.v.klass),
  field_get_value:(o,f,out)=>{assert(callback);assert.equal(o.v.klass,f.v.owner);const v=o.v.fields[f.v.name];if(v instanceof P)out.writePointer(v);else out.v.scalar=v;},
  gchandle_new:(o,pinned)=>{assert.equal(pinned,0);if(mutable.failHoldOf===o){mutable.failHoldOf=null;return nil();}const h=new P(0x100000000+(++alloc)*8);handles.set(h.v,o);return h;},gchandle_get_target:h=>{assert(h.v>0xffffffff,'handle must be pointer width');assert(handles.has(h.v),'released handle');return handles.get(h.v);},gchandle_free:h=>{assert(handles.has(h.v),'double-free');if(mutable.failFreeOnce){mutable.failFreeOnce=false;throw Error('free failed');}handles.delete(h.v);},
  object_unbox:o=>new P({scalar:o.v.value,doubles:o.v.doubles}),runtime_invoke:(m,o,args,ex)=>{
   assert(callback,'Unity methods cannot run from timer');calls.push('invoke:'+m.v.name);if(mutable.reenter){const fn=mutable.reenter;mutable.reenter=null;fn();}
   if(m.v.name==='budn')return o.v.pokemon||nil();
   if(m.v.name==='GetPokemonLevelOfCpMultiplier'){const b=obj(I32);b.v.value=25;return b;}
   if(m.v.name==='CpMultiplierAtLevel'){const b=obj('System.Single');b.v.value=args.add(8).readPointer().readS32()===25?Math.fround(.7):Math.fround(.72);return b;}
   if(m.v.name==='get_fontSize'){const b=obj(I32);b.v.value=40;return b;}
   if(m.v.name==='get_text'){const b=obj(STR);b.v.text=o.v.label||'';return b;}
   if(['get_supportRichText','get_resizeTextForBestFit','get_horizontalOverflow','get_alignment'].includes(m.v.name)){const b=obj(m.v.ret);b.v.value=0;return b;}
   if(m.v.name==='set_text'){o.v.label=args.readPointer().v.text;return nil();}
   if(['set_supportRichText','set_fontSize','set_resizeTextForBestFit','set_horizontalOverflow','set_alignment'].includes(m.v.name))return nil();
   if(m.v.name==='GetWorldLocation'){const b=obj(LATLNG);b.v.doubles=[37.8,-122.4];return b;}
   if(m.v.name==='op_Implicit'){const target=args.readPointer();const b=obj('System.Boolean');b.v.value=mutable.destroyed||target.v.destroyed?0:1;return b;}
   if(m.v.name==='get_speed'){if(mutable.getFail){ex.writePointer(str('exception'));return nil();}const b=obj(options.badBox?'System.Int32':'System.Single');b.v.value=o.v.speed;return b;}
   if(m.v.name==='set_speed'){const v=args.readPointer().readFloat();if((v===1&&mutable.restoreFail)||(v===20&&mutable.applyFail)){ex.writePointer(str('exception'));return nil();}o.v.speed=v;writes.push(v);if(v===20&&mutable.throwAfterApply)ex.writePointer(str('exception'));return nil();}
   if(m.v.name==='GetCurrentAnimatorStateInfo'||m.v.name==='GetNextAnimatorStateInfo'){assert.equal(o.v.klass,AN);assert.equal(args.readPointer().readS32(),0);if(mutable.stateFail){ex.writePointer(str('exception'));return nil();}
    const x=(m.v.name==='GetCurrentAnimatorStateInfo'?o.v.st.cur:o.v.st.next)||[0,0,0];return obj(AINFO,{m_Name:x[0],m_Path:0,m_FullPath:x[3]||0,m_NormalizedTime:x[1],m_Length:x[2],m_Speed:1,m_SpeedMultiplier:1,m_Tag:0,m_Loop:0});}
   if(m.v.name==='IsInTransition'){assert.equal(args.readPointer().readS32(),0);const b=obj('System.Boolean');b.v.value=o.v.st.trans?1:0;return b;}
   if(m.v.name==='get_runtimeAnimatorController')return o.v.controller||nil();
   if(m.v.name==='get_layerCount'||m.v.name==='get_parameterCount'){const b=obj(I32);b.v.value=m.v.name==='get_layerCount'?o.v.layers:o.v.parameters;return b;}
   if(m.v.name==='get_name'){assert([RAC,AOC].includes(o.v.klass)||o.v.klass==='Other.Controller');if(mutable.nameFail){ex.writePointer(str('exception'));return nil();}return o.v.name===null?nil():new P({klass:STR,text:o.v.name,id:++nextObject});}
   if(m.v.name==='GetBool'||m.v.name==='GetInteger'){assert.deepEqual(m.v.params,[I32],'Only the Int32 overload is invoked');const hash=args.readPointer().readS32()>>>0;paramReads.push([m.v.name,hash]);
    const b=obj(m.v.name==='GetBool'?'System.Boolean':I32);b.v.value=m.v.name==='GetBool'?(hash===H.Wait&&o.v.wait?1:0):(hash===H.NumShakes?o.v.shakes:0);return b;}
   throw Error('unexpected invocation '+m.v.name);
  }
 };
 const mod={base:new P(BASE),size:0x10000000,findExportByName:n=>{const key=n.replace('il2cpp_','');if(key==='runtime_invoke_convert_args')return str(key);assert(key in api,key);return key===options.missingExport?null:new P(exports[key]||key);}};
 let filter=null;
 const context={Process:{arch:options.arch||'arm64',pointerSize:8,getCurrentThreadId:()=>tid,findModuleByName:n=>{calls.push('module');return n==='libc.so'?{findExportByName:()=>str('clock')}:options.noModule?null:mod;},findRangeByAddress:()=>({base:new P(BASE),size:0x10000000,protection:'r-x'})},Memory:{alloc:mem,allocUtf8String:str},ptr:v=>new P(v),Uint8Array,
  NativeFunction:function(p,ret,types,opts){assert.equal(opts.scheduling,'exclusive');let key=Object.keys(exports).find(k=>exports[k]===p.v)||p.v;if(key==='clock')return(id,out)=>{assert.equal(id,7);return mutable.clockFail?1:0;};return(...args)=>{for(let i=0;i<types.length;i++)if(['bool','int','uint32'].includes(types[i]))assert(Number.isInteger(args[i]),'FFI integer arg required');calls.push(key);return api[key](...args);};},
  NativeCallback:function(fn){return fn;},CModule:function(code,symbols){assert(code.includes('get_nth_argument(ic,0)==execute_method'));filter=symbols;this.on_enter=str('filter-enter');this.on_leave=str('filter-leave');},
  Interceptor:{attach(address,callbacks){const h={address,callbacks,detached:false,detach(){this.detached=true;}};hooks.push(h);return h;}},setTimeout:(fn,delay)=>{const id=++nextTimer;timers.set(id,{fn,delay});return id;},clearTimeout:id=>timers.delete(id),console:{log:line=>{assert(!callback);events.push(JSON.parse(line.slice(line.indexOf('{'))));}}
 };
 context.Math=Object.create(Math);context.Math.random=options.random||Math.random;
 const probe=vm.runInNewContext(source,context);
 function timer(){assert(timers.size);const[id,t]=timers.entries().next().value;timers.delete(id);ms+=t.delay;t.fn();}
 function inside(fn){const old=callback;callback=true;try{return fn();}finally{callback=old;}}
 function unity(){assert(filter);inside(()=>{assert.equal(filter.enter_match(tid),1);filter.leave_match(tid);});}
 function ready(){probe.start({enabled:true});timer();assert(events.some(e=>e.kind==='metadata-ready'));unity();unity();unity();}
 function make(opts={}){const a=obj(AN);a.v.speed=opts.speed??1;a.v.st={cur:[H.Closed,0,0]};a.v.wait=false;a.v.shakes=0;a.v.layers=opts.layers??1;a.v.parameters=opts.parameters??13;
  if(!opts.nullController){const c=obj(opts.controllerClass||RAC);c.v.name=opts.controllerName??'PokeballAnimator';a.v.controller=c;}
  const hashes={...Object.fromEntries(BALL_HASHES),...opts.hashes};
  const b=obj(opts.ballClass||BALL,{animator:opts.nullAnimator?nil():a,'<HoldingBreadBall>k__BackingField':opts.bread?1:0,...hashes});const r=opts.nullResult?nil():obj(opts.responseClass||RESP,{Status:opts.status??42});return{ball:b,animator:a,response:r};}
 function begin(b,opts={}){const hook=hooks.find(h=>h.address.v===BASE+0xad58170);assert(hook&&!hook.detached);const ctx={threadId:tid,returnAddress:new P(BASE+(opts.badCaller?0xad691d4:0xad691d0))};inside(()=>hook.callbacks.onEnter.call(ctx,[b.ball]));return()=>inside(()=>hook.callbacks.onLeave.call(ctx,b.response));}
 function ball(b=make(),opts={}){begin(b,opts)();return b;}
 function caught(b){const h=hooks.find(h=>h.address.v===BASE+0xacf0a8c);inside(()=>h.callbacks.onEnter.call({threadId:tid},[obj(CAP),b.response]));}
 function iv(opts={}){const hook=hooks.find(h=>h.address.v===BASE+0xad84364),text=obj(UITEXT),plate=obj(NAMEPLATE,{cpText:text,dsqf:obj('GameMaster')}),pk=obj(PK,{cp_:1234,individualAttack_:15,individualDefense_:15,individualStamina_:15,cpMultiplier_:Math.fround(.7),additionalCpMultiplier_:0,...opts}),map=obj(WILDMAP,{eluo:obj(WILDPROTO,{EncounterId:55}),elup:opts.missingResponse?nil():obj(ENCOUNTEROUT,{Pokemon:obj(WILDPROTO,{EncounterId:opts.wrongId?66:55,Pokemon:pk})}),elus:opts.tappable?obj(TAPPABLE,{Pokemon:pk}):nil()});map.v.pokemon=obj(PK,{cp_:0,individualAttack_:0,individualDefense_:0,individualStamina_:0,cpMultiplier_:0,additionalCpMultiplier_:0});plate.v.pokemon=obj(PK,{cp_:984,individualAttack_:10,individualDefense_:11,individualStamina_:11,cpMultiplier_:.5,additionalCpMultiplier_:0});
 text.v.label=String(opts.cp_??1234);probe.setIvDisplayOptions(true,false,true);const ctx={threadId:tid};inside(()=>{hook.callbacks.onEnter.call(ctx,[plate,map,new P(0)]);hook.callbacks.onLeave.call(ctx);});return text.v.label;}
 function mapTap(opts={}){const t=hooks.find(h=>h.address.v===BASE+0x9e8307c),g=hooks.find(h=>h.address.v===BASE+0x9e87638),ctx={threadId:tid},gctx={threadId:tid};inside(()=>{
 if(!opts.outside)t.callbacks.onEnter.call(ctx,[obj(GROUND)]);
 g.callbacks.onEnter.call(gctx,[obj(MAPVIEW),new P({floats:[10,0,20]})]);
 if(opts.expire)ms+=3000;
 if(opts.change)probe.setMapTapOptions(2,2,true);
 g.callbacks.onLeave.call(gctx,new P(opts.miss?0:1));
 if(!opts.outside)t.callbacks.onLeave.call(ctx);
 });}
 function throwCall(first,second,opts={}){const h=hooks.find(h=>h.address.v===BASE+(opts.hit?0xad93280:0xad5fbdc));const args=[opts.hit?obj('WildEncounter'):make(opts).ball,str(first),new P(second)];inside(()=>h.callbacks.onEnter.call({threadId:tid},args));return [args[1].toString(),args[2].toInt32()];}
 function escaped(b){const h=hooks.find(h=>h.address.v===BASE+0xacf104c);inside(()=>h.callbacks.onEnter.call({threadId:tid},[obj(CAP),b.response]));}
 return{probe,timer,timers,unity,ready,make,ball,begin,caught,escaped,throwCall,mapTap,iv,handles,writes,paramReads,events,hooks,stats,calls,mutable,advance:n=>ms+=n,setTid:n=>tid=n};
}
async function main(){
 const slowOptions={noDomain:true},slow=fixture(slowOptions);slow.probe.start({enabled:true});
 for(let i=0;i<300;i++){slow.timer();slow.advance(1000);}
 assert(!slow.events.some(e=>e.kind==='finished'),'Slow loading must not permanently disable hooks');
 slowOptions.noDomain=false;slow.timer();assert(slow.events.some(e=>e.kind==='metadata-ready'),'Hooks recover when metadata arrives');await close(slow);

 const idle=fixture();assert.equal(idle.calls.length,0);assert.equal((await idle.probe.start()).status,'disabled');assert.equal(idle.probe.setAllowed(true),false);
 for(const options of[{arch:'x64'},{badCode:true},{badDebug:true},{badGc:true},{badField:'Status'},{badOffset:true},{enum64:true},{badParam:true},{badReturn:true},{generic:true},{missingExport:'gchandle_new'},{manyAssemblies:true},{missingLiteral:'CatchSuccess'},{nonLiteral:'CatchSuccess'}]){
  const f=fixture(options),done=f.probe.start({enabled:true});f.timer();if(f.timers.size)f.timer();const result=await done;assert.equal(result.status,'unavailable',JSON.stringify(options));assert.equal(f.writes.length,0);assert.equal(f.handles.size,0);
 }
 const off=fixture();off.ready();off.ball();assert.equal(off.writes.length,0);await close(off);
 for(const opts of[{badCaller:true},{status:0},{nullResult:true},{bread:true},{speed:0},{speed:-1},{speed:1.5},{speed:NaN},{speed:Infinity}]){
  // A reported response keeps one outcome identity handle until a Unity-thread release.
  const f=fixture();f.ready();f.probe.setAllowed(true);f.ball(f.make(opts),opts);assert.equal(f.writes.length,0,JSON.stringify(opts));assert.equal(f.handles.size,opts.badCaller||opts.nullResult?0:1);await close(f);
 }
 const wrongResult=fixture();wrongResult.ready();wrongResult.probe.setAllowed(true);wrongResult.ball(wrongResult.make({responseClass:'Other'}));wrongResult.timer();assert.equal(wrongResult.writes.length,0);assert.equal(wrongResult.handles.size,0);
 const enum42=fixture({successValue:7});enum42.ready();enum42.probe.setAllowed(true);enum42.ball(enum42.make({status:42}));assert.equal(enum42.writes.length,0);const enumBall=enum42.ball(enum42.make({status:7}));enum42.caught(enumBall);assert.deepEqual(enum42.writes,[20,1]);await close(enum42);
 const noThread=fixture();noThread.probe.start({enabled:true});noThread.timer();noThread.probe.setAllowed(true);noThread.ball();assert.equal(noThread.writes.length,0);await close(noThread);
 const proof=fixture();proof.probe.start({enabled:true});proof.timer();proof.probe.setAllowed(true);proof.unity();proof.ball();proof.unity();proof.ball();assert.equal(proof.writes.length,0);proof.unity();proof.setTid(88);proof.ball();assert.equal(proof.writes.length,0);proof.setTid(77);const pb=proof.ball();proof.caught(proof.make());assert.deepEqual(proof.writes,[20]);proof.setTid(88);proof.caught(pb);assert.equal(proof.handles.size,3);proof.setTid(77);proof.unity();proof.timer();assert.deepEqual(proof.writes,[20,1]);assert.equal(proof.handles.size,0);
 const changedThread=fixture();changedThread.ready();changedThread.probe.setAllowed(true);changedThread.setTid(88);changedThread.unity();changedThread.ball();changedThread.timer();assert.equal(changedThread.writes.length,0);assert.equal(changedThread.handles.size,0);
 const ordinary=fixture();ordinary.ready();ordinary.probe.setAllowed(true);const b=ordinary.ball();assert.deepEqual(ordinary.writes,[20]);assert.equal(ordinary.handles.size,3);ordinary.caught(b);assert.deepEqual(ordinary.writes,[20,1]);assert.equal(ordinary.handles.size,1);ordinary.timer();assert(ordinary.events.some(e=>e.kind==='speed-restored'));
 const caughtOutcome=ordinary.events.find(e=>e.kind==='outcome-confirmed');assert.equal(caughtOutcome.outcome,'caught');assert.equal(caughtOutcome.statusName,'CatchSuccess');assert.equal(caughtOutcome.tid,77);assert(Number.isFinite(caughtOutcome.boottimeMs));
 assert(ordinary.events.findIndex(e=>e.kind==='outcome-confirmed')<ordinary.events.findIndex(e=>e.kind==='speed-applied'),'Success outcome is immediate, not deferred to completion');await close(ordinary);
 for(const reason of['off','expiry','timeout']){const f=fixture();f.ready();f.probe.setAllowed(true);f.ball();if(reason==='off')f.probe.setAllowed(false);if(reason==='expiry')f.advance(2501);if(reason==='timeout'){f.advance(8001);f.probe.setAllowed(true);}assert.deepEqual(f.writes,[20]);f.unity();assert.deepEqual(f.writes,[20,1]);assert.equal(f.handles.size,0);await close(f);}
 const bg=fixture();bg.ready();bg.probe.setAllowed(true);bg.ball();let settled=false;const pending=bg.probe.stop().then(()=>settled=true);assert.strictEqual(bg.probe.stop(),bg.probe.stop());bg.timer();bg.advance(20000);bg.timer();await Promise.resolve();assert.equal(settled,false);assert.equal(bg.handles.size,3);assert(bg.hooks.every(h=>!h.detached));bg.unity();bg.timer();await pending;assert.equal(settled,true);assert.equal(bg.handles.size,0);assert(bg.hooks.every(h=>h.detached));
 const external=fixture();external.ready();external.probe.setAllowed(true);const e=external.ball();e.animator.v.speed=.75;external.caught(e);assert.deepEqual(external.writes,[20]);assert.equal(e.animator.v.speed,.75);external.timer();assert(external.events.some(e=>e.kind==='speed-external-change'));await close(external);
 const destroyed=fixture();destroyed.ready();destroyed.probe.setAllowed(true);const db=destroyed.ball();destroyed.mutable.destroyed=true;destroyed.caught(db);assert.deepEqual(destroyed.writes,[20]);assert.equal(destroyed.handles.size,1);destroyed.timer();assert(destroyed.events.some(e=>e.kind==='speed-target-destroyed'));await close(destroyed);
 const recursive=fixture();recursive.ready();recursive.probe.setAllowed(true);recursive.mutable.reenter=()=>recursive.ball();const rb=recursive.ball();assert.deepEqual(recursive.writes,[20]);recursive.caught(rb);assert.equal(recursive.handles.size,1);await close(recursive);
 const moved=fixture();moved.ready();moved.probe.setAllowed(true);const mb=moved.ball(),replacement=moved.make().response;for(const[h,o]of moved.handles)if(o===mb.response)moved.handles.set(h,replacement);mb.response=replacement;moved.caught(mb);assert.deepEqual(moved.writes,[20,1]);await close(moved);
 const failure=fixture();failure.ready();failure.probe.setAllowed(true);failure.ball();failure.mutable.restoreFail=true;const done=failure.probe.stop();failure.unity();failure.timer();assert.equal(failure.handles.size,2);assert(failure.events.some(e=>e.kind==='cleanup-failed'));assert(failure.hooks.every(h=>!h.detached));failure.mutable.restoreFail=false;failure.advance(1001);failure.unity();failure.timer();await done;assert.equal(failure.handles.size,0);assert.deepEqual(failure.writes,[20,1]);
 const failedApply=fixture();failedApply.ready();failedApply.probe.setAllowed(true);failedApply.mutable.applyFail=true;failedApply.ball();assert.equal(failedApply.handles.size,3);failedApply.unity();failedApply.timer();assert.equal(failedApply.handles.size,0);assert.equal(failedApply.writes.length,0);
 const badBox=fixture({badBox:true});badBox.ready();badBox.probe.setAllowed(true);badBox.ball();badBox.unity();badBox.timer();assert.equal(badBox.writes.length,0);assert.equal(badBox.handles.size,0);
 const partial=fixture();partial.ready();partial.probe.setAllowed(true);partial.mutable.throwAfterApply=true;partial.ball();assert.deepEqual(partial.writes,[20]);assert.equal(partial.handles.size,3);partial.unity();partial.timer();assert.deepEqual(partial.writes,[20,1]);assert.equal(partial.handles.size,0);
 const duplicate=fixture();duplicate.ready();duplicate.probe.setAllowed(true);const du=duplicate.ball();duplicate.ball(du);duplicate.ball();assert.deepEqual(duplicate.writes,[20]);duplicate.caught(du);duplicate.ball(du);assert.deepEqual(duplicate.writes,[20,1,20]);duplicate.caught(du);duplicate.timer();
 assert.equal(duplicate.events.filter(e=>e.kind==='outcome-confirmed').length,2,'Repeated polls of one response report once');await close(duplicate);
 const clock=fixture();clock.ready();clock.probe.setAllowed(true);clock.ball();clock.mutable.clockFail=true;clock.unity();clock.timer();assert.deepEqual(clock.writes,[20,1]);assert.equal(clock.handles.size,0);assert(clock.events.some(e=>e.kind==='finished'));
 const retryClock=fixture();retryClock.ready();retryClock.probe.setAllowed(true);retryClock.ball();retryClock.mutable.restoreFail=true;retryClock.mutable.clockFail=true;const rcStop=retryClock.probe.stop();retryClock.unity();retryClock.timer();assert.equal(retryClock.handles.size,2);retryClock.mutable.clockFail=false;retryClock.mutable.restoreFail=false;retryClock.advance(1001);retryClock.unity();retryClock.timer();await rcStop;assert.deepEqual(retryClock.writes,[20,1]);
 const inflight=fixture();inflight.ready();inflight.probe.setAllowed(true);const leave=inflight.begin(inflight.make());const stopped=inflight.probe.stop();inflight.timer();assert.equal(inflight.handles.size,1);leave();inflight.timer();await stopped;assert.equal(inflight.handles.size,0);assert.equal(inflight.writes.length,0);
 const outcomes=f=>f.events.filter(e=>e.kind.startsWith('outcome-'));
 const result=fixture();result.ready();
 assert.deepEqual(result.events.find(e=>e.kind==='metadata-ready').outcomes,{available:true,values:{CatchError:40,CatchSuccess:42,CatchEscape:43,CatchFlee:44,CatchMissed:45,CatchItemReplacement:46}});
 result.probe.setAllowed(true);const escape=result.ball(result.make({status:43}));assert.deepEqual(result.writes,[20]);result.escaped(result.make({status:43}));assert.deepEqual(result.writes,[20]);result.escaped(escape);result.ball(result.make({status:44}));const sb=result.ball();
 assert.deepEqual(result.writes,[20,1,20],'Escape and success accelerate; flee stays unchanged');result.caught(sb);result.timer();
 assert.deepEqual(outcomes(result).map(e=>[e.kind,e.outcome,e.statusName]),[['outcome-confirmed','broke-free','CatchEscape'],['outcome-confirmed','fled','CatchFlee'],['outcome-confirmed','caught','CatchSuccess']]);
 assert.equal(result.handles.size,3,'Reported responses stay rooted for deduplication');
 result.advance(8000);result.probe.setAllowed(true);result.unity();assert.equal(result.handles.size,0,'Expired identities are released on the Unity thread');await close(result);
 const pendingResult=fixture();pendingResult.ready();pendingResult.probe.setAllowed(true);pendingResult.ball(pendingResult.make({nullResult:true}));pendingResult.timer();
 assert.equal(outcomes(pendingResult).length,0,'Pending null response reports nothing');assert.equal(pendingResult.handles.size,0);await close(pendingResult);
 const unsure=fixture();unsure.ready();unsure.probe.setAllowed(true);for(const status of[45,40,46,99])unsure.ball(unsure.make({status}));unsure.timer();
 assert.deepEqual(outcomes(unsure).map(e=>[e.kind,e.statusName,e.reason]),[['outcome-suppressed','CatchMissed','ambiguous-status'],['outcome-suppressed','CatchError','not-confirmed'],['outcome-suppressed','CatchItemReplacement','not-confirmed'],['outcome-suppressed',null,'unknown-status']]);
 assert(outcomes(unsure).every(e=>e.outcome===undefined),'Unconfirmed statuses never carry an outcome');assert.equal(unsure.writes.length,0);await close(unsure);
 const repeat=fixture();repeat.ready();repeat.probe.setAllowed(true);const esc=repeat.make({status:43,speed:1.5}),missed=repeat.make({status:45});
 repeat.ball(esc);repeat.ball(esc);repeat.ball(esc);repeat.ball(missed);repeat.ball(missed);
 const movedEsc=repeat.make({status:43,speed:1.5}).response;for(const[h,o]of repeat.handles)if(o===esc.response)repeat.handles.set(h,movedEsc);esc.response=movedEsc;repeat.ball(esc);repeat.timer();
 assert.deepEqual(outcomes(repeat).map(e=>e.statusName),['CatchEscape','CatchMissed'],'Same response (even after a GC move) reports once');assert.equal(repeat.handles.size,2);await close(repeat);
 const bounded=fixture();bounded.ready();bounded.probe.setAllowed(true);for(let i=0;i<6;i++)bounded.ball(bounded.make({status:43,speed:1.5}));
 assert.equal(bounded.handles.size,4,'At most four outcomes per second are rooted');bounded.advance(1000);bounded.probe.setAllowed(true);bounded.ball(bounded.make({status:43,speed:1.5}));
 assert.equal(bounded.handles.size,4,'Oldest identity is released at the bound');bounded.timer();const reported=outcomes(bounded);assert.equal(reported.length,5);assert.equal(reported.at(-1).dropped,2);await close(bounded);
 const gated=fixture();gated.ready();gated.ball(gated.make({status:43}));gated.probe.setAllowed(true);gated.probe.setAllowed(false);gated.ball(gated.make({status:43}));
 gated.probe.setAllowed(true);gated.advance(2501);gated.ball(gated.make({status:43}));gated.probe.setAllowed(true);const late=gated.begin(gated.make({status:44}));gated.probe.setAllowed(false);late();
 gated.timer();assert.equal(outcomes(gated).length,0,'No outcome without a current lease');assert.equal(gated.handles.size,0);await close(gated);
 const released=fixture();released.ready();released.probe.setAllowed(true);released.ball(released.make({status:44}));assert.equal(released.handles.size,1);
 released.probe.setAllowed(false);released.unity();assert.equal(released.handles.size,0,'OFF releases outcome identities at the next Unity tick');
 released.probe.setAllowed(true);released.ball(released.make({status:44}));let stopDone=false;const stopping=released.probe.stop().then(()=>stopDone=true);released.timer();released.advance(20000);released.timer();await Promise.resolve();
 assert(!stopDone,'Background stop keeps the identity until a Unity tick');assert.equal(released.handles.size,1);released.unity();released.timer();await stopping;assert.equal(released.handles.size,0);
 for(const options of[{missingLiteral:'CatchFlee'},{nonLiteral:'CatchEscape'},{badLiteralType:'CatchMissed'},{literals:{CatchItemReplacement:43}},{literals:{CatchError:42}}]){
  const f=fixture(options);f.ready();const meta=f.events.find(e=>e.kind==='metadata-ready');assert.equal(meta.outcomes.available,false,JSON.stringify(options));assert(meta.outcomes.reason.startsWith('outcome-'));
  f.probe.setAllowed(true);f.ball(f.make({status:43,speed:1.5}));const b=f.ball();f.caught(b);f.timer();
  assert.deepEqual(f.writes,[20,1],'Speed path is independent of outcome metadata');assert.equal(outcomes(f).length,0);assert.equal(f.handles.size,0);await close(f);
 }
 const isolated=fixture();isolated.ready();isolated.probe.setAllowed(true);const ib=isolated.make();isolated.mutable.failHoldOf=ib.response;isolated.ball(ib);
 assert.deepEqual(isolated.writes,[20],'Outcome failure does not block acceleration');isolated.caught(ib);isolated.ball(isolated.make({status:43,speed:1.5}));const ib2=isolated.ball();isolated.caught(ib2);isolated.timer();
 assert.deepEqual(isolated.writes,[20,1,20,1]);assert.deepEqual(outcomes(isolated).map(e=>e.kind),['outcome-unavailable']);assert.equal(isolated.handles.size,0);
 const isolatedStop=isolated.probe.stop();isolated.unity();isolated.timer();assert.equal((await isolatedStop).status,'cancelled','Outcome failure does not fault the feature');
 const label=fixture({iv:true});label.ready();assert.equal(label.events.find(e=>e.kind==='metadata-ready').iv.available,true);
 assert.equal(label.iv(),'1234 \u00b7 Lv 25 \u00b7 <color="#ff5252">IV 100%</color>');assert.equal(label.handles.size,1,'Active IV label remains rooted until replacement or stop');
 assert.equal(label.iv({individualAttack_:0,individualDefense_:0,individualStamina_:0}),'1234 \u00b7 Lv 25 \u00b7 <color="#dedede">IV 0%</color>');
 assert.equal(label.iv({individualAttack_:14,individualDefense_:14,individualStamina_:14}),'1234 \u00b7 Lv 25 \u00b7 <color="#cda0ff">IV 93.3%</color>');
 assert.equal(label.iv({cpMultiplier_:Math.sqrt((.7*.7+.72*.72)/2)}),'1234 \u00b7 Lv 25.5 \u00b7 <color="#ff5252">IV 100%</color>');
 assert.equal(label.iv({cpMultiplier_:0,cp_:0}),'0','Keep original game label when encounter stats are missing');assert.equal(label.iv({individualAttack_:99}),'1234 \u00b7 Lv 25 \u00b7 <color="#dedede">IV --</color>');assert.equal(label.iv({cp_:321,individualAttack_:9,individualDefense_:12,individualStamina_:15}),'321 \u00b7 Lv 25 \u00b7 <color="#69d787">IV 80%</color>');
 assert.equal(label.iv({wrongId:true}),'1234');assert.equal(label.iv({missingResponse:true}),'1234');
 assert.match(label.iv({missingResponse:true,tappable:true,cp_:567}),/^567 /);
 assert.equal(label.handles.size,1,'Active IV label remains rooted until replacement or stop');await close(label);assert.equal(label.handles.size,0,'Stop releases the IV label');
 const mt=fixture({map:true});mt.ready();assert.equal(mt.events.find(e=>e.kind==='metadata-ready').mapTap.available,true);
 mt.mapTap();mt.timer();assert.equal(mt.events.filter(e=>e.kind==='map-tap').length,0);
 mt.probe.setMapTapOptions(1,1,true);mt.mapTap({outside:true});mt.mapTap({miss:true});mt.mapTap();mt.timer();
 const tap=mt.events.filter(e=>e.kind==='map-tap');assert.equal(tap.length,1);assert.deepEqual([tap[0].latitude,tap[0].longitude,tap[0].mode,tap[0].epoch],[37.8,-122.4,1,1]);assert.equal(mt.handles.size,0);
 mt.mapTap({change:true});mt.mapTap({expire:true});mt.timer();assert.equal(mt.events.filter(e=>e.kind==='map-tap').length,1);assert.equal(mt.handles.size,0);await close(mt);
 await throwTests();
 await traceTests();
 console.log('PASS quick-ball: metadata/ABI, exact caller/result, Unity-thread proof, 20x success/escape apply, completion/off/lease/timeout restore, external/destroyed ownership, failed restore, background pending cleanup, duplicate/inflight cancellation, confirmed outcome/null/ambiguous/dedup/bounds/lease/enum/isolation, read-only trace route/identity/layout/isolation/bounds/lifecycle');
}
async function throwTests(){
 let random=0,draws=0;
 const f=fixture({throws:true,random:()=>{draws++;return random;}});f.ready();assert.equal(f.events.find(e=>e.kind==='metadata-ready').throws.available,true);
 const radius=word=>{const view=new DataView(new ArrayBuffer(4));view.setUint32(0,parseInt(word.replace(/^0x/,'').padStart(16,'0').slice(0,8),16));return view.getFloat32(0);};
 const word='0x3f00000000000002'; // Great ball + ring 0.5, clean hit but outside bullseye.
 assert.deepEqual(f.throwCall(word,0),[word,0]);
 for(const [grade,low,high]of [[1,.75,.95],[2,.35,.65],[3,.10,.25]]){
  f.probe.setThrowOptions(grade,true,true);
  const values=[];
  for(const unit of [0,.5,.999999]){
   random=unit;const before=draws,submit=f.throwCall(word,0);
   assert.equal(radius(submit[0]),Math.fround(low+(high-low)*unit));
   assert.equal(submit[0].slice(-8),'00000002','Ball type preserved');assert.equal(submit[1],257);
   f.probe.setThrowOptions(grade,true,true); // Heartbeat renewal cannot resample the throw.
   assert.deepEqual(f.throwCall(...submit,{hit:true}),submit,'Hit reuses the submitted float');
   assert.equal(draws,before+1,'Exactly one random draw per paired throw');values.push(radius(submit[0]));
  }
  assert.equal(new Set(values).size,3,'Separate throws vary within each rating');
  random=.25;const submit=f.throwCall(word,0),before=draws;random=.75;
  assert.deepEqual(f.throwCall(word,0,{hit:true}),submit,'Original payload also pairs');assert.equal(draws,before);
  assert.equal(radius(f.throwCall(word,0,{hit:true})[0]),Math.fround(low+(high-low)*.75),'Unpaired hit samples independently');
 }
 f.probe.setThrowOptions(0,true,true);assert.deepEqual(f.throwCall(word,0),[word,256]);
 f.probe.setThrowOptions(2,false,true);assert.equal(f.throwCall(word,256)[1],257,'Existing curve retained');
 const beforeInvalid=draws;
 assert.deepEqual(f.throwCall(word,0x10000),[word,0x10000],'Miss stays missed');
 assert.deepEqual(f.throwCall('0x3f00000000000004',0),['0x3f00000000000004',0],'Master ball excluded');
 assert.deepEqual(f.throwCall(word,0,{bread:true}),[word,0]);
 f.setTid(88);assert.deepEqual(f.throwCall(word,0),[word,0]);f.setTid(77);
 f.advance(2501);assert.deepEqual(f.throwCall(word,0),[word,0],'Lease expiry');
 assert.equal(draws,beforeInvalid,'Miss/excluded ball/mode/thread/expiry do not sample');
 f.probe.setThrowOptions(2,false,true);random=0;
 assert.equal(radius(f.throwCall(word,0,{hit:true})[0]),Math.fround(.35),'Expired pairing cannot survive lease renewal');
 random=.25;f.throwCall(word,0);f.probe.setThrowOptions(2,false,false);f.probe.setThrowOptions(2,false,true);random=.75;
 assert.equal(radius(f.throwCall(word,0,{hit:true})[0]),Math.fround(.575),'Off clears the pending sample');
 f.probe.setThrowOptions(3,true,false);assert.deepEqual(f.throwCall(word,0),[word,0]);
 assert.equal(f.writes.length,0,'Throw choices independent of animation speed toggle');await close(f);
 const bad=fixture({throws:true,badThrowSize:true});bad.ready();assert.equal(bad.events.find(e=>e.kind==='metadata-ready').throws.available,false);assert(!bad.hooks.some(h=>h.address.v===BASE+0xad5fbdc));await close(bad);
}
async function traceTests(){
 const T=o=>fixture({trace:true,...o}),of=(f,k)=>f.events.filter(e=>e.kind===k),meta=f=>f.events.find(e=>e.kind==='metadata-ready').trace;
 const step=(f,a,st,n=16)=>{if(st)a.v.st=Array.isArray(st)?{cur:st}:st;f.advance(n);f.unity();};
 const traceInvokes=f=>f.calls.filter(c=>/^invoke:(Get|IsIn|get_(runtime|layer|parameter|name))/.test(c));
 const noAddresses=f=>assert(!/0x|obj\d/.test(JSON.stringify(f.events.filter(e=>e.kind.startsWith('trace-')))),'Trace events carry no managed addresses');
 // Missing trace classes: reported unavailable, no trace work; original paths are covered above unchanged.
 const plain=fixture();plain.ready();assert.deepEqual(meta(plain),{available:false,reason:'trace-class-missing:'+AINFO});plain.probe.setAllowed(true);const pb=plain.ball();plain.caught(pb);plain.timer();
 assert.deepEqual(plain.writes,[20,1]);assert.equal(traceInvokes(plain).length,0);assert(!plain.events.some(e=>e.kind.startsWith('trace-')));await close(plain);

 // 1. Ordinary route Closed->Drop->Bounce->ShakeWait->3Shakes->Caught->End, natural speed-1 reset, natural btdd.
 const nat=T();nat.ready();
 assert.deepEqual(meta(nat),{available:true,stateInfo:{valueSize:36,align:4,offsets:[16,20,24,28,32,36,40,44,48]},identity:{available:true},name:{available:true},params:{available:true},limits:{ms:8000,ticks:960,events:48}});
 assert.equal(nat.stats.staticReads,0,'No Pokeball static read during timer-thread metadata preparation');
 nat.probe.setAllowed(true);const nb=nat.make();nat.ball(nb);assert.deepEqual(nat.writes,[20]);assert.equal(nat.handles.size,5,'attempt 2 + outcome 1 + trace 2');assert.equal(nat.stats.staticReads,1);
 nb.animator.v.shakes=4;
 for(const st of[[H.Drop,.1,.817],[H.Drop,.5,.817],[H.Bounce,.2,1.467],[H.ShakeWait,0,0],[H.Shakes3,.5,3.633],{cur:[H.Shakes3,.99,3.633],trans:true,next:[H.Caught,0,1]},[H.Caught,.5,1],{cur:[H.Caught,.8,1],trans:true,next:[H.End,0,0]},[H.End,0,0]])step(nat,nb.animator,st);
 nb.animator.v.speed=1;step(nat,nb.animator); // eee's natural speed-1 reset, not a fixture write
 nat.caught(nb);nat.timer();
 const nid=of(nat,'trace-identity')[0];assert.equal(nid.graphApplicable,true);assert.deepEqual(nid.reasons,[]);
 assert.deepEqual([nid.controllerKind,nid.controllerClass,nid.controllerName,nid.layerCount,nid.parameterCount,nid.numShakesHash,nid.paramsRead],['base',RAC,'PokeballAnimator',1,13,H.NumShakes,true]);
 assert.deepEqual(nid.ballHashes,{Close:3535843008,Closed:57362642,Spawn:1372578019,Open:71445658,Drop:3492260380,Shake:3049724742,CodeDrivenDrop:1232599366,Fly:1808254291,End:951154001,Breakout:596303718,Wait:3722194139,ShrinkFinished:382342365,Land:137525990});
 const ns=of(nat,'trace-state');
 assert.deepEqual(ns.map(e=>e.cur.name),['Closed','Drop','Bounce','ShakeWait','3Shakes','3Shakes','Caught','Caught','End','End'],'normalizedTime alone is not a change');
 assert.deepEqual(ns.map(e=>e.next&&e.next.name),[null,null,null,null,null,'Caught',null,'End',null,null]);assert.deepEqual(ns.map(e=>e.speed),[20,20,20,20,20,20,20,20,20,1]);
 assert.deepEqual(ns.map(e=>e.numShakes),[0,4,4,4,4,4,4,4,4,4]);assert(ns.every(e=>e.wait===false&&!e.cur.unknown));assert.equal(ns[1].cur.h,H.Drop,'Hashes are unsigned');assert.equal(ns[2].prevT,.5);
 const ne=of(nat,'trace-end')[0];assert.equal(ne.reason,'btdd');assert.equal(ne.graphApplicable,true);
 assert.deepEqual(ne.visited.map(v=>v.name),['Closed','Drop','Bounce','ShakeWait','3Shakes','Caught','End']);assert.deepEqual(ne.unknown,[]);assert.equal(ne.waitSeen,false);assert.deepEqual(ne.numShakesSeen,[0,4]);
 assert.deepEqual([ne.firstEndMs,ne.speedResetMs,ne.btddMs,ne.ticks,ne.maxTickGapMs,ne.events,ne.capped,ne.dropped],[144,160,160,12,16,10,0,0]);assert.deepEqual(ne.endState,{h:H.End,name:'End',trans:false,next:null});
 assert(nat.paramReads.length&&nat.paramReads.every(([n,h])=>n==='GetBool'?h===H.Wait:h===H.NumShakes));noAddresses(nat);
 assert.deepEqual(nat.writes,[20],'Trace adds no Animator writes');assert(nat.events.some(e=>e.kind==='speed-external-change'));assert.equal(nat.handles.size,1);await close(nat);

 // 2. Wait path and an unknown hash are flagged explicitly; speed restore is unchanged.
 const wt=T();wt.ready();wt.probe.setAllowed(true);const wb=wt.make();wt.ball(wb);wb.animator.v.shakes=4;
 step(wt,wb.animator,[H.Shakes3,.99,3.633]);wb.animator.v.wait=true;step(wt,wb.animator,{cur:[H.Shakes3,1,3.633],trans:true,next:[H.Wait,0,0]});step(wt,wb.animator,[H.Wait,0,0]);step(wt,wb.animator,[12345,0,0]);
 wt.caught(wb);wt.timer();assert.deepEqual(wt.writes,[20,1]);
 const ws=of(wt,'trace-state');assert.deepEqual(ws.at(-3).next,{h:H.Wait,full:0,name:'Wait',t:0,len:0,flagged:true});assert.equal(ws.at(-2).cur.name,'Wait');assert.equal(ws.at(-2).cur.flagged,true);assert.equal(ws.at(-2).wait,true);
 assert.deepEqual(ws.at(-1).cur,{h:12345,full:0,name:null,t:0,len:0,unknown:true});
 const we=of(wt,'trace-end')[0];assert.equal(we.reason,'btdd');assert.equal(we.waitSeen,true);assert.deepEqual(we.unknown,[12345]);assert.equal(we.firstEndMs,null);assert.deepEqual(we.endState,{h:12345,name:null,trans:false,next:null});await close(wt);

 // 3. Identity mismatches make the graph inapplicable and suppress parameter reads; states are still traced.
 for(const[opts,reason,fx]of[[{hashes:{dsmp:1}},'ball-hash:End'],[{},'num-shakes-hash',{numShakesHash:1}],[{controllerName:'Other'},'controller-name'],[{parameters:12},'parameter-count'],[{layers:2},'layer-count'],
  [{controllerClass:AOC},'override-base-unverified'],[{controllerClass:'Other.Controller'},'controller-class'],[{nullController:true},'controller-none']]){
  const f=T(fx);f.ready();f.probe.setAllowed(true);const b=f.make(opts);f.ball(b);step(f,b.animator,[H.Drop,0,1]);f.caught(b);f.timer();
  const id=of(f,'trace-identity')[0];assert.equal(id.graphApplicable,false,reason);assert(id.reasons.includes(reason),reason+' '+JSON.stringify(id.reasons));assert.equal(id.paramsRead,false);assert.equal(id.paramsReason,'graph-not-applicable');
  assert.equal(f.paramReads.length,0,'No GetBool/GetInteger without a verified graph');assert(of(f,'trace-state').every(e=>e.wait===null&&e.numShakes===null));assert.equal(of(f,'trace-state').length,2);
  assert.equal(of(f,'trace-end')[0].reason,'btdd');assert.equal(of(f,'trace-end')[0].graphApplicable,false);assert.deepEqual(f.writes,[20,1]);await close(f);
 }
 const ov=T();ov.ready();ov.probe.setAllowed(true);ov.ball(ov.make({controllerClass:AOC}));ov.timer();assert.equal(of(ov,'trace-identity')[0].controllerKind,'override');assert.equal(of(ov,'trace-identity')[0].controllerClass,AOC);await close(ov);

 // 4. Required trace layout/class failures disable only the trace.
 for(const[opts,reason]of[[{infoSize:40},'trace-abi:'+AINFO],[{infoAlign:8},'trace-abi:'+AINFO],[{infoRef:true},'trace-abi:'+AINFO],[{infoOffsets:[16,20,24,28,36,32,40,44,48]},'field-mismatch:m_Length'],
  [{infoOffsets:[16,20,24,28,32,36,40,44,52]},'field-mismatch:m_Loop'],[{omitClass:AINFO},'trace-class-missing:'+AINFO],[{omitClass:I32},'trace-class-missing:System.Int32']]){
  const f=T(opts);f.ready();assert.deepEqual(meta(f),{available:false,reason},JSON.stringify(opts));
  f.probe.setAllowed(true);f.ball(f.make({status:43,speed:1.5}));const b=f.ball();assert.equal(f.handles.size,4);f.caught(b);f.timer();
  assert.deepEqual(f.writes,[20,1],'Speed is independent of trace metadata');assert.equal(of(f,'outcome-confirmed').length,2);assert(!f.events.some(e=>e.kind.startsWith('trace-')));assert.equal(traceInvokes(f).length,0);
  const r=f.probe.stop();f.unity();f.timer();assert.equal((await r).status,'cancelled');assert.equal(f.handles.size,0);assert.equal(f.stats.names,f.stats.freeNames);
 }
 // Optional parts degrade individually and are reported; nothing is inferred.
 for(const[opts,name,reason,graph]of[[{noIntOverload:true},'params','overload-missing:GetInteger',true],[{dupOverload:true},'params','overload-duplicate:GetBool',true],[{missingExport:'class_get_methods'},'params','trace-export:class_get_methods',true],
  [{missingExport:'string_chars'},'name','trace-export:string',false],[{omitClass:STR},'name','trace-class-missing:System.String',false],[{omitClass:RAC},'identity','trace-class-missing:'+RAC,false],[{staticFlags:0x1},'identity','static-mismatch:dsjv',false]]){
  const f=T(opts);f.ready();const m=meta(f);assert.equal(m.available,true,JSON.stringify(opts));assert.deepEqual(m[name],{available:false,reason});
  f.probe.setAllowed(true);const b=f.ball();step(f,b.animator,[H.Drop,0,1]);f.caught(b);f.timer();
  const id=of(f,'trace-identity')[0];assert.equal(id.graphApplicable,graph,JSON.stringify(opts));assert.equal(id.paramsRead,false);assert.equal(f.paramReads.length,0);
  if(name==='params')assert.equal(id.paramsReason,reason);if(name==='name')assert.equal(id.nameReason,reason);if(name==='identity'){assert.equal(id.identityReason,reason);assert.equal(f.stats.staticReads,0);}
  assert.equal(of(f,'trace-state').length,2);assert.equal(of(f,'trace-end')[0].reason,'btdd');assert.deepEqual(f.writes,[20,1]);await close(f);
 }
 // A managed exception reading the controller name is isolated to identity.
 const nf=T();nf.ready();nf.probe.setAllowed(true);nf.mutable.nameFail=true;const nfb=nf.ball();nf.caught(nfb);nf.timer();
 const nfi=of(nf,'trace-identity')[0];assert.equal(nfi.graphApplicable,false);assert(nfi.reasons.includes('controller-name-unavailable'));assert.equal(nfi.controllerKind,'base');assert.equal(nfi.nameReason,'Error: managed-exception');
 assert.equal(of(nf,'trace-unavailable').length,0);assert.equal(of(nf,'trace-end')[0].reason,'btdd');assert.deepEqual(nf.writes,[20,1]);await close(nf);

 // 5. A managed exception on the first state read disables the trace once; speed/outcomes continue.
 const ex=T();ex.ready();ex.probe.setAllowed(true);ex.mutable.stateFail=true;const eb=ex.ball();assert.deepEqual(ex.writes,[20]);assert.equal(ex.handles.size,3,'Failed trace releases its handles immediately');
 ex.caught(eb);const eb2=ex.ball();ex.caught(eb2);ex.timer();
 assert.deepEqual(ex.writes,[20,1,20,1]);assert.equal(of(ex,'trace-unavailable').length,1);assert.equal(of(ex,'trace-unavailable')[0].reason,'Error: managed-exception');assert.deepEqual(of(ex,'trace-end').map(e=>e.reason),['fault']);
 assert.equal(of(ex,'trace-identity').length,1);assert.equal(of(ex,'outcome-confirmed').length,2);const exStop=ex.probe.stop();ex.unity();ex.timer();assert.equal((await exStop).status,'cancelled');assert.equal(ex.handles.size,0);

 // 6. Bounds: 48 change records, tick limit and a queue quota below the outcome reserve.
 const flap=T();flap.ready();flap.probe.setAllowed(true);const fb=flap.ball(flap.make({speed:1.5}));
 for(let i=0;i<200;i++)step(flap,fb.animator,[i%2?H.Drop:H.Bounce,0,1],8);flap.timer();assert.equal(of(flap,'trace-state').length,48);
 flap.probe.setAllowed(false);flap.unity();flap.timer();const fe=of(flap,'trace-end')[0];assert.deepEqual([fe.reason,fe.events,fe.capped,fe.dropped,fe.ticks],['off',48,153,0,201]);
 assert(!flap.events.some(e=>e.kind==='finished'));await close(flap);
 const lim=T();lim.ready();lim.probe.setAllowed(true);lim.ball(lim.make({speed:1.5}));for(let i=0;i<1000;i++){lim.advance(1);lim.unity();}lim.timer();
 assert.deepEqual(of(lim,'trace-end').map(e=>[e.reason,e.ticks]),[['tick-limit',960]]);assert.equal(lim.calls.filter(c=>c==='invoke:GetCurrentAnimatorStateInfo').length,960);await close(lim);
 const quota=T();quota.ready();quota.probe.setAllowed(true);const qb=quota.ball();for(let i=0;i<70;i++)quota.ball();
 for(let i=0;i<10;i++)step(quota,qb.animator,[i%2?H.Drop:H.Bounce,0,1]);quota.timer();
 assert.equal(of(quota,'skip').length,70,'Speed events keep their reserve');assert(!quota.events.some(e=>e.kind==='finished'));assert(of(quota,'trace-skip').length<70);
 quota.caught(qb);quota.timer();const qe=of(quota,'trace-end')[0];assert.equal(qe.reason,'btdd');assert.equal(qe.dropped,10);assert(qe.sessionDropped>0);assert.deepEqual(quota.writes,[20,1]);await close(quota);

 // 7. Lifecycle ends; after each, no further trace invokes and handles are released on the Unity tick.
 for(const reason of['off','lease-expired','timeout','animator-destroyed']){
  const f=T();f.ready();f.probe.setAllowed(true);const b=f.ball(f.make({speed:1.5}));
  if(reason==='off')f.probe.setAllowed(false);if(reason==='lease-expired')f.advance(2501);if(reason==='animator-destroyed')b.animator.v.destroyed=true;
  if(reason==='timeout')for(let i=0;i<5;i++){f.advance(1700);f.probe.setAllowed(true);f.unity();}else f.unity();
  const n=f.calls.length;f.unity();f.unity();assert(!f.calls.slice(n).some(c=>c.startsWith('invoke:')),'No trace invokes after '+reason);
  f.timer();assert.deepEqual(of(f,'trace-end').map(e=>e.reason),[reason]);assert.equal(f.handles.size,reason==='animator-destroyed'?1:0,reason);await close(f);
 }
 // Background stop: with outcomes unavailable, only the trace holds the session open until a Unity tick.
 for(const reason of ['off','stop','lease-expired']){
  const f=T({missingLiteral:'CatchFlee'});f.ready();f.probe.setAllowed(true);const b=f.ball(f.make({speed:1.5}));
  if(reason==='off')f.probe.setAllowed(false);if(reason==='stop')f.probe.stop();if(reason==='lease-expired')f.advance(2501);
  const before=traceInvokes(f).length;f.caught(b);f.timer();assert.equal(traceInvokes(f).length,before,'No sampling from btdd after '+reason);
  assert.equal(of(f,'trace-end')[0].reason,reason);assert.equal(f.handles.size,0);if(reason!=='stop')await close(f);
 }
 const free=T({missingLiteral:'CatchFlee'});free.ready();free.probe.setAllowed(true);free.ball(free.make({speed:1.5}));
 let freeDone=false;const freeStop=free.probe.stop().then(()=>freeDone=true);free.mutable.failFreeOnce=true;free.unity();free.timer();await Promise.resolve();
 assert.equal(freeDone,false);assert.equal(free.handles.size,1,'Failed free retains ownership');assert.equal(of(free,'trace-end').length,0);
 assert.equal(of(free,'trace-unavailable')[0].pending,true);free.advance(1000);free.unity();free.timer();await freeStop;
 assert.equal(free.handles.size,0);assert.deepEqual(of(free,'trace-end')[0].freeFailed,['animator']);
 const bgt=T({missingLiteral:'CatchFlee'});bgt.ready();bgt.probe.setAllowed(true);bgt.ball(bgt.make({speed:1.5}));assert.equal(bgt.handles.size,2);
 let bgDone=false;const bgStop=bgt.probe.stop().then(()=>bgDone=true);bgt.timer();bgt.advance(20000);bgt.timer();await Promise.resolve();
 assert.equal(bgDone,false);assert.equal(bgt.handles.size,2);assert(bgt.hooks.every(h=>!h.detached));bgt.unity();bgt.timer();await bgStop;
 assert.equal(bgt.handles.size,0);assert.deepEqual(of(bgt,'trace-end').map(e=>e.reason),['stop']);assert(bgt.hooks.every(h=>h.detached));
 // Destroyed or special balls never start a trace.
 const dz=T();dz.ready();dz.probe.setAllowed(true);const dzb=dz.make();dzb.animator.v.destroyed=true;dz.ball(dzb);const br=dz.make({bread:true});dz.ball(br);dz.timer();
 assert.deepEqual(of(dz,'trace-skip').map(e=>e.reason),['destroyed-animator','special-ball']);assert.equal(dz.handles.size,2);assert.equal(of(dz,'trace-identity').length,0);await close(dz);

 // 8. Concurrency, re-entry and btdd threads.
 const con=T();con.ready();con.probe.setAllowed(true);const c1=con.ball(con.make({speed:1.5})),c2=con.ball();
 assert.deepEqual(con.writes,[20],'A second success still accelerates while the first is traced');con.timer();assert.deepEqual(of(con,'trace-skip').map(e=>e.reason),['concurrent-trace']);
 const handlesBefore=con.handles.size;con.mutable.reenter=()=>{con.ball();con.unity();};step(con,c1.animator,[H.Drop,0,1]);assert.deepEqual(con.writes,[20]);assert.equal(con.handles.size,handlesBefore,'Re-entry during a sample is rejected');
 con.caught(c2);assert.deepEqual(con.writes,[20,1]);assert.equal(of(con,'trace-end').length,0,'Another response does not end the trace');
 con.setTid(88);con.caught(c1);con.setTid(77);con.timer();assert.equal(of(con,'trace-end').length,0,'btdd on another thread is ignored by the trace');assert(!con.events.some(e=>e.kind==='finished'));
 con.caught(c1);con.timer();assert.deepEqual(of(con,'trace-end').map(e=>e.reason),['btdd']);await close(con);
 // With a speed attempt, wrong-thread btdd keeps its pre-existing fault; the trace then ends on the next Unity tick.
 const wrong=T();wrong.ready();wrong.probe.setAllowed(true);const wrb=wrong.ball();wrong.setTid(88);wrong.caught(wrb);wrong.setTid(77);wrong.unity();wrong.timer();
 const wf=of(wrong,'finished')[0];assert.deepEqual([wf.status,wf.reason],['unavailable','Error: completion-thread-mismatch']);assert.deepEqual(of(wrong,'trace-end').map(e=>e.reason),['stop']);assert.deepEqual(wrong.writes,[20,1]);assert.equal(wrong.handles.size,0);

 // 9. Nonstandard speed: no writes, still traced, natural reset observed.
 const nsp=T();nsp.ready();nsp.probe.setAllowed(true);const nsb=nsp.ball(nsp.make({speed:1.5}));step(nsp,nsb.animator,[H.End,0,0]);nsb.animator.v.speed=1;step(nsp,nsb.animator);nsp.caught(nsb);nsp.timer();
 assert.equal(of(nsp,'skip')[0].reason,'nonstandard-speed');const nse=of(nsp,'trace-end')[0];assert.deepEqual([nse.reason,nse.firstEndMs,nse.speedResetMs,nse.btddMs],['btdd',16,32,32]);
 assert.deepEqual(of(nsp,'trace-state').map(e=>e.speed),[1.5,1.5,1]);assert.equal(nsp.writes.length,0);noAddresses(nsp);await close(nsp);
}
async function close(f){const p=f.probe.stop();f.unity();f.timer();await p;assert.equal(f.handles.size,0);assert.equal(f.stats.ownedAttach,f.stats.ownedDetach);assert.equal(f.stats.names,f.stats.freeNames);}
main().catch(e=>{console.error(e);process.exitCode=1;});
