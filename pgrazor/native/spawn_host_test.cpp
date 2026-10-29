// Actual dlopen/query71 + real existing v1 spawn Dobby hook + real52 wire.
// Synthetic validated56 output supplies ONLY the test-owned native target.
#include "spawn_host.h"
#include "pgrazor_native_api.h"
#include "runtime_transport.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <vector>
#include <limits>
#include <thread>
#include <dlfcn.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
namespace {
thread_local bool attached=true;int thread_token,method_token;unsigned originals=0;
void *thread_current(){return attached?&thread_token:nullptr;}
__attribute__((noinline)) double target(void *self,void *method){asm volatile("nop;nop;nop;nop;nop;nop;nop;nop":::"memory");assert(method==&method_token);++originals;return *static_cast<double*>(self);}
double (*volatile call)(void*,void*)=target;
void put32(std::vector<uint8_t>&b,unsigned off,uint32_t n){std::memcpy(b.data()+off,&n,4);}
void put64(std::vector<uint8_t>&b,unsigned off,uint64_t n){std::memcpy(b.data()+off,&n,8);}
uint32_t u32(const std::vector<uint8_t>&b,unsigned off){uint32_t n;std::memcpy(&n,b.data()+off,4);return n;}
std::vector<unsigned> drain(const PgrRuntimeTransportApi *tr,const std::vector<uint8_t> *snapshot=nullptr){
 std::vector<uint8_t> out(65536);uint32_t length=0;assert(tr->exchange(snapshot?snapshot->data():nullptr,snapshot?snapshot->size():0,out.data(),out.size(),&length)==0);assert(length>=32&&u32(out,0)==PGR_TRANSPORT_EVENTS_MAGIC);
 unsigned at=32;std::vector<unsigned> ops;for(unsigned i=0;i<u32(out,12);++i){ops.push_back(u32(out,at));auto n=u32(out,at+4),s=u32(out,at+8);at+=12+n*8;for(unsigned j=0;j<s;++j)at+=4+u32(out,at);}assert(at==length);return ops;
}
void scenario(const char *library,unsigned mode){
 auto lib=dlopen(library,RTLD_NOW|RTLD_LOCAL);assert(lib);auto get=reinterpret_cast<const void*(*)(uint32_t)>(dlsym(lib,"pgrazor_native_api"));assert(get);
 auto core=static_cast<const pgrazor_native_api_v2*>(get(2));assert(core&&core->query_feature);auto api=static_cast<const PgrSpawnHostApi*>(core->query_feature(71,1));assert(api&&api->version==1&&api->size==64&&!core->query_feature(71,2));
 auto tr=static_cast<const PgrRuntimeTransportApi*>(core->query_feature(52,1));assert(tr);
 PgrSharedMetadataOutput md{};md.version=2;md.size=sizeof(md);md.optional[pgr_shared_meta::OPT_SPAWN]=mode==2?PGR_SHARED_OPT_UNAVAILABLE:PGR_SHARED_OPT_AVAILABLE;md.opt_handles[pgr_shared_meta::H_SP_CODE]=reinterpret_cast<void*>(target);md.opt_handles[pgr_shared_meta::H_SP_METHOD]=&method_token;
 PgrSpawnHostConfig c{1,sizeof(c),mode==1?1u:0u,1,&md,reinterpret_cast<void*>(thread_current),uint64_t(gettid())};
 if(mode==1){PgrRuntimeTransportConfig tc{1,sizeof(tc),500,128,4096,65536,0,0};assert(tr->install(&tc)==0);}
 auto bad=c;bad.exclusive_owner=0;assert(api->configure(&bad)==PGR_SPAH_BAD_CONFIG);assert(api->configure(&c)==0&&api->configure(&c)==0);
 bad=c;bad.flags^=1;assert(api->configure(&bad)==PGR_SPAH_REBIND);
 double radius=40;assert(call(&radius,&method_token)==40);
 if(mode==2){assert(api->tick(gettid())==PGR_SPAH_UNAVAILABLE);assert(call(&radius,&method_token)==40);return;}
 std::vector<uint8_t> frame(180,0);put32(frame,0,PGR_TRANSPORT_SNAPSHOT_MAGIC);put32(frame,4,1);put32(frame,8,frame.size());put64(frame,16,1);
 frame[32+27]=frame[32+28]=frame[32+3]=1;
 if(mode==1)drain(tr,&frame);
 PgrSpawnHostOptions p{1,sizeof(p),1,1,2500,0};if(!mode)assert(api->update(&p)==0);
 assert(api->tick(gettid())==0);assert(call(&radius,&method_token)==80);assert(api->tick(gettid())==0);
 PgrSpawnHostSample s{};assert(api->sample(&s)==0&&s.installed&&s.ready&&s.original==40&&s.effective==80);
 if(mode==1){auto ops=drain(tr);bool ready=false,range=false,event=false;for(auto op:ops){ready|=op==49;range|=op==48;event|=op==19;}assert(ready&&range&&event);assert(drain(tr).empty());}
 else{PgrSpawnHostEvent e{};unsigned ranges=0;while(api->next_event(&e)==0)ranges+=std::strstr(e.json,"spawn-range")!=nullptr;assert(ranges==1);}
 radius=120;assert(call(&radius,&method_token)==120);assert(api->tick(gettid())==0);
 radius=std::numeric_limits<double>::quiet_NaN();assert(std::isnan(call(&radius,&method_token)));assert(api->tick(gettid())==0);
 attached=false;assert(api->tick(gettid())==PGR_SPAH_OFF_OWNER);attached=true;
 std::thread foreign([&]{assert(api->tick(c.owner_tid)==PGR_SPAH_OFF_OWNER);});foreign.join();
 if(mode==1){put64(frame,16,2);frame[32+27]=0;drain(tr,&frame);assert(api->tick(gettid())==0);}
 else{p.enabled=0;assert(api->update(&p)==0);}
 radius=40;assert(call(&radius,&method_token)==40);
 if(mode==1){put64(frame,16,3);frame[32+27]=1;drain(tr,&frame);assert(api->tick(gettid())==0);assert(call(&radius,&method_token)==80);}
 else{p.enabled=1;p.lease_ms=10;assert(api->update(&p)==0);assert(api->tick(gettid())==0);assert(call(&radius,&method_token)==80);}
 timespec delay{0,150000000};nanosleep(&delay,nullptr);assert(call(&radius,&method_token)==40);
 assert(api->stop()==0&&api->stop()==0);assert(call(&radius,&method_token)==40);assert(api->update(&p)==PGR_SPAH_STOPPED);assert(api->tick(gettid())==PGR_SPAH_STOPPED);
 assert(api->sample(&s)==0&&s.stopping&&!s.ready);assert(originals>=8);
 // Published trampoline intentionally retained; never dlclose or unhook here.
}
}
int main(int argc,char **argv){assert(argc==2);for(unsigned mode=0;mode<3;++mode){pid_t child=fork();assert(child>=0);if(!child){scenario(argv[1],mode);_exit(0);}int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);}puts("PASS spawn_host: query71 actual Dobby/v1 leaf, finite scalar chain, real52 indices/actions, disable/lease/stop and optional-unavailable");}
