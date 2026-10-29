#include "routes_services.h"
#include "hook_engine.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <time.h>
#include <unistd.h>

// Feature 45 native Routes service capture. See routes_services.h for the ABI and the division of labour
// with routes.js. Built -fno-exceptions / -fno-rtti like every sibling: no C++ throw ever escapes. Two
// il2cpp hooks capture the live RoutesService / RouteConsumptionService receivers; poll() commits them and
// re-derives the consumption service from RoutesService.dica (authoritative on replacement). Every GC root
// is owned by this module; the host borrows the active handles and never frees them.
//
// Thread model: all root manipulation (both hooks, poll, drain) runs on the Unity owner thread, so the
// pending/active handle slots are single-threaded and need no mutex. revoke() touches only atomics and may
// run on any thread. No mutex is ever held across the chained original or a field read.

static_assert(sizeof(PgrRoutesServicesConfig) == 96, "config 96");
static_assert(sizeof(PgrRoutesServicesRequest) == 32, "request 32");
static_assert(sizeof(PgrRoutesServicesOutput) == 40, "output 40");
static_assert(sizeof(PgrRoutesServicesApi) == 48, "table 48");

namespace {

constexpr uint64_t kMaxLeaseMs = PGR_RS_MAX_LEASE_MS;

// --- il2cpp runtime ABI typedefs ---------------------------------------------------
using ObjClass = void *(*)(void *obj);
using FieldGet = void (*)(void *obj, void *field, void *out);
using GcNew    = uintptr_t (*)(void *obj, int pinned);
using GcGet    = void *(*)(uintptr_t handle);
using GcFree   = void (*)(uintptr_t handle);
using ThreadCur= void *(*)();

// Chained originals: RequestNearbyRoutes returns a managed reference; get_InRangeOfEnd returns a bool.
using RequestNearbyFn = void *(*)(void *self, const void *method);
using InRangeFn       = bool (*)(void *self, const void *method);

// --- lifetime / publication --------------------------------------------------------
std::mutex g_install_mutex;
PgrRoutesServicesConfig g_cfg{};
std::atomic<bool> g_installed{false};
std::atomic<bool> g_hook_failed{false};

std::atomic<RequestNearbyFn> g_orig_request_nearby{nullptr};
std::atomic<InRangeFn> g_orig_in_range{nullptr};

// Permanent Unity owner, latched once from a genuine on-thread poll/drain.
std::atomic<uint64_t> g_owner_tid{0};

// Revocation generation (initially 1); bumped by revoke(). Compared against request.generation and stamped
// onto each pending capture so a revoke drops uncommitted captures.
std::atomic<uint64_t> g_gen{1};

// Capture heartbeat deadline (CLOCK_BOOTTIME ms). poll() refreshes it; revoke() clears it; hooks capture
// only while now < deadline. Independent of the gameRoutesAllowed planning lease.
std::atomic<uint64_t> g_capture_deadline{0};
std::atomic<uint64_t> g_capture_gen{0};
uint64_t g_active_gen = 0;
bool g_derived = false;

// Hook reentry guard (owner thread). A reentrant hook chains the original without capturing.
std::atomic<bool> g_hook_busy{false};

// Owner-thread-only capture slots. kind 0 = routes, 1 = consumption.
struct Slot {
  uintptr_t pending = 0;
  uint64_t pending_gen = 0;
  uintptr_t active = 0;
};
Slot g_routes;       // routes service
Slot g_consumption;  // consumption service

// --- clock / tid -------------------------------------------------------------------
uint64_t current_tid() { return static_cast<uint64_t>(static_cast<pid_t>(gettid())); }

bool read_boottime_ms(uint64_t *out_ms) {
  struct timespec ts {};
  if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0 || ts.tv_sec < 0 || ts.tv_nsec < 0) return false;
  *out_ms = static_cast<uint64_t>(ts.tv_sec) * 1000ULL + static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
  return true;
}

// Typed accessors onto the immutable published config.
ObjClass obj_class() { return reinterpret_cast<ObjClass>(g_cfg.object_get_class); }
GcNew gc_new()       { return reinterpret_cast<GcNew>(g_cfg.gchandle_new); }
GcGet gc_get()       { return reinterpret_cast<GcGet>(g_cfg.gchandle_get_target); }
GcFree gc_free()     { return reinterpret_cast<GcFree>(g_cfg.gchandle_free); }

bool is_exact(void *obj, void *klass) { return obj != nullptr && obj_class()(obj) == klass; }

void *handle_target(uintptr_t handle) { return handle ? gc_get()(handle) : nullptr; }
void free_handle(uintptr_t *slot) {
  if (slot && *slot) { uintptr_t h = *slot; *slot = 0; gc_free()(h); }
}

// Capture the receiver into the slot's PENDING root (owner thread, before the chained original). A prior
// pending of a different object is freed after the new receiver is rooted. Returns the re-resolved
// receiver to pass to the original (the live rooted pointer), or null on root failure (caller passes raw).
void *capture_pending(Slot *slot, void *self, uint64_t gen) {
  if (handle_target(slot->active) == self) {
    // Retain the stable active id; retiring a pending root may move its target.
    free_handle(&slot->pending);
    return handle_target(slot->active);
  }
  if (handle_target(slot->pending) == self) {
    slot->pending_gen = gen;
    return handle_target(slot->pending);
  }
  const uintptr_t h = gc_new()(self, 0);
  if (!h) return nullptr;
  const uintptr_t previous = slot->pending;
  slot->pending = h;
  slot->pending_gen = gen;
  if (previous) gc_free()(previous);
  return handle_target(h);
}

// Commit a slot's pending root into its active root. Frees the previous active only when the object
// actually changed. A pending stamped under a stale generation is dropped. Returns true when the active
// object was replaced this poll.
bool commit_slot(Slot *slot) {
  if (!slot->pending) return false;
  if (slot->pending_gen != g_gen.load(std::memory_order_acquire)) { free_handle(&slot->pending); return false; }
  void *pending_obj = handle_target(slot->pending);
  void *active_obj = handle_target(slot->active);
  if (!pending_obj) { free_handle(&slot->pending); return false; }
  if (pending_obj == active_obj) { free_handle(&slot->pending); return false; }
  free_handle(&slot->active);
  slot->active = slot->pending;  // transfer ownership
  slot->pending = 0;
  return true;
}

// --- hooks (owner thread) ----------------------------------------------------------
// Gate shared by both hooks: capture only when installed, on the latched owner thread, with a live
// heartbeat, not reentrant. On a false gate the caller chains the original unchanged (no fabricated return).
bool hook_gate_ok() {
  if (!g_installed.load(std::memory_order_acquire)) return false;
  const uint64_t owner = g_owner_tid.load(std::memory_order_acquire);
  if (!owner || current_tid() != owner ||
      !reinterpret_cast<ThreadCur>(g_cfg.thread_current)()) return false;
  const uint64_t gen = g_gen.load(std::memory_order_acquire);
  uint64_t now = 0;
  return read_boottime_ms(&now) &&
      g_capture_gen.load(std::memory_order_acquire) == gen &&
      now < g_capture_deadline.load(std::memory_order_acquire);
}

void *hook_request_nearby(void *self, const void *method) {
  const auto orig = g_orig_request_nearby.load(std::memory_order_acquire);
  const uint64_t gen = g_gen.load(std::memory_order_acquire);
  if (!hook_gate_ok() || g_hook_busy.exchange(true, std::memory_order_acq_rel))
    return orig ? orig(self, method) : nullptr;
  void *receiver = self;
  if (is_exact(self, g_cfg.class_routes)) {
    if (void *resolved = capture_pending(&g_routes, self, gen)) receiver = resolved;
  }
  void *result = orig ? orig(receiver, method) : nullptr;
  // Atomic bookkeeping only: no GC operation may move the managed return.
  g_hook_busy.store(false, std::memory_order_release);
  return result;
}

bool hook_in_range_of_end(void *self, const void *method) {
  const auto orig = g_orig_in_range.load(std::memory_order_acquire);
  const uint64_t gen = g_gen.load(std::memory_order_acquire);
  if (!hook_gate_ok() || g_hook_busy.exchange(true, std::memory_order_acq_rel))
    return orig ? orig(self, method) : false;
  void *receiver = self;
  // Once RoutesService is known, its dependency is the sole authority.
  if (!g_routes.active && is_exact(self, g_cfg.class_consumption)) {
    if (void *resolved = capture_pending(&g_consumption, self, gen)) receiver = resolved;
  }
  const bool result = orig ? orig(receiver, method) : false;
  g_hook_busy.store(false, std::memory_order_release);
  return result;
}

// --- owner latching ----------------------------------------------------------------
// Supplied tid must be nonzero AND equal the current OS tid BEFORE the owner is latched; a later foreign
// tid refuses. Mirrors the sibling owner latch.
bool owner_ready(uint64_t tid) {
  if (tid == 0 || current_tid() != tid) return false;
  uint64_t owner = g_owner_tid.load(std::memory_order_acquire);
  if (!(owner == 0 || owner == tid)) return false;
  uint64_t expected = 0;
  g_owner_tid.compare_exchange_strong(expected, tid, std::memory_order_acq_rel);
  return g_owner_tid.load(std::memory_order_acquire) == tid;
}

void init_output(PgrRoutesServicesOutput *out) {
  std::memset(out, 0, sizeof(*out));
  out->version = PGR_RS_OUTPUT_VERSION;
  out->size = sizeof(PgrRoutesServicesOutput);
}

// --- config validation / install ---------------------------------------------------
bool validate_config(const PgrRoutesServicesConfig *c, int *rc) {
  if (c == nullptr) { *rc = PGR_RS_ERR_NULL_ARG; return false; }
  if (c->version != PGR_RS_CONFIG_VERSION) { *rc = PGR_RS_ERR_BAD_VERSION; return false; }
  if (c->size != sizeof(PgrRoutesServicesConfig)) { *rc = PGR_RS_ERR_BAD_SIZE; return false; }
  void *const *ptrs = reinterpret_cast<void *const *>(
      reinterpret_cast<const unsigned char *>(c) + offsetof(PgrRoutesServicesConfig, object_get_class));
  for (int i = 0; i < PGR_RS_PTR_COUNT; ++i)
    if (ptrs[i] == nullptr) { *rc = PGR_RS_ERR_MISSING_PTR; return false; }
  *rc = 0;
  return true;
}

// Install both hooks. On a partial failure, roll back whatever was patched (no stale original) and latch
// the failure so the pilot stays fail-closed. Never patches the same address with a second engine.
int install_hooks() {
  void *req_orig = nullptr;
  if (pgrazor_hook(g_cfg.addr_request_nearby, reinterpret_cast<void *>(&hook_request_nearby), &req_orig) != 0 ||
      req_orig == nullptr) {
    return PGR_RS_ERR_HOOK;
  }
  // Publish the trampoline before placing the second hook so a call racing the first patch still chains.
  g_orig_request_nearby.store(reinterpret_cast<RequestNearbyFn>(req_orig), std::memory_order_release);
  void *ir_orig = nullptr;
  if (pgrazor_hook(g_cfg.addr_in_range_of_end, reinterpret_cast<void *>(&hook_in_range_of_end), &ir_orig) != 0 ||
      ir_orig == nullptr) {
    pgrazor_unhook(g_cfg.addr_request_nearby);  // roll back the one we placed (no stale original)
    return PGR_RS_ERR_HOOK;
  }
  g_orig_in_range.store(reinterpret_cast<InRangeFn>(ir_orig), std::memory_order_release);
  return 0;
}

int api_install(const PgrRoutesServicesConfig *config) {
  int rc = 0;
  if (!validate_config(config, &rc)) return rc;
  uint64_t probe = 0;
  if (!read_boottime_ms(&probe)) return PGR_RS_ERR_CLOCK;

  std::lock_guard<std::mutex> install_lock(g_install_mutex);
  if (g_hook_failed.load(std::memory_order_acquire)) return PGR_RS_ERR_HOOK;
  if (g_installed.load(std::memory_order_acquire)) {
    return std::memcmp(&g_cfg, config, sizeof(*config)) == 0 ? 0 : PGR_RS_ERR_RECONFIG;
  }
  g_cfg = *config;
  const int hrc = install_hooks();
  if (hrc != 0) {
    g_hook_failed.store(true, std::memory_order_release);
    return hrc;
  }
  g_installed.store(true, std::memory_order_release);
  return 0;
}

uint64_t api_generation() { return g_gen.load(std::memory_order_acquire); }

uint64_t api_revoke() {
  g_capture_deadline.store(0, std::memory_order_release);  // stop capturing; no managed work, no frees
  return g_gen.fetch_add(1, std::memory_order_acq_rel) + 1;
}

// Validate the request header + owner/thread gate shared by poll/drain. Returns 0 on pass or a negative
// error for a hard fault; *skip is set true for a benign gate refusal.
int gate_request(const PgrRoutesServicesRequest *req, bool require_lease, uint64_t *deadline_out, bool *skip) {
  *skip = false;
  if (!g_installed.load(std::memory_order_acquire)) return PGR_RS_ERR_NOT_INSTALLED;
  if (!req) return PGR_RS_ERR_NULL_ARG;
  if (req->version != PGR_RS_REQUEST_VERSION) return PGR_RS_ERR_BAD_VERSION;
  if (req->size != sizeof(*req)) return PGR_RS_ERR_BAD_SIZE;
  if (!req->tid || req->tid != current_tid() ||
      !reinterpret_cast<ThreadCur>(g_cfg.thread_current)() || !owner_ready(req->tid)) {
    *skip = true; return 0;  // off owner thread / not attached -> benign SKIP
  }
  if (require_lease) {
    if (req->generation != g_gen.load(std::memory_order_acquire)) { *skip = true; return 0; }
    uint64_t residual = req->residual_ms > kMaxLeaseMs ? kMaxLeaseMs : req->residual_ms;
    if (!residual) { *skip = true; return 0; }
    uint64_t now = 0;
    if (!read_boottime_ms(&now)) return PGR_RS_ERR_CLOCK;
    *deadline_out = now + residual;
  }
  return 0;
}

void clear_roots() {
  free_handle(&g_routes.pending);
  free_handle(&g_routes.active);
  free_handle(&g_consumption.pending);
  free_handle(&g_consumption.active);
  g_routes.pending_gen = g_consumption.pending_gen = 0;
  g_active_gen = 0;
  g_derived = false;
}

bool poll_live(uint64_t gen, uint64_t deadline) {
  uint64_t now = 0;
  return g_gen.load(std::memory_order_acquire) == gen &&
      current_tid() == g_owner_tid.load(std::memory_order_acquire) &&
      reinterpret_cast<ThreadCur>(g_cfg.thread_current)() &&
      read_boottime_ms(&now) && now < deadline;
}

int api_poll(const PgrRoutesServicesRequest *req, PgrRoutesServicesOutput *out) {
  if (!out) return PGR_RS_ERR_NULL_ARG;
  init_output(out);
  uint64_t deadline = 0;
  bool skip = false;
  const int grc = gate_request(req, true, &deadline, &skip);
  if (grc != 0) return out->status = grc;
  if (skip) return out->status = PGR_RS_SKIP;
  if (g_hook_busy.exchange(true, std::memory_order_acq_rel))
    return out->status = PGR_RS_ERR_BUSY;
  const uint64_t gen = req->generation;
  uint32_t flags = 0;
  if (g_active_gen != gen) clear_roots();
  g_active_gen = gen;
  bool live = poll_live(gen, deadline);
  if (live) {
    const bool replaced = commit_slot(&g_routes);
    if (replaced) { flags |= PGR_RS_FLAG_ROUTES_REPLACED; g_derived = false; }
    live = poll_live(gen, deadline);
    if (live && g_routes.active) {
      free_handle(&g_consumption.pending);
      // A changed RoutesService must never retain the preceding account's service.
      if (!g_derived && poll_live(gen, deadline)) {
        void *routes = handle_target(g_routes.active);
        void *dica = nullptr;
        if (is_exact(routes, g_cfg.class_routes))
          reinterpret_cast<FieldGet>(g_cfg.field_get_value)(routes, g_cfg.f_routes_dica, &dica);
        uintptr_t replacement = 0;
        if (poll_live(gen, deadline) && is_exact(dica, g_cfg.class_consumption)) {
          if (handle_target(g_consumption.active) == dica) g_derived = true;
          else replacement = gc_new()(dica, 0);
        }
        if (replacement) {
          const uintptr_t old = g_consumption.active;
          g_consumption.active = replacement;
          if (old) gc_free()(old);
          g_derived = true;
          flags |= PGR_RS_FLAG_CONSUMPTION_REPLACED;
        } else if (!g_derived && g_consumption.active) {
          free_handle(&g_consumption.active);
          flags |= PGR_RS_FLAG_CONSUMPTION_REPLACED;
        }
      }
    } else if (live && commit_slot(&g_consumption)) {
      flags |= PGR_RS_FLAG_CONSUMPTION_REPLACED;
    }
  }
  live = live && poll_live(gen, deadline);
  if (live) {
    // A concurrent revoke can clear the deadline between these stores, but the
    // separate generation stamp ensures the old heartbeat cannot revive capture.
    g_capture_gen.store(gen, std::memory_order_release);
    g_capture_deadline.store(deadline, std::memory_order_release);
    if (g_routes.active) flags |= PGR_RS_FLAG_ROUTES_PRESENT;
    if (g_consumption.active) flags |= PGR_RS_FLAG_CONSUMPTION_PRESENT;
    if (g_derived) flags |= PGR_RS_FLAG_CONSUMPTION_FROM_DICA;
    out->flags = flags;
    out->generation = gen;
    out->routes_handle = g_routes.active;
    out->consumption_handle = g_consumption.active;
    out->status = PGR_RS_OK;
  } else {
    g_capture_deadline.store(0, std::memory_order_release);
    out->status = PGR_RS_SKIP;
  }
  g_hook_busy.store(false, std::memory_order_release);
  return out->status;
}

int api_drain(const PgrRoutesServicesRequest *req) {
  uint64_t deadline = 0;
  bool skip = false;
  const int grc = gate_request(req, false, &deadline, &skip);
  if (grc != 0) return grc;
  if (skip) return PGR_RS_SKIP;
  if (g_hook_busy.exchange(true, std::memory_order_acq_rel)) return PGR_RS_ERR_BUSY;
  g_capture_deadline.store(0, std::memory_order_release);
  g_capture_gen.store(0, std::memory_order_release);
  clear_roots();
  g_hook_busy.store(false, std::memory_order_release);
  return PGR_RS_OK;
}


const PgrRoutesServicesApi kApiTable = {
    PGR_RS_API_VERSION, static_cast<uint32_t>(sizeof(PgrRoutesServicesApi)),
    &api_install, &api_generation, &api_revoke, &api_poll, &api_drain,
};

}  // namespace

extern "C" const void *pgrazor_routes_services_api(uint32_t version) {
  if (version != PGR_RS_API_VERSION) return nullptr;
  return &kApiTable;
}
