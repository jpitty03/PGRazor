// Actual production-library ABI/fail-closed boundary. Flow fixture is separate.
#include "shared_host.h"
#include "pgrazor_native_api.h"
#include <cassert>
#include <cstring>
#include <dlfcn.h>
#include <cstdio>
int main(int argc,char **argv){assert(argc==2);void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);assert(lib);auto get=reinterpret_cast<const void *(*)(uint32_t)>(dlsym(lib,"pgrazor_native_api"));assert(get);auto core=static_cast<const pgrazor_native_api_v2*>(get(2));assert(core);auto api=static_cast<const PgrSharedHostApi*>(core->query_feature(61,1));assert(api&&api->size==72&&!core->query_feature(61,2));
 assert(api->configure(nullptr)==1);assert(api->configure(nullptr)==1);PgrSharedHostConfig cfg{};cfg.version=1;cfg.size=sizeof(cfg);assert(api->configure(&cfg)==-1);assert(!api->metadata());
 PgrSharedHostOptions o{1,sizeof(o),1,63,1,2500,nullptr,0,0};assert(api->update(&o)==0);o.flags=16;assert(api->update(&o)==-1);o.flags=1;o.protect=64;assert(api->update(&o)==-1);o.protect=63;o.keep_count=1;assert(api->update(&o)==-1);o.keep_count=0;o.epoch=0;assert(api->update(&o)==-1);
 assert(api->tick(1,1)==-1);PgrSharedHostSample sample{};assert(api->sample(&sample)==0&&sample.version==1&&sample.size==56&&!sample.ready&&!sample.roots);PgrSharedHostEvent event{};assert(api->next_event(&event)==0);assert(api->stop()==0);assert(api->sample(&sample)==0&&sample.stopping);puts("PASS actual61 ABI/options/unconfigured/stop boundaries");}
