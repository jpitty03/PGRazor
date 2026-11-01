// Compile this TU with gym_automation.cpp and -ldl. Exercises actual private poll/permission state.
#include <cassert>
#include <cstdio>
#include <map>
#include "remote_gym.cpp"
namespace {
std::map<uintptr_t,void*> test_roots;uintptr_t next_root=1;
unsigned allocations=0,frees=0,polls=0,bootstraps=0,events=0;
int object_tokens[4]{};int poll_result=PGR_VR_PENDING;
bool fresh=true,changing=false,coordinates=true,active_flag=true,ready_flag=true,hold=false;
unsigned samples=0;double latitude=37,longitude=-122;
std::string config="1|123|1|10|20|1|30|0",last_event;
pgr_gym::History shared_history;
uintptr_t root_new(void *p,int){assert(p);auto h=next_root++;test_roots[h]=p;++allocations;return h;}
void *root_get(uintptr_t h){return test_roots.at(h);}
void root_free(uintptr_t h){assert(test_roots.erase(h)==1);++frees;}
int sample(PgrRuntimeTransportSample *s){*s={};s->snapshot_fresh=fresh;s->snapshot_seq=100+(changing?samples:0);++samples;return 0;}
int get_string(uint32_t f,char *p,uint32_t cap,uint32_t *n){assert(f==12&&cap>config.size());*n=config.size();memcpy(p,config.data(),*n);return 0;}
int get_bool(uint32_t f,int *v){assert(f==3||f==16);*v=f==3?active_flag:ready_flag;return 0;}
int get_double(uint32_t f,double *v,int *has){assert(f<2);*has=coordinates;*v=f==0?latitude:longitude;return 0;}
int emit(uint32_t op,const int64_t*,uint32_t scalar_count,const PgrRuntimeTransportString *s,uint32_t n){assert(op==19&&!scalar_count&&n==2);last_event.assign(s[1].ptr,s[1].len);++events;return 0;}
int poll_promise(void *p,uint32_t phase,uint64_t tid,PgrVgpResultsResult *out){assert(p==&object_tokens[0]&&phase==1&&tid==owner);++polls;*out={};return poll_result;}
int bootstrap(PgrBootBinding*){++bootstraps;return -1;}
PgrRuntimeTransportApi transport{};PgrVgpResultsApi results{};PgrBootApi bootstrap_api{};
void pending_setup(){
 assert(test_roots.empty());session=Session{};shared_history=pgr_gym::History{};
 owner=gettid();session.active=true;session.command.token=123;session.command.mode=1;
 session.account="account-a";session.targets.push_back({"gym",42,42});
 session.promise=Root(&object_tokens[0]);session.reflection=Root(&object_tokens[1]);session.bag=Root(&object_tokens[2]);
 session.uncertain=true;
 session.since=clock_ms();session.started=session.since;polls=events=0;poll_result=PGR_VR_PENDING;hold=true;
 assert(shared_history.feed_attempt(42,session.since));
}
void cleared(){assert(!session.active&&!session.promise&&!session.reflection&&test_roots.empty());assert(allocations==frees&&!hold&&finished_token==123);}
}
void pgr_gym_remote_hold(bool value){hold=value;}
pgr_gym::History *pgr_gym_remote_history(const std::string &account){assert(account=="account-a");return &shared_history;}
extern "C" const void *pgrazor_runtime_transport_api(uint32_t v){assert(v==1);return &transport;}
extern "C" const void *pgrazor_vgp_results_api(uint32_t v){assert(v==1);return &results;}
extern "C" const void *pgrazor_runtime_bootstrap_api(uint32_t v){assert(v==1);return &bootstrap_api;}
extern "C" const void *pgrazor_runtime_metadata_api(uint32_t){assert(false&&"poll fixtures must not resolve managed metadata");return nullptr;}
int main(){
 transport.sample=sample;transport.get_string=get_string;transport.get_bool=get_bool;transport.get_double=get_double;transport.emit=emit;
 results.poll=poll_promise;bootstrap_api.snapshot=bootstrap;
 boot.runtime.gchandle_new=reinterpret_cast<void*>(root_new);boot.runtime.gchandle_get_target=reinterpret_cast<void*>(root_get);boot.runtime.gchandle_free=reinterpret_cast<void*>(root_free);
 auto p=permission();assert(p.valid&&p.allowed&&p.command.token==123);
 fresh=false;assert(!permission().valid);fresh=true;changing=true;assert(!permission().valid);changing=false;
 coordinates=false;assert(permission().valid&&!permission().allowed);coordinates=true;
 latitude=91;assert(!permission().allowed);latitude=37;longitude=181;assert(!permission().allowed);longitude=-122;
 active_flag=false;assert(!permission().allowed);active_flag=true;ready_flag=false;assert(!permission().allowed);ready_flag=true;
 config="1|123|1|0|0|0|0|2";assert(!permission().valid);config="1|123|1|10|20|1|30|0";
 pending_setup();session.canceled=true;poll(shared_history);
 assert(session.active&&session.promise&&polls==1&&events==0&&session.berries==0); // cancellation cannot replay/drop submitted work
 poll_result=PGR_VR_REJECTED;poll(shared_history);cleared();assert(polls==2&&last_event.find("\"berries\":0")!=std::string::npos);
 assert(!shared_history.feed_allowed(42,clock_ms())); // uncertain/rejected outcomes reserve the window
 pending_setup();session.canceled=true;session.since=clock_ms()-16000;poll(shared_history);cleared();
 assert(polls==1&&last_event.find("not retried")!=std::string::npos&&!shared_history.feed_allowed(42,clock_ms()));
 pending_setup();poll_result=PGR_VR_ERR_SHAPE;poll(shared_history);cleared();assert(polls==1&&!shared_history.feed_allowed(42,clock_ms()));
 pending_setup();pgr_remote_gym_stop(owner+1);assert(session.active&&session.promise);
 pgr_remote_gym_stop(owner);cleared();assert(polls==0); // owner stop releases roots, never invokes a second RPC
 assert(!shared_history.feed_allowed(42,clock_ms())); // submitted outcome remains reserved on explicit stop
 pending_setup();finish("stopped","Account changed; submitted request not replayed");cleared();
 assert(!shared_history.feed_allowed(42,clock_ms())); // unknown result cannot leak to a later account/run
 // Actual entrypoint startup failure consumes the token; coherent repeated snapshots do not restart it.
 finished_token=0;PgrSharedMetadataOutput metadata{};owner=0;bootstraps=0;
 pgr_remote_gym_tick(gettid(),&metadata);assert(bootstraps==1&&finished_token==123&&!session.active);
 pgr_remote_gym_tick(gettid(),&metadata);assert(bootstraps==1);
 config="1|124|1|10|20|1|30|0";pgr_remote_gym_tick(gettid(),&metadata);assert(bootstraps==2&&finished_token==124);
 assert(test_roots.empty()&&allocations==frees);
 std::puts("PASS remote_gym runtime: permissions, canceled pending, timeout, refusal, owner, no replay");
}
