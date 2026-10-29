// Coordinator regressions against the actual core/Dobby. Reuse only the fake runtime,
// not the production implementation. Each scenario starts a fresh process.
#define main routes_services_primary_main
#include "routes_services_test.cpp"
#undef main
#include <sys/wait.h>

namespace {
int frees = 0, frees_at_return = 0, request_calls = 0, range_calls = 0;
const void *last_method = nullptr;
void *last_receiver = nullptr;
FakeObj *move_from = nullptr, *move_to = nullptr, *reenter_receiver = nullptr;
bool revoke_on_free = false, revoke_on_new = false, reenter_original = false;

extern "C" __attribute__((noinline)) bool range_target(void *self, const void *method) {
  asm volatile(".rept 24; nop; .endr" ::: "memory");
  ++range_calls; last_method = method; last_receiver = self;
  frees_at_return = frees;
  return false;
}
extern "C" __attribute__((noinline)) void *request_target(void *self, const void *method) {
  asm volatile(".rept 24; nop; .endr" ::: "memory");
  ++request_calls; last_method = method; last_receiver = self;
  if (reenter_original) { reenter_original = false; range_target(reenter_receiver, method); }
  frees_at_return = frees;
  return &g_req_return;
}
uintptr_t regression_new(void *obj, int pinned) {
  assert(obj && static_cast<FakeObj *>(obj)->klass && "stale raw receiver passed to gc_new");
  const auto h = fake_gchandle_new(obj, pinned);
  if (revoke_on_new) { revoke_on_new = false; api()->revoke(); }
  return h;
}
void regression_free(uintptr_t handle) {
  ++frees;
  fake_gchandle_free(handle);
  if (move_from) {
    for (auto &target : g_handle_targets) if (target == move_from) target = move_to;
    move_from->klass = nullptr; move_from = nullptr;
  }
  if (revoke_on_free) { revoke_on_free = false; api()->revoke(); }
  if (reenter_receiver) {
    auto obj = reenter_receiver; reenter_receiver = nullptr;
    request_target(obj, reinterpret_cast<void *>(0x1234));
  }
}
PgrRoutesServicesOutput poll_now(int expected = PGR_RS_OK) {
  PgrRoutesServicesRequest r; make_request(&r,self_tid(),2500,api()->generation());
  PgrRoutesServicesOutput out{};
  assert(api()->poll(&r,&out) == expected);
  assert(out.status == expected);
  return out;
}
void drain_all() {
  PgrRoutesServicesRequest r; make_request(&r,self_tid(),0,api()->generation());
  assert(api()->drain(&r) == PGR_RS_OK && g_live_handles == 0);
}
void setup_library(const char *path) {
  void *library=dlopen(path,RTLD_NOW|RTLD_LOCAL);assert(library);
  auto get=reinterpret_cast<const void *(*)(uint32_t)>(dlsym(library,"pgrazor_native_api"));
  auto core=get?static_cast<const pgrazor_native_api_v2 *>(get(2)):nullptr;assert(core);
  loaded=static_cast<const PgrRoutesServicesApi *>(core->query_feature(45,1));assert(loaded);
  PgrRoutesServicesConfig c;make_config(&c);
  c.gchandle_new=reinterpret_cast<void *>(&regression_new);
  c.gchandle_free=reinterpret_cast<void *>(&regression_free);
  c.addr_request_nearby=reinterpret_cast<void *>(&request_target);
  c.addr_in_range_of_end=reinterpret_cast<void *>(&range_target);
  assert(api()->install(&c)==0);
}
int scenario(const char *name) {
  FakeObj consA{&kConsumptionClass,nullptr},consB{&kConsumptionClass,nullptr};
  FakeObj routesA{&kRoutesClass,&consA},routesB{&kRoutesClass,&consB};
  if (!std::strcmp(name,"detached-owner")) {
    g_thread_attached=false;
    std::thread t([]{poll_now(PGR_RS_SKIP);});t.join();
    g_thread_attached=true;poll_now();
    request_target(&routesA,nullptr);assert(poll_now().routes_handle);
  } else if (!std::strcmp(name,"moving-pending")) {
    poll_now();request_target(&routesA,nullptr);
    FakeObj moved=routesB;move_from=&routesB;move_to=&moved;
    request_target(&routesB,nullptr);
    assert(last_receiver==&moved && "incoming receiver must survive previous pending cleanup");
    auto out=poll_now();assert(fake_gchandle_get_target(out.routes_handle)==&moved);
    drain_all();
  } else if (!std::strcmp(name,"stable-authority")) {
    poll_now();request_target(&routesA,nullptr);auto old=poll_now();
    const auto allocated=g_handle_targets.size();
    for(int i=0;i<20;i++) {
      request_target(&routesA,nullptr);range_target(&consB,nullptr);auto out=poll_now();
      assert(out.routes_handle==old.routes_handle&&out.consumption_handle==old.consumption_handle);
    }
    assert(g_handle_targets.size()==allocated && "hot getter must not churn roots");
  } else if (!std::strcmp(name,"replacement-null-dica")) {
    poll_now();request_target(&routesA,nullptr);auto old=poll_now();assert(old.consumption_handle);
    routesB.dica=nullptr;request_target(&routesB,nullptr);auto out=poll_now();
    assert(out.routes_handle && !out.consumption_handle && !(out.flags&PGR_RS_FLAG_CONSUMPTION_PRESENT));
    assert(!fake_gchandle_get_target(old.consumption_handle));
  } else if (!std::strcmp(name,"revoke-cleanup")) {
    poll_now();request_target(&routesA,nullptr);poll_now();request_target(&routesB,nullptr);
    revoke_on_free=true;auto out=poll_now(PGR_RS_SKIP);
    assert(!out.routes_handle&&!out.consumption_handle);
    const auto allocated=g_handle_targets.size();request_target(&routesA,nullptr);
    assert(g_handle_targets.size()==allocated && "revoked heartbeat must not capture");
    out=poll_now();assert(!out.routes_handle&&!out.consumption_handle);
  } else if (!std::strcmp(name,"revoke-derivation")) {
    poll_now();request_target(&routesA,nullptr);revoke_on_new=true;
    auto out=poll_now(PGR_RS_SKIP);assert(!out.routes_handle&&!out.consumption_handle);
    const auto allocated=g_handle_targets.size();request_target(&routesB,nullptr);
    assert(g_handle_targets.size()==allocated);
  } else if (!std::strcmp(name,"cleanup-reentry")) {
    poll_now();request_target(&routesA,nullptr);poll_now();request_target(&routesB,nullptr);
    reenter_receiver=&routesA;auto out=poll_now();
    assert(fake_gchandle_get_target(out.routes_handle)==&routesB);
    assert(g_live_handles==2 && "cleanup reentry cannot retain another pending capture");
  } else if (!std::strcmp(name,"original-reentry-return")) {
    poll_now();reenter_receiver=&consA;reenter_original=true;
    auto method=reinterpret_cast<void *>(0x5577);
    assert(request_target(&routesA,method)==&g_req_return);
    assert(request_calls==1&&range_calls==1&&last_method==method&&frees_at_return==frees);
    assert(g_live_handles==1 && "nested original cannot capture consumption while outer hook is busy");
    reenter_receiver=nullptr;poll_now();
    drain_all();
    const auto allocated=g_handle_targets.size();
    assert(!range_target(&consA,method));
    assert(last_method==method && g_handle_targets.size()==allocated && frees_at_return==frees);
  } else if (!std::strcmp(name,"detached-passthrough")) {
    poll_now();g_thread_attached=false;
    assert(request_target(&routesA,reinterpret_cast<void *>(0x9988))==&g_req_return);
    assert(last_method==reinterpret_cast<void *>(0x9988)&&request_calls==1&&g_live_handles==0);
    g_thread_attached=true;
  } else return 5;
  drain_all();std::printf("PASS RoutesServices %s\n",name);return 0;
}
}
int main(int argc,char **argv) {
  if(argc==2) {
    const char *cases[]={"detached-owner","moving-pending","stable-authority","replacement-null-dica",
      "revoke-cleanup","revoke-derivation","cleanup-reentry","original-reentry-return","detached-passthrough"};
    for(auto name:cases) {
      const auto child=fork();assert(child>=0);
      if(!child){execl(argv[0],argv[0],argv[1],name,static_cast<char *>(nullptr));_exit(127);}
      int status=0;assert(waitpid(child,&status,0)==child);
      if(!WIFEXITED(status)||WEXITSTATUS(status)){std::printf("FAIL %s status=%d\n",name,status);return 4;}
    }
    return 0;
  }
  if(argc!=3)return 1;
  setup_library(argv[1]);return scenario(argv[2]);
}
