#include "catch_discovery.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <time.h>
#include <unistd.h>

// Feature 40: synchronous, read-only post-catch candidate discovery. No hooks, no callbacks, no retained
// managed state beyond the handles explicitly transferred to the caller. Every temporary managed value is
// rooted on the owner thread and freed before return; the array base is re-resolved from its root on every
// indexing. Built -fno-exceptions / -fno-rtti like every sibling: managed exceptions surface through
// runtime_invoke's exc out-param, never as C++ throws. No mutex is held across a managed call.

static_assert(sizeof(PgrCatchDiscoveryConfig) == 104, "config 104");
static_assert(sizeof(PgrCatchDiscoveryRequest) == 32, "request 32");
static_assert(sizeof(PgrCatchDiscoveryOutput) == 144, "output 144");
static_assert(sizeof(PgrCatchDiscoveryApi) == 40, "table 40");
static_assert(offsetof(PgrCatchDiscoveryConfig, m_find) == 96, "config layout");
static_assert(offsetof(PgrCatchDiscoveryRequest, layer) == 24, "request layout");
static_assert(offsetof(PgrCatchDiscoveryOutput, handles) == 16, "output layout");

namespace {

constexpr uint64_t kMaxLeaseMs = PGR_CD_MAX_LEASE_MS;
constexpr int kConfigPtrCount = 12;  // validated pointers after the 8-byte header

using New        = uintptr_t (*)(void *, int);
using Get        = void *(*)(uintptr_t);
using FreeH      = void (*)(uintptr_t);
using ObjClass   = void *(*)(void *);
using Invoke     = void *(*)(const void *, void *, void **, void **);
using ClassType  = void *(*)(void *);
using TypeObject = void *(*)(void *);
using ArrayLen   = uintptr_t (*)(void *);

std::mutex g_install_mutex;
std::mutex g_state_mutex;
PgrCatchDiscoveryConfig g_cfg{};
std::atomic<bool> g_installed{false};

// Permanent Unity owner, latched once from a genuine on-thread call.
std::atomic<uint64_t> g_owner_tid{0};
bool g_busy = false;  // discovery, INCLUDING root cleanup, is in flight (guarded by g_state_mutex)

uint64_t current_tid() { return static_cast<uint64_t>(static_cast<pid_t>(gettid())); }

bool read_boottime_ms(uint64_t *out_ms) {
  struct timespec ts {};
  if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0 || ts.tv_sec < 0 || ts.tv_nsec < 0) return false;
  *out_ms = static_cast<uint64_t>(ts.tv_sec) * 1000ULL + static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
  return true;
}

ObjClass obj_class() { return reinterpret_cast<ObjClass>(g_cfg.object_get_class); }
bool is_exact(void *obj, void *klass) { return obj != nullptr && obj_class()(obj) == klass; }

// Owning temporary root. set() roots a raw managed object; duplicate() roots a borrowed handle's current
// target; release() hands the handle to the caller WITHOUT freeing. Destructor frees any live handle, so
// an early return always balances every root allocated on that path.
struct Root {
  uintptr_t handle = 0;
  Root() = default;
  Root(const Root &) = delete;
  Root &operator=(const Root &) = delete;
  ~Root() { if (handle) reinterpret_cast<FreeH>(g_cfg.gchandle_free)(handle); }
  void *get() const { return handle ? reinterpret_cast<Get>(g_cfg.gchandle_get_target)(handle) : nullptr; }
  bool set(void *object) {
    if (!object) return false;
    handle = reinterpret_cast<New>(g_cfg.gchandle_new)(object, 0);
    return handle && get();
  }
  bool duplicate(void *borrowed) {
    return borrowed && set(reinterpret_cast<Get>(g_cfg.gchandle_get_target)(reinterpret_cast<uintptr_t>(borrowed)));
  }
  uintptr_t release() { uintptr_t h = handle; handle = 0; return h; }
};

// Busy reset runs AFTER the Roots declared below it are destroyed, so reentrant root cleanup still observes
// busy == true (the guard spans destruction/root cleanup).
struct BusyExit { ~BusyExit() { std::lock_guard<std::mutex> lock(g_state_mutex); g_busy = false; } };

// Invoke a method and root its (possibly null) return value. receiver null => static call; otherwise the
// receiver is re-resolved from its root (relocating collector). Returns one of CALL_*.
enum CallResult { CALL_OK, CALL_THROW, CALL_GCHANDLE, CALL_DEAD };

CallResult call(void *method, const Root *receiver, void **args, Root &out, bool allowNull) {
  void *self = nullptr;
  if (receiver) {
    self = receiver->get();
    if (!self) return CALL_DEAD;  // receiver collected despite its root (never expected)
  }
  void *exc = nullptr;
  void *value = reinterpret_cast<Invoke>(g_cfg.runtime_invoke)(method, self, args, &exc);
  if (exc) return CALL_THROW;
  if (!value) return allowNull ? CALL_OK : CALL_GCHANDLE;
  return out.set(value) ? CALL_OK : CALL_GCHANDLE;
}

// Build a managed System.Type for `klass` and root it. type_get_object is a pure metadata export whose
// result is a managed object that must be rooted before any further managed call.
bool type_object(void *klass, Root &out, int &err) {
  void *type = reinterpret_cast<ClassType>(g_cfg.class_get_type)(klass);
  if (!type) { err = PGR_CD_ERR_METADATA; return false; }
  void *obj = reinterpret_cast<TypeObject>(g_cfg.type_get_object)(type);
  if (!obj) { err = PGR_CD_ERR_METADATA; return false; }
  if (!out.set(obj)) { err = PGR_CD_ERR_GCHANDLE; return false; }
  return true;
}

void init_output(PgrCatchDiscoveryOutput *out) {
  std::memset(out, 0, sizeof(*out));
  out->version = PGR_CD_OUTPUT_VERSION;
  out->size = sizeof(PgrCatchDiscoveryOutput);
}

// Supplied tid must be nonzero AND equal the current OS tid BEFORE the owner is latched; a later foreign
// tid refuses. Mirrors feature 38's owner latch.
bool owner_ready(uint64_t tid) {
  if (tid == 0 || current_tid() != tid) return false;
  uint64_t owner = g_owner_tid.load(std::memory_order_acquire);
  if (!(owner == 0 || owner == tid)) return false;
  uint64_t expected = 0;
  g_owner_tid.compare_exchange_strong(expected, tid, std::memory_order_acq_rel);
  return g_owner_tid.load(std::memory_order_acquire) == tid;
}

// Shared gate/clock preamble. Fills *deadline, or returns a non-OK code the caller returns verbatim: 0 for
// a closed lease / owner mismatch, negative for a clock failure. The owner latch and lease clamp run here.
int gate(const PgrCatchDiscoveryRequest *request, uint64_t *deadline) {
  if (!owner_ready(request->tid)) return PGR_CD_SKIP;
  const uint64_t remaining = request->remaining_ms > kMaxLeaseMs ? kMaxLeaseMs : request->remaining_ms;
  if (remaining == 0) return PGR_CD_SKIP;
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) return PGR_CD_ERR_CLOCK_FAILURE;
  *deadline = now + remaining;
  return PGR_CD_OK;
}

// Common request header validation; *rc set, returns false on failure (output already initialised).
bool check_request(const PgrCatchDiscoveryRequest *request, int *rc) {
  if (!g_installed.load(std::memory_order_acquire)) { *rc = PGR_CD_ERR_NOT_INSTALLED; return false; }
  if (request == nullptr) { *rc = PGR_CD_ERR_NULL_ARG; return false; }
  if (request->version != PGR_CD_REQUEST_VERSION) { *rc = PGR_CD_ERR_BAD_VERSION; return false; }
  if (request->size != sizeof(PgrCatchDiscoveryRequest)) { *rc = PGR_CD_ERR_BAD_SIZE; return false; }
  *rc = PGR_CD_OK;
  return true;
}

// --- config validation / install ---------------------------------------------------
bool validate_config(const PgrCatchDiscoveryConfig *c, int *rc) {
  if (c == nullptr) { *rc = PGR_CD_ERR_NULL_ARG; return false; }
  if (c->version != PGR_CD_CONFIG_VERSION) { *rc = PGR_CD_ERR_BAD_VERSION; return false; }
  if (c->size != sizeof(PgrCatchDiscoveryConfig)) { *rc = PGR_CD_ERR_BAD_SIZE; return false; }
  void *ptrs[kConfigPtrCount];
  std::memcpy(ptrs, reinterpret_cast<const unsigned char *>(c) + offsetof(PgrCatchDiscoveryConfig, object_get_class),
              sizeof(ptrs));
  for (int i = 0; i < kConfigPtrCount; ++i)
    if (ptrs[i] == nullptr) { *rc = PGR_CD_ERR_MISSING_PTR; return false; }
  *rc = PGR_CD_OK;
  return true;
}

int api_install(const PgrCatchDiscoveryConfig *config) {
  int rc = PGR_CD_OK;
  if (!validate_config(config, &rc)) return rc;
  uint64_t probe = 0;
  if (!read_boottime_ms(&probe)) return PGR_CD_ERR_CLOCK_FAILURE;

  std::lock_guard<std::mutex> install_lock(g_install_mutex);
  if (g_installed.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    return std::memcmp(&g_cfg, config, sizeof(*config)) == 0 ? 0 : PGR_CD_ERR_RECONFIG;
  }
  {
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    g_cfg = *config;
    g_busy = false;
  }
  g_installed.store(true, std::memory_order_release);
  return 0;
}

int api_top(const PgrCatchDiscoveryRequest *request, PgrCatchDiscoveryOutput *out) {
  if (out == nullptr) return PGR_CD_ERR_NULL_ARG;  // cannot report
  init_output(out);
  int rc = PGR_CD_OK;
  if (!check_request(request, &rc)) return rc;
  if (request->layer == nullptr) return PGR_CD_ERR_NULL_ARG;  // top requires the borrowed layer handle

  uint64_t deadline = 0;
  const int g = gate(request, &deadline);
  if (g != PGR_CD_OK) return g;

  { std::lock_guard<std::mutex> lock(g_state_mutex); if (g_busy) return PGR_CD_ERR_BUSY; g_busy = true; }
  BusyExit busy_exit;

  // Root the layer and prove it is the exact GuiLayerService before dispatching TopGui.
  Root layer;
  if (!layer.duplicate(request->layer)) return PGR_CD_ERR_GCHANDLE;
  if (!is_exact(layer.get(), g_cfg.class_layer)) return PGR_CD_ERR_METADATA;

  // TopGui() -> IGuiController (any GUI; may be null for excluded opening screens). Instance call, no args.
  Root candidate;
  switch (call(g_cfg.m_top, &layer, nullptr, candidate, /*allowNull=*/true)) {
    case CALL_THROW:    return PGR_CD_ERR_MANAGED;
    case CALL_GCHANDLE: return PGR_CD_ERR_GCHANDLE;
    case CALL_DEAD:     return PGR_CD_ERR_GCHANDLE;
    case CALL_OK:       break;
  }
  if (!candidate.get()) return PGR_CD_SKIP;                       // TopGui excluded this screen: fall to scene
  if (!is_exact(candidate.get(), g_cfg.class_details)) return PGR_CD_SKIP;  // not a Details screen: benign

  // Fresh clock AFTER the lookup / root allocation and before publication.
  uint64_t final_now = 0;
  if (!read_boottime_ms(&final_now)) return PGR_CD_ERR_CLOCK_FAILURE;
  if (final_now >= deadline) return PGR_CD_SKIP;

  out->count = 1;
  out->handles[0] = reinterpret_cast<void *>(candidate.release());
  return PGR_CD_OK;
}

int api_scene(const PgrCatchDiscoveryRequest *request, PgrCatchDiscoveryOutput *out) {
  if (out == nullptr) return PGR_CD_ERR_NULL_ARG;
  init_output(out);
  int rc = PGR_CD_OK;
  if (!check_request(request, &rc)) return rc;  // layer intentionally ignored

  uint64_t deadline = 0;
  const int g = gate(request, &deadline);
  if (g != PGR_CD_OK) return g;

  { std::lock_guard<std::mutex> lock(g_state_mutex); if (g_busy) return PGR_CD_ERR_BUSY; g_busy = true; }
  BusyExit busy_exit;

  int err = 0;
  // Root the Details System.Type BEFORE FindObjectsOfType (it is the method's reference argument).
  Root detailsType;
  if (!type_object(g_cfg.class_details, detailsType, err)) return err;

  // FindObjectsOfType(typeof DetailsGuiController) -> UnityEngine.Object[] (static call). The Type is a
  // reference argument passed DIRECTLY (not pointer-to-pointer).
  Root arrayObj;
  {
    void *type = detailsType.get();
    if (!type) return PGR_CD_ERR_GCHANDLE;
    void *args[] = {type};
    switch (call(g_cfg.m_find, nullptr, args, arrayObj, /*allowNull=*/true)) {
      case CALL_THROW:    return PGR_CD_ERR_MANAGED;
      case CALL_GCHANDLE: return PGR_CD_ERR_GCHANDLE;
      case CALL_DEAD:     return PGR_CD_ERR_GCHANDLE;
      case CALL_OK:       break;
    }
  }
  if (!arrayObj.get()) return PGR_CD_SKIP;  // no array yet: not ready

  const uintptr_t count = reinterpret_cast<ArrayLen>(g_cfg.array_length)(arrayObj.get());
  if (count > PGR_CD_MAX_CANDIDATES) return PGR_CD_ERR_BAD_COUNT;  // strict ceiling: reject, never truncate

  // Root each nonnull exact-Details element individually. A wrong-class element from a TYPED query is a
  // metadata fault; null elements are skipped. On any gchandle failure every rooted candidate is freed
  // (the Root array destructors run before BusyExit resets the guard).
  Root candidates[PGR_CD_MAX_CANDIDATES];
  uint32_t found = 0;
  for (uintptr_t i = 0; i < count; ++i) {
    void *base = arrayObj.get();  // re-resolve the (possibly relocated) array base on every indexing
    if (!base) return PGR_CD_ERR_GCHANDLE;
    void *element = nullptr;
    std::memcpy(&element,
                static_cast<char *>(base) + PGR_CD_ARRAY_DATA_OFF + static_cast<size_t>(i) * sizeof(void *),
                sizeof(void *));
    if (!element) continue;  // null element: skip (not a candidate)
    if (!candidates[found].set(element)) return PGR_CD_ERR_GCHANDLE;
    if (!is_exact(candidates[found].get(), g_cfg.class_details)) return PGR_CD_ERR_METADATA;
    ++found;
  }

  // Fresh clock AFTER the walk / root allocation and before publication.
  uint64_t final_now = 0;
  if (!read_boottime_ms(&final_now)) return PGR_CD_ERR_CLOCK_FAILURE;
  if (final_now >= deadline) return PGR_CD_SKIP;

  out->count = found;
  for (uint32_t i = 0; i < found; ++i) out->handles[i] = reinterpret_cast<void *>(candidates[i].release());
  return PGR_CD_OK;
}

int api_release(PgrCatchDiscoveryOutput *out) {
  if (out == nullptr) return PGR_CD_ERR_NULL_ARG;
  if (!g_installed.load(std::memory_order_acquire)) return PGR_CD_ERR_NOT_INSTALLED;
  { std::lock_guard<std::mutex> lock(g_state_mutex); if (g_busy) return PGR_CD_ERR_BUSY; g_busy = true; }
  BusyExit busy_exit;
  // Detach the entire batch before any free can reenter a reader using this output.
  void *detached[PGR_CD_MAX_CANDIDATES];
  std::memcpy(detached, out->handles, sizeof(detached));
  std::memset(out->handles, 0, sizeof(out->handles));
  out->count = 0;
  auto gc_free = reinterpret_cast<FreeH>(g_cfg.gchandle_free);
  for (int i = 0; i < PGR_CD_MAX_CANDIDATES; ++i) {
    const auto handle = reinterpret_cast<uintptr_t>(detached[i]);
    if (handle) gc_free(handle);
  }
  out->count = 0;
  return PGR_CD_OK;
}

const PgrCatchDiscoveryApi kApiTable = {
    PGR_CD_API_VERSION, static_cast<uint32_t>(sizeof(PgrCatchDiscoveryApi)),
    &api_install, &api_top, &api_scene, &api_release,
};

}  // namespace

extern "C" const void *pgrazor_catch_discovery_api(uint32_t version) {
  if (version != PGR_CD_API_VERSION) return nullptr;
  return &kApiTable;
}
