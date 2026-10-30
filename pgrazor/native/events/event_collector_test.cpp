// Standalone fixture includes the actual collector implementation, not a mirror.
#include "event_collector.cpp"
#include <cassert>
#include <stdexcept>
extern "C" const void *pgrazor_runtime_bootstrap_api(uint32_t){return nullptr;}
extern "C" const void *pgrazor_runtime_metadata_api(uint32_t){return nullptr;}
int pgrazor_hook(void*,void*,void**){return -1;}
namespace {
struct Obj {alignas(16) unsigned char b[2048]{};};
template<class T>void put(Obj&o,size_t at,T value){std::memcpy(o.b+at,&value,sizeof(value));}
void *ptr(Obj&o){return o.b;}
int cs,cc,cr,ce,cv,cl,ca,ck,ci,cloot,citem,cspecies,cenum,crp,cri,crpa,cria;
Obj session,list,array,controller,manual_controller,service,response,encounter,visible,key,reward,reward_array,loot,items,item_array,candy_item,rare_item,candy_box,rare_box;
void *roots[512]{};uintptr_t handle_next=1;unsigned live_roots=0;
void *fail_root_object=nullptr;
uintptr_t make_root(void *p,int pinned){if(p==fail_root_object)return 0;assert(pinned==1);assert(handle_next<512);roots[handle_next]=p;++live_roots;return handle_next++;}
void *get_root(uintptr_t h){assert(h<512);return roots[h];}
void free_root(uintptr_t h){assert(h<512&&roots[h]);roots[h]=nullptr;--live_roots;}
void *current(){return &cs;}
int updates=0,taps=0,results=0,encounters=0,visibility_calls=0;
bool nested=false,throw_result=false,immediate=false,manual_during_result=false,async_result=false,callback_queued=false;
void *queued_self=nullptr,*queued_target=nullptr,*queued_enc=nullptr;
int visibility_mutation=0;
void natural_update(void*,const void*){++updates;}
void natural_tap(void*,void*,uint8_t,const void*){++taps;}
void natural_encounter(void*,void*,void*,const void*){++encounters;}
void natural_result(void *self,void *target,void *out,const void*){
 ++results;
 if(throw_result)throw std::runtime_error("managed callback fixture");
 if(nested){nested=false;result_hook(self,ptr(manual_controller),out,nullptr);}
 if(manual_during_result){manual_during_result=false;tap_hook(self,target,0,nullptr);}
 void *enc=nullptr;assert(read_field(out,0x20,enc));
 if(async_result&&enc){queued_self=self;queued_target=target;queued_enc=enc;callback_queued=true;return;}
 if(enc)encounter_hook(self,target,enc,nullptr);
}
void *fake_invoke(const void *method,void *self,void **args,void **exc){
 *exc=nullptr;
 if(method==visible_method){assert(self==ptr(controller));++visibility_calls;
   if(visibility_mutation==1)pgr_event_collector_location(NAN,0,now_ms());
   if(visibility_mutation==2)enable_until_ms.store(0);
   if(visibility_mutation==3)put(session,0x98,uint8_t(0));
   if(visibility_mutation==4)put(controller,0x100,int64_t(1));
   if(visibility_mutation==5)put(controller,0xe8,double(NAN));
   return ptr(visible);}
 assert(method==tap_method&&self==ptr(session)&&args&&args[0]==ptr(controller));
 tap_hook(ptr(service),args[0],0,nullptr);
 if(immediate)result_hook(ptr(service),args[0],ptr(response),nullptr);
 return nullptr;
}
void string(Obj &o,const char *s){put(o,0,static_cast<const void*>(&ck));int32_t n=int32_t(std::strlen(s));put(o,0x10,n);for(int32_t i=0;i<n;++i)put(o,0x14+i*2,uint16_t(s[i]));}
void reset(){
 if(controller_root)clear_attempt();for(auto &r:owned_encounters)clear_owned(r);release(session_root);assert(live_roots==0);
 handle_next=1;
 counters={};fail_root_object=nullptr;stopped.store(false);dispatching=false;ambiguous=false;result_depth=0;encounter_scope={};next_ms=0;seen_cursor=0;std::memset(seen,0,sizeof(seen));
 session={};list={};array={};controller={};manual_controller={};service={};response={};encounter={};visible={};key={};reward={};
 put(session,0,static_cast<const void*>(&cs));put(list,0,static_cast<const void*>(&cl));put(array,0,static_cast<const void*>(&ca));
 put(controller,0,static_cast<const void*>(&cc));put(manual_controller,0,static_cast<const void*>(&cc));put(service,0,static_cast<const void*>(&ci));
 put(response,0,static_cast<const void*>(&cr));put(encounter,0,static_cast<const void*>(&ce));put(visible,0,static_cast<const void*>(&cv));put(reward,0,static_cast<const void*>(&crp));
 put(session,0x98,uint8_t(1));put(session,0x58,ptr(list));put(list,0x10,ptr(array));put(list,0x18,int32_t(1));put(array,0x18,uintptr_t(1));put(array,0x20,ptr(controller));
 put(controller,0xd0,uint64_t(100));put(manual_controller,0xd0,uint64_t(100));put(controller,0xe0,ptr(key));put(controller,0xe8,double(37));put(controller,0xf0,double(-122));put(controller,0x100,INT64_MAX);
 string(key,"TAPPABLE_TYPE_BOX");put(visible,0x10,uint8_t(1));put(response,0x10,int32_t(1));put(response,0x18,ptr(reward));put(response,0x20,ptr(encounter));
 reward_array={};loot={};items={};item_array={};candy_item={};rare_item={};candy_box={};rare_box={};
 put(reward_array,0,static_cast<const void*>(&crpa));put(reward_array,0x18,uintptr_t(1));put(reward_array,0x20,ptr(loot));put(reward,0x10,ptr(reward_array));put(reward,0x18,int32_t(1));
 put(loot,0,static_cast<const void*>(&cloot));put(loot,0x10,ptr(items));put(items,0,static_cast<const void*>(&cri));put(items,0x10,ptr(item_array));put(items,0x18,int32_t(2));
 put(item_array,0,static_cast<const void*>(&cria));put(item_array,0x18,uintptr_t(2));put(item_array,0x20,ptr(candy_item));put(item_array,0x28,ptr(rare_item));
 put(candy_item,0,static_cast<const void*>(&citem));put(candy_item,0x10,int32_t(5));put(candy_item,0x18,ptr(candy_box));put(candy_item,0x20,int32_t(4));put(candy_box,0,static_cast<const void*>(&cspecies));put(candy_box,0x10,int32_t(25));
 put(rare_item,0,static_cast<const void*>(&citem));put(rare_item,0x10,int32_t(3));put(rare_item,0x18,ptr(rare_box));put(rare_item,0x20,int32_t(1));put(rare_box,0,static_cast<const void*>(&cenum));put(rare_box,0x10,int32_t(1301));
 session_root=pin(ptr(session));pgr_event_collector_location(37,-122,now_ms());
 taps=updates=results=encounters=visibility_calls=0;nested=throw_result=immediate=manual_during_result=async_result=callback_queued=false;queued_self=queued_target=queued_enc=nullptr;visibility_mutation=0;
 assert(pgr_event_collector_tick(true,uint64_t(gettid()))==0);
}
void start(){pump(true,now_ms());assert(active_id==100&&controller_root&&service_root&&counters.inflight==1&&taps==1);}
}
int main(){
 assert(initialize_memory_copy());root_new=make_root;root_get=get_root;root_free=free_root;invoke=fake_invoke;
 binding.runtime.thread_current=reinterpret_cast<void*>(current);owner.store(uint64_t(gettid()));ready.store(true);state=0;
 session_class=&cs;controller_class=&cc;response_class=&cr;encounter_class=&ce;list_class=&cl;list_array_class=&ca;string_class=&ck;service_class=&ci;
 loot_class=&cloot;loot_item_class=&citem;species_class=&cspecies;item_class=&cenum;int32_class=&ci;reward_layout={&crp,&crpa};items_layout={&cri,&cria};
 tap_method=&ci;visible_method=&cv;original_update.store(natural_update);original_tap.store(natural_tap);original_result.store(natural_result);original_encounter.store(natural_encounter);
 reset();assert(pgr_event_collector_tick(true,uint64_t(gettid())+1)==-6);assert(taps==0);
 // Default OFF and stale/incompatible input never dispatch.
 pump(false,now_ms());assert(taps==0);
 location.at=now_ms()-6000;pump(true,now_ms());assert(taps==0);pgr_event_collector_location(37,-122,now_ms());
 string(key,"ROUTE_PIN");pump(true,now_ms());assert(taps==0);string(key,"TAPPABLE_TYPE_BOX");
 put(controller,0xe8,double(NAN));pump(true,now_ms());assert(taps==0);put(controller,0xe8,double(37));
 put(visible,0x10,uint8_t(0));pump(true,now_ms());assert(taps==0);put(visible,0x10,uint8_t(1));
 put(list,0x18,int32_t(129));pump(true,now_ms());assert(taps==0);put(list,0x18,int32_t(1));
 put(array,0x18,uintptr_t(0));pump(true,now_ms());assert(taps==0);put(array,0x18,uintptr_t(1));
 // Invalid new location clears an earlier valid anchor, instead of retaining it.
 pgr_event_collector_location(NAN,0,now_ms());pump(true,now_ms());assert(taps==0);pgr_event_collector_location(37,-122,now_ms());
 enable_until_ms.store(now_ms()-1);pump(true,now_ms());assert(taps==0);assert(pgr_event_collector_tick(true,uint64_t(gettid()))==0);
 // Reentrant managed visibility can invalidate each action gate before dispatch.
 for(int mutation=1;mutation<=5;++mutation){reset();visibility_mutation=mutation;pump(true,now_ms());assert(taps==0&&!active_id);}
 // Real live controllers retain default DespawnTimeMs=0: zero is unset,
 // while an explicit elapsed positive deadline and malformed negatives reject.
 reset();put(controller,0x100,int64_t(0));pump(true,now_ms(),true);
 assert(taps==1&&active_id==100&&counters.zero_expiry_count==1&&counters.expired_count==0);
 result_hook(ptr(service),ptr(controller),ptr(response),nullptr);
 reset();put(controller,0x100,int64_t(1));pump(true,now_ms());assert(taps==0);
 put(controller,0x100,int64_t(-1));pump(true,now_ms());assert(taps==0);
 // Any visible finite-coordinate box can be opened; distance only orders targets.
 reset();put(controller,0xe8,double(38));pump(true,now_ms());assert(taps==1&&active_id==100);
 result_hook(ptr(service),ptr(controller),ptr(response),nullptr);
 reset();put(controller,0xe8,double(38));put(visible,0x10,uint8_t(0));pump(true,now_ms());assert(taps==0&&!active_id);
 reset();
 // Tick does not invoke gameplay from ExecuteTasks; natural Update owns dispatch.
 assert(pgr_event_collector_tick(true,uint64_t(gettid()))==0&&taps==0);
 update_hook(ptr(session),nullptr);assert(updates==1&&taps==1&&active_id==100);
 // Unrelated/manual response with same ID but a different controller is untouched.
 result_hook(ptr(service),ptr(manual_controller),ptr(response),nullptr);assert(encounters==1&&active_id==100&&counters.skipped_encounters==0);
 // Owned response forwards full reward-processing callback, suppresses exact encounter.
 result_hook(ptr(service),ptr(controller),ptr(response),nullptr);
 assert(results==2&&encounters==1&&counters.completed==1&&counters.skipped_encounters==1&&!active_id&&live_roots==1);
 assert(counters.candy==5&&counters.rare_candy==3&&counters.xl_candy==0&&counters.xl_rare_candy==0);
 pump(true,now_ms()+3000);assert(taps==1); // completed-ID dedupe
 reset();start();nested=true;result_hook(ptr(service),ptr(controller),ptr(response),nullptr);
 assert(results==2&&encounters==1&&counters.skipped_encounters==1&&!encounter_scope.controller&&result_depth==0);
 // OFF cancels future work; already dispatched late response still skips its screen.
 reset();start();assert(pgr_event_collector_tick(false,uint64_t(gettid()))==0);pump(false,dispatched_ms+31000);assert(counters.timed_out&&taps==1);
 result_hook(ptr(service),ptr(controller),ptr(response),nullptr);assert(encounters==0&&counters.skipped_encounters==1&&!active_id);
 // Reentrant manual intent during owned bfvy also revokes its active scope.
 reset();start();manual_during_result=true;result_hook(ptr(service),ptr(controller),ptr(response),nullptr);assert(encounters==1&&counters.skipped_encounters==0&&!active_id);
 // Same-target manual intent revokes attribution and remains fully natural.
 reset();start();tap_hook(ptr(service),ptr(controller),0,nullptr);assert(ambiguous&&taps==2);
 result_hook(ptr(service),ptr(controller),ptr(response),nullptr);assert(encounters==1&&!active_id&&counters.skipped_encounters==0);
 // A natural tap before selection prevents this collector selecting its ID.
 reset();tap_hook(ptr(service),ptr(controller),0,nullptr);pump(true,now_ms());assert(taps==1&&!active_id);
 // Callback exceptions restore scope and release temporary roots; in-flight owner
 // remains quarantined because successful processing was never established.
 reset();start();unsigned roots_before=live_roots;throw_result=true;bool threw=false;
 try{result_hook(ptr(service),ptr(controller),ptr(response),nullptr);}catch(const std::runtime_error&){threw=true;}
 assert(threw&&live_roots==roots_before+3&&!encounter_scope.controller&&!encounter_scope.encounter&&result_depth==0&&active_id==100);
 throw_result=false;encounter_hook(ptr(service),ptr(controller),ptr(encounter),nullptr);assert(counters.skipped_encounters==1);clear_attempt();
 // Real game path: bfvy queues AnimateInteraction(Action), returns, and its
 // cay.bfvr callback invokes bfwa later. Attribution survives that boundary.
 reset();start();async_result=true;result_hook(ptr(service),ptr(controller),ptr(response),nullptr);
 assert(callback_queued&&!active_id&&encounters==0&&counters.completed==1&&live_roots==4);
 assert(pgr_event_collector_state().pending_encounters==1);
 assert(pgr_event_collector_tick(true,uint64_t(gettid()))==0);pump(true,now_ms()+4000,true);assert(taps==1&&counters.pump_reason==17);
 // OFF and even stale ownership never release a delayed owned screen to UI.
 assert(pgr_event_collector_tick(false,uint64_t(gettid()))==0);
 inspect_owned_age(owned_encounters[0].created+31000);assert(counters.timed_out&&owned_encounters[0].stale);
 encounter_hook(queued_self,queued_target,queued_enc,nullptr);callback_queued=false;
 assert(encounters==0&&counters.skipped_encounters==1&&pgr_event_collector_state().pending_encounters==0&&live_roots==1);
 // A lost animation no longer stalls unrelated boxes. Exact late ownership
 // survives both the timeout and rotation/expiry of the ordinary dedupe cache.
 reset();put(manual_controller,0xd0,uint64_t(99));
 assert(retain_owned(ptr(service),ptr(manual_controller),ptr(encounter),99));
 pump(true,now_ms(),true);assert(taps==0&&counters.pump_reason==17);
 inspect_owned_age(owned_encounters[0].created+31000);
 for(unsigned i=0;i<256;++i)remember(1000+i,now_ms()+1000);
 assert(seen_before(99,now_ms()+7200000));
 immediate=true;pump(true,now_ms(),true);
 assert(taps==1&&counters.completed==1&&counters.skipped_encounters==1&&!active_id);
 assert(pgr_event_collector_state().pending_encounters==1&&live_roots==4);
 assert(pgr_event_collector_tick(false,uint64_t(gettid()))==0);
 encounter_hook(ptr(service),ptr(manual_controller),ptr(encounter),nullptr);
 assert(encounters==0&&counters.skipped_encounters==2&&live_roots==1);
 // Bounded quarantine never evicts a delayed screen. Capacity pauses dispatch,
 // then a real callback frees a slot and collection resumes without a restart.
 reset();put(manual_controller,0xd0,uint64_t(99));
 for(unsigned i=0;i<kOwnedEncounterLimit;++i)
   assert(retain_owned(ptr(service),ptr(manual_controller),ptr(encounter),99+i*1000));
 inspect_owned_age(now_ms()+31000);pump(true,now_ms(),true);
 assert(taps==0&&counters.pump_reason==17&&live_roots==1+3*kOwnedEncounterLimit);
 assert(!retain_owned(ptr(service),ptr(manual_controller),ptr(encounter),500));
 encounter_hook(ptr(service),ptr(manual_controller),ptr(encounter),nullptr);
 assert(encounters==0&&counters.skipped_encounters==1);
 immediate=true;pump(true,now_ms(),true);
 assert(taps==1&&counters.completed==1&&counters.skipped_encounters==2);
 assert(pgr_event_collector_state().pending_encounters==kOwnedEncounterLimit-1);
 assert(live_roots==1+3*(kOwnedEncounterLimit-1));
 // Unrelated delayed presentation cannot consume the exact pending record.
 reset();start();async_result=true;result_hook(ptr(service),ptr(controller),ptr(response),nullptr);
 encounter_hook(ptr(service),ptr(manual_controller),ptr(encounter),nullptr);assert(encounters==1&&pgr_event_collector_state().pending_encounters==1);
 encounter_hook(queued_self,queued_target,queued_enc,nullptr);assert(encounters==1&&counters.skipped_encounters==1);
 // Explicit manual tap before queued callback revokes its collector intent.
 reset();start();async_result=true;result_hook(ptr(service),ptr(controller),ptr(response),nullptr);
 tap_hook(ptr(service),ptr(controller),0,nullptr);assert(pgr_event_collector_state().pending_encounters==0);
 encounter_hook(queued_self,queued_target,queued_enc,nullptr);assert(encounters==1&&counters.skipped_encounters==0&&live_roots==1);
 // Failed service rooting aborts before original gameplay/network dispatch.
 reset();fail_root_object=ptr(service);pump(true,now_ms());assert(taps==0&&!active_id&&stopped.load()&&counters.error==-48);
 // A GC failure on the exactly-owned response fails closed instead of opening.
 reset();start();fail_root_object=ptr(response);result_hook(ptr(service),ptr(controller),ptr(response),nullptr);
 assert(results==0&&encounters==0&&!active_id&&stopped.load()&&counters.error==-49);
 // Synchronous completion while invoking TapTappable does not resurrect ownership.
 reset();immediate=true;pump(true,now_ms());assert(taps==1&&!active_id&&counters.completed==1&&live_roots==1);
 reset();start();pgr_event_collector_stop();pump(true,now_ms()+40000);assert(taps==1&&counters.timed_out);
 result_hook(ptr(service),ptr(controller),ptr(response),nullptr);assert(encounters==0);assert(pgr_event_collector_tick(false,uint64_t(gettid()))==0&&live_roots==0);
 reset();assert(pgr_event_collector_tick(false,uint64_t(gettid()))==0);
 auto before_diag=pgr_event_collector_state();update_hook(ptr(session),nullptr);auto diag=pgr_event_collector_state();
 assert(diag.update_calls==before_diag.update_calls+1&&diag.owner_updates==before_diag.owner_updates+1&&diag.last_update_tid==uint64_t(gettid())&&diag.session_active==1&&diag.pump_reason==1);
 assert(pgr_event_collector_tick(true,uint64_t(gettid()))==0);put(controller,0xe8,double(38));pump(true,now_ms(),true);diag=pgr_event_collector_state();
 assert(diag.scan_calls==1&&diag.list_count==1&&diag.candidate_count==1&&diag.matched_box_count==1&&diag.visible_count==1&&diag.nearest_meters>40&&diag.pump_reason==12);
 result_hook(ptr(service),ptr(controller),ptr(response),nullptr);release(session_root);assert(live_roots==0);
 std::puts("event_collector: lifecycle, attribution, manual forwarding, nested/exception scope, late OFF, bounds and dedupe passed");
}
