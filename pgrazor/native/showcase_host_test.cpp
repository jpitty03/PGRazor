// Executes the production host's permission/journal/promise phases. Managed
// access graphs are covered by the separate moving-GC inventory/tag fixtures.
#include "showcase_inventory.h"
#include "showcase_tag_runtime.h"
#include "showcase_bindings.h"
#include "runtime_transport.h"
#include "runtime_bootstrap.h"
#include "runtime_metadata.h"
#include "unity_scheduler.h"
#include <time.h>
#include <unistd.h>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>
namespace fixture {
uint64_t now=100,generation=7;int tid=71,freed=0,rpcs=0;bool fresh=true,bootstrap_ok=false;
bool transfer_allowed=true;
std::string config="S1|0",ownership="T1|0";std::vector<std::string> events;
pgr_showcase::TagPollStatus poll_result=pgr_showcase::TagPollStatus::pending;
int clock(int,timespec* t){t->tv_sec=now/1000;t->tv_nsec=(now%1000)*1000000;return 0;}
int get_tid(){return tid;}
uint64_t boottime(){return now;}
bool owner(uint64_t o,uint64_t g){return o==71&&tid==71&&g==generation;}
void free_handle(uintptr_t h){assert(h==99);++freed;}
int scheduler_snapshot(PgrUnitySchedulerSnapshot* s){*s={generation,1,71,1,0,0};return 0;}
int scheduler_valid(uint64_t g){return g==generation;}
int sample(PgrRuntimeTransportSample* s){*s={};s->snapshot_fresh=fresh;s->snapshot_seq=1;s->generation=1;return 0;}
int get_string(uint32_t index,char* out,uint32_t cap,uint32_t* length){const auto& s=index==23?config:ownership;if(s.size()>cap)return -1;std::memcpy(out,s.data(),s.size());*length=uint32_t(s.size());return 0;}
int get_bool(uint32_t,int* out){*out=transfer_allowed?1:0;return 0;}
int emit(uint32_t op,const int64_t*,uint32_t,const PgrRuntimeTransportString* args,uint32_t n){
 if(op==19){assert(n==2&&std::string(args[0].ptr,args[0].len)=="showcase-owned-tag");events.emplace_back(args[1].ptr,args[1].len);}return 0;
}
int bootstrap(PgrBootBinding* b){*b={};return bootstrap_ok?0:-1;}
void* symbol(void*,const char*){return nullptr;}
}
extern "C" const void* pgrazor_runtime_bootstrap_api(uint32_t){static PgrBootApi a{};a.snapshot=fixture::bootstrap;return &a;}
extern "C" const void* pgrazor_runtime_metadata_api(uint32_t){static PgrMetaApi a{};return &a;}
extern "C" const void* pgrazor_runtime_transport_api(uint32_t){static PgrRuntimeTransportApi a{};a.sample=fixture::sample;a.get_string=fixture::get_string;a.get_bool=fixture::get_bool;a.emit=fixture::emit;return &a;}
extern "C" const void* pgrazor_unity_scheduler_api(uint32_t){static PgrUnitySchedulerApi a{};a.snapshot=fixture::scheduler_snapshot;a.valid=fixture::scheduler_valid;return &a;}
extern "C" const void* pgrazor_shared_host_api(uint32_t){return nullptr;}
extern "C" int pgr_showcase_transfer_candidate(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,int){assert(false);return 0;}
bool pgr_evolution_busy(){return false;}
namespace pgr_showcase {
bool resolve_live_score_bindings(const ScoreRequest&,ScoreBindings& b){b={};b.boottime_ms=fixture::boottime;b.owner_valid=fixture::owner;return true;}
bool scan_inventory(const InventoryRequest&,std::vector<Row>&,const char*&){assert(false);return false;}
TagStartStatus start_tag_rpc(const TagRpcRequest&,uintptr_t&){++fixture::rpcs;return TagStartStatus::refused;}
TagPollStatus poll_tag_rpc(const TagRpcRequest&,uintptr_t p,uint64_t& id){assert(p==99);id=fixture::poll_result==TagPollStatus::created?700:0;return fixture::poll_result;}
}
#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME 7
#endif
#define clock_gettime fixture::clock
#define gettid fixture::get_tid
#define dlsym fixture::symbol
#include "showcase_host.cpp"
#undef clock_gettime
#undef gettid
#undef dlsym
int main(){
 using namespace pgr_showcase;
 for(int i=0;i<18;++i)fixture::ownership+="|0";
 assert(!pgr_showcase_keep(123,71)); // configured off, independent of services
 fixture::fresh=false;assert(pgr_showcase_keep(123,71));fixture::fresh=true;
 fixture::config="S1|3";fixture::ownership="T0";assert(pgr_showcase_keep(123,71));
 fixture::ownership="T1|0";for(int i=0;i<18;++i)fixture::ownership+="|0";
 pgr_showcase_caught(123,71);assert(state.retention.waiting_catches.count(123));
 fixture::transfer_allowed=false;pgr_showcase_caught(124,71);assert(!state.retention.waiting_catches.count(124)&&state.next==0);
 fixture::transfer_allowed=true;
 fixture::tid=72;pgr_showcase_caught(456,71);assert(!state.retention.waiting_catches.count(456));fixture::tid=71;
 assert(pgr_showcase_keep(123,71)); // missing services keep; never fake a valid rank
 reset();fixture::bootstrap_ok=true;auto c=config();Access a(lease(71,2600),permitted,&c);Services services;
 Cache observed;TagPlan plan;plan.create_types=1u<<9;
 assert(update_tags(a,services,c,observed,plan));assert(state.journal_pending&&fixture::rpcs==0);
 assert(fixture::events.back()=="B1|0|9|0");
 assert(update_tags(a,services,c,observed,plan));assert(fixture::rpcs==0&&fixture::events.size()==1); // no replay before durable ack
 fixture::ownership="T2|1|10";for(int i=0;i<18;++i)fixture::ownership+="|0";c=config();
 // Poll the first operation after a simulated durable begin ack and dispatch.
 state.journal_pending=false;state.creation_owner=c.owned;state.pending_type=9;state.pending_op=TagRpcOp::create;state.promise=99;state.free_handle=fixture::free_handle;state.since=100;
 assert(poll(a,services,c)&&state.promise==99&&fixture::freed==0);
 retry_access("Showcase session or lease changed");
 assert(state.promise==99&&!state.unknown&&fixture::rpcs==0&&fixture::freed==0);
 fixture::poll_result=TagPollStatus::created;
 assert(poll(a,services,c)&&!state.promise&&fixture::freed==1&&state.store_pending&&state.cache_pending);
 assert(fixture::events.back()=="T1|1|9|0|700");
 retry_access("Showcase session or lease changed");
 assert(state.cache_pending&&state.store_pending&&!state.unknown&&fixture::rpcs==0);
 observed.tags.emplace(700,AccessRoot{});assert(confirm(c,observed)); // cache alone cannot grant ownership
 c.owned.revision=2;c.owned.pending_create=0;c.owned.ids[9]=700;
 assert(!confirm(c,observed)&&!state.cache_pending&&!state.store_pending);
 state.pending_op=TagRpcOp::add;state.pending_member={123,700};state.cache_pending=true;
 assert(confirm(c,observed));observed.memberships.push_back({123,700});assert(!confirm(c,observed));
 state.pending_op=TagRpcOp::remove;state.cache_pending=true;assert(confirm(c,observed));observed.memberships.clear();assert(!confirm(c,observed));
 state.cache_pending=true;observed.memberships.push_back({123,700});fixture::now=15101;
 try{confirm(c,observed);assert(false);}catch(const AccessFailure&){}
 reset();fixture::now=100;fixture::ownership="T1|0";for(int i=0;i<18;++i)fixture::ownership+="|0";c=config();
 Row winner;winner.id=1;winner.created=1;winner.species=1;winner.types=1;winner.scores[0]=900;
 Row loser=winner;loser.id=2;loser.scores[0]=800;assert(state.retention.refresh({winner,loser},1));
 busy=true;assert(pgr_showcase_keep(2,71)); // reentry cannot release arbitrary IDs
 state.allowed_release=2;state.release_context=&a;assert(!pgr_showcase_keep(2,71));assert(pgr_showcase_keep(1,71));
 fixture::now=2600;assert(pgr_showcase_keep(2,71));busy=false;state.release_context=nullptr;state.allowed_release=0;
 owner_deadline=2600;state.promise=99;state.free_handle=fixture::free_handle;pgr_showcase_stop(71);assert(!state.promise&&state.unknown&&fixture::freed==2&&!owner_deadline);
 reset();state.promise=99;state.free_handle=fixture::free_handle;state.since=100;fixture::now=15100;
 retry_access("Showcase session or lease changed");assert(!state.promise&&state.unknown&&fixture::freed==3);
 reset();fixture::now=100;c=config();state.account="account-a";state.transport_generation=c.generation;state.slots=3;
 state.promise=99;state.free_handle=fixture::free_handle;state.retention.release_candidates.insert(123);
 c.slots=1;adopt_scope("account-a",c);assert(state.promise==99&&!state.unknown&&state.slots==1&&state.retention.release_candidates.empty());
 drop_promise();assert(fixture::freed==4);
 std::puts("PASS Showcase host phases: disabled/stale permissions, owner, durable intent, cache/ownership acknowledgement and pending membership");
}
