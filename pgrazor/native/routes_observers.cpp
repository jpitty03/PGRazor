#include "routes_observers.h"
#include "hook_engine.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <mutex>
#include <time.h>
#include <unistd.h>

// Feature 46 native Routes result-presentation observers. See routes_observers.h and
// .artifacts/native-migration/routes48-contract.txt for the ABI and the division of labour with
// routes.js and features 43/44/45. Built -fno-exceptions / -fno-rtti like every sibling: no C++ throw
// ever escapes. Three il2cpp hooks (reward Initialize, rating Initialize, bjan completion-response) and
// the result/tracking state live here; the host drains events + completion through poll().
//
// Thread model: the three hooks, poll, set_completing, clear_completing and drain run on the Unity owner
// thread, so completing/completion/rewardAt/pendingUi are single-threaded and need no mutex. The
// route-tracking depth table, lastTrackingError, lastWaypointAt and the event ring are touched by the
// lite forward entries (tracking_enter/leave, location_gate, waypoint_observed) which may run on a
// non-owner Unity thread; those are guarded by g_obs_mutex, held only for bookkeeping/push -- never
// across a managed call or a chained hook original. revoke() touches only atomics (any thread).

static_assert(sizeof(PgrRoutesObserversConfig) == 184, "config 184");
static_assert(sizeof(PgrRoutesObserversRequest) == 32, "request 32");
static_assert(sizeof(PgrRoutesObserversCompleting) == 312, "completing 312");
static_assert(sizeof(PgrRoutesObserversOutput) == 2672, "output 2672");
static_assert(sizeof(PgrRoutesObserversApi) == 96, "table 96");

namespace {

constexpr uint64_t kMaxLeaseMs = PGR_RO_MAX_LEASE_MS;
constexpr int kDepthSlots = 16;

// --- il2cpp runtime ABI typedefs ---------------------------------------------------
using RuntimeInvoke = void *(*)(const void *method, void *obj, void **params, void **exc);
using ObjClass      = void *(*)(void *obj);
using FieldGet      = void (*)(void *obj, void *field, void *out);
using GcNew         = uintptr_t (*)(void *obj, int pinned);
using GcGet         = void *(*)(uintptr_t handle);
using GcFree        = void (*)(uintptr_t handle);
using ThreadCur     = void *(*)();
using StrLen        = int32_t (*)(void *str);
using StrChars      = uint16_t *(*)(void *str);

// Chained originals: all three hooked methods are void instance methods with one managed argument.
using InitFn = void (*)(void *self, void *arg, const void *method);
using BjanFn = void (*)(void *self, void *progress, const void *method);

// --- lifetime / publication --------------------------------------------------------
std::mutex g_install_mutex;
PgrRoutesObserversConfig g_cfg{};
std::atomic<bool> g_installed{false};
std::atomic<bool> g_hook_failed{false};

std::atomic<InitFn> g_orig_reward{nullptr};
std::atomic<InitFn> g_orig_rating{nullptr};
std::atomic<BjanFn> g_orig_bjan{nullptr};

// Permanent Unity owner, latched once from a genuine on-thread poll/set_completing/clear_completing/drain.
std::atomic<uint64_t> g_owner_tid{0};

// Revocation generation (initially 1); bumped by revoke(). Owner requests carry a stamp compared here.
std::atomic<uint64_t> g_gen{1};

// Lease deadline (CLOCK_BOOTTIME ms). poll/set_completing refresh it; revoke clears it. The hooks and the
// lite entries act only while now < deadline, so a revoke or a stopped tick disarms everything.
std::atomic<uint64_t> g_lease_deadline{0};
std::atomic<uint64_t> g_lease_gen{0};
uint64_t g_state_gen=0;
thread_local bool g_in_bjan_original=false;
thread_local bool g_in_reward_original=false;

// Hook/poll reentry guard (owner thread). A reentrant managed path captures/correlates nothing.
std::atomic<bool> g_hook_busy{false};

// --- owner-thread-only result state (no mutex) -------------------------------------
struct Completing {
  bool active = false;
  uint64_t handle = 0;  // borrowed feature-45 consumption GC handle (never freed here)
  uint64_t seq = 0;
  uint64_t at = 0;      // boottime ms
  uint32_t id_length = 0;
  uint16_t id[PGR_RO_ID_MAX] = {0};
};
Completing g_completing;

struct Completion {
  bool active = false;
  uint64_t seq = 0;
  uint32_t id_length = 0;
  uint16_t id[PGR_RO_ID_MAX] = {0};
};
Completion g_completion;
uint64_t g_reward_at = 0;  // boottime ms the completion was confirmed

struct PendingUi {
  uintptr_t handle = 0;  // owned GC root on the GUI receiver
  uint64_t at = 0;       // boottime ms captured
  uint32_t screen = 0;   // PgrRoutesObserversScreen
};
PendingUi g_pending[PGR_RO_PENDING_UI_MAX];

// --- cross-thread observer state (g_obs_mutex) -------------------------------------
std::mutex g_obs_mutex;
struct DepthSlot { uint64_t tid = 0; uint32_t depth = 0; uint64_t generation=0; };
DepthSlot g_depth[kDepthSlots];
bool g_has_last_tracking = false;
int32_t g_last_tracking = 0;
uint64_t g_last_waypoint_at = 0;
PgrRoutesObserversEvent g_ev[PGR_RO_MAX_EVENTS];
uint32_t g_ev_count = 0;
uint32_t g_ev_dropped = 0;

// --- clock / tid -------------------------------------------------------------------
uint64_t current_tid() { return static_cast<uint64_t>(static_cast<pid_t>(gettid())); }

bool read_boottime_ms(uint64_t *out_ms) {
  struct timespec ts {};
  if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0 || ts.tv_sec < 0 || ts.tv_nsec < 0) return false;
  *out_ms = static_cast<uint64_t>(ts.tv_sec) * 1000ULL + static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
  return true;
}

// Typed accessors onto the immutable published config.
RuntimeInvoke rt_invoke() { return reinterpret_cast<RuntimeInvoke>(g_cfg.runtime_invoke); }
ObjClass  obj_class()  { return reinterpret_cast<ObjClass>(g_cfg.object_get_class); }
FieldGet  field_get()  { return reinterpret_cast<FieldGet>(g_cfg.field_get_value); }
GcNew     gc_new()     { return reinterpret_cast<GcNew>(g_cfg.gchandle_new); }
GcGet     gc_get()     { return reinterpret_cast<GcGet>(g_cfg.gchandle_get_target); }
GcFree    gc_free()    { return reinterpret_cast<GcFree>(g_cfg.gchandle_free); }
ThreadCur thread_cur() { return reinterpret_cast<ThreadCur>(g_cfg.thread_current); }
StrLen    str_len()    { return reinterpret_cast<StrLen>(g_cfg.string_length); }
StrChars  str_chars()  { return reinterpret_cast<StrChars>(g_cfg.string_chars); }

bool is_exact(void *obj, void *klass) { return obj != nullptr && obj_class()(obj) == klass; }
void *handle_target(uintptr_t handle) { return handle ? gc_get()(handle) : nullptr; }
void free_handle(uintptr_t *slot) { if (slot && *slot) { uintptr_t h = *slot; *slot = 0; gc_free()(h); } }

bool lease_live(uint64_t *now_out = nullptr) {
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) return false;
  if (now_out) *now_out = now;
  return g_lease_gen.load(std::memory_order_acquire)==g_gen.load(std::memory_order_acquire) &&
         now < g_lease_deadline.load(std::memory_order_acquire);
}
bool fresh(uint64_t generation){return generation==g_gen.load(std::memory_order_acquire)&&lease_live();}

// --- event ring (g_obs_mutex held by caller via push_event) -------------------------
void push_event_locked(uint32_t type, uint32_t aux, int64_t i0, double f0, double f1,
                       const uint16_t *id, uint32_t id_length) {
  if (g_ev_count >= PGR_RO_MAX_EVENTS) { ++g_ev_dropped; return; }
  PgrRoutesObserversEvent &e = g_ev[g_ev_count++];
  std::memset(&e, 0, sizeof(e));
  e.type = type;
  e.aux = aux;
  e.i0 = i0;
  e.f0 = f0;
  e.f1 = f1;
  if (id && id_length) {
    uint32_t n = id_length > PGR_RO_ID_MAX ? PGR_RO_ID_MAX : id_length;
    std::memcpy(e.id, id, n * sizeof(uint16_t));
    e.id_length = n;
  }
}
void push_event(uint32_t type, uint32_t aux, int64_t i0, double f0, double f1,
                const uint16_t *id, uint32_t id_length) {
  std::lock_guard<std::mutex> lock(g_obs_mutex);
  push_event_locked(type, aux, i0, f0, f1, id, id_length);
}

// --- depth table (g_obs_mutex held by caller) --------------------------------------
DepthSlot *find_depth_locked(uint64_t tid, bool create) {
  DepthSlot *freeslot = nullptr;
  for (int i = 0; i < kDepthSlots; ++i) {
    if (g_depth[i].tid == tid && tid != 0) return &g_depth[i];
    if (!freeslot && g_depth[i].tid == 0 && g_depth[i].depth == 0) freeslot = &g_depth[i];
  }
  if (create && freeslot) { freeslot->tid = tid; freeslot->depth = 0; return freeslot; }
  return nullptr;
}

// --- owner latching ----------------------------------------------------------------
bool owner_ready(uint64_t tid) {
  if (tid == 0 || current_tid() != tid) return false;
  uint64_t owner = g_owner_tid.load(std::memory_order_acquire);
  if (!(owner == 0 || owner == tid)) return false;
  uint64_t expected = 0;
  g_owner_tid.compare_exchange_strong(expected, tid, std::memory_order_acq_rel);
  return g_owner_tid.load(std::memory_order_acquire) == tid;
}

// Hook gate: capture/correlate only when installed, on the latched owner thread, attached, lease live.
bool hook_gate_owner() {
  if (!g_installed.load(std::memory_order_acquire)) return false;
  const uint64_t owner = g_owner_tid.load(std::memory_order_acquire);
  if (!owner || current_tid() != owner || !thread_cur()()) return false;
  return lease_live();
}

// --- bjan correlation: read the current route id through our OWN config -------------
// Returns true and fills out_id/out_len when a current route id was read; the caller compares it with
// completing.id. receiver is the rooted (feature-45) consumption service. A leaf managed frame -- it
// does NOT call feature 43. No g_obs_mutex is held across the invoke.
bool read_current_id(void *receiver, uint16_t *out_id, uint32_t *out_len) {
  *out_len = 0;
  if (!is_exact(receiver, g_cfg.class_consumption)) return false;
  void *exc = nullptr;
  void *play = rt_invoke()(g_cfg.m_current_route, receiver, nullptr, &exc);
  if (exc || !is_exact(play, g_cfg.class_play)) return false;
  const uintptr_t ph = gc_new()(play, 0);
  if (!ph) return false;
  bool ok = false;
  void *p = handle_target(ph);
  if (is_exact(p, g_cfg.class_play)) {
    void *shared = nullptr;
    field_get()(p, g_cfg.f_play_route, &shared);  // reference read, no invoke before the id read
    if (is_exact(shared, g_cfg.class_shared)) {
      void *str = nullptr;
      field_get()(shared, g_cfg.f_shared_id, &str);
      if (str) {
        int32_t len = str_len()(str);
        if (len > 0 && len <= PGR_RO_ID_MAX) {
          uint16_t *chars = str_chars()(str);
          if (chars) {
            uint32_t n = static_cast<uint32_t>(len);
            std::memcpy(out_id, chars, n * sizeof(uint16_t));
            *out_len = n;
            ok = true;
          }
        }
      }
    }
  }
  uintptr_t to_free = ph;
  free_handle(&to_free);
  return ok;
}

bool same_id(const uint16_t *a, uint32_t alen, const uint16_t *b, uint32_t blen) {
  return alen == blen && (alen == 0 || std::memcmp(a, b, alen * sizeof(uint16_t)) == 0);
}

// --- hooks (owner thread) ----------------------------------------------------------
void hook_reward_or_rating(void *self, void *arg, const void *method, void *klass, uint32_t screen,
                           std::atomic<InitFn> &orig_slot) {
  InitFn orig=orig_slot.load(std::memory_order_acquire);
  if(!orig){std::lock_guard<std::mutex> lock(g_install_mutex);orig=orig_slot.load(std::memory_order_acquire);}
  if(!orig)std::abort();
  const bool nested=g_in_bjan_original&&!g_in_reward_original;
  if(!hook_gate_owner()||g_in_reward_original){orig(self,arg,method);return;}
  if(!nested&&g_hook_busy.exchange(true,std::memory_order_acq_rel)){orig(self,arg,method);return;}
  const uint64_t generation=g_gen.load(std::memory_order_acquire);
  uint64_t now=0;int slot=-1;
  if(g_state_gen==generation&&g_completion.active&&read_boottime_ms(&now)&&
     now>=g_reward_at&&now-g_reward_at<=PGR_RO_REWARD_CAPTURE_MS&&is_exact(self,klass))
    for(int i=0;i<PGR_RO_PENDING_UI_MAX;++i)if(!g_pending[i].handle){slot=i;break;}
  uintptr_t ah=0,sh=0;
  if(slot>=0){
    // Pin incoming managed arguments before any invoke/cleanup. Pinned argument
    // roots stay stable while the receiver root is established.
    if(arg)ah=gc_new()(arg,1);
    if(!arg||ah)sh=gc_new()(self,1);
    if(sh&&fresh(generation)){
      g_pending[slot]={sh,now,screen};
    }
  }
  g_in_reward_original=true;
  orig(sh?handle_target(sh):self,ah?handle_target(ah):arg,method);
  g_in_reward_original=false;
  // Initialize is void; cleanup occurs only after original with busy still held.
  if(sh&&(slot<0||g_pending[slot].handle!=sh))free_handle(&sh);
  free_handle(&ah);
  if(!nested)g_hook_busy.store(false,std::memory_order_release);
}

void hook_reward_init(void *self, void *arg, const void *method) {
  hook_reward_or_rating(self, arg, method, g_cfg.class_reward_gui, PGR_RO_SCREEN_REWARD, g_orig_reward);
}
void hook_rating_init(void *self, void *arg, const void *method) {
  hook_reward_or_rating(self, arg, method, g_cfg.class_rating_gui, PGR_RO_SCREEN_RATING, g_orig_rating);
}

void hook_bjan(void *self, void *progress, const void *method) {
  BjanFn orig = g_orig_bjan.load(std::memory_order_acquire);
  if(!orig){std::lock_guard<std::mutex> lock(g_install_mutex);orig=g_orig_bjan.load(std::memory_order_acquire);}
  if(!orig)std::abort();
  if (!hook_gate_owner() || g_hook_busy.exchange(true, std::memory_order_acq_rel)) {
    orig(self, progress, method);
    return;
  }
  uint64_t now = 0;
  const uint64_t generation=g_gen.load(std::memory_order_acquire);
  uintptr_t self_root=0,progress_root=0;
  if (g_state_gen==generation && g_completing.active && read_boottime_ms(&now) && now - g_completing.at <= PGR_RO_COMPLETING_MS &&
      is_exact(self, g_cfg.class_consumption) && is_exact(progress, g_cfg.class_progress) &&
      handle_target(g_completing.handle) == self) {
    const int32_t status =
        *reinterpret_cast<const int32_t *>(reinterpret_cast<const char *>(progress) + PGR_RO_PROGRESS_STATUS_OFFSET);
    progress_root=gc_new()(progress,1);
    if(progress_root)self_root=gc_new()(handle_target(g_completing.handle),1);
    uint16_t cur[PGR_RO_ID_MAX];
    uint32_t cur_len = 0;
    const bool read = self_root&&progress_root&&fresh(generation)&&read_current_id(handle_target(self_root), cur, &cur_len);
    if (read && fresh(generation) && same_id(cur, cur_len, g_completing.id, g_completing.id_length)) {
      if (status == PGR_RO_COMPLETION_SUCCESS) {
        g_completion.active = true;
        g_completion.seq = g_completing.seq;
        g_completion.id_length = g_completing.id_length;
        std::memcpy(g_completion.id, g_completing.id, g_completing.id_length * sizeof(uint16_t));
        g_reward_at = now;
        push_event(PGR_RO_EV_REWARD_CONFIRMED, 0, static_cast<int64_t>(g_completing.seq), 0, 0,
                   g_completing.id, g_completing.id_length);
      } else {
        push_event(PGR_RO_EV_REWARD_REJECTED, 0, status, 0, 0,
                   g_completing.id, g_completing.id_length);
      }
      g_completing.active = false;
    }
  }
  g_in_bjan_original=true;
  orig(self_root?handle_target(self_root):self,progress_root?handle_target(progress_root):progress,method);
  g_in_bjan_original=false;
  free_handle(&self_root);free_handle(&progress_root);
  g_hook_busy.store(false, std::memory_order_release);
}

// --- install -----------------------------------------------------------------------
void init_output(PgrRoutesObserversOutput *out) {
  std::memset(out, 0, sizeof(*out));
  out->version = PGR_RO_OUTPUT_VERSION;
  out->size = sizeof(PgrRoutesObserversOutput);
}

bool validate_config(const PgrRoutesObserversConfig *c, int *rc) {
  if (c == nullptr) { *rc = PGR_RO_ERR_NULL_ARG; return false; }
  if (c->version != PGR_RO_CONFIG_VERSION) { *rc = PGR_RO_ERR_BAD_VERSION; return false; }
  if (c->size != sizeof(PgrRoutesObserversConfig)) { *rc = PGR_RO_ERR_BAD_SIZE; return false; }
  void *const *ptrs = reinterpret_cast<void *const *>(
      reinterpret_cast<const unsigned char *>(c) + offsetof(PgrRoutesObserversConfig, runtime_invoke));
  for (int i = 0; i < PGR_RO_PTR_COUNT; ++i)
    if (ptrs[i] == nullptr) { *rc = PGR_RO_ERR_MISSING_PTR; return false; }
  *rc = 0;
  return true;
}

// Install the three hooks. On a partial failure, roll back whatever was patched and latch the failure.
int install_hooks() {
  void *o0 = nullptr;
  int rc=pgrazor_hook(g_cfg.addr_reward_init, reinterpret_cast<void *>(&hook_reward_init), &o0);
  g_orig_reward.store(reinterpret_cast<InitFn>(o0), std::memory_order_release);
  if(rc!=0||!o0)return PGR_RO_ERR_HOOK;
  void *o1 = nullptr;
  rc=pgrazor_hook(g_cfg.addr_rating_init, reinterpret_cast<void *>(&hook_rating_init), &o1);
  g_orig_rating.store(reinterpret_cast<InitFn>(o1), std::memory_order_release);
  if(rc!=0||!o1)return PGR_RO_ERR_HOOK;
  void *o2 = nullptr;
  rc=pgrazor_hook(g_cfg.addr_completion_resp, reinterpret_cast<void *>(&hook_bjan), &o2);
  g_orig_bjan.store(reinterpret_cast<BjanFn>(o2), std::memory_order_release);
  if(rc!=0||!o2)return PGR_RO_ERR_HOOK;
  return 0;
}

int api_install(const PgrRoutesObserversConfig *config) {
  int rc = 0;
  if (!validate_config(config, &rc)) return rc;
  uint64_t probe = 0;
  if (!read_boottime_ms(&probe)) return PGR_RO_ERR_CLOCK;

  std::lock_guard<std::mutex> install_lock(g_install_mutex);
  if (g_hook_failed.load(std::memory_order_acquire)) return PGR_RO_ERR_HOOK;
  if (g_installed.load(std::memory_order_acquire))
    return std::memcmp(&g_cfg, config, sizeof(*config)) == 0 ? 0 : PGR_RO_ERR_RECONFIG;
  g_cfg = *config;
  const int hrc = install_hooks();
  if (hrc != 0) { g_hook_failed.store(true, std::memory_order_release); return hrc; }
  g_installed.store(true, std::memory_order_release);
  return 0;
}

uint64_t api_generation() { return g_gen.load(std::memory_order_acquire); }

uint64_t api_revoke() {
  g_lease_deadline.store(0, std::memory_order_release);  // disarm hooks + lite entries
  g_lease_gen.store(0,std::memory_order_release);
  return g_gen.fetch_add(1, std::memory_order_acq_rel) + 1;
}

// Validate an owner request header + owner/thread/generation gate. Returns 0 on pass or a hard fault;
// *skip is true for a benign gate refusal. deadline_out is set from a positive residual when require_lease.
int gate_owner(const PgrRoutesObserversRequest *req, bool require_lease, uint64_t *deadline_out, bool *skip) {
  *skip = false;
  if (!g_installed.load(std::memory_order_acquire)) return PGR_RO_ERR_NOT_INSTALLED;
  if (!req) return PGR_RO_ERR_NULL_ARG;
  if (req->version != PGR_RO_REQUEST_VERSION) return PGR_RO_ERR_BAD_VERSION;
  if (req->size != sizeof(*req)) return PGR_RO_ERR_BAD_SIZE;
  if (!req->tid || req->tid != current_tid() || !thread_cur()() || !owner_ready(req->tid)) { *skip = true; return 0; }
  if (req->generation != g_gen.load(std::memory_order_acquire)) { *skip = true; return 0; }
  if (require_lease) {
    uint64_t residual = req->residual_ms > kMaxLeaseMs ? kMaxLeaseMs : req->residual_ms;
    if (!residual) { *skip = true; return 0; }
    uint64_t now = 0;
    if (!read_boottime_ms(&now)) return PGR_RO_ERR_CLOCK;
    *deadline_out = now + residual;
  }
  return 0;
}

void clear_pending() { for (int i = 0; i < PGR_RO_PENDING_UI_MAX; ++i) free_handle(&g_pending[i].handle); }

int api_poll(const PgrRoutesObserversRequest *req, PgrRoutesObserversOutput *out) {
  if (!out) return PGR_RO_ERR_NULL_ARG;
  init_output(out);
  // poll runs every tick even with the lease off (to drop stale pendingUi), so it does NOT require a lease.
  uint64_t unused = 0;
  bool skip = false;
  const int grc = gate_owner(req, false, &unused, &skip);
  if (grc != 0) return out->status = grc;
  if (skip) return out->status = PGR_RO_SKIP;
  if (g_hook_busy.exchange(true, std::memory_order_acq_rel)) return out->status = PGR_RO_ERR_BUSY;

  uint64_t now = 0;
  if (!read_boottime_ms(&now)) { g_hook_busy.store(false, std::memory_order_release); return out->status = PGR_RO_ERR_CLOCK; }

  // Refresh the lease from this tick's residual (0 => lease off, deadline cleared).
  const uint64_t residual = req->residual_ms > kMaxLeaseMs ? kMaxLeaseMs : req->residual_ms;
  g_lease_deadline.store(residual ? now + residual : 0, std::memory_order_release);
  g_lease_gen.store(req->generation,std::memory_order_release);
  if(g_state_gen!=req->generation){
    clear_pending();g_completing.active=false;g_completion.active=false;g_reward_at=0;
    g_state_gen=req->generation;
    std::lock_guard<std::mutex> lock(g_obs_mutex);g_ev_count=0;g_ev_dropped=0;
  }

  // Dismiss settled pendingUi. Drop on no-lease / age / wrong class; otherwise Dismiss once state==2.
  for (int i = 0; i < PGR_RO_PENDING_UI_MAX; ++i) {
    if (!g_pending[i].handle) continue;
    if(!read_boottime_ms(&now))break;
    const uint64_t age = now - g_pending[i].at;
    if (age < PGR_RO_UI_SETTLE_MS) continue;
    void *gui = handle_target(g_pending[i].handle);
    void *klass = g_pending[i].screen == PGR_RO_SCREEN_RATING ? g_cfg.class_rating_gui : g_cfg.class_reward_gui;
    if (!fresh(req->generation) || age >= PGR_RO_UI_MAX_MS || !is_exact(gui, klass)) { free_handle(&g_pending[i].handle); continue; }
    const int32_t state = *reinterpret_cast<const int32_t *>(reinterpret_cast<const char *>(gui) + PGR_RO_GUI_STATE_OFFSET);
    if (state == PGR_RO_GUI_STATE_DISMISSED) {
      if(!fresh(req->generation))break;
      void *exc = nullptr;
      rt_invoke()(g_cfg.m_dismiss_gui, gui, nullptr, &exc);
      if(fresh(req->generation))push_event(exc ? PGR_RO_EV_RESULT_DISMISS_ERR : PGR_RO_EV_RESULT_DISMISSED, g_pending[i].screen, 0, 0, 0, nullptr, 0);
      free_handle(&g_pending[i].handle);
    }
  }

  // Completion snapshot flag.
  bool pending_empty = true;
  for (int i = 0; i < PGR_RO_PENDING_UI_MAX; ++i) if (g_pending[i].handle) { pending_empty = false; break; }
  if (fresh(req->generation) && read_boottime_ms(&now) && g_completion.active && now - g_reward_at >= PGR_RO_COMPLETION_MS && pending_empty) {
    out->flags |= PGR_RO_FLAG_COMPLETION_READY;
    out->completion_seq = g_completion.seq;
    out->completion_id_length = g_completion.id_length;
    std::memcpy(out->completion_id, g_completion.id, g_completion.id_length * sizeof(uint16_t));
  }

  // Drain the event ring.
  {
    std::lock_guard<std::mutex> lock(g_obs_mutex);
    out->event_count = g_ev_count;
    out->dropped_events = g_ev_dropped;
    if (g_ev_count) std::memcpy(out->events, g_ev, g_ev_count * sizeof(PgrRoutesObserversEvent));
    g_ev_count = 0;
    g_ev_dropped = 0;
  }

  out->generation = req->generation;
  if(req->generation!=g_gen.load(std::memory_order_acquire)||!fresh(req->generation)){
    init_output(out);out->generation=req->generation;out->status=PGR_RO_SKIP;
    g_hook_busy.store(false,std::memory_order_release);return out->status;
  }
  out->status = PGR_RO_OK;
  g_hook_busy.store(false, std::memory_order_release);
  return out->status;
}

int api_set_completing(const PgrRoutesObserversCompleting *c) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_RO_ERR_NOT_INSTALLED;
  if (!c) return PGR_RO_ERR_NULL_ARG;
  if (c->version != PGR_RO_COMPLETING_VERSION) return PGR_RO_ERR_BAD_VERSION;
  if (c->size != sizeof(*c)) return PGR_RO_ERR_BAD_SIZE;
  if (!c->tid || c->tid != current_tid() || !thread_cur()() || !owner_ready(c->tid)) return PGR_RO_SKIP;
  if (c->generation != g_gen.load(std::memory_order_acquire)) return PGR_RO_SKIP;
  if (c->id_length < 1 || c->id_length > PGR_RO_ID_MAX) return PGR_RO_SKIP;
  const uint64_t residual = c->residual_ms > kMaxLeaseMs ? kMaxLeaseMs : c->residual_ms;
  if (!residual) return PGR_RO_SKIP;
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) return PGR_RO_ERR_CLOCK;
  if (g_hook_busy.exchange(true, std::memory_order_acq_rel)) return PGR_RO_ERR_BUSY;
  if(g_state_gen!=c->generation){clear_pending();g_completion.active=false;g_completing.active=false;
    std::lock_guard<std::mutex> lock(g_obs_mutex);g_ev_count=0;g_ev_dropped=0;}
  if(c->generation!=g_gen.load(std::memory_order_acquire)){
    g_hook_busy.store(false,std::memory_order_release);return PGR_RO_SKIP;
  }
  g_lease_deadline.store(now + residual, std::memory_order_release);  // keep the lease live for the imminent bjan
  g_lease_gen.store(c->generation,std::memory_order_release);g_state_gen=c->generation;
  if(!fresh(c->generation)){g_hook_busy.store(false,std::memory_order_release);return PGR_RO_SKIP;}
  g_completing.active = true;
  g_completing.handle = c->consumption_handle;
  g_completing.seq = c->seq;
  g_completing.at = now;
  g_completing.id_length = c->id_length;
  std::memcpy(g_completing.id, c->id, c->id_length * sizeof(uint16_t));
  g_completion.active = false;  // routes.js clears completion when a new complete is issued
  g_hook_busy.store(false, std::memory_order_release);
  return PGR_RO_OK;
}

int api_clear_completing(const PgrRoutesObserversRequest *req) {
  uint64_t unused = 0;
  bool skip = false;
  const int grc = gate_owner(req, false, &unused, &skip);
  if (grc != 0) return grc;
  if (skip) return PGR_RO_SKIP;
  if (g_hook_busy.exchange(true, std::memory_order_acq_rel)) return PGR_RO_ERR_BUSY;
  g_completing.active = false;
  g_hook_busy.store(false, std::memory_order_release);
  return PGR_RO_OK;
}

int api_drain(const PgrRoutesObserversRequest *req) {
  // drain needs an attached owner thread but no lease (teardown).
  if (!g_installed.load(std::memory_order_acquire)) return PGR_RO_ERR_NOT_INSTALLED;
  if (!req) return PGR_RO_ERR_NULL_ARG;
  if (req->version != PGR_RO_REQUEST_VERSION) return PGR_RO_ERR_BAD_VERSION;
  if (req->size != sizeof(*req)) return PGR_RO_ERR_BAD_SIZE;
  if (!req->tid || req->tid != current_tid() || !thread_cur()() || !owner_ready(req->tid)) return PGR_RO_SKIP;
  if (g_hook_busy.exchange(true, std::memory_order_acq_rel)) return PGR_RO_ERR_BUSY;
  g_lease_deadline.store(0, std::memory_order_release);
  g_lease_gen.store(0,std::memory_order_release);
  clear_pending();
  g_completing.active = false;
  g_completion.active = false;
  g_reward_at = 0;
  {
    std::lock_guard<std::mutex> lock(g_obs_mutex);
    for (int i = 0; i < kDepthSlots; ++i) { g_depth[i].tid = 0; g_depth[i].depth = 0; }
    g_has_last_tracking = false;
    g_last_tracking = 0;
    g_last_waypoint_at = 0;
    g_ev_count = 0;
    g_ev_dropped = 0;
  }
  g_hook_busy.store(false, std::memory_order_release);
  return PGR_RO_OK;
}

// --- lite forward entries (any attached Unity thread; tid must equal gettid()) ------
int api_tracking_enter(uint64_t tid) {
  if (!g_installed.load(std::memory_order_acquire) || !tid || tid != current_tid() || !thread_cur()()) return 0;
  if(tid!=g_owner_tid.load(std::memory_order_acquire))return 0;
  if (!lease_live()) return 0;
  std::lock_guard<std::mutex> lock(g_obs_mutex);
  if(!lease_live())return 0;
  DepthSlot *s = find_depth_locked(tid, true);
  if (!s) return 0;
  const uint64_t generation=g_gen.load(std::memory_order_acquire);
  if(s->generation!=generation){s->generation=generation;s->depth=0;}
  if(s->depth==std::numeric_limits<uint32_t>::max())return 0;
  ++s->depth;
  return 1;
}

int api_tracking_leave(uint64_t tid, int32_t code) {
  if (!g_installed.load(std::memory_order_acquire) || !tid || tid != current_tid()) return 0;
  std::lock_guard<std::mutex> lock(g_obs_mutex);
  DepthSlot *s = find_depth_locked(tid, false);
  if(!s||!s->depth)return 0;
  const bool publish=s->generation==g_gen.load(std::memory_order_acquire)&&lease_live();
  if (s && s->depth) { if (--s->depth == 0) s->tid = 0; }
  if(!publish)return 0;
  if (!g_has_last_tracking || code != g_last_tracking) {
    g_last_tracking = code;
    g_has_last_tracking = true;
    push_event_locked(PGR_RO_EV_TRACKING_STATUS, 0, code, 0, 0, nullptr, 0);
  }
  return 1;
}

int api_location_gate(uint64_t tid) {
  if (!g_installed.load(std::memory_order_acquire) || !tid || tid != current_tid()) return 0;
  if(tid!=g_owner_tid.load(std::memory_order_acquire)||!thread_cur()())return 0;
  if (!lease_live()) return 0;
  std::lock_guard<std::mutex> lock(g_obs_mutex);
  DepthSlot *s = find_depth_locked(tid, false);
  return (s && s->depth && s->generation==g_gen.load(std::memory_order_acquire)&&lease_live()) ? 1 : 0;
}

int api_waypoint_observed(uint64_t tid, double lat, double lon) {
  if (!g_installed.load(std::memory_order_acquire) || !tid || tid != current_tid() || !thread_cur()()) return 0;
  if(tid!=g_owner_tid.load(std::memory_order_acquire)||!std::isfinite(lat)||!std::isfinite(lon)||std::abs(lat)>90||std::abs(lon)>180)return 0;
  uint64_t now = 0;
  if (!lease_live(&now)) return 0;
  std::lock_guard<std::mutex> lock(g_obs_mutex);
  if(!lease_live())return 0;
  if (g_last_waypoint_at && now - g_last_waypoint_at < PGR_RO_WAYPOINT_MS) return 0;
  g_last_waypoint_at = now;
  push_event_locked(PGR_RO_EV_WAYPOINT, 0, 0, lat, lon, nullptr, 0);
  return 1;
}

const PgrRoutesObserversApi kApiTable = {
    PGR_RO_API_VERSION, static_cast<uint32_t>(sizeof(PgrRoutesObserversApi)),
    &api_install, &api_generation, &api_revoke, &api_poll, &api_set_completing, &api_clear_completing,
    &api_drain, &api_tracking_enter, &api_tracking_leave, &api_location_gate, &api_waypoint_observed,
};

}  // namespace

extern "C" const void *pgrazor_routes_observers_api(uint32_t version) {
  if (version != PGR_RO_API_VERSION) return nullptr;
  return &kApiTable;
}
