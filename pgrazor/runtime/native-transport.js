// Transitional scalar adapter to compiled feature52. No file/JSON transport, queue or lease policy.
(function(root){
 const SCHEMA={"BOOL_READERS":["accountManagementAllowed","adventureSyncDismissAllowed","gameRoutesAllowed","active","autoIncubate","autoTransferAllowed","battlesAllowed","berriesAllowed","buddyAllowed","buddySamplingRequested","cameraSteeringRequested","desiredEnabled","giftsAllowed","incubateLongest","incubatorAllowed","interactionDiagnosticsReady","itemAccessAllowed","itemTransferEnabled","ivCatchEnabled","ivDisplayAllowed","ivOwnedEnabled","mapPoiAllowed","mapTapAllowed","quickBallAllowed","quickSpinAllowed","quickSummaryAllowed","rocketAllowed","spawnAllowed","spawnBooster","throwAllowed","throwAlwaysHit","throwAlwaysHitReady","throwAr","throwArReady","throwCurve","transferKeepAll","vgpAllowed","vgpEnabled","vgpMapVisible"],"INT_READERS":["battleOptions","buddyBerry","giftOptions","itemCatalogMode","mapTapEpoch","mapTapMode","rocketOptions","throwGrade","transferProtect","vgpOptions"],"DOUBLE_READERS":["latitude","longitude"],"STRING_READERS":["accountCommand","gameRoutesCommand","effectiveBerryPriority","itemRules","transferKeepSpecies"],"PULSE_READERS":["takeCameraSampleRequest","takeInteractionStopRequest","takeInteractionTraceRequest"],"ACTIONS":["accountIdentity:String,String:void","setAccountsReady:boolean:void","accountSessionEnded::void","accountLogoutComplete::void","accountLogoutFailed::void","accountSignInResetComplete::void","accountSessionSaved::void","setGameRoutesReady:boolean,String:void","setGameRoutesSnapshot:String:void","gameRoutesAction:String:void","applyMapTap:double,double,int,int,double:void","cameraDiagnostic:String:void","configureCameraDiagnostics:boolean:void","configureCameraSteering:boolean:void","configureInteractionDiagnostics:boolean:void","configureQuickBall:boolean:void","configureQuickSummary:boolean:void","cooldownAction:String:void","embedded:String:void","event:String,String:void","interactionDiagnostic:String:void","invalidateCameraHeading:String:void","quickBallOutcome:String:boolean","setAutoTransferReady:boolean,String:void","setBattleStatus:String:void","setBattlesReady:boolean,String:void","setBerryReady:boolean:void","setBuddyReady:boolean,String:void","setBuddyStatus:String:void","setCameraDiagnosticRunning:boolean:void","setCooldownEncounter:String:void","setEncounterBerry:boolean,boolean:void","setGiftStatus:String:void","setGiftsReady:boolean,String:void","setIncubatorReady:boolean,String:void","setIncubatorStatus:String:void","setInteractionDiagnosticRunning:boolean:void","setItemCatalog:String:void","setItemReady:boolean,String:void","setItemStatus:String:void","setIvOwnedReady:boolean:void","setMapPoiSnapshot:String:void","setMapTapReady:boolean:void","setQuickBallReady:boolean,String:void","setQuickSpinReady:boolean,String:void","setQuickSummaryReady:boolean,String:void","setRocketReady:boolean,String:void","setRocketStatus:String:void","setSpawnRange:double,double:void","setSpawnReady:boolean:void","setThrowArReady:boolean:void","setThrowAlwaysHitReady:boolean:void","setThrowReady:boolean:void","setVgpReady:boolean,String:void","setVgpStatus:String:void","setVgpTarget:boolean,double,double,boolean:void","transferResult:String:void","updateCameraHeadingAt:double,int,boolean,long:void","vgpResult:String,boolean:void"]};

 root.PGRazorNativeTransport={schema:SCHEMA,create(){
  const table=root.PGRazorNativeFeature(52,1,96);
  // Every feature52 call is bounded and callback-free. Keep the JS lock while
  // using shared scalar/string buffers; cooperative FFI could race another hook.
  const fn=(i,args)=>new NativeFunction(table.add(8+i*8).readPointer(),'int',args,{exceptions:'propagate',scheduling:'exclusive'});
  const emit=fn(2,['uint32','pointer','uint32','pointer','uint32']),getBool=fn(3,['uint32','pointer']),getInt=fn(4,['uint32','pointer']),
   getDouble=fn(5,['uint32','pointer','pointer']),getString=fn(6,['uint32','pointer','uint32','pointer']),
   getCommand=fn(7,['pointer','uint32','pointer','pointer','uint32','pointer']),pulse=fn(8,['uint32','pointer']),sample=fn(9,['pointer']);
  const scalar=Memory.alloc(8),present=Memory.alloc(4),length=Memory.alloc(4),length2=Memory.alloc(4),
   text=Memory.alloc(1048576),text2=Memory.alloc(1048576),snap=Memory.alloc(88);
  let stopped=false;
  function utf8(pointer,size){
   if(!Number.isSafeInteger(size)||size<0||size>1048576)throw Error('Compiled string length');
   const raw=new Uint8Array(pointer.readByteArray(size));let binary='';
   for(let i=0;i<raw.length;i++)binary+=String.fromCharCode(raw[i]);
   return decodeURIComponent(escape(binary));
  }
  function checked(rc){if(stopped||rc!==0){const e=Error('Compiled control snapshot unavailable:'+rc);e.code='PGRAZOR_TRANSLATED_SNAPSHOT_UNAVAILABLE';throw e;}}
  const control={connect(){},disconnect(){},renewLease(){}};
  SCHEMA.BOOL_READERS.forEach((name,i)=>control[name]=()=>{checked(getBool(i,scalar));return scalar.readS32()!==0;});
  SCHEMA.INT_READERS.forEach((name,i)=>control[name]=()=>{checked(getInt(i,scalar));return scalar.readS32();});
  SCHEMA.DOUBLE_READERS.forEach((name,i)=>control[name]=()=>{checked(getDouble(i,scalar,present));return present.readS32()?scalar.readDouble():NaN;});
  SCHEMA.STRING_READERS.forEach((name,i)=>control[name]=()=>{checked(getString(i,text,1048576,length));return utf8(text,length.readU32());});
  control.accountCommandSnapshot=()=>{checked(getCommand(text,1048576,length,text2,1048576,length2));return[utf8(text,length.readU32()),utf8(text2,length2.readU32())];};
  SCHEMA.PULSE_READERS.forEach((name,i)=>control[name]=()=>{if(stopped)return false;checked(pulse(i,scalar));return scalar.readS32()!==0;});
  SCHEMA.ACTIONS.forEach((spec,opcode)=>{const [name,signature]=spec.split(':'),types=signature?signature.split(','):[];
   control[name]=(...args)=>{
    if(stopped)return;if(args.length!==types.length)throw Error('Compiled action arity:'+name);
    const scalars=Memory.alloc(40),strings=Memory.alloc(32),owned=[];let ns=0,nt=0;
    types.forEach((type,i)=>{const value=args[i];
     if(type==='String'){
      if(typeof value!=='string')throw Error('Compiled action string:'+name);
      // encodeURIComponent rejects malformed UTF16; compute bytes without truncating embedded NUL.
      const encoded=unescape(encodeURIComponent(value)),bytes=encoded.length;if(bytes>1048576)throw Error('Compiled action string length');
      // Frida17.24.2 allocUtf8String uses g_strdup and truncates embedded NUL.
      // Allocate/write explicit bytes because the native ABI carries a length.
      const raw=new Uint8Array(bytes+1);for(let j=0;j<bytes;j++)raw[j]=encoded.charCodeAt(j);
      const p=Memory.alloc(bytes+1);p.writeByteArray(raw);owned.push(p);strings.add(nt*16).writePointer(p);strings.add(nt*16+8).writeU32(bytes);strings.add(nt*16+12).writeU32(0);nt++;
     }else{
      const at=scalars.add(ns++*8);
      if(type==='boolean'){if(typeof value!=='boolean')throw Error('Compiled action boolean');at.writeS64(value?1:0);}
      else if(type==='double'){if(typeof value!=='number'||!Number.isFinite(value))throw Error('Compiled action double');at.writeDouble(value);}
      else{if(!Number.isSafeInteger(value)||(type==='int'&&(value<-2147483648||value>2147483647)))throw Error('Compiled action integer');at.writeS64(value);}
     }
    });
    const rc=emit(opcode,scalars,ns,strings,nt);if(rc!==0)throw Error('Compiled action rejected:'+name+':'+rc);
   };
  });
  const fresh=()=>!stopped&&sample(snap)===0&&snap.add(80).readU32()===1&&snap.add(84).readU32()===0;
  return {control,fresh,tick:fresh,stop(){stopped=true;}};
 }};
})(globalThis);
