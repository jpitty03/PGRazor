// Private, per-process mailbox between the host Java runtime and translated ARM hooks.
// No socket, external controller, root requirement, or persisted activation state.
(function(root){
 const readers='active autoIncubate autoTransferAllowed battleOptions battlesAllowed berriesAllowed buddyAllowed buddyBerry buddySamplingRequested cameraSteeringRequested desiredEnabled effectiveBerryPriority giftOptions giftsAllowed incubateLongest incubatorAllowed interactionDiagnosticsReady itemAccessAllowed itemRules itemTransferEnabled ivCatchEnabled ivDisplayAllowed ivOwnedEnabled latitude longitude mapPoiAllowed mapTapAllowed mapTapEpoch mapTapMode quickBallAllowed quickSpinAllowed quickSummaryAllowed rocketAllowed rocketOptions shinyScannerEnabled shinyScannerAllowed spawnAllowed spawnBooster throwAllowed throwAr throwArReady throwCurve throwGrade transferKeepAll transferKeepSpecies transferProtect vgpAllowed vgpEnabled vgpMapVisible vgpOptions'.split(' ');
 const pulses='takeCameraSampleRequest takeInteractionStopRequest takeInteractionTraceRequest'.split(' ');
 const actions='applyMapTap cameraDiagnostic configureCameraDiagnostics configureCameraSteering configureInteractionDiagnostics configureQuickBall configureQuickSummary cooldownAction embedded event interactionDiagnostic invalidateCameraHeading quickBallOutcome setAutoTransferReady setBattleStatus setBattlesReady setBerryReady setBuddyReady setBuddyStatus setCameraDiagnosticRunning setCooldownEncounter setEncounterBerry setGiftStatus setGiftsReady setIncubatorReady setIncubatorStatus setInteractionDiagnosticRunning setItemCatalog setItemReady setItemStatus setIvOwnedReady setMapPoiSnapshot setMapTapReady setQuickBallReady setQuickSpinReady setQuickSummaryReady setRocketReady setRocketStatus setShinyScannerReady setShinyScannerStatus shinyScannerFound setSpawnRange setSpawnReady setThrowArReady setThrowReady setVgpReady setVgpStatus setVgpTarget transferResult updateCameraHeadingAt vgpResult'.split(' ');
 const guarded=name=>name==='active'||name==='vgpEnabled'||name.endsWith('Allowed')||name.endsWith('Requested');
 function host({io,control,session,now=Date.now}){
  let ack=0,fault='',stopped=false;const counts=Object.fromEntries(pulses.map(n=>[n,0]));
  function tick(){if(stopped)return;try{
   const packet=io.read('events');
   if(packet&&packet.session===session&&packet.seq===ack+1){
    if(!Array.isArray(packet.calls)||packet.calls.length>128)throw Error('Invalid translated event batch');
    for(const call of packet.calls){if(!Array.isArray(call)||!actions.includes(call[0])||call.length!==2||!Array.isArray(call[1]))throw Error('Invalid translated action');}
    // Consume before dispatch: a setter exception must never repeat a gameplay action.
    ack=packet.seq;
    for(const [name,args]of packet.calls)control[name](...args);
   }
   const values={};for(const name of readers){const v=control[name]();values[name]=typeof v==='object'&&v!==null?String(v):v;}
   for(const name of pulses)if(control[name]())counts[name]++;
   io.write('state',{session,at:now(),ack,values,pulses:counts,fault});
  }catch(e){fault=String(e);try{io.write('state',{session,at:now(),ack,values:{},pulses:counts,fault});}catch(_){}control.event('translated-bridge-error',fault);stopped=true;}}
  return {tick,stop(){stopped=true;io.write('state',{session,at:now(),ack,values:{},pulses:counts,fault:'Host stopped'});}};
 }
 function client({io,session,now=Date.now}){
  let state=null,seq=0,pending=null,queue=[],stopped=false;const consumed={};
  const fresh=()=>!stopped&&state&&!state.fault&&now()-state.at>=0&&now()-state.at<=500;
  const control={renewLease(){},disconnect(){},connect(){}};
  for(const name of readers)control[name]=()=>{if(!fresh()){if(guarded(name))return false;throw Error('Translated control snapshot unavailable');}return state.values[name];};
  for(const name of pulses)control[name]=()=>{if(!fresh())return false;const count=state.pulses[name]||0;if(count>(consumed[name]||0)){consumed[name]=count;return true;}return false;};
  for(const name of actions)control[name]=(...args)=>{if(stopped)return;if(queue.length>=2048)throw Error('Translated event queue full');queue.push([name,args]);};
  function tick(){if(stopped)return false;const next=io.read('state');if(next&&next.session===session)state=next;
   if(pending&&state&&state.ack===pending.seq)pending=null;
   if(!pending&&queue.length){pending={session,seq:++seq,calls:queue.splice(0,128)};io.write('events',pending);}
   return fresh();
  }
  return {control,tick,fresh,stop(){stopped=true;queue=[];pending=null;}};
 }
 function files(directory){
  const rename=new NativeFunction(Module.getGlobalExportByName('rename'),'int',['pointer','pointer']);
  return {read(name){try{const text=File.readAllText(directory+'/'+name+'.json');if(text.length>1048576)throw Error('Mailbox exceeds limit');return JSON.parse(text);}catch(e){if(String(e).includes('No such file'))return null;throw e;}},
   write(name,value){const text=JSON.stringify(value);if(text.length>1048576)throw Error('Mailbox exceeds limit');const path=directory+'/'+name+'.json',temp=path+'.tmp';File.writeAllText(temp,text);if(rename(Memory.allocUtf8String(temp),Memory.allocUtf8String(path))!==0)throw Error('Mailbox rename failed');}};
 }
 // Houdini maps guest executable segments read-only in the host. Validate the
 // ELF's PF_X segments as well as the live readable mapping, never arbitrary data.
 function installCodeValidation(){
  if(Process.arch!=='arm64')throw Error('Guest code validation requires ARM64');
  const cache=new Map();
  root.PGRazorGuestCode=function(module,address,size,range){try{
   if(!Number.isSafeInteger(size)||size<=0||!range||!range.protection.includes('r')||range.protection.includes('w')||!range.file||range.file.path!==module.path)return false;
   if(address.compare(module.base)<0||address.add(size).compare(module.base.add(module.size))>0||address.add(size).compare(range.base.add(range.size))>0)return false;
   const key=module.base.toString();let segments=cache.get(key);
   if(!segments){
    const base=module.base;
    if(base.readU32()!==0x464c457f||base.add(4).readU8()!==2||base.add(5).readU8()!==1||base.add(18).readU16()!==183)return false;
    const offset=base.add(32).readU64().toNumber(),stride=base.add(54).readU16(),count=base.add(56).readU16();
    if(stride!==56||count<1||count>128||offset<64||offset+count*stride>65536)return false;
    segments=[];
    for(let i=0;i<count;i++){
     const ph=base.add(offset+i*stride);if(ph.readU32()!==1||(ph.add(4).readU32()&1)===0)continue;
     const start=ph.add(16).readU64().toNumber(),length=ph.add(40).readU64().toNumber();
     if(!Number.isSafeInteger(start)||!Number.isSafeInteger(length)||start<0||length<=0||start+length>module.size)return false;
     segments.push([base.add(start),base.add(start+length)]);
    }
    cache.set(key,segments);
   }
   return segments.some(([start,end])=>address.compare(start)>=0&&address.add(size).compare(end)<=0);
  }catch(_){return false;}};
 }
 root.PGRazorTranslated=Object.freeze({host,client,files,readers,actions,installCodeValidation});
})(globalThis);
