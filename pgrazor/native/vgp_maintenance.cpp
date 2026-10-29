#include "vgp_maintenance.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <time.h>
#include <unistd.h>

// Feature 32: synchronous VGP maintenance. No hooks, no callbacks, no retained managed state -- both
// entry points free every temporary root before returning and transfer nothing. Built with
// -fno-exceptions / -fno-rtti (see CMakeLists.txt): managed exceptions surface through runtime_invoke's
// exc out-param, never as C++ throws.

namespace {

PgrVgpMaintenanceConfig cfg{};
std::mutex installMutex;
std::atomic<bool> installed{false};
std::atomic<bool> busy{false};

using New      = uintptr_t (*)(void *, int);
using Get      = void *(*)(uintptr_t);
using FreeH    = void (*)(uintptr_t);
using Class    = void *(*)(void *);
using Invoke   = void *(*)(void *, void *, void **, void **);
using Unbox    = void *(*)(void *);
using Virtual  = void *(*)(void *, void *);
using FieldGet = void (*)(void *, void *, void *);

uint64_t now() {
  timespec t{};
  if (clock_gettime(CLOCK_BOOTTIME, &t) || t.tv_sec < 0) return 0;
  return uint64_t(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
}

// Owning temporary root: allocates a GC handle for a managed object and frees it on destruction. Nothing
// here ever releases ownership -- neither entry point transfers a handle to the caller.
struct Root {
  uintptr_t h = 0;
  Root() = default;
  Root(const Root &) = delete;
  Root &operator=(const Root &) = delete;
  ~Root() { if (h) reinterpret_cast<FreeH>(cfg.gchandle_free)(h); }
  bool set(void *o) { if (!o) return false; h = reinterpret_cast<New>(cfg.gchandle_new)(o, 0); return h != 0; }
  void *get() const { return h ? reinterpret_cast<Get>(cfg.gchandle_get_target)(h) : nullptr; }
};

bool exact(void *o, void *klass) {
  return o && reinterpret_cast<Class>(cfg.object_get_class)(o) == klass;
}

// Invoke a method and root its (non-null) return value. `receiver` null => static call; otherwise it is
// re-resolved from its root (relocating collector). err is set on a managed throw (PGR_VM_ERR_MANAGED), a
// collected receiver, or a null/unrootable return (PGR_VM_ERR_GCHANDLE).
bool call(void *method, const Root *receiver, void **args, Root &out, int &err) {
  void *self = nullptr;
  if (receiver) {
    self = receiver->get();
    if (!self) { err = PGR_VM_ERR_GCHANDLE; return false; }
  }
  void *exc = nullptr;
  void *value = reinterpret_cast<Invoke>(cfg.runtime_invoke)(method, self, args, &exc);
  if (exc) { err = PGR_VM_ERR_MANAGED; return false; }
  if (!out.set(value)) { err = PGR_VM_ERR_GCHANDLE; return false; }
  return true;
}

// Read a boxed System.Boolean result; -1 on error (err set), else 0/1.
int boolValue(const Root &r, int &err) {
  void *o = r.get();
  if (!exact(o, cfg.class_bool)) { err = PGR_VM_ERR_METADATA; return -1; }
  void *p = reinterpret_cast<Unbox>(cfg.object_unbox)(o);
  if (!p) { err = PGR_VM_ERR_METADATA; return -1; }
  const uint8_t value = *static_cast<uint8_t *>(p);
  if (value > 1) { err = PGR_VM_ERR_METADATA; return -1; }
  return value;
}

// Shared owner/lease/clock preamble. Returns PGR_VM_OK and fills *start/*deadline, or a non-OK code the
// caller returns verbatim (PGR_VM_SKIP for a closed lease / owner mismatch, negative for not-installed /
// a clock failure).
int gate(uint64_t owner, uint64_t lease_ms, uint64_t *start, uint64_t *deadline) {
  if (!installed.load(std::memory_order_acquire)) return PGR_VM_ERR_NOT_INSTALLED;
  if (!owner || owner != static_cast<uint64_t>(gettid())) return PGR_VM_SKIP;  // not the proven owner tid
  const uint64_t s = now();
  if (!s) return PGR_VM_ERR_CLOCK_FAILURE;
  const uint64_t lease = lease_ms > PGR_VM_MAX_LEASE_MS ? PGR_VM_MAX_LEASE_MS : lease_ms;
  if (!lease) return PGR_VM_SKIP;  // closed window: benign skip
  *start = s;
  *deadline = s + lease;
  return PGR_VM_OK;
}

// Scoped reentrancy guard across both entry points.
struct BusyGuard {
  bool held = false;
  BusyGuard() { bool idle = false; held = busy.compare_exchange_strong(idle, true); }
  ~BusyGuard() { if (held) busy.store(false); }
};

int install(const PgrVgpMaintenanceConfig *c) {
  if (!c) return PGR_VM_ERR_NULL_ARG;
  if (c->version != PGR_VM_CONFIG_VERSION) return PGR_VM_ERR_BAD_VERSION;
  if (c->size != sizeof(*c)) return PGR_VM_ERR_BAD_SIZE;
  void *pointers[20];  // the 20 il2cpp pointers; the trailing Int32/reserved words are not checked here
  std::memcpy(pointers, reinterpret_cast<const char *>(c) + 8, sizeof(pointers));
  for (void *p : pointers) if (!p) return PGR_VM_ERR_MISSING_PTR;
  std::lock_guard<std::mutex> guard(installMutex);
  if (installed.load()) return std::memcmp(c, &cfg, sizeof(cfg)) == 0 ? 0 : PGR_VM_ERR_RECONFIG;
  cfg = *c;
  installed.store(true, std::memory_order_release);
  return 0;
}

int balls(void *svc, uint64_t owner, uint64_t lease_ms, int32_t *out_count) {
  if (!out_count) return PGR_VM_ERR_NULL_ARG;
  *out_count = 0;  // initialised before any early return
  if (!svc) return PGR_VM_ERR_NULL_ARG;

  uint64_t start, deadline;
  int g = gate(owner, lease_ms, &start, &deadline);
  if (g != PGR_VM_OK) return g;

  BusyGuard bg;
  if (!bg.held) return PGR_VM_ERR_BUSY;

  // Root the service and confirm its exact class before touching its field (identity check).
  Root service;
  if (!service.set(svc)) return PGR_VM_ERR_GCHANDLE;
  if (!exact(service.get(), cfg.class_service)) return PGR_VM_ERR_METADATA;

  // Item bag from the service field (reference read; root immediately before any further allocation, then
  // re-validate the exact item-bag class).
  Root itemBag;
  {
    void *self = service.get();
    if (!self) return PGR_VM_ERR_GCHANDLE;
    void *rawBag = nullptr;
    reinterpret_cast<FieldGet>(cfg.field_get_value)(self, cfg.f_item_bag, &rawBag);
    if (!rawBag) return PGR_VM_SKIP;  // service not ready yet: benign skip
    if (!itemBag.set(rawBag)) return PGR_VM_ERR_GCHANDLE;
    if (!exact(itemBag.get(), cfg.class_item_bag)) return PGR_VM_ERR_METADATA;
  }

  // GetItemCount(item id 1) -> boxed Int32 (non-virtual; legacy itemInt semantics: invoke, exact Int32,
  // unbox, fail closed on a negative count). The item id lives on the C stack, not a managed object.
  int32_t count = 0;
  {
    int32_t itemId = PGR_VM_BALL_ITEM_ID;
    void *args[] = {&itemId};
    Root box;
    int err = 0;
    if (!call(cfg.m_item_count, &itemBag, args, box, err)) return err;
    if (!exact(box.get(), cfg.class_int32)) return PGR_VM_ERR_METADATA;
    void *p = reinterpret_cast<Unbox>(cfg.object_unbox)(box.get());
    if (!p) return PGR_VM_ERR_METADATA;
    std::memcpy(&count, p, sizeof(int32_t));
    if (count < 0) return PGR_VM_ERR_NEG_COUNT;
  }

  // Fresh clock AFTER the reads: only commit a positive count if the lease is still open.
  const uint64_t end = now();
  if (!end || end < start || end >= deadline) return PGR_VM_SKIP;

  *out_count = count;
  return PGR_VM_OK;
}

int cooldown(void *stop, void *response, uint64_t owner, uint64_t lease_ms) {
  if (!stop || !response) return PGR_VM_ERR_NULL_ARG;

  uint64_t start, deadline;
  int g = gate(owner, lease_ms, &start, &deadline);
  if (g != PGR_VM_OK) return g;

  BusyGuard bg;
  if (!bg.held) return PGR_VM_ERR_BUSY;

  // Root BOTH arguments before any managed call, then confirm their exact classes.
  Root stopR, respR;
  if (!stopR.set(stop)) return PGR_VM_ERR_GCHANDLE;
  if (!respR.set(response)) return PGR_VM_ERR_GCHANDLE;
  if (!exact(stopR.get(), cfg.class_stop)) return PGR_VM_ERR_METADATA;
  if (!exact(respR.get(), cfg.class_spin_result)) return PGR_VM_ERR_METADATA;

  // Success gate: read the Int32 result code off the response (direct field read, no allocation) and
  // compare it to the configured spin-success sentinel. A non-success result applies no cooldown.
  {
    void *resp = respR.get();
    if (!resp) return PGR_VM_ERR_GCHANDLE;
    int32_t result = 0;
    reinterpret_cast<FieldGet>(cfg.field_get_value)(resp, cfg.f_result, &result);
    if (result != cfg.spin_success) return PGR_VM_SKIP;  // not a successful spin
  }

  // Liveness: static UnityEngine.Object.op_Implicit(stop) -> boxed Boolean (the stop is the argument).
  {
    void *self = stopR.get();
    if (!self) return PGR_VM_ERR_GCHANDLE;
    void *args[] = {self};
    Root box;
    int err = 0;
    if (!call(cfg.m_alive, nullptr, args, box, err)) return err;
    int live = boolValue(box, err);
    if (live < 0) return err;
    if (!live) return PGR_VM_SKIP;  // stop no longer alive: nothing to cool down
  }

  // The full 64-bit cooldown target off the response (direct field read; all 64 bits preserved). It lives
  // on the C stack so the relocating collector cannot move it out from under the setter invocation.
  int64_t until = 0;
  {
    void *resp = respR.get();
    if (!resp) return PGR_VM_ERR_GCHANDLE;
    reinterpret_cast<FieldGet>(cfg.field_get_value)(resp, cfg.f_until, &until);
  }

  // Resolve the virtual SetCooldownTime slot on the freshly re-resolved stop.
  void *self = stopR.get();
  if (!self) return PGR_VM_ERR_GCHANDLE;
  void *method = reinterpret_cast<Virtual>(cfg.object_get_virtual_method)(self, cfg.m_cooldown);
  if (!method) return PGR_VM_ERR_METADATA;

  // Re-check owner/lease after the managed resolves and BEFORE the setter: a closed window skips without
  // applying the cooldown.
  self = stopR.get();
  if (!self) return PGR_VM_ERR_GCHANDLE;
  const uint64_t end = now();
  if (!end || end < start || end >= deadline) return PGR_VM_SKIP;

  // Invoke SetCooldownTime(stop, &until). Re-resolve the receiver one last time. Once it has run without a
  // managed throw the local setter has already applied, so a clock expiry DURING the invocation does not
  // retract success.
  void *args[] = {&until};
  void *exc = nullptr;
  reinterpret_cast<Invoke>(cfg.runtime_invoke)(method, self, args, &exc);
  if (exc) return PGR_VM_ERR_MANAGED;
  return PGR_VM_OK;
}

const PgrVgpMaintenanceApi api = {
    PGR_VM_API_VERSION, uint32_t(sizeof(PgrVgpMaintenanceApi)), install, balls, cooldown};

}  // namespace

extern "C" const void *pgrazor_vgp_maintenance_api(uint32_t version) {
  return version == PGR_VM_API_VERSION ? &api : nullptr;
}
