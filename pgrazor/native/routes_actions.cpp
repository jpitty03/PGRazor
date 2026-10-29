#include "routes_actions.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <time.h>
#include <unistd.h>

// Feature 43 native Routes managed state/actions. See routes_actions.h for the ABI and the division of
// labour with routes.js. Built -fno-exceptions / -fno-rtti like every sibling: a JS "throw" path
// becomes a fail-closed return; every GC root is freed exactly once on the owner thread; the one
// mutating invoke is issued at most once per call. No mutex is held across a managed call. This module
// installs NO hook and NEVER calls back into JS.
//
// Lifetime discipline (identical intent to the routes.js roots/host.hold helpers and the sibling
// readers/actions): service and route objects arrive as borrowed host handles; each is re-resolved via
// gchandle_get_target and rooted with our OWN gchandle_new before any other managed work, and the host
// handle is never freed. Every managed value is re-resolved from its root after each invoke / new /
// free, and a fresh deadline + generation preflight precedes every managed call. The pre-invoke
// preflight is re-checked immediately before the single mutating invoke; a revoke that races the
// construction aborts WITHOUT invoking. Once the invoke has issued, the result is SUBMITTED even if a
// revoke raced the call -- never retried. A busy flag spans all root cleanup; only one matching CURRENT leaf is allowed during confirmation.

static_assert(sizeof(PgrRoutesActionsConfig) == 352, "config 352");
static_assert(sizeof(PgrRoutesActionsRequest) == 328, "request 328");
static_assert(sizeof(PgrRoutesActionsOutput) == 280, "output 280");
static_assert(sizeof(PgrRoutesActionsApi) == 40, "table 40");

namespace {

constexpr uint64_t kMaxLeaseMs = PGR_RA_MAX_LEASE_MS;
constexpr double kMaxSafeInteger = 9007199254740991.0;  // 2^53 - 1 (JS Number.MAX_SAFE_INTEGER)

// --- il2cpp runtime ABI typedefs ---------------------------------------------------
using Invoke   = void *(*)(const void *method, void *obj, void **params, void **exc);
using ObjClass = void *(*)(void *obj);
using ObjNew   = void *(*)(void *klass);
using Unbox    = void *(*)(void *boxed);
using GcNew    = uintptr_t (*)(void *obj, int pinned);
using GcGet    = void *(*)(uintptr_t handle);
using GcFree   = void (*)(uintptr_t handle);
using ThreadCur= void *(*)();
using FieldGet = void (*)(void *obj, void *field, void *out);
using FieldSet = void (*)(void *obj, void *field, void *value);
using StrLen   = int32_t (*)(void *string);
using StrChars = uint16_t *(*)(void *string);

// --- lifetime / publication --------------------------------------------------------
std::mutex g_install_mutex;
std::mutex g_state_mutex;
PgrRoutesActionsConfig g_cfg{};
std::atomic<bool> g_installed{false};

// Permanent Unity owner, latched once from a genuine on-thread call.
std::atomic<uint64_t> g_owner_tid{0};

// Revocation generation (initially 1). Bumped only by revoke(); compared against request.generation.
std::atomic<uint64_t> g_gen{1};

// Process-wide mutating sequence high-water and the reentrancy busy flag (guarded by g_state_mutex).
uint64_t g_seq_highwater = 0;
bool g_busy = false;

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
bool is_exact(void *obj, void *klass) { return obj != nullptr && obj_class()(obj) == klass; }

// Owning temporary root. set() roots a raw managed object; the destructor frees any live handle, so an
// early return always balances every root allocated on that path. No handle ever leaves this module.
struct Root {
  uintptr_t handle = 0;
  Root() = default;
  Root(const Root &) = delete;
  Root &operator=(const Root &) = delete;
  ~Root() { if (handle) reinterpret_cast<GcFree>(g_cfg.gchandle_free)(handle); }
  void *get() const { return handle ? reinterpret_cast<GcGet>(g_cfg.gchandle_get_target)(handle) : nullptr; }
  bool set(void *object) {
    if (handle) { reinterpret_cast<GcFree>(g_cfg.gchandle_free)(handle); handle = 0; }
    if (!object) return false;
    handle = reinterpret_cast<GcNew>(g_cfg.gchandle_new)(object, 0);
    return handle && get();
  }
};

// Busy reset runs AFTER the Roots declared below it are destroyed, so reentrant root cleanup still
// observes busy == true (the guard spans destruction / root cleanup).
struct ConfirmationContext { const Root *service; uint64_t deadline, generation; bool leaf = false; };
thread_local ConfirmationContext *g_confirmation = nullptr;
struct BusyExit {
  ConfirmationContext *parent;
  explicit BusyExit(ConfirmationContext *p = nullptr) : parent(p) {}
  ~BusyExit() {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (parent) parent->leaf = false; else g_busy = false;
  }
};

// Fresh deadline + attached-thread + generation gate before every managed step and once more before
// the mutating invoke. On failure sets *status/*reason: a bad clock is a FAULT; an expiry / detached
// thread / revoked generation is a benign REFUSE (lease-expired). true keeps the window open.
bool preflight(uint64_t deadline, uint64_t generation, int *status, int *reason) {
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) { *status = PGR_RA_FAULT; *reason = PGR_RA_REASON_NATIVE_ERROR; return false; }
  if (now >= deadline) { *status = PGR_RA_REFUSE; *reason = PGR_RA_REASON_LEASE_EXPIRED; return false; }
  if (!reinterpret_cast<ThreadCur>(g_cfg.thread_current)()) {
    *status = PGR_RA_REFUSE; *reason = PGR_RA_REASON_LEASE_EXPIRED; return false;
  }
  if (g_gen.load(std::memory_order_acquire) != generation) {
    *status = PGR_RA_REFUSE; *reason = PGR_RA_REASON_LEASE_EXPIRED; return false;
  }
  return true;
}

// Read a value-type bool field (0/1 valid; malformed values return -1), re-resolving the base
// from its root immediately before the read (no allocation happens inside).
int read_bool_field(void *obj, void *field) {
  if (!obj) return -1;
  uint8_t v = 0;
  reinterpret_cast<FieldGet>(g_cfg.field_get_value)(obj, field, &v);
  return v <= 1 ? v : -1;
}

// Read a reference field (object pointer), re-resolving the base from its root first.
void *read_ref_field(void *obj, void *field) {
  if (!obj) return nullptr;
  void *out = nullptr;
  reinterpret_cast<FieldGet>(g_cfg.field_get_value)(obj, field, &out);
  return out;
}

// Invoke a boxed-Boolean getter on the receiver rooted in `svc`.
//   Strict mode (START/PAUSE gates): any anomaly (throw / null / wrong class / unbox null / byte>1) is
//   a FAULT. Lenient mode (CURRENT getters, like JS safeBool): managed exceptions yield false;
//   malformed values, root-allocation failure and expiry still abort.
// Returns 1 true, 0 false, or -1 (abort: *status/*reason set).
int invoke_getter_bool(void *method, const Root &svc, uint64_t deadline, uint64_t generation,
                       bool strict, int *status, int *reason) {
  if (!preflight(deadline, generation, status, reason)) return -1;
  void *self = svc.get();
  if (!self) { *status = PGR_RA_FAULT; *reason = PGR_RA_REASON_NATIVE_ERROR; return -1; }
  void *exc = nullptr;
  void *box = reinterpret_cast<Invoke>(g_cfg.runtime_invoke)(method, self, nullptr, &exc);
  auto anomaly = [&]() -> int {
    *status = PGR_RA_FAULT; *reason = PGR_RA_REASON_NATIVE_ERROR; return -1;
  };
  if (exc && !strict) return 0;  // Only managed getter exceptions retain legacy safeBool behavior.
  if (exc || !box) return anomaly();
  Root boxRoot;
  if (!boxRoot.set(box)) { *status = PGR_RA_FAULT; *reason = PGR_RA_REASON_NATIVE_ERROR; return -1; }  // alloc aborts
  if (!is_exact(boxRoot.get(), g_cfg.class_bool)) return anomaly();
  void *value = reinterpret_cast<Unbox>(g_cfg.object_unbox)(boxRoot.get());
  if (!value) return anomaly();
  uint8_t b = *static_cast<uint8_t *>(value);
  if (b > 1) return anomaly();
  return b;
}

// True if the managed string (already proven exact class_string) equals the request UTF16 id.
bool string_equals_id(void *str, const uint16_t *id, uint32_t id_len) {
  int32_t len = reinterpret_cast<StrLen>(g_cfg.string_length)(str);
  if (len < 0 || static_cast<uint32_t>(len) != id_len) return false;
  if (id_len == 0) return true;
  uint16_t *chars = reinterpret_cast<StrChars>(g_cfg.string_chars)(str);
  if (!chars) return false;
  return std::memcmp(chars, id, static_cast<size_t>(id_len) * sizeof(uint16_t)) == 0;
}

// Staged authoritative current-route state, used by CURRENT (to publish flags/id) and COMPLETE (to
// revalidate). Populated with every root released before it is read back.
struct CurrentState {
  bool present = false;
  bool started = false, in_range_end = false, in_range_resume = false, is_completing = false;
  bool paused = false, completed_walk = false, acquired_reward = false, reversed = false;
  uint32_t id_len = 0;
  uint16_t id[PGR_RA_ID_MAX];
};

// Read the authoritative current route on the consumption service rooted in `svc`. Returns 1 (state
// read; cs.present indicates a play), 0 (refuse; *status/*reason set), or -1 (fault; set). Every root
// is released before return. A play null / wrong class is an empty present=false state (success 1). A
// present play with a malformed route (wrong class / bad id string / id length) fails closed.
int read_current_state(const Root &svc, uint64_t deadline, uint64_t generation, CurrentState *cs,
                       int *status, int *reason) {
  *cs = CurrentState{};
  if (!preflight(deadline, generation, status, reason)) return *status == PGR_RA_FAULT ? -1 : 0;

  // CurrentRoute getter: nullable, exact class_play. A throw / non-root here is a fault.
  Root play;
  {
    void *self = svc.get();
    if (!self) { *status = PGR_RA_FAULT; *reason = PGR_RA_REASON_NATIVE_ERROR; return -1; }
    void *exc = nullptr;
    void *obj = reinterpret_cast<Invoke>(g_cfg.runtime_invoke)(g_cfg.m_currentRoute, self, nullptr, &exc);
    if (exc) { *status = PGR_RA_FAULT; *reason = PGR_RA_REASON_NATIVE_ERROR; return -1; }
    if (!obj || !is_exact(obj, g_cfg.class_play)) return 1;       // empty current (legacy: no play)
    if (!play.set(obj)) { *status = PGR_RA_FAULT; *reason = PGR_RA_REASON_NATIVE_ERROR; return -1; }
  }
  cs->present = true;

  // Four getter bools (lenient safeBool). Run every invoke FIRST (each can relocate play), then read
  // the re-resolved play's raw fields with no further invoke between them.
  int b;
  b = invoke_getter_bool(g_cfg.m_hasStarted, svc, deadline, generation, false, status, reason);
  if (b < 0) return *status == PGR_RA_FAULT ? -1 : 0; cs->started = b == 1;
  b = invoke_getter_bool(g_cfg.m_inRangeOfEnd, svc, deadline, generation, false, status, reason);
  if (b < 0) return *status == PGR_RA_FAULT ? -1 : 0; cs->in_range_end = b == 1;
  b = invoke_getter_bool(g_cfg.m_inRangeToResume, svc, deadline, generation, false, status, reason);
  if (b < 0) return *status == PGR_RA_FAULT ? -1 : 0; cs->in_range_resume = b == 1;
  b = invoke_getter_bool(g_cfg.m_isCompleting, svc, deadline, generation, false, status, reason);
  if (b < 0) return *status == PGR_RA_FAULT ? -1 : 0; cs->is_completing = b == 1;

  void *p = play.get();
  if (!p) { *status = PGR_RA_FAULT; *reason = PGR_RA_REASON_NATIVE_ERROR; return -1; }
  const int paused = read_bool_field(p, g_cfg.f_playPaused);
  const int walked = read_bool_field(p, g_cfg.f_playCompletedWalk);
  const int rewarded = read_bool_field(p, g_cfg.f_playAcquiredReward);
  const int reversed = read_bool_field(p, g_cfg.f_playTravelInReverse);
  if (paused < 0 || walked < 0 || rewarded < 0 || reversed < 0) {
    *status = PGR_RA_FAULT; *reason = PGR_RA_REASON_NATIVE_ERROR; return -1;
  }
  cs->paused = paused == 1; cs->completed_walk = walked == 1;
  cs->acquired_reward = rewarded == 1; cs->reversed = reversed == 1;

  // Route + id. A present play requires a valid Route and nonempty Id. The Route must
  // be an exact SharedRouteProto whose Id string decodes to length 1..128; otherwise fail closed.
  void *route_raw = read_ref_field(p, g_cfg.f_playRoute);
  if (route_raw) {
    Root route;
    if (!route.set(route_raw)) { *status = PGR_RA_FAULT; *reason = PGR_RA_REASON_NATIVE_ERROR; return -1; }
    if (!is_exact(route.get(), g_cfg.class_shared)) { *status = PGR_RA_FAULT; *reason = PGR_RA_REASON_NATIVE_ERROR; return -1; }
    void *id_raw = read_ref_field(route.get(), g_cfg.f_sharedId);
    if (id_raw) {
      Root idStr;
      if (!idStr.set(id_raw)) { *status = PGR_RA_FAULT; *reason = PGR_RA_REASON_NATIVE_ERROR; return -1; }
      if (!is_exact(idStr.get(), g_cfg.class_string)) { *status = PGR_RA_FAULT; *reason = PGR_RA_REASON_NATIVE_ERROR; return -1; }
      void *s = idStr.get();
      int32_t len = reinterpret_cast<StrLen>(g_cfg.string_length)(s);
      if (len < 1 || len > PGR_RA_ID_MAX) { *status = PGR_RA_FAULT; *reason = PGR_RA_REASON_NATIVE_ERROR; return -1; }
      uint16_t *chars = reinterpret_cast<StrChars>(g_cfg.string_chars)(s);
      if (!chars) { *status = PGR_RA_FAULT; *reason = PGR_RA_REASON_NATIVE_ERROR; return -1; }
      std::memcpy(cs->id, chars, static_cast<size_t>(len) * sizeof(uint16_t));
      cs->id_len = static_cast<uint32_t>(len);
    }
  }
  if (!cs->id_len) { *status = PGR_RA_FAULT; *reason = PGR_RA_REASON_NATIVE_ERROR; return -1; }
  return 1;  // roots released as play/route/idStr destruct on return
}

// --- per-action implementations ----------------------------------------------------
// Each resolves/roots its service (consumption or routes) in api_execute, runs under g_busy with a
// fresh deadline + generation, and on success sets out->status = SUCCESS, out->reason = NONE.

int do_current(const Root &svc, uint64_t deadline, uint64_t generation, PgrRoutesActionsOutput *out) {
  CurrentState cs;
  int status = 0, reason = 0;
  int rc = read_current_state(svc, deadline, generation, &cs, &status, &reason);
  if (rc <= 0) { out->status = status; out->reason = reason; return status; }

  // All roots released inside read_current_state; final expiry / generation check before publishing.
  if (!preflight(deadline, generation, &status, &reason)) {  // current publication expires empty
    out->status = status; out->reason = reason; return status;
  }
  uint32_t flags = 0;
  if (cs.present) {
    flags |= PGR_RA_FLAG_PRESENT;
    if (cs.started)         flags |= PGR_RA_FLAG_STARTED;
    if (cs.paused)          flags |= PGR_RA_FLAG_PAUSED;
    if (cs.completed_walk)  flags |= PGR_RA_FLAG_COMPLETED_WALK;
    if (cs.acquired_reward) flags |= PGR_RA_FLAG_ACQUIRED_REWARD;
    if (cs.in_range_end)    flags |= PGR_RA_FLAG_IN_RANGE_END;
    if (cs.in_range_resume) flags |= PGR_RA_FLAG_IN_RANGE_RESUME;
    if (cs.is_completing)   flags |= PGR_RA_FLAG_IS_COMPLETING;
    if (cs.reversed)        flags |= PGR_RA_FLAG_REVERSED;
    out->id_length = cs.id_len;
    if (cs.id_len) std::memcpy(out->id, cs.id, static_cast<size_t>(cs.id_len) * sizeof(uint16_t));
  }
  out->flags = flags;
  out->status = PGR_RA_SUCCESS;
  out->reason = PGR_RA_REASON_NONE;
  return PGR_RA_SUCCESS;
}

int do_discover(const Root &svc, uint64_t deadline, uint64_t generation, PgrRoutesActionsOutput *out) {
  int status = 0, reason = 0;
  if (!preflight(deadline, generation, &status, &reason)) { out->status = status; out->reason = reason; return status; }
  void *self = svc.get();
  if (!self) { out->status = PGR_RA_FAULT; out->reason = PGR_RA_REASON_NATIVE_ERROR; return PGR_RA_FAULT; }
  void *exc = nullptr;
  void *ret = reinterpret_cast<Invoke>(g_cfg.runtime_invoke)(g_cfg.m_requestNearby, self, nullptr, &exc);
  if (exc) { out->status = PGR_RA_FAULT; out->reason = PGR_RA_REASON_NATIVE_ERROR; return PGR_RA_FAULT; }
  // Submitted. Root/free any returned value immediately; a root failure here does not un-submit.
  if (ret) { Root tmp; tmp.set(ret); }
  out->status = PGR_RA_SUCCESS;
  out->reason = PGR_RA_REASON_NONE;
  return PGR_RA_SUCCESS;
}

int do_start(const Root &svc, const PgrRoutesActionsRequest *r, uint64_t deadline, uint64_t generation,
             PgrRoutesActionsOutput *out) {
  int status = 0, reason = 0;
  int b;
  b = invoke_getter_bool(g_cfg.m_flagInitialized, svc, deadline, generation, true, &status, &reason);
  if (b < 0) { out->status = status; out->reason = reason; return status; }
  if (b == 0) { out->status = PGR_RA_REFUSE; out->reason = PGR_RA_REASON_NOT_INIT; return PGR_RA_REFUSE; }
  b = invoke_getter_bool(g_cfg.m_enabledForPlayer, svc, deadline, generation, true, &status, &reason);
  if (b < 0) { out->status = status; out->reason = reason; return status; }
  if (b == 0) { out->status = PGR_RA_REFUSE; out->reason = PGR_RA_REASON_NOT_PERMITTED; return PGR_RA_REFUSE; }
  b = invoke_getter_bool(g_cfg.m_isConsuming, svc, deadline, generation, true, &status, &reason);
  if (b < 0) { out->status = status; out->reason = reason; return status; }
  if (b == 1) { out->status = PGR_RA_REFUSE; out->reason = PGR_RA_REASON_ALREADY_ACTIVE; return PGR_RA_REFUSE; }

  if (r->route_handle == 0) { out->status = PGR_RA_REFUSE; out->reason = PGR_RA_REASON_NOT_DISCOVERED; return PGR_RA_REFUSE; }
  if (r->id_length < 1 || r->id_length > PGR_RA_ID_MAX) { out->status = PGR_RA_REFUSE; out->reason = PGR_RA_REASON_ID_MISMATCH; return PGR_RA_REFUSE; }

  if (!preflight(deadline, generation, &status, &reason)) { out->status = status; out->reason = reason; return status; }
  // Resolve + root the borrowed route handle; never free the host handle.
  void *route_raw = reinterpret_cast<GcGet>(g_cfg.gchandle_get_target)(static_cast<uintptr_t>(r->route_handle));
  if (!route_raw) { out->status = PGR_RA_REFUSE; out->reason = PGR_RA_REASON_ROUTE_STALE; return PGR_RA_REFUSE; }
  Root shared;
  if (!shared.set(route_raw)) { out->status = PGR_RA_FAULT; out->reason = PGR_RA_REASON_NATIVE_ERROR; return PGR_RA_FAULT; }
  if (!is_exact(shared.get(), g_cfg.class_shared)) { out->status = PGR_RA_REFUSE; out->reason = PGR_RA_REASON_ROUTE_STALE; return PGR_RA_REFUSE; }

  // Route id must match the requested id (defends against a stale / replaced handle).
  void *id_raw = read_ref_field(shared.get(), g_cfg.f_sharedId);
  if (!id_raw) { out->status = PGR_RA_REFUSE; out->reason = PGR_RA_REASON_ID_MISMATCH; return PGR_RA_REFUSE; }
  {
    Root idStr;
    if (!idStr.set(id_raw)) { out->status = PGR_RA_FAULT; out->reason = PGR_RA_REASON_NATIVE_ERROR; return PGR_RA_FAULT; }
    if (!is_exact(idStr.get(), g_cfg.class_string) || !string_equals_id(idStr.get(), r->id, r->id_length)) {
      out->status = PGR_RA_REFUSE; out->reason = PGR_RA_REASON_ID_MISMATCH; return PGR_RA_REFUSE;
    }
  }
  const bool reversed = r->reversed != 0;
  const int can_reverse = read_bool_field(shared.get(), g_cfg.f_sharedReversible);
  if (can_reverse < 0) { out->status = PGR_RA_FAULT; out->reason = PGR_RA_REASON_NATIVE_ERROR; return out->status; }
  if (reversed && can_reverse == 0) {
    out->status = PGR_RA_REFUSE; out->reason = PGR_RA_REASON_NOT_REVERSIBLE; return PGR_RA_REFUSE;
  }

  // Build RoutesConsumptionStartData: object_new + ctor + PublishedRouteProto + Reversed. Root the
  // request immediately so it survives the ctor and every re-resolution.
  if (!preflight(deadline, generation, &status, &reason)) { out->status = status; out->reason = reason; return status; }
  void *data_raw = reinterpret_cast<ObjNew>(g_cfg.object_new)(g_cfg.class_startData);
  if (!data_raw) { out->status = PGR_RA_FAULT; out->reason = PGR_RA_REASON_ALLOC_FAILED; return PGR_RA_FAULT; }
  Root data;
  if (!data.set(data_raw)) { out->status = PGR_RA_FAULT; out->reason = PGR_RA_REASON_NATIVE_ERROR; return PGR_RA_FAULT; }
  if (!is_exact(data.get(), g_cfg.class_startData)) { out->status = PGR_RA_FAULT; out->reason = PGR_RA_REASON_NATIVE_ERROR; return PGR_RA_FAULT; }

  if (!preflight(deadline, generation, &status, &reason)) { out->status = status; out->reason = reason; return status; }
  {
    void *exc = nullptr;
    reinterpret_cast<Invoke>(g_cfg.runtime_invoke)(g_cfg.m_ctor, data.get(), nullptr, &exc);
    if (exc) { out->status = PGR_RA_FAULT; out->reason = PGR_RA_REASON_NATIVE_ERROR; return PGR_RA_FAULT; }
  }
  // Refresh BOTH service + shared + data after the ctor (any of them may have relocated).
  void *data_obj = data.get(), *shared_obj = shared.get(), *svc_obj = svc.get();
  if (!data_obj || !shared_obj || !svc_obj ||
      !is_exact(data_obj, g_cfg.class_startData) || !is_exact(shared_obj, g_cfg.class_shared) ||
      !is_exact(svc_obj, g_cfg.class_consumption)) {
    out->status = PGR_RA_FAULT; out->reason = PGR_RA_REASON_NATIVE_ERROR; return PGR_RA_FAULT;
  }
  if (!preflight(deadline, generation, &status, &reason)) { out->status = status; out->reason = reason; return status; }
  reinterpret_cast<FieldSet>(g_cfg.field_set_value_object)(data_obj, g_cfg.f_dataPublishedRoute, shared_obj);
  if (!preflight(deadline, generation, &status, &reason)) { out->status = status; out->reason = reason; return status; }
  data_obj = data.get();
  if (!data_obj) { out->status = PGR_RA_FAULT; out->reason = PGR_RA_REASON_NATIVE_ERROR; return out->status; }
  uint8_t rev_byte = reversed ? 1 : 0;
  reinterpret_cast<FieldSet>(g_cfg.field_set_value)(data_obj, g_cfg.f_dataReversed, &rev_byte);

  // Pre-invoke eligible-fresh re-check; a revoke that raced construction aborts WITHOUT invoking.
  if (!preflight(deadline, generation, &status, &reason)) { out->status = status; out->reason = reason; return status; }
  svc_obj = svc.get();
  data_obj = data.get();
  if (!svc_obj || !data_obj) { out->status = PGR_RA_FAULT; out->reason = PGR_RA_REASON_NATIVE_ERROR; return PGR_RA_FAULT; }
  void *params[1] = {data_obj};  // StartRoute argument[0] = the data reference directly
  void *exc = nullptr;
  reinterpret_cast<Invoke>(g_cfg.runtime_invoke)(g_cfg.m_start, svc_obj, params, &exc);
  if (exc) { out->status = PGR_RA_FAULT; out->reason = PGR_RA_REASON_NATIVE_ERROR; return PGR_RA_FAULT; }
  out->status = PGR_RA_SUCCESS;  // submitted (never retried, even if revoked during the call)
  out->reason = PGR_RA_REASON_NONE;
  return PGR_RA_SUCCESS;
}

int do_complete(const Root &svc, const PgrRoutesActionsRequest *r, uint64_t deadline, uint64_t generation,
                PgrRoutesActionsOutput *out) {
  CurrentState cs;
  int status = 0, reason = 0;
  int rc = read_current_state(svc, deadline, generation, &cs, &status, &reason);
  if (rc <= 0) { out->status = status; out->reason = reason; return status; }
  if (!cs.present) { out->status = PGR_RA_REFUSE; out->reason = PGR_RA_REASON_NO_CURRENT; return PGR_RA_REFUSE; }

  if (r->id_length < 1 || r->id_length > PGR_RA_ID_MAX ||
      cs.id_len != r->id_length ||
      std::memcmp(cs.id, r->id, static_cast<size_t>(r->id_length) * sizeof(uint16_t)) != 0) {
    out->status = PGR_RA_REFUSE; out->reason = PGR_RA_REASON_ID_MISMATCH; return PGR_RA_REFUSE;
  }
  if (cs.is_completing)   { out->status = PGR_RA_REFUSE; out->reason = PGR_RA_REASON_COMPLETING; return PGR_RA_REFUSE; }
  if (cs.acquired_reward) { out->status = PGR_RA_REFUSE; out->reason = PGR_RA_REASON_REWARDED; return PGR_RA_REFUSE; }
  if (cs.paused || !cs.started || !cs.completed_walk || !cs.in_range_end) {
    out->status = PGR_RA_REFUSE; out->reason = PGR_RA_REASON_NOT_ELIGIBLE; return PGR_RA_REFUSE;
  }

  // Confirmed completion (bizv), the game's normal reward path; no reward invented. One invoke only.
  if (!preflight(deadline, generation, &status, &reason)) { out->status = status; out->reason = reason; return status; }
  void *self = svc.get();
  if (!self) { out->status = PGR_RA_FAULT; out->reason = PGR_RA_REASON_NATIVE_ERROR; return PGR_RA_FAULT; }
  void *exc = nullptr;
  // The real success continuation can synchronously inspect CURRENT. Permit exactly
  // one read-only leaf during this invoke, preserving callback-time route identity.
  ConfirmationContext context{&svc, deadline, generation, false};
  ConfirmationContext *previous = g_confirmation;
  g_confirmation = &context;
  reinterpret_cast<Invoke>(g_cfg.runtime_invoke)(g_cfg.m_confirmComplete, self, nullptr, &exc);
  g_confirmation = previous;
  if (exc) { out->status = PGR_RA_FAULT; out->reason = PGR_RA_REASON_NATIVE_ERROR; return PGR_RA_FAULT; }
  out->status = PGR_RA_SUCCESS;
  out->reason = PGR_RA_REASON_NONE;
  return PGR_RA_SUCCESS;
}

int do_pause(const Root &svc, uint64_t deadline, uint64_t generation, PgrRoutesActionsOutput *out) {
  int status = 0, reason = 0;
  int b;
  b = invoke_getter_bool(g_cfg.m_isConsuming, svc, deadline, generation, true, &status, &reason);
  if (b < 0) { out->status = status; out->reason = reason; return status; }
  if (b == 0) { out->status = PGR_RA_REFUSE; out->reason = PGR_RA_REASON_NO_CURRENT; return PGR_RA_REFUSE; }
  b = invoke_getter_bool(g_cfg.m_isPaused, svc, deadline, generation, true, &status, &reason);
  if (b < 0) { out->status = status; out->reason = reason; return status; }
  if (b == 1) { out->status = PGR_RA_REFUSE; out->reason = PGR_RA_REASON_ALREADY_PAUSED; return PGR_RA_REFUSE; }

  if (!preflight(deadline, generation, &status, &reason)) { out->status = status; out->reason = reason; return status; }
  void *self = svc.get();
  if (!self) { out->status = PGR_RA_FAULT; out->reason = PGR_RA_REASON_NATIVE_ERROR; return PGR_RA_FAULT; }
  uint8_t one = 1;
  void *params[1] = {&one};  // PauseRoute(bool) argument[0] = address of the value-type bool
  void *exc = nullptr;
  void *promise = reinterpret_cast<Invoke>(g_cfg.runtime_invoke)(g_cfg.m_pause, self, params, &exc);
  if (exc) { out->status = PGR_RA_FAULT; out->reason = PGR_RA_REASON_NATIVE_ERROR; return PGR_RA_FAULT; }
  if (promise) { Root tmp; tmp.set(promise); }  // root/free the returned IPromise we do not observe
  out->status = PGR_RA_SUCCESS;
  out->reason = PGR_RA_REASON_NONE;
  return PGR_RA_SUCCESS;
}

// --- owner latching ----------------------------------------------------------------
// Supplied tid must be nonzero AND equal the current OS tid BEFORE the owner is latched; a later
// foreign tid refuses. Mirrors the sibling owner latch.
bool owner_ready(uint64_t tid) {
  if (tid == 0 || current_tid() != tid) return false;
  uint64_t owner = g_owner_tid.load(std::memory_order_acquire);
  if (!(owner == 0 || owner == tid)) return false;
  uint64_t expected = 0;
  g_owner_tid.compare_exchange_strong(expected, tid, std::memory_order_acq_rel);
  return g_owner_tid.load(std::memory_order_acquire) == tid;
}

void init_output(PgrRoutesActionsOutput *out) {
  std::memset(out, 0, sizeof(*out));
  out->version = PGR_RA_OUTPUT_VERSION;
  out->size = sizeof(PgrRoutesActionsOutput);
}

bool is_safe_seq(uint64_t v) { return static_cast<double>(v) <= kMaxSafeInteger; }

// --- config validation / install ---------------------------------------------------
bool validate_config(const PgrRoutesActionsConfig *c, int *rc) {
  if (c == nullptr) { *rc = PGR_RA_ERR_NULL_ARG; return false; }
  if (c->version != PGR_RA_CONFIG_VERSION) { *rc = PGR_RA_ERR_BAD_VERSION; return false; }
  if (c->size != sizeof(PgrRoutesActionsConfig)) { *rc = PGR_RA_ERR_BAD_SIZE; return false; }
  void *const *ptrs = reinterpret_cast<void *const *>(
      reinterpret_cast<const unsigned char *>(c) + offsetof(PgrRoutesActionsConfig, runtime_invoke));
  for (int i = 0; i < PGR_RA_PTR_COUNT; ++i)
    if (ptrs[i] == nullptr) { *rc = PGR_RA_ERR_MISSING_PTR; return false; }
  *rc = 0;  // success; api_install ignores *rc when this returns true
  return true;
}

int api_install(const PgrRoutesActionsConfig *config) {
  int rc = 0;
  if (!validate_config(config, &rc)) return rc;
  uint64_t probe = 0;
  if (!read_boottime_ms(&probe)) return PGR_RA_ERR_CLOCK;

  std::lock_guard<std::mutex> install_lock(g_install_mutex);
  if (g_installed.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    return std::memcmp(&g_cfg, config, sizeof(*config)) == 0 ? 0 : PGR_RA_ERR_RECONFIG;
  }
  {
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    g_cfg = *config;
    g_busy = false;
    g_seq_highwater = 0;
  }
  g_installed.store(true, std::memory_order_release);
  return 0;
}

uint64_t api_generation() { return g_gen.load(std::memory_order_acquire); }

uint64_t api_revoke() { return g_gen.fetch_add(1, std::memory_order_acq_rel) + 1; }

int api_execute(const PgrRoutesActionsRequest *req, PgrRoutesActionsOutput *out) {
  if (!out) return PGR_RA_ERR_NULL_ARG;
  init_output(out);
  auto reject = [&](int status, int reason) { out->status=status; out->reason=reason; return status; };
  if (!g_installed.load(std::memory_order_acquire)) return reject(PGR_RA_ERR_NOT_INSTALLED, PGR_RA_REASON_NATIVE_ERROR);
  if (!req) return reject(PGR_RA_ERR_NULL_ARG, PGR_RA_REASON_NATIVE_ERROR);
  if (req->version != 1) return reject(PGR_RA_ERR_BAD_VERSION, PGR_RA_REASON_NATIVE_ERROR);
  if (req->size != sizeof(*req)) return reject(PGR_RA_ERR_BAD_SIZE, PGR_RA_REASON_NATIVE_ERROR);
  const PgrRoutesActionsRequest r=*req;
  if (r.action>PGR_RA_ACT_PAUSE || r.reserved || r.reversed>1 || r.id_length>PGR_RA_ID_MAX ||
      (r.action==PGR_RA_ACT_CURRENT && r.seq!=0)) return reject(PGR_RA_FAULT,PGR_RA_REASON_NATIVE_ERROR);
  const bool mutating=r.action!=PGR_RA_ACT_CURRENT;
  if (!r.tid || r.tid!=current_tid() || !reinterpret_cast<ThreadCur>(g_cfg.thread_current)() || !owner_ready(r.tid))
    return reject(PGR_RA_REFUSE,PGR_RA_REASON_LEASE_EXPIRED);
  const uint64_t residual=r.residual_ms>kMaxLeaseMs?kMaxLeaseMs:r.residual_ms;
  if (!residual || r.generation!=g_gen.load(std::memory_order_acquire)) return reject(PGR_RA_REFUSE,PGR_RA_REASON_LEASE_EXPIRED);
  uint64_t now=0;
  if (!read_boottime_ms(&now)) return reject(PGR_RA_ERR_CLOCK,PGR_RA_REASON_NATIVE_ERROR);
  uint64_t deadline=now+residual;
  const uint64_t generation=r.generation;
  ConfirmationContext *parent=nullptr;
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_busy) {
      if (mutating || !g_confirmation || g_confirmation->leaf || g_confirmation->generation!=generation)
        return reject(PGR_RA_FAULT,PGR_RA_REASON_NATIVE_ERROR);
      parent=g_confirmation;parent->leaf=true;
    } else g_busy=true;
  }
  BusyExit busy_exit(parent);
  if (parent) {
    if (parent->deadline<deadline) deadline=parent->deadline;
    void *requested=reinterpret_cast<GcGet>(g_cfg.gchandle_get_target)(static_cast<uintptr_t>(r.service_handle));
    if (!requested || requested!=parent->service->get()) return reject(PGR_RA_REFUSE,PGR_RA_REASON_AWAITING_SVC);
  }
  if (mutating) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (!r.seq || !is_safe_seq(r.seq) || r.seq<=g_seq_highwater) return reject(PGR_RA_REFUSE,PGR_RA_REASON_STALE_SEQ);
    g_seq_highwater=r.seq;
  }
  PgrRoutesActionsOutput staged{};init_output(&staged);
  // The lambda owns every managed root. Its destruction completes before publication,
  // while the outer busy/leaf guard remains held through final validation.
  const int result=[&]() {
    int status=0,reason=0;
    auto fail=[&](int st,int why) { staged.status=st;staged.reason=why;return st; };
    if (!preflight(deadline,generation,&status,&reason)) return fail(status,reason);
    void *raw=reinterpret_cast<GcGet>(g_cfg.gchandle_get_target)(static_cast<uintptr_t>(r.service_handle));
    if (!raw) return fail(PGR_RA_REFUSE,PGR_RA_REASON_AWAITING_SVC);
    Root service;
    if (!service.set(raw)) return fail(PGR_RA_FAULT,PGR_RA_REASON_NATIVE_ERROR);
    void *klass=r.action==PGR_RA_ACT_DISCOVER?g_cfg.class_routes:g_cfg.class_consumption;
    if (!is_exact(service.get(),klass)) return fail(PGR_RA_REFUSE,PGR_RA_REASON_AWAITING_SVC);
    switch(r.action) {
      case PGR_RA_ACT_CURRENT:return do_current(service,deadline,generation,&staged);
      case PGR_RA_ACT_DISCOVER:return do_discover(service,deadline,generation,&staged);
      case PGR_RA_ACT_START:return do_start(service,&r,deadline,generation,&staged);
      case PGR_RA_ACT_COMPLETE:return do_complete(service,&r,deadline,generation,&staged);
      case PGR_RA_ACT_PAUSE:return do_pause(service,deadline,generation,&staged);
    }
    return fail(PGR_RA_FAULT,PGR_RA_REASON_NATIVE_ERROR);
  }();
  int status=0,reason=0;
  if (!mutating && result==PGR_RA_SUCCESS && !preflight(deadline,generation,&status,&reason)) {
    init_output(&staged);staged.status=status;staged.reason=reason;
  }
  *out=staged;
  return staged.status;
}

const PgrRoutesActionsApi kApiTable = {
    PGR_RA_API_VERSION, static_cast<uint32_t>(sizeof(PgrRoutesActionsApi)),
    &api_install, &api_generation, &api_revoke, &api_execute,
};

}  // namespace

extern "C" const void *pgrazor_routes_actions_api(uint32_t version) {
  if (version != PGR_RA_API_VERSION) return nullptr;
  return &kApiTable;
}
