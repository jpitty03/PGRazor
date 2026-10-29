#include "routes_gate.h"
#include "pgrazor_native_api.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <dlfcn.h>
#include <thread>

// Runs against the production library via its exported v2 registry, not a copied policy.
int main(int argc,char **argv){
  assert(argc==2);
  void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);assert(lib);
  auto get=reinterpret_cast<const void *(*)(uint32_t)>(dlsym(lib,"pgrazor_native_api"));assert(get);
  const auto *core=static_cast<const pgrazor_native_api_v2 *>(get(2));assert(core&&core->query_feature);
  assert(!core->query_feature(27,2));
  const auto *a=static_cast<const PgrRoutesGateApi *>(core->query_feature(27,1));
  assert(a&&a->version==1&&a->size==56);
  assert(a->open(NAN)==0&&a->open(0)==0&&a->open(2501)==0);
  const uint64_t h=a->open(100);assert(h);
  PgrRoutesGateResult out{};
  assert(a->set(h,0,1,1,PGR_RG_DISCOVER,1000)==PGR_RG_NOT_ALLOWED);
  assert(a->last_seq(h)==0);
  assert(a->set(h,1,1,1,PGR_RG_DISCOVER,1000)==PGR_RG_ACCEPTED);
  assert(a->set(h,1,1,1,PGR_RG_PAUSE,1000)==PGR_RG_STALE_SEQ);
  assert(a->active(h,1050)==1);
  assert(a->take(h,1050,&out)==1&&out.seq==1&&out.action==PGR_RG_DISCOVER&&!out.expired);
  assert(a->take(h,1050,&out)==0);
  assert(a->set(h,1,1,2,PGR_RG_START,1100)==1);
  assert(a->take(h,1200,&out)==1&&out.expired); // exact lease boundary
  assert(a->set(h,1,1,3,PGR_RG_START,1200)==1);
  assert(a->set(h,0,0,0,0,1210)==PGR_RG_NOT_ALLOWED);
  assert(a->set(h,1,1,3,PGR_RG_START,1220)==PGR_RG_STALE_SEQ);
  assert(a->active(h,1230)==1);
  assert(a->take(h,1230,&out)==1&&out.seq==3&&out.expired);
  assert(a->set(h,1,1,4,PGR_RG_COMPLETE,1230)==1);
  assert(a->set(h,1,0,0,0,1231)==PGR_RG_NO_COMMAND);
  assert(a->take(h,1232,&out)==1&&out.expired);
  assert(a->set(h,1,1,5,PGR_RG_PAUSE,1240)==1);
  assert(a->set(h,1,-1,0,0,1241)==PGR_RG_INVALID_COMMAND);
  assert(a->set(h,1,1,4,PGR_RG_START,1242)==PGR_RG_STALE_SEQ);
  assert(a->take(h,1243,&out)==1&&out.seq==5&&!out.expired); // stale/malformed preserve newest
  assert(a->set(h,1,1,6,PGR_RG_DISCOVER,1250)==1);
  assert(a->clear(h)==0&&a->last_seq(h)==6&&a->take(h,1250,&out)==0);
  assert(a->set(h,1,1,6,PGR_RG_DISCOVER,1250)==PGR_RG_STALE_SEQ);
  assert(a->set(h,1,1,7,PGR_RG_START,1260)==1);
  assert(a->active(h,NAN)==PGR_RG_BAD_TIME);
  assert(a->take(h,1261,&out)==1&&out.seq==7&&out.expired);
  assert(a->set(h,1,1,8,PGR_RG_START,1270)==1);
  assert(a->set(h,1,1,9,PGR_RG_START,1269)==PGR_RG_BAD_TIME);
  assert(a->take(h,1271,&out)==1&&out.seq==8&&out.expired&&a->last_seq(h)==8);
  assert(a->set(h,1,1,9,PGR_RG_START,1280)==1);
  assert(a->take(h,INFINITY,&out)==1&&out.expired);
  assert(a->set(h,1,1,10,PGR_RG_START,-1)==PGR_RG_BAD_TIME);
  assert(a->set(h,1,1,9007199254740992ULL,PGR_RG_START,1280)==PGR_RG_INVALID_COMMAND);
  const uint64_t newer=a->open(100);assert(newer>h&&a->last_seq(newer)==9);
  assert(a->set(h,1,1,10,PGR_RG_START,1280)==PGR_RG_BAD_HANDLE);
  assert(a->active(h,1280)==PGR_RG_BAD_HANDLE&&a->clear(h)==PGR_RG_BAD_HANDLE);
  assert(a->set(newer,1,1,9,PGR_RG_START,1280)==PGR_RG_STALE_SEQ);
  // Concurrent refreshes: serialization preserves the greatest sequence and a single drain.
  std::thread one([&]{for(uint64_t n=10;n<110;n+=2)a->set(newer,1,1,n,PGR_RG_PAUSE,1300);});
  std::thread two([&]{for(uint64_t n=11;n<110;n+=2)a->set(newer,1,1,n,PGR_RG_PAUSE,1300);});
  one.join();two.join();assert(a->last_seq(newer)==109);
  assert(a->take(newer,1300,&out)==1&&out.seq==109&&!out.expired);
  assert(a->take(newer,1300,&out)==0);
  assert(a->set(newer,1,1,110,PGR_RG_PAUSE,1300)==1);
  std::thread stop([&]{assert(a->clear(newer)==0);});stop.join();
  assert(a->last_seq(newer)==110&&a->take(newer,1300,&out)==0);
  puts("PASS Routes gate: production ABI, cancellation, replay, clock, reconstruction, thread serialization");
  dlclose(lib);
}
