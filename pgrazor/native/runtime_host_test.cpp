#include "runtime_host.h"
#include "unity_scheduler.h"
#include "pgrazor_native_api.h"
#include <cassert>
#include <cstdio>
#include <dlfcn.h>
int main(int argc,char **argv){
 assert(argc==2);void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);assert(lib);
 auto get=reinterpret_cast<const void *(*)(uint32_t)>(dlsym(lib,"pgrazor_native_api"));assert(get);
 auto core=static_cast<const pgrazor_native_api_v2*>(get(2));assert(core);
 auto host=static_cast<const PgrRuntimeHostApi*>(core->query_feature(70,1));assert(host&&host->size==32&&!core->query_feature(70,2));
 auto scheduler=static_cast<const PgrUnitySchedulerApi*>(core->query_feature(48,2));assert(scheduler);
 PgrUnitySchedulerSnapshot before{},after{};assert(!scheduler->snapshot(&before)&&!before.installed);
 assert(host->configure(nullptr)==-1);PgrRuntimeHostConfig c{1,sizeof(c),0,0};assert(host->configure(&c)==-1);c.exclusive_owner=2;assert(host->configure(&c)==-1);
 c.exclusive_owner=1;c.reserved=1;assert(host->configure(&c)==-1);c.reserved=0;
 // Actual54 has not been probed in this process. Attestation does not cause
 // implicit discovery, thread attachment or scheduler/consumer hook installation.
 assert(host->configure(&c)==1);assert(host->configure(&c)==1);
 assert(!scheduler->snapshot(&after)&&!after.installed&&!after.generation);
 PgrRuntimeHostSample s{};assert(host->sample(nullptr)==-1);assert(!host->sample(&s)&&s.size==sizeof(s)&&!s.installed&&!s.ready_mask&&!s.fault);
 assert(s.pending_mask==0);
 for(unsigned i=0;i<10;++i)assert(s.hosts[i].feature==60+i&&s.hosts[i].state==((s.pending_mask&(1u<<i))?PGR_RTH_PENDING:PGR_RTH_WAITING));
 assert(s.hosts[10].feature==58&&s.hosts[11].feature==71);
 assert(!host->stop()&&!host->stop());assert(host->configure(&c)==-5);assert(!host->sample(&s)&&s.stopping&&!s.installed);
 puts("PASS actual70 ABI, exclusive-owner attestation,54-not-ready retries, no premature scheduler hooks, pending-host visibility, stop latch");
}
