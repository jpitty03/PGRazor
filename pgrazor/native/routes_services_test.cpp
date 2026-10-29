// Routes service lifetime and original chaining against the actual compiled core/Dobby.
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <vector>

#include "routes_services.h"
#include "pgrazor_native_api.h"
#include <dlfcn.h>

static_assert(sizeof(PgrRoutesServicesConfig) == 96, "config 96");
static_assert(sizeof(PgrRoutesServicesRequest) == 32, "request 32");
static_assert(sizeof(PgrRoutesServicesOutput) == 40, "output 40");
static_assert(sizeof(PgrRoutesServicesApi) == 48, "table 48");

namespace {

// --- fake il2cpp runtime -----------------------------------------------------------
int kRoutesClass = 0;        // &kRoutesClass is the RoutesService Il2CppClass*
int kConsumptionClass = 0;   // &kConsumptionClass is the RouteConsumptionService Il2CppClass*
int kDicaField = 0;          // &kDicaField is RoutesService.dica FieldInfo*
int kThread = 0;             // attached-thread sentinel

struct FakeObj {
  void *klass = nullptr;
  FakeObj *dica = nullptr;  // for RoutesService objects
};

std::vector<FakeObj *> g_handle_targets;  // handle id -> obj (null once freed)
int g_live_handles = 0;
bool g_thread_attached = true;

void *fake_object_get_class(void *obj) { return obj ? reinterpret_cast<FakeObj *>(obj)->klass : nullptr; }
void fake_field_get_value(void *obj, void *field, void *out) {
  if (field == &kDicaField && obj) *reinterpret_cast<void **>(out) = reinterpret_cast<FakeObj *>(obj)->dica;
}
uintptr_t fake_gchandle_new(void *obj, int) {
  if (!obj) return 0;
  g_handle_targets.push_back(reinterpret_cast<FakeObj *>(obj));
  ++g_live_handles;
  return static_cast<uintptr_t>(g_handle_targets.size());  // 1-based handle id
}
void *fake_gchandle_get_target(uintptr_t h) {
  if (h == 0 || h > g_handle_targets.size()) return nullptr;
  return g_handle_targets[h - 1];
}
void fake_gchandle_free(uintptr_t h) {
  if (h == 0 || h > g_handle_targets.size()) return;
  if (g_handle_targets[h - 1]) { g_handle_targets[h - 1] = nullptr; --g_live_handles; }
}
void *fake_thread_current() { return g_thread_attached ? &kThread : nullptr; }

// --- fixture originals (chained exactly once; return preserved) ---------------------
std::atomic<int> g_req_calls{0};
std::atomic<int> g_ir_calls{0};
void *g_last_req_receiver = nullptr;
void *g_last_ir_receiver = nullptr;
FakeObj g_req_return;  // a managed reference RequestNearbyRoutes "returns"

extern "C" __attribute__((noinline)) void *fixture_req_orig_impl(void *self, const void *) {
  asm volatile(".rept 24; nop; .endr" ::: "memory");
  g_req_calls.fetch_add(1);
  g_last_req_receiver = self;
  return &g_req_return;
}
extern "C" __attribute__((noinline)) bool fixture_ir_orig_impl(void *self, const void *) {
  asm volatile(".rept 24; nop; .endr" ::: "memory");
  g_ir_calls.fetch_add(1);
  g_last_ir_receiver = self;
  return true;
}

void *kAddrReq = reinterpret_cast<void *>(&fixture_req_orig_impl);
void *kAddrIr = reinterpret_cast<void *>(&fixture_ir_orig_impl);

void make_config(PgrRoutesServicesConfig *c) {
  std::memset(c, 0, sizeof(*c));
  c->version = PGR_RS_CONFIG_VERSION;
  c->size = sizeof(*c);
  c->object_get_class = reinterpret_cast<void *>(&fake_object_get_class);
  c->field_get_value = reinterpret_cast<void *>(&fake_field_get_value);
  c->gchandle_new = reinterpret_cast<void *>(&fake_gchandle_new);
  c->gchandle_get_target = reinterpret_cast<void *>(&fake_gchandle_get_target);
  c->gchandle_free = reinterpret_cast<void *>(&fake_gchandle_free);
  c->thread_current = reinterpret_cast<void *>(&fake_thread_current);
  c->class_routes = &kRoutesClass;
  c->class_consumption = &kConsumptionClass;
  c->f_routes_dica = &kDicaField;
  c->addr_request_nearby = kAddrReq;
  c->addr_in_range_of_end = kAddrIr;
}

const PgrRoutesServicesApi *loaded = nullptr;
const PgrRoutesServicesApi *api() { return loaded; }

void make_request(PgrRoutesServicesRequest *r, uint64_t tid, uint64_t residual, uint64_t gen) {
  std::memset(r, 0, sizeof(*r));
  r->version = PGR_RS_REQUEST_VERSION;
  r->size = sizeof(*r);
  r->tid = tid;
  r->residual_ms = residual;
  r->generation = gen;
}

// Simulate the game calling the hooked methods (stub mode only).
void *call_request_nearby(void *self) {
  return fixture_req_orig_impl(self, nullptr);
}
bool call_in_range(void *self) {
  return fixture_ir_orig_impl(self, nullptr);
}

uint64_t self_tid() { return static_cast<uint64_t>(static_cast<pid_t>(gettid())); }

}  // namespace

int main(int argc, char **argv) {
  assert(argc == 2);
  void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!library) { std::puts(dlerror()); return 2; }
  auto get = reinterpret_cast<const void *(*)(uint32_t)>(dlsym(library, "pgrazor_native_api"));
  auto core = get ? static_cast<const pgrazor_native_api_v2 *>(get(2)) : nullptr;
  assert(core && core->query_feature(45, 99) == nullptr);
  loaded = static_cast<const PgrRoutesServicesApi *>(core->query_feature(45, 1));
  assert(loaded && loaded->size == sizeof(PgrRoutesServicesApi));

  // install() validates header, places both hooks, is idempotent, rejects reconfig.
  {
    PgrRoutesServicesConfig c;
    make_config(&c);
    assert(api()->install(&c) == 0);            // install() success == 0 (NOT status OK 1)
    assert(api()->install(&c) == 0);            // idempotent, same config
    PgrRoutesServicesConfig c2 = c;
    c2.f_routes_dica = reinterpret_cast<void *>(&kThread);
    assert(api()->install(&c2) == PGR_RS_ERR_RECONFIG);
    PgrRoutesServicesConfig bad;
    make_config(&bad);
    bad.class_routes = nullptr;
    assert(api()->install(&bad) == PGR_RS_ERR_MISSING_PTR);
  }

  const uint64_t tid = self_tid();
  FakeObj routesA; routesA.klass = &kRoutesClass;
  FakeObj consC;   consC.klass = &kConsumptionClass;
  routesA.dica = &consC;

  // Capture off the owner thread BEFORE any poll: hook must passthrough (no capture) but still chain the
  // original exactly once, returning its value.
  {
    g_req_calls = 0;
    void *ret = call_request_nearby(&routesA);  // owner not yet latched + heartbeat 0
    assert(ret == &g_req_return);               // return preserved
    assert(g_req_calls.load() == 1);            // original chained exactly once
    PgrRoutesServicesRequest r; make_request(&r, tid, 2500, api()->generation());
    PgrRoutesServicesOutput out;
    assert(api()->poll(&r, &out) == PGR_RS_OK);
    assert(!(out.flags & PGR_RS_FLAG_ROUTES_PRESENT));  // nothing captured before owner/heartbeat
  }

  // Now poll latched the owner + armed the heartbeat. A capture + commit publishes a borrowed handle.
  {
    g_req_calls = 0;
    void *ret = call_request_nearby(&routesA);
    assert(ret == &g_req_return && g_req_calls.load() == 1);
    assert(g_last_req_receiver == &routesA);  // re-resolved receiver == the same object
    PgrRoutesServicesRequest r; make_request(&r, tid, 2500, api()->generation());
    PgrRoutesServicesOutput out;
    assert(api()->poll(&r, &out) == PGR_RS_OK);
    assert(out.flags & PGR_RS_FLAG_ROUTES_PRESENT);
    assert(out.flags & PGR_RS_FLAG_ROUTES_REPLACED);
    assert(out.routes_handle != 0);
    // dica derivation produced the consumption service authoritatively.
    assert(out.flags & PGR_RS_FLAG_CONSUMPTION_PRESENT);
    assert(out.flags & PGR_RS_FLAG_CONSUMPTION_FROM_DICA);
    assert(out.consumption_handle != 0);
    assert(fake_gchandle_get_target(out.routes_handle) == &routesA);
    assert(fake_gchandle_get_target(out.consumption_handle) == &consC);

    // Host BORROWS the handles: it must never free them and they stay live across a second poll.
    uint64_t borrowed_routes = out.routes_handle;
    PgrRoutesServicesOutput out2;
    assert(api()->poll(&r, &out2) == PGR_RS_OK);
    assert(out2.routes_handle == borrowed_routes);                 // stable, not replaced
    assert(!(out2.flags & PGR_RS_FLAG_ROUTES_REPLACED));
    assert(fake_gchandle_get_target(borrowed_routes) == &routesA);  // still live
  }

  // Same-object repeated capture is stable: no extra gchandle churn, handle unchanged.
  {
    int live_before = g_live_handles;
    call_request_nearby(&routesA);
    call_request_nearby(&routesA);
    PgrRoutesServicesRequest r; make_request(&r, tid, 2500, api()->generation());
    PgrRoutesServicesOutput out;
    assert(api()->poll(&r, &out) == PGR_RS_OK);
    assert(!(out.flags & PGR_RS_FLAG_ROUTES_REPLACED));
    assert(g_live_handles == live_before);  // no leak, no net new root
  }

  // New RoutesService replaces the old ACTIVE root only on the owner poll; the old root is freed then.
  {
    FakeObj routesB; routesB.klass = &kRoutesClass;
    FakeObj consD;   consD.klass = &kConsumptionClass;
    routesB.dica = &consD;
    PgrRoutesServicesRequest r; make_request(&r, tid, 2500, api()->generation());
    PgrRoutesServicesOutput before;
    assert(api()->poll(&r, &before) == PGR_RS_OK);
    uint64_t old_routes = before.routes_handle, old_cons = before.consumption_handle;

    call_request_nearby(&routesB);  // hook only sets pending; active unchanged until poll
    assert(fake_gchandle_get_target(old_routes) == &routesA);  // old active still valid mid-capture

    PgrRoutesServicesOutput out;
    assert(api()->poll(&r, &out) == PGR_RS_OK);
    assert(out.flags & PGR_RS_FLAG_ROUTES_REPLACED);
    assert(fake_gchandle_get_target(out.routes_handle) == &routesB);
    assert(fake_gchandle_get_target(out.consumption_handle) == &consD);  // dica re-derived (no stale)
    assert(fake_gchandle_get_target(old_routes) == nullptr);  // old active freed by poll
    assert(fake_gchandle_get_target(old_cons) == nullptr);
  }

  // Consumption capture via get_InRangeOfEnd before any routes service exists.
  {
    PgrRoutesServicesRequest rdrain; make_request(&rdrain, tid, 0, api()->generation());
    assert(api()->drain(&rdrain) == PGR_RS_OK);  // clear everything first
    assert(g_live_handles == 0);

    FakeObj cons; cons.klass = &kConsumptionClass;
    PgrRoutesServicesRequest r; make_request(&r, tid, 2500, api()->generation());
    PgrRoutesServicesOutput arm;
    assert(api()->poll(&r, &arm) == PGR_RS_OK);  // arm heartbeat / latch owner
    bool b = call_in_range(&cons);
    assert(b == true && g_ir_calls.load() >= 1 && g_last_ir_receiver == &cons);
    PgrRoutesServicesOutput out;
    assert(api()->poll(&r, &out) == PGR_RS_OK);
    assert(out.flags & PGR_RS_FLAG_CONSUMPTION_PRESENT);
    assert(!(out.flags & PGR_RS_FLAG_CONSUMPTION_FROM_DICA));  // hook-captured, no routes/dica yet
    assert(fake_gchandle_get_target(out.consumption_handle) == &cons);
  }

  // revoke(): any-thread atomic invalidation. A pending captured under the old generation is dropped by the
  // next poll; no managed work, frees nothing itself; capture cannot revive without a fresh heartbeat.
  {
    PgrRoutesServicesRequest rdrain; make_request(&rdrain, tid, 0, api()->generation());
    assert(api()->drain(&rdrain) == PGR_RS_OK);
    assert(g_live_handles == 0);

    FakeObj routes; routes.klass = &kRoutesClass;
    PgrRoutesServicesRequest r; make_request(&r, tid, 2500, api()->generation());
    PgrRoutesServicesOutput arm; assert(api()->poll(&r, &arm) == PGR_RS_OK);
    call_request_nearby(&routes);        // pending captured under current generation
    uint64_t newgen = api()->revoke();   // bump + clear heartbeat
    int live_after_revoke = g_live_handles;
    (void)live_after_revoke;             // revoke frees nothing

    // A stale-generation poll is a benign SKIP; it must not commit the pending.
    PgrRoutesServicesOutput stale;
    assert(api()->poll(&r, &stale) == PGR_RS_SKIP);
    // A fresh-generation poll drops the stale pending (captured under the prior gen) without committing.
    PgrRoutesServicesRequest r2; make_request(&r2, tid, 2500, newgen);
    PgrRoutesServicesOutput out;
    assert(api()->poll(&r2, &out) == PGR_RS_OK);
    assert(!(out.flags & PGR_RS_FLAG_ROUTES_PRESENT));
    PgrRoutesServicesRequest rd; make_request(&rd, tid, 0, api()->generation());
    assert(api()->drain(&rd) == PGR_RS_OK);
    assert(g_live_handles == 0);
  }

  // Heartbeat expiry: a hook fired without a live heartbeat captures nothing but still chains the original.
  {
    api()->revoke();                       // clears the capture heartbeat (deadline -> 0)
    FakeObj routes; routes.klass = &kRoutesClass;
    g_req_calls = 0;
    call_request_nearby(&routes);          // no live heartbeat -> capture nothing
    assert(g_req_calls.load() == 1);       // original still chained exactly once
    PgrRoutesServicesRequest r; make_request(&r, tid, 2500, api()->generation());
    PgrRoutesServicesOutput out;
    assert(api()->poll(&r, &out) == PGR_RS_OK);  // re-arms heartbeat, but nothing was pending
    assert(!(out.flags & PGR_RS_FLAG_ROUTES_PRESENT));
  }

  // Foreign-thread poll / drain are benign SKIPs that free nothing; a foreign-thread hook never captures.
  {
    FakeObj routes; routes.klass = &kRoutesClass; routes.dica = &consC;
    PgrRoutesServicesRequest r; make_request(&r, tid, 2500, api()->generation());
    PgrRoutesServicesOutput arm; assert(api()->poll(&r, &arm) == PGR_RS_OK);
    call_request_nearby(&routes);
    PgrRoutesServicesOutput committed; assert(api()->poll(&r, &committed) == PGR_RS_OK);
    assert(committed.flags & PGR_RS_FLAG_ROUTES_PRESENT);

    std::thread foreign([&]() {
      // Foreign poll: tid != real gettid() for this thread -> SKIP.
      PgrRoutesServicesRequest fr; make_request(&fr, tid, 2500, api()->generation());
      PgrRoutesServicesOutput fo;
      assert(api()->poll(&fr, &fo) == PGR_RS_SKIP);
      // Foreign drain frees nothing.
      PgrRoutesServicesRequest fd; make_request(&fd, self_tid(), 0, api()->generation());
      // self_tid() here is the foreign thread's tid, not the latched owner -> SKIP.
      assert(api()->drain(&fd) == PGR_RS_SKIP);
      // Foreign hook invocation: gate fails, captures nothing, chains original once.
      g_req_calls = 0;
      void *ret = call_request_nearby(&routes);
      assert(ret == &g_req_return && g_req_calls.load() == 1);
    });
    foreign.join();
    assert(committed.routes_handle != 0 && fake_gchandle_get_target(committed.routes_handle) != nullptr);
    PgrRoutesServicesRequest rd; make_request(&rd, tid, 0, api()->generation());
    assert(api()->drain(&rd) == PGR_RS_OK);
    assert(g_live_handles == 0);
  }

  std::printf("routes_services_test (real-dobby): OK\n");
  return 0;
}