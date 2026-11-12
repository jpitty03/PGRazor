#include "showcase_host.h"
#include "showcase_access.h"
#include "showcase_inventory.h"
#include "showcase_retention.h"
#include "showcase_tag_runtime.h"
#include "runtime_transport.h"
#include "shared_host.h"
#include "unity_scheduler.h"
#include "batch_evolution.h"
#include <time.h>
#include <unistd.h>
#include <algorithm>
#if defined(__ANDROID__)
#include <android/log.h>
#endif

// Reuses the existing catch transfer classifier/broker, including all of its
// favorite/shiny/IV/PvP/Keep All protections. Adopts the three handles only on 1.
extern "C" int pgr_showcase_transfer_candidate(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,int);
namespace {
using namespace pgr_showcase;
uint64_t now_ms(){timespec t{};if(clock_gettime(CLOCK_BOOTTIME,&t)||t.tv_sec<0)return 0;return uint64_t(t.tv_sec)*1000+t.tv_nsec/1000000;}
const PgrRuntimeTransportApi* transport(){return static_cast<const PgrRuntimeTransportApi*>(pgrazor_runtime_transport_api(1));}
struct Config {unsigned slots=0;uint64_t generation=0;TagOwnership owned;std::string wire;bool valid=false;};
Config read_config(){
 Config c;auto tr=transport();if(!tr)return c;PgrRuntimeTransportSample first{},last{};char wire[513]{};uint32_t n=0;
 if(tr->sample(&first)||!first.snapshot_fresh||first.fault||tr->get_string(23,wire,sizeof(wire),&n)||!parse(wire,n,c.slots))return c;
 if(c.slots){if(tr->get_string(24,wire,sizeof(wire),&n)||!parse_tag_ownership(std::string_view(wire,n),c.owned))return c;c.wire.assign(wire,n);}
 if(tr->sample(&last)||!last.snapshot_fresh||last.fault||first.snapshot_seq!=last.snapshot_seq||first.generation!=last.generation)return c;
 c.generation=first.generation;c.valid=true;return c;
}
Config config(){
 // Transport publishes whole frames concurrently. A frame boundary is not a
 // revocation if a new, internally consistent frame confirms the same rules.
 for(unsigned attempt=0;attempt<3;++attempt){auto c=read_config();if(c.valid)return c;}
 return {};
}
bool same(const Config& a,const Config& b){return a.valid&&b.valid&&a.slots==b.slots&&a.generation==b.generation&&a.wire==b.wire;}
bool permitted(void* data){return same(*static_cast<Config*>(data),config());}
uint64_t owner_generation(uint64_t owner){
 if(!owner||owner!=uint64_t(gettid()))return 0;
 auto api=static_cast<const PgrUnitySchedulerApi*>(pgrazor_unity_scheduler_api(2));PgrUnitySchedulerSnapshot s{};
 if(!api||api->snapshot(&s)||!s.installed||s.poisoned||s.owner_tid<=0||uint64_t(s.owner_tid)!=owner||api->valid(s.generation)!=1)return 0;
 return s.generation;
}
struct State {
 Retention retention;std::string account,last_status;unsigned slots=0;uint64_t transport_generation=0;
 uintptr_t promise=0;void(*free_handle)(uintptr_t)=nullptr;TagRpcOp pending_op=TagRpcOp::create;
 unsigned pending_type=0;TagMembership pending_member{};TagOwnership creation_owner{},journal_owner{};
 uint64_t since=0,next=0,created_id=0;bool cache_pending=false,store_pending=false,journal_pending=false,unknown=false;
 uint64_t allowed_release=0;const Access* release_context=nullptr;
};
thread_local State state;thread_local bool busy=false;
// Captured from the encounter host's original lease, never renewed by a
// transfer callback. A callback before the first live pulse keeps the Pokemon.
thread_local uint64_t owner_deadline=0;
void status(const std::string& text){
 if(state.last_status==text)return;auto tr=transport();if(!tr)return;
#if defined(__ANDROID__)
 __android_log_print(ANDROID_LOG_INFO,"PGRazorShowcase","%s",text.c_str());
#endif
 PgrRuntimeTransportString arg{text.data(),uint32_t(text.size())};if(tr->emit(59,nullptr,0,&arg,1)==0)state.last_status=text;
}
void drop_promise(){if(state.promise&&state.free_handle)state.free_handle(state.promise);state.promise=0;}
void reset(){drop_promise();state=State{};}
void retry_access(const char* error){
 if(state.promise&&now_ms()-state.since>=15000){drop_promise();state.unknown=true;}
 status(error);state.next=now_ms()+2000;
}
struct Work {Work(){busy=true;}~Work(){state.allowed_release=0;state.release_context=nullptr;busy=false;}};
struct Services {
 AccessRoot bag,gm,player,score,tags,prediction;
 Services()=default;
 Services(const Access& a,const PgrSharedMetadataOutput& metadata){
  bag=resolve_service(a,metadata,"Niantic.Holoholo","IPokemonBag");
  gm=reference(a,bag.h,PGR_GAME_BATCH_EVOLUTION_CPP_ERIK_NAME,"IGameMasterData");need(bool(gm),"Game Master unavailable");
  player=resolve_service(a,metadata,"Niantic.Holoholo","IPlayerService");
  score=resolve_service(a,metadata,"Niantic.Holoholo.Contests","IContestService");
  a.exact(score.h,a.b.service_class);
  prediction=new_instance(a,a.b.contest_class);
 }
 std::string account(const Access& a){AccessRoot id(a,a.call(player.h,"get_PlayerId","System.String"));return a.text(id.h);}
};
void adopt_scope(const std::string& id,const Config& c){
 if((!state.account.empty()&&state.account!=id)||state.transport_generation!=c.generation){
  // Cancelling an in-flight operation cannot assert it did not reach the server.
  const bool uncertain=state.promise||state.cache_pending||state.store_pending||state.journal_pending||state.unknown;
  const bool same_account=state.account==id,first_account=state.account.empty();
  auto catches=std::move(state.retention.waiting_catches),background=std::move(state.retention.background_catches);reset();
  if(same_account)state.unknown=uncertain;
  if(same_account||first_account){state.retention.waiting_catches=std::move(catches);state.retention.background_catches=std::move(background);}
 }else if(state.slots!=c.slots){
  // A slot change invalidates cleanup eligibility, not an already dispatched
  // tag operation. Finish confirming that operation, then compute the new
  // memberships. This avoids both replay and a permanently stopped toggle.
  auto catches=std::move(state.retention.waiting_catches),background=std::move(state.retention.background_catches);state.retention=Retention{};
  state.retention.waiting_catches=std::move(catches);state.retention.background_catches=std::move(background);
 }
 state.account=id;state.slots=c.slots;state.transport_generation=c.generation;
 if(c.owned.pending_create&&!state.journal_pending&&!state.promise&&!state.cache_pending&&!state.store_pending)
  state.unknown=true; // Prior process/session may have sent AddTag; never replay it.
}
void adopt(const Access& a,const Config& c,Services& services){adopt_scope(services.account(a),c);}
void refresh(const Access& a,Services& s){
 const auto started=now_ms();const auto previous=state.retention.winners.types;
 InventoryRequest request;request.gate=a.lease;request.gate.service=s.score.h;
 request.bag=s.bag.h;request.game_master=s.gm.h;request.prediction_reference=s.prediction.h;
 request.authorized=a.permitted;request.authorization_context=a.permission;
 std::vector<Row> rows;const char* error=nullptr;
 need(scan_inventory(request,rows,error),error?error:"Showcase inventory unavailable");
 need(s.account(a)==state.account,"Showcase account changed");a.gate();
 need(state.retention.refresh(rows,state.slots),"Invalid Showcase ranking");
#if defined(__ANDROID__)
 if(previous!=state.retention.winners.types){
  __android_log_print(ANDROID_LOG_INFO,"PGRazorShowcase","Predicted ranking: %zu Pokemon in %llu ms",rows.size(),static_cast<unsigned long long>(now_ms()-started));
  for(unsigned t=0;t<kTypes;++t)for(auto id:state.retention.winners.types[t]){
   const auto& row=state.retention.inventory.at(id);
   __android_log_print(ANDROID_LOG_INFO,"PGRazorShowcase","type=%u species=%d predicted=%.4f",t,row.species,row.scores[t]);
  }
 }
#else
 (void)started;(void)previous;
#endif
}
int count(const Access& a,uintptr_t list,int limit){int n=boxed<int>(a,a.call(list,"get_Count","System.Int32"),"System","Int32");need(n>=0&&n<=limit,"Showcase collection bound exceeded");return n;}
struct Cache {
 std::array<uint64_t,kTypes> owned{};std::vector<TagMembership> memberships;
 std::map<uint64_t,AccessRoot> tags;
};
Cache cache(const Access& a,Services& s,const Config& c,const PgrSharedMetadataOutput& md){
 s.tags=resolve_service(a,md,"Niantic.Holoholo.Tagging","ITaggingRpcService");
 AccessRoot ready(a,a.call(s.tags.h,"WaitForTagCacheInitialized","Niantic.Promises.IPromise"));
 need(ready&&a.boolean(ready.h,"completeCalled")&&!a.boolean(ready.h,"errorCalled"),"Waiting for Pokemon tags");
 int n=boxed<int>(a,a.call(s.tags.h,"get_NumberOfTags","System.Int32"),"System","Int32");need(n>=0&&n<=1000,"Showcase tag bound exceeded");
 Cache out;std::set<uint64_t> seen;
 for(int i=0;i<n;++i){
  void* args[]={&i};AccessRoot binding(a,a.call(s.tags.h,"GetCachedTagToPokemonBindingForIndex","Niantic.Holoholo.Tagging.TagToPokemonBinding",{"System.Int32"},args));
  need(bool(binding),"Tag cache changed");a.exact(binding.h,a.cls("Niantic.Holoholo.Tagging","TagToPokemonBinding"));
  auto tag=reference(a,binding.h,"TagProto","Holoholo.Rpc.PokemonTagProto");a.exact(tag.h,a.cls("Holoholo.Rpc","PokemonTagProto"));
  auto id=a.value<uint64_t>(tag.h,"Id","System.UInt64");need(id&&seen.insert(id).second,"Invalid tag cache identity");
  auto type=std::find(c.owned.ids.begin(),c.owned.ids.end(),id);
  const bool created=state.created_id==id;
  if(type==c.owned.ids.end()&&!created)continue;
  if(type!=c.owned.ids.end())out.owned[size_t(type-c.owned.ids.begin())]=id;
  auto list=reference(a,binding.h,"PokemonProtos","System.Collections.Generic.List<Holoholo.Rpc.PokemonProto>");
  const int m=count(a,list.h,int(kMaxPokemon));std::set<uint64_t> members;
  for(int j=0;j<m;++j){
   void* index[]={&j};AccessRoot pokemon(a,a.call(list.h,"get_Item","Holoholo.Rpc.PokemonProto",{"System.Int32"},index));
   a.exact(pokemon.h,a.b.pokemon_class);auto pid=a.value<uint64_t>(pokemon.h,"id_","System.UInt64");
   need(pid&&members.insert(pid).second&&out.memberships.size()<kMaxTagMemberships,"Invalid Showcase tag membership");out.memberships.push_back({pid,id});
  }
  need(count(a,list.h,int(kMaxPokemon))==m,"Tag memberships changed during scan");out.tags.emplace(id,std::move(tag));
 }
 need(boxed<int>(a,a.call(s.tags.h,"get_NumberOfTags","System.Int32"),"System","Int32")==n,"Tag cache changed during scan");a.gate();return out;
}
TagRpcRequest rpc_request(const Access& a,Services& s){
 TagRpcRequest req;req.owner=a.lease.owner;req.generation=a.lease.generation;req.deadline_ms=a.lease.deadline_ms;req.service=s.tags.h;
 req.authorized=a.permitted;req.authorization_context=a.permission;return req;
}
bool poll(const Access& a,Services& services,const Config& c){
 if(!state.promise)return false;
 auto req=rpc_request(a,services);req.op=state.pending_op;uint64_t id=0;
 const auto result=poll_tag_rpc(req,state.promise,id);
 if(result==TagPollStatus::pending||result==TagPollStatus::refused){need(now_ms()-state.since<15000,"Tag update outcome unknown; Pokemon retained");return true;}
 drop_promise();
 if(result!=TagPollStatus::created&&result!=TagPollStatus::confirmed){state.unknown=true;status("Tag update unconfirmed; Pokemon retained");return true;}
 state.cache_pending=true;
 if(result==TagPollStatus::created){
  state.created_id=id;std::string command;
  need(c.owned.revision==state.creation_owner.revision&&c.owned.ids==state.creation_owner.ids,"Tag ownership changed during creation");
  need(tag_ownership_command(c.owned,state.pending_type,id,command),"Cannot persist Showcase tag ownership");
  auto tr=transport();PgrRuntimeTransportString args[]={{"showcase-owned-tag",18},{command.data(),uint32_t(command.size())}};
  state.store_pending=true;
  need(tr&&tr->emit(19,nullptr,0,args,2)==0,"Showcase tag ownership could not be queued");
 }
 return true;
}
bool confirm(const Config& c,const Cache& observed){
 if(!state.cache_pending)return false;
 bool confirmed=false;
 if(state.pending_op==TagRpcOp::create){
  if(state.store_pending){
   const auto& before=state.creation_owner;
   bool expected=!c.owned.pending_create&&c.owned.revision==before.revision+1;
   for(unsigned i=0;i<kTypes;++i)expected=expected&&c.owned.ids[i]==(i==state.pending_type?state.created_id:before.ids[i]);
   if(expected)state.store_pending=false;
  }
  confirmed=!state.store_pending&&observed.tags.count(state.created_id);
 }else {
  const bool member=std::find(observed.memberships.begin(),observed.memberships.end(),state.pending_member)!=observed.memberships.end();
  confirmed=state.pending_op==TagRpcOp::add?member:!member;
 }
 if(confirmed){state.cache_pending=false;state.created_id=0;return false;}
 need(now_ms()-state.since<15000,"Tag cache confirmation timed out; Pokemon retained");return true;
}
bool update_tags(const Access& a,Services& services,const Config& c,const Cache& observed,const TagPlan& plan){
 auto req=rpc_request(a,services);AccessRoot pokemon;
 if(plan.create_types||c.owned.pending_create||state.journal_pending){
  if(!state.journal_pending){
   for(unsigned t=0;t<kTypes;++t)if(plan.create_types&(1u<<t)){state.pending_type=t;break;}
   std::string command;need(tag_creation_command(c.owned,state.pending_type,command),"Unconfirmed Showcase tag creation; Pokemon retained");
   state.journal_owner=c.owned;state.journal_pending=true;state.since=now_ms();
   auto tr=transport();PgrRuntimeTransportString args[]={{"showcase-owned-tag",18},{command.data(),uint32_t(command.size())}};
   need(tr&&tr->emit(19,nullptr,0,args,2)==0,"Showcase tag journal could not be queued");
   status("Saving Showcase tag creation intent");return true;
  }
  bool acknowledged=c.owned.pending_create==state.pending_type+1&&c.owned.revision==state.journal_owner.revision+1&&c.owned.ids==state.journal_owner.ids;
  if(!acknowledged){need(now_ms()-state.since<15000,"Showcase tag journal was not confirmed");return true;}
  req.type_index=state.pending_type;req.op=TagRpcOp::create;state.creation_owner=c.owned;
 }else {
  TagMembership member;
  if(!plan.add.empty()){req.op=TagRpcOp::add;member=plan.add.front();}
  else if(!plan.remove.empty()&&tag_removals_ready(plan,observed.memberships)){req.op=TagRpcOp::remove;member=plan.remove.front();}
  else return false;
  auto tag=observed.tags.find(member.tag);need(tag!=observed.tags.end(),"Owned Showcase tag missing");
  void* args[]={&member.pokemon};pokemon=AccessRoot(a,a.call(services.bag.h,"GetPokemon","Holoholo.Rpc.PokemonProto",{"System.UInt64"},args));
  req.pokemon=pokemon.h;req.pokemon_id=member.pokemon;req.tag=tag->second.h;req.tag_id=member.tag;state.pending_member=member;
 }
 need(services.account(a)==state.account,"Showcase account changed before tagging");a.gate();
 state.pending_op=req.op;state.since=now_ms();state.free_handle=a.b.release;
 auto result=start_tag_rpc(req,state.promise);
 if(result==TagStartStatus::outcome_unknown){state.journal_pending=false;state.unknown=true;status("Tag update outcome unknown; Pokemon retained");return true;}
 need(result==TagStartStatus::dispatched,"Showcase tag operation refused");state.journal_pending=false;status("Updating Showcase keeper tags");return true;
}
const PgrSharedMetadataOutput* metadata(){auto api=static_cast<const PgrSharedHostApi*>(pgrazor_shared_host_api(1));return api&&api->metadata?api->metadata():nullptr;}
ScoreRequest lease(uint64_t owner,uint64_t deadline){ScoreRequest r;r.owner=owner;r.generation=owner_generation(owner);r.deadline_ms=deadline;need(r.generation,"Showcase Unity owner unavailable");return r;}
void release_candidate(const Access& a,Services& services,const TagPlan& plan,const Cache& observed){
 int allowed=0;auto tr=transport();if(!tr||tr->get_bool(5,&allowed)||!allowed||pgr_evolution_busy())return;
 for(auto id:state.retention.release_candidates){
  if(!state.retention.release_ready(id,plan,observed.memberships))continue;
  void* args[]={&id};AccessRoot pokemon(a,a.call(services.bag.h,"GetPokemon","Holoholo.Rpc.PokemonProto",{"System.UInt64"},args));
  a.exact(pokemon.h,a.b.pokemon_class);need(a.value<uint64_t>(pokemon.h,"id_","System.UInt64")==id,"Replacement inventory ID changed");
  // Duplicate roots belong to the existing transfer front-end only on adoption.
  AccessRoot bag(a,services.bag.get()),gm(a,services.gm.get());
  state.allowed_release=id;state.release_context=&a;a.gate();
  int adopted=pgr_showcase_transfer_candidate(id,bag.h,pokemon.h,gm.h,a.lease.owner,state.retention.background_releases.count(id)?0:1);
  state.allowed_release=0;state.release_context=nullptr;
  if(adopted){bag.release();pokemon.release();gm.release();state.retention.release_candidates.erase(id);state.retention.background_releases.erase(id);}
  return;
 }
}
}

void pgr_showcase_caught(uint64_t id,uint64_t owner,bool honor_keep_all){
 if(!id||!owner_generation(owner))return;auto c=config();if(!c.valid||!c.slots)return;
 int transfer=0;auto tr=transport();
 // Tagging is useful with transfer disabled, but those catches must never
 // become a delayed storage purge when the user later enables transfer.
 if(tr&&!tr->get_bool(5,&transfer)&&transfer&&!state.retention.caught(id,honor_keep_all))state.unknown=true;
 state.next=0;
}
bool pgr_showcase_keep(uint64_t id,uint64_t owner){
 auto c=config();if(c.valid&&!c.slots)return false;
 if(!c.valid||!id||!owner_generation(owner))return true;
 if(busy){
  if(state.allowed_release!=id||!state.release_context||state.unknown)return true;
  try{state.release_context->gate();return state.retention.keep(id);}catch(...){return true;}
 }
 Work work;
 try {
  const auto* md=metadata();need(md,"Showcase service metadata unavailable");
  Access a(lease(owner,owner_deadline),permitted,&c);
  Services services(a,*md);adopt(a,c,services);
  if(state.unknown)return true;
  refresh(a,services);a.gate();
  // Catch losers and displaced keepers wait for the host's confirmed-tag
  // handoff. Only allowed_release may bypass this hold during that handoff.
  return state.retention.keep(id)||state.retention.release_candidates.count(id)!=0;
 }catch(const AccessFailure& error){status(error.text);}catch(...){status("Showcase ranking unavailable; Pokemon retained");}
 return true;
}
void pgr_showcase_tick(uint64_t owner,const PgrSharedMetadataOutput* md,uint64_t deadline){
 if(busy||!md||!owner_generation(owner))return;
 owner_deadline=deadline;
 auto c=config();if(!c.valid)return;
 int transfer=0;auto tr=transport();
 if(tr&&!tr->get_bool(5,&transfer)&&!transfer){state.retention.waiting_catches.clear();state.retention.release_candidates.clear();state.retention.background_catches.clear();state.retention.background_releases.clear();}
 if(!c.slots){if(state.promise||state.cache_pending||state.store_pending||state.journal_pending)state.unknown=true;drop_promise();state.retention=Retention{};state.slots=0;return;}
 const auto now=now_ms();if(!now||now>=deadline||now<state.next)return;
 Work work;
 try {
  Access a(lease(owner,deadline),permitted,&c);Services services(a,*md);adopt(a,c,services);
  if(state.unknown){status("Showcase tag outcome unconfirmed; Pokemon retained");state.next=now+2000;return;}
  state.next=now+750;
  if(poll(a,services,c))return;
  refresh(a,services);
  auto observed=cache(a,services,c,*md);
  if(confirm(c,observed))return;
  TagPlan plan;need(plan_tags(state.retention.winners,observed.owned,observed.memberships,plan),"Invalid Showcase tag plan");
  if(!state.retention.waiting_catches.empty()){status("Waiting for caught Pokemon inventory update");return;}
  if(update_tags(a,services,c,observed,plan))return;
  status("Keeping top "+std::to_string(c.slots)+" per type by predicted Showcase points");
  release_candidate(a,services,plan,observed);
  // Catches wake the host immediately. Idle storage does not need to be
  // rescored every tag/promise polling interval; transfer gates still rescan.
  if(state.retention.waiting_catches.empty()&&state.retention.release_candidates.empty())state.next=now_ms()+15000;
 }catch(const AccessFailure& error){
  // Ownership acknowledgements arrive asynchronously and deliberately revoke
  // this access context. Preserve the rooted promise/confirmation state and
  // resume under the next original lease; never dispatch the operation again.
  retry_access(error.text);
 }catch(...){drop_promise();state.unknown=true;status("Showcase stopped after an unexpected error; Pokemon retained");}
}
void pgr_showcase_stop(uint64_t owner){
 if(!owner||owner!=uint64_t(gettid())||busy)return;
 owner_deadline=0;
 if(state.promise||state.cache_pending||state.store_pending||state.journal_pending)state.unknown=true;
 drop_promise();state.retention=Retention{};state.next=0;
}
