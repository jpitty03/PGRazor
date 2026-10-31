#include "encounter_host.h"

#include <dlfcn.h>
#include <atomic>
#include <string>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "encounter_berries.h"
#include "catch_observers.h"
#include "hook_engine.h"
#include "inventory_host.h"
#include "items.h"
#include "map_poi.h"
#include "quickball_host.h"
#include "runtime_bootstrap.h"
#include "runtime_metadata.h"
#include "runtime_transport.h"
#include "shared_host.h"
#include "shared_metadata.h"
#include "throws.h"
#include "transfer_execution.h"
#include "transfer_facts.h"
#include "transfer_observers.h"
#include "transfer_policy.h"
#include "vgp_actions.h"
#include "vgp_aux.h"
#include "vgp_candidates.h"
#include "vgp_controller.h"
#include "vgp_discovery.h"
#include "vgp_maintenance.h"
#include "vgp_marker.h"
#include "vgp_policy.h"
#include "vgp_results.h"
#include "gym_automation.h"
#include "batch_evolution.h"
#include "buddy_emotion.h"

namespace {

using namespace pgr_shared_meta;

static uint64_t current_os_tid() {
#if defined(__BIONIC__)
  return static_cast<uint64_t>(gettid());
#elif defined(__linux__)
  return static_cast<uint64_t>(syscall(SYS_gettid));
#else
  return 1ULL;
#endif
}

static uint64_t monotonic_ms() {
  struct timespec ts;
  if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0) {
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  }
  return static_cast<uint64_t>(ts.tv_sec) * 1000ULL +
         static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
}

static uint64_t wall_clock_ms() {
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return monotonic_ms();
  return static_cast<uint64_t>(ts.tv_sec) * 1000ULL +
         static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
}

static uint64_t double_to_bits(double v) {
  uint64_t u = 0;
  memcpy(&u, &v, sizeof(u));
  return u;
}

// ============================================================================
// Bounded Event Queue (for poll_event / emit_json / Feature 52 transport)
// ============================================================================
struct QueuedEvent {
  char type[48];
  char json[4096];
};

static constexpr uint32_t kEventQueueCap = 128;
static pthread_mutex_t g_event_mu = PTHREAD_MUTEX_INITIALIZER;
static QueuedEvent g_event_ring[kEventQueueCap];
static uint32_t g_event_head = 0;
static uint32_t g_event_tail = 0;
static uint32_t g_event_count = 0;

// ============================================================================
// Resolved IL2CPP Exports & Host State
// ============================================================================
struct ResolvedExports {
  PgrMetaExports meta;
  PgrSharedMetaExtraExports extra;
  void *(*thread_current)(void);
  void *(*runtime_invoke)(const void *method, void *obj, void **params, void **exc);
  void *(*object_get_class)(void *obj);
  void *(*object_unbox)(void *boxed);
  void *(*object_new)(void *klass);
  void (*field_get_value)(void *obj, void *field, void *out);
  void (*field_set_value)(void *obj, void *field, void *val);
  void (*field_set_value_object)(void *obj, void *field, void *val);
  int32_t (*string_length)(void *str);
  const uint16_t *(*string_chars)(void *str);
  void *(*object_get_virtual_method)(void *obj, void *method);
  void *(*gchandle_new)(void *obj, int pinned);
  void *(*gchandle_get_target)(void *handle);
  void (*gchandle_free)(void *handle);
};

// Nonblocking ownership: managed calls may synchronously reenter or join another thread.
static std::atomic_flag g_busy = ATOMIC_FLAG_INIT;
static thread_local unsigned g_depth = 0;
static std::atomic<bool> g_stop_requested{false};
struct StateGuard {
  bool held;
  StateGuard() : held(g_depth != 0 || !g_busy.test_and_set(std::memory_order_acquire)) { if (held) ++g_depth; }
  ~StateGuard() { if (held && --g_depth == 0) g_busy.clear(std::memory_order_release); }
  explicit operator bool() const { return held; }
};
static bool g_hook_poisoned = false;
static unsigned g_hook_frames = 0;
static bool g_configured = false;
static bool g_installed = false;
static bool g_stopped = false;
static uint64_t g_owner_tid = 0;

static PgrEncounterHostConfig g_cfg = {};
static ResolvedExports g_exp = {};
static PgrSharedMetadataOutput g_md = {};
static PgrEncounterHostOptions g_opts = {};
static PgrEncounterHostSample g_sample = {PGR_EH_API_VERSION, sizeof(PgrEncounterHostSample), 0, 0, 0, 0, 0, 0, 0, 0, 0};

static uint32_t g_active_caps = 0;
static uint64_t g_lease_deadline_ms = 0;
static uint64_t g_last_map_poi_ms = 0;
static uint64_t g_vgp_epoch = 1;
static uint64_t g_transfer_epoch = 1;
static uint64_t g_vgp_cleanup_until_ms = 0;
static uint64_t g_vgp_cleanup_ticket = 0;
static int32_t g_last_vgp_status = -1;
static char g_last_berry_prio_str[128] = {};
static char g_last_keep_species_str[10000] = {};
static bool g_last_vgp_enabled = false;
static uint32_t g_last_vgp_mask = 0;
static int32_t g_keep_species[2000] = {};
static uint32_t g_keep_species_count = 0;

// Leaf API pointers
static const pgr_berry_api_v1 *g_berries = nullptr;
static const PgrMapPoiApi *g_map_poi = nullptr;
static const PgrVgpPolicyApi *g_vp = nullptr;
static const PgrVgpActionsApi *g_va = nullptr;
static const PgrVgpMarkerApi *g_vmk = nullptr;
static const PgrVgpCandidateApi *g_vc = nullptr;
static const PgrVgpResultsApi *g_vr = nullptr;
static const PgrVgpDiscoveryApi *g_vd = nullptr;
static const PgrVgpMaintenanceApi *g_vm = nullptr;
static const PgrVgpAuxApi *g_vx = nullptr;
static const PgrVgpControllerApi *g_vgc = nullptr;
static const PgrTransferApi *g_tp = nullptr;
static const PgrTransferExecApi *g_tx = nullptr;
static const PgrTransferFactsApi *g_tf = nullptr;
// Feature61 is the sole owner of executor23 updates, cleanup and event routing.
struct TransferBroker { uint32_t version,size; int(*ready)(); int(*submit)(const PgrTransferExecRequest*,uint64_t*); int(*next_event)(PgrTransferExecEvent*); };
static const TransferBroker *g_broker = nullptr;
static void *g_berry_status_field = nullptr;
static void *g_berry_response_class = nullptr;
static bool g_ticking = false;
static bool g_direct_transport = false;
// Heap-allocated 294,928-byte MapPOI output buffer (never on stack)
static PgrMapPoiOutput *g_map_poi_out = nullptr;

// VGP -> Transfer active handoff state (4 owned GC handles from PGR_VGC_EV_TRANSFER_READY)
struct ActiveVgpTransfer {
  bool active;
  uint64_t vgc_token;
  uint64_t tx_token;
  uint64_t caught_id;
  int32_t species;
  void *response_handle;
  void *bag_handle;
  void *pokemon_handle;
  void *gm_handle;
};
static ActiveVgpTransfer g_vgp_transfer = {};

static inline void *md_h(int i) { return const_cast<void *>(g_md.opt_handles[i]); }
static inline void *md_k(int i) { return const_cast<void *>(g_md.opt_classes[i]); }
static inline void *md_c(int i) { return const_cast<void *>(g_md.classes[i]); }
static inline void *md_f(int i) { return const_cast<void *>(g_md.fields[i]); }
static inline void *md_m(int i) { return const_cast<void *>(g_md.methods[i]); }

static PgrThrowValue unpack_throw_value(uint64_t first, uint64_t second) {
  PgrThrowValue tv = {};
  uint32_t low = static_cast<uint32_t>(first & 0xFFFFFFFFULL);
  uint32_t high = static_cast<uint32_t>((first >> 32) & 0xFFFFFFFFULL);
  tv.item = static_cast<int32_t>(low);
  memcpy(&tv.radius, &high, sizeof(float));
  tv.bullseye = static_cast<uint8_t>(second & 0xFFULL);
  tv.spinning = static_cast<uint8_t>((second >> 8) & 0xFFULL);
  tv.missed = static_cast<uint8_t>((second >> 16) & 0xFFULL);
  tv.pad = static_cast<uint8_t>((second >> 24) & 0xFFULL);
  return tv;
}

static void pack_throw_value(const PgrThrowValue &tv, uint64_t *first, uint64_t *second) {
  uint32_t low = static_cast<uint32_t>(tv.item);
  uint32_t high = 0;
  memcpy(&high, &tv.radius, sizeof(float));
  *first = static_cast<uint64_t>(low) | (static_cast<uint64_t>(high) << 32);
  *second = (*second & ~0xFFFFFFFFULL) |
            static_cast<uint64_t>(tv.bullseye) |
            (static_cast<uint64_t>(tv.spinning) << 8) |
            (static_cast<uint64_t>(tv.missed) << 16) |
            (static_cast<uint64_t>(tv.pad) << 24);
}

// ============================================================================
// Transport & Event Emission Helpers
// ============================================================================
static const PgrRuntimeTransportApi *transport_api() {
  const auto *t = static_cast<const PgrRuntimeTransportApi *>(
      pgrazor_runtime_transport_api(PGR_TRANSPORT_API_VERSION));
  return t;
}

static void transport_emit_op(uint32_t op, const uint64_t *scalars, uint32_t sc_cnt,
                              const char *const *strings, uint32_t st_cnt) {
  const PgrRuntimeTransportApi *t = transport_api();
  if (!t || !t->emit) return;
  int64_t sc_vals[PGR_TRANSPORT_MAX_SCALARS] = {};
  PgrRuntimeTransportString st_vals[PGR_TRANSPORT_MAX_STR_ARGS] = {};
  for (uint32_t i = 0; i < sc_cnt && i < PGR_TRANSPORT_MAX_SCALARS; ++i) {
    sc_vals[i] = static_cast<int64_t>(scalars[i]);
  }
  for (uint32_t i = 0; i < st_cnt && i < PGR_TRANSPORT_MAX_STR_ARGS; ++i) {
    const char *s = strings[i] ? strings[i] : "";
    st_vals[i].ptr = s;
    st_vals[i].len = static_cast<uint32_t>(strlen(s));
  }
  if (t->emit(op, sc_cnt ? sc_vals : nullptr, sc_cnt, st_cnt ? st_vals : nullptr, st_cnt) != PGR_TRANSPORT_OK && g_direct_transport)
    g_stop_requested.store(true,std::memory_order_release);
}

static void emit_event(const char *type, const char *json) {
  if (!type || !json) return;
  pthread_mutex_lock(&g_event_mu);
  if (!g_direct_transport && g_event_count < kEventQueueCap) {
    QueuedEvent &ev = g_event_ring[g_event_tail];
    snprintf(ev.type, sizeof(ev.type), "%s", type);
    snprintf(ev.json, sizeof(ev.json), "%s", json);
    g_event_tail = (g_event_tail + 1) % kEventQueueCap;
    g_event_count++;
  }
  pthread_mutex_unlock(&g_event_mu);

  if (g_cfg.emit_json) {
    g_cfg.emit_json(type, json);
  }
  std::string env="{\"kind\":\"";env+=type;env+="\",\"backend\":\"native\"";
  size_t length=strlen(json);
  if(length>2 && json[0]=='{' && json[length-1]=='}'){env+=",";env.append(json+1,length-2);}
  env+="}";
  const char *strs[2] = {"quick-close", env.c_str()};
  transport_emit_op(19, nullptr, 0, strs, 2);
}

static void emit_eventf(const char *type, const char *fmt, ...) {
  char buf[4096];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  emit_event(type, buf);
}

static void notify_berry_ready(int ready) {
  if (g_cfg.set_berry_ready) g_cfg.set_berry_ready(ready);
  uint64_t sc[1] = {ready ? 1ULL : 0ULL};
  transport_emit_op(26, sc, 1, nullptr, 0);
}

static void notify_encounter_berry(int active, int busy) {
  if (g_cfg.set_encounter_berry) g_cfg.set_encounter_berry(active, busy);
  uint64_t sc[2] = {active ? 1ULL : 0ULL, busy ? 1ULL : 0ULL};
  transport_emit_op(31, sc, 2, nullptr, 0);
}

static void notify_cooldown_encounter(int active) {
  if (g_cfg.set_cooldown_encounter) g_cfg.set_cooldown_encounter(active);
  const char *s[1] = {""};
  transport_emit_op(30, nullptr, 0, s, 1);
}

static void notify_cooldown_action(const char *action) {
  if (!action) return;
  if (g_cfg.cooldown_action) g_cfg.cooldown_action(action);
  const char *s[1] = {action};
  transport_emit_op(17, nullptr, 0, s, 1);
}

static void notify_vgp_ready(int ready, const char *reason) {
  const char *r = reason ? reason : "";
  if (g_cfg.set_vgp_ready) g_cfg.set_vgp_ready(ready, r);
  uint64_t sc[1] = {ready ? 1ULL : 0ULL};
  const char *s[1] = {r};
  transport_emit_op(53, sc, 1, s, 1);
}

static void notify_vgp_status(const char *status) {
  const char *st = status ? status : "";
  const char *s[1] = {st};
  transport_emit_op(54, nullptr, 0, s, 1);
}

static void notify_vgp_target(int visible, double x, double y, int is_spin) {
  uint64_t sc[4] = {
      visible ? 1ULL : 0ULL,
      double_to_bits(x),
      double_to_bits(y),
      is_spin ? 1ULL : 0ULL,
  };
  transport_emit_op(55, sc, 4, nullptr, 0);
}

static void notify_vgp_result(int success, const char *action) {
  uint64_t sc[1] = {success ? 1ULL : 0ULL};
  const char *s[1] = {action ? action : ""};
  transport_emit_op(58, sc, 1, s, 1);
}

static void notify_transfer_result(const char *summary) {
  if (!summary) return;
  const char *s[1] = {summary};
  transport_emit_op(56, nullptr, 0, s, 1);
}

static void notify_transfer_outcome(const char *outcome){const char *strings[]={outcome};transport_emit_op(22,nullptr,0,strings,1);}
static void notify_transfer_error(const char *reason){char text[256];snprintf(text,sizeof(text),"Transfer Error: %s",reason);notify_transfer_result(text);notify_transfer_outcome("transfer-error");}
static const char *transfer_reason(int code){static const char *reasons[]={"","keep-all","invalid-keep-list","unknown-species","custom-keep-list","unknown-iv","unknown-category","unknown-size","favorite-or-egg","iv-at-least-90","protected-rare","protected-shiny","protected-event","protected-background","protected-xxs","protected-xxl","invalid-input"};return code>0&&code<17?reasons[code]:"invalid-input";}

static void notify_map_poi_snapshot(const char *json_array) {
  if (!json_array) return;
  if (g_cfg.set_map_poi_snapshot) g_cfg.set_map_poi_snapshot(json_array);
  const char *s[1] = {json_array};
  transport_emit_op(41, nullptr, 0, s, 1);
}

// ============================================================================
// RAII GC Pin Discipline for Hook Observers
// ============================================================================
class PinnedArg {
 public:
  explicit PinnedArg(void *raw) : raw_(raw), handle_(nullptr) {
    if (raw_ && g_exp.gchandle_new) {
      handle_ = g_exp.gchandle_new(raw_, 0);
    }
  }
  ~PinnedArg() {
    if (handle_ && g_exp.gchandle_free) {
      g_exp.gchandle_free(handle_);
      handle_ = nullptr;
    }
  }
  void *detach() {void *h=handle_;handle_=nullptr;raw_=nullptr;return h;}
  void release() { if(handle_ && g_exp.gchandle_free) g_exp.gchandle_free(handle_); handle_=nullptr; raw_=nullptr; }
  PinnedArg(const PinnedArg &) = delete;
  PinnedArg &operator=(const PinnedArg &) = delete;

  bool valid() const { return !raw_ || handle_ != nullptr; }
  void *get() const {
    if (handle_ && g_exp.gchandle_get_target) {
      return g_exp.gchandle_get_target(handle_);
    }
    return raw_;
  }

  void rebind(void *new_raw) {
    if (!new_raw || new_raw == get()) return;
    void *new_h = g_exp.gchandle_new ? g_exp.gchandle_new(new_raw, 0) : nullptr;
    if (!new_h) return;
    if (handle_ && g_exp.gchandle_free) {
      g_exp.gchandle_free(handle_);
    }
    handle_ = new_h;
    raw_ = new_raw;
  }

 private:
  void *raw_;
  void *handle_;
};

class IvFrameGuard {
 public:
  IvFrameGuard(const PgrQuickballHostApi *qb, uint64_t tok, uint64_t tid)
      : qb_(qb), tok_(tok), tid_(tid) {}
  ~IvFrameGuard() {
    if (tok_ != 0 && qb_ && qb_->end_catch) {
      qb_->end_catch(tok_, tid_);
      tok_ = 0;
    }
  }
  IvFrameGuard(const IvFrameGuard &) = delete;
  IvFrameGuard &operator=(const IvFrameGuard &) = delete;

 private:
  const PgrQuickballHostApi *qb_;
  uint64_t tok_;
  uint64_t tid_;
};

// ============================================================================
// Hook Trampolines & Observer Callbacks
// ============================================================================
typedef void (*fn_enter_t)(void *scope, void *method_info);
typedef void (*fn_exit_t)(void *scope, void *method_info);
typedef void (*fn_result_t)(void *callback, void *response, void *method_info);
typedef void (*fn_error_t)(void *callback, void *error_str, void *method_info);
typedef void (*fn_submit_t)(void *ball, uint64_t first, uint64_t second, void *method_info);
typedef void (*fn_nameplate_t)(void *self, void *map_pokemon, int32_t weather, void *method_info);

static fn_enter_t g_orig_berry_enter = nullptr;
static fn_exit_t g_orig_berry_exit = nullptr;
static fn_result_t g_orig_berry_result = nullptr;
static fn_error_t g_orig_berry_error = nullptr;
static fn_submit_t g_orig_pokeball_submit = nullptr;
static fn_nameplate_t g_orig_nameplate_ui = nullptr;

static void *g_hooked_addrs[7] = {};
static uint32_t g_hooked_count = 0;

static uint64_t proven_unity_tid() {
  if (!g_exp.thread_current || !g_exp.thread_current()) return 0;
  uint64_t tid = current_os_tid();
  return (tid == g_owner_tid) ? tid : 0;
}

static void hook_berry_enter(void *scope, void *method_info) {
  StateGuard state;
  if (!state || !g_installed || g_stopped || g_stop_requested.load(std::memory_order_acquire) || !proven_unity_tid()) { if (g_orig_berry_enter) g_orig_berry_enter(scope, method_info); return; }
  struct Frame { Frame(){++g_hook_frames;} ~Frame(){--g_hook_frames;} } frame;
  PinnedArg pin_scope(scope);
  uint64_t tid = pin_scope.valid() ? proven_unity_tid() : 0;
  if (tid != 0 && !g_stopped && g_berries && (g_active_caps & PGR_EH_CAP_BERRIES)) {
    g_berries->observe_enter(pin_scope.get(), tid);
  }
  if (g_orig_berry_enter) {
    g_orig_berry_enter(pin_scope.get(), method_info);
  }
}

static void hook_berry_exit(void *scope, void *method_info) {
  StateGuard state;
  if (!state || !g_installed || g_stopped || g_stop_requested.load(std::memory_order_acquire) || !proven_unity_tid()) { if (g_orig_berry_exit) g_orig_berry_exit(scope, method_info); return; }
  struct Frame { Frame(){++g_hook_frames;} ~Frame(){--g_hook_frames;} } frame;
  PinnedArg pin_scope(scope);
  uint64_t tid = pin_scope.valid() ? proven_unity_tid() : 0;
  if (tid != 0 && !g_stopped && g_berries && (g_active_caps & PGR_EH_CAP_BERRIES)) {
    g_berries->observe_exit(pin_scope.get(), tid);
  }
  if (g_orig_berry_exit) {
    g_orig_berry_exit(pin_scope.get(), method_info);
  }
}

static void hook_berry_result(void *callback, void *response, void *method_info) {
  StateGuard state;
  if (!state || !g_installed || g_stopped || g_stop_requested.load(std::memory_order_acquire) || !proven_unity_tid()) { if (g_orig_berry_result) g_orig_berry_result(callback, response, method_info); return; }
  struct Frame { Frame(){++g_hook_frames;} ~Frame(){--g_hook_frames;} } frame;
  PinnedArg pin_cb(callback);
  PinnedArg pin_resp(response);
  uint64_t tid = pin_cb.valid() && pin_resp.valid() ? proven_unity_tid() : 0;
  uint64_t tok = 0;
  if (tid != 0 && !g_stopped && g_berries && (g_active_caps & PGR_EH_CAP_BERRIES)) {
    tok = g_berries->result_begin(pin_cb.get(), tid);
    void *resp_now = pin_resp.get();
    if (resp_now != nullptr) {
      int32_t status_val = -1;
      if (g_berry_status_field && g_exp.object_get_class(resp_now)==g_berry_response_class)
        g_exp.field_get_value(resp_now,g_berry_status_field,&status_val);
      if (status_val == 0) {
        emit_event("cooldown-action", "{\"action\":\"Berry feeding\"}");
        notify_cooldown_action("Berry feeding");
      }
    }
  }
  if (g_orig_berry_result) {
    try { g_orig_berry_result(pin_cb.get(), pin_resp.get(), method_info); }
    catch (...) { if(tok && g_berries) g_berries->update(nullptr,0,0,tid); throw; }
  }
  // Only declare result done if original did NOT throw.
  if (tok != 0 && g_berries) {
    g_berries->result_end(tok, tid);
  }
}

static void hook_berry_error(void *callback, void *error_str, void *method_info) {
  StateGuard state;
  if (!state || !g_installed || g_stopped || g_stop_requested.load(std::memory_order_acquire) || !proven_unity_tid()) { if (g_orig_berry_error) g_orig_berry_error(callback, error_str, method_info); return; }
  struct Frame { Frame(){++g_hook_frames;} ~Frame(){--g_hook_frames;} } frame;
  PinnedArg pin_cb(callback);
  PinnedArg pin_err(error_str);
  uint64_t tid = pin_cb.valid() && pin_err.valid() ? proven_unity_tid() : 0;
  uint64_t tok = 0;
  if (tid != 0 && !g_stopped && g_berries && (g_active_caps & PGR_EH_CAP_BERRIES)) {
    tok = g_berries->result_begin(pin_cb.get(), tid);
  }
  if (g_orig_berry_error) {
    try { g_orig_berry_error(pin_cb.get(), pin_err.get(), method_info); }
    catch (...) { if(tok && g_berries) g_berries->update(nullptr,0,0,tid); throw; }
  }
  // Only declare result done if original did NOT throw.
  if (tok != 0 && g_berries) {
    g_berries->result_end(tok, tid);
  }
}

static void hook_pokeball_submit(void *ball, uint64_t first, uint64_t second, void *method_info) {
  StateGuard state;
  if (!state || !g_installed || g_stopped || g_stop_requested.load(std::memory_order_acquire) || !proven_unity_tid()) { if (g_orig_pokeball_submit) g_orig_pokeball_submit(ball, first, second, method_info); return; }
  struct Frame { Frame(){++g_hook_frames;} ~Frame(){--g_hook_frames;} } frame;
  PinnedArg pin_ball(ball);
  uint64_t tid = pin_ball.valid() ? proven_unity_tid() : 0;
  if (tid != 0 && !g_stopped && g_berries && (g_active_caps & PGR_EH_CAP_BERRIES)) {
    int dec = g_berries->submit(pin_ball.get(), first, second, method_info, tid);
    if (dec == PGR_BERRY_HOLD) {
      return;
    }
  }
  const auto *qb = static_cast<const PgrQuickballHostApi *>(
      pgrazor_quickball_host_api(1));
  if (tid && qb && qb->rewrite_submit) {
    PgrThrowValue tv = unpack_throw_value(first, second);
    qb->rewrite_submit(pin_ball.get(), &tv);
    pack_throw_value(tv, &first, &second);
  }
  if (g_orig_pokeball_submit) {
    g_orig_pokeball_submit(pin_ball.get(), first, second, method_info);
  }
}

// Feature49 is the only btxu owner. Its outer root protects ball across this callback.
static int berry_hold_filter(void *ball, uint64_t tid) {
  StateGuard state;
  if (!state || !g_installed || g_stopped || g_stop_requested.load(std::memory_order_acquire) ||
      !tid || tid!=proven_unity_tid() || !g_berries || !(g_active_caps&PGR_EH_CAP_BERRIES)) return 0;
  return g_berries->peek_held(ball,tid)==PGR_BERRY_HOLD ? 1 : 0;
}
using BerryFilterApi = PgrObsApi2;

static void hook_nameplate_set_pokemon_ui(void *self, void *map_pokemon, int32_t weather, void *method_info) {
  StateGuard state;
  if (!state || !g_installed || g_stopped || g_stop_requested.load(std::memory_order_acquire) || !proven_unity_tid()) { if (g_orig_nameplate_ui) g_orig_nameplate_ui(self, map_pokemon, weather, method_info); return; }
  struct Frame { Frame(){++g_hook_frames;} ~Frame(){--g_hook_frames;} } frame;
  // Pin raw managed arguments BEFORE invoking any leaf that may allocate GC handles or managed objects.
  PinnedArg pin_self(self);
  PinnedArg pin_pokemon(map_pokemon);
  uint64_t tid = pin_self.valid() && pin_pokemon.valid() ? proven_unity_tid() : 0;

  if (tid != 0 && !g_stopped && g_vgc && (g_active_caps & PGR_EH_CAP_VGP)) {
    g_vgc->observe(pin_self.get(), pin_pokemon.get(), tid);
  }

  const auto *qb = static_cast<const PgrQuickballHostApi *>(
      pgrazor_quickball_host_api(1));
  uint64_t iv_tok = 0;
  if (tid != 0 && !g_stopped && qb && qb->begin_catch) {
    // begin_catch returns two rooted arguments: nameplate, then map Pokemon.
    // Managed work in that call can refresh either argument.
    void *refreshed[2] = {pin_self.get(), pin_pokemon.get()};
    iv_tok = qb->begin_catch(refreshed[0], refreshed[1], tid, refreshed);
    pin_self.rebind(refreshed[0]);
    pin_pokemon.rebind(refreshed[1]);
  }

  // RAII guard ensures end_catch runs on both normal exit and exception unwind,
  // before pin_pokemon and pin_self destructors unpin the GC handles.
  IvFrameGuard iv_guard(qb, iv_tok, tid);
  if (g_orig_nameplate_ui) {
    g_orig_nameplate_ui(pin_self.get(), pin_pokemon.get(), weather, method_info);
  }
}

// ============================================================================
// Proxy Items API for VGP Controller (Feature 33 -> Feature 65 / Feature 14)
// ============================================================================
static void *proxy_items_service_acquire(void) {
  const auto *inv = static_cast<const PgrInventoryHostApi *>(
      pgrazor_inventory_host_api(1));
  if (inv && inv->acquire_item_service) {
    void *h = inv->acquire_item_service();
    if (h) return h;
  }
  const auto *it = static_cast<const pgrazor_items_api_v1 *>(
      pgrazor_items_api(PGRAZOR_ITEMS_API_VERSION));
  if (it && it->service_acquire) {
    return it->service_acquire();
  }
  return nullptr;
}

static void *proxy_items_service_handle(void) {
  const auto *it = static_cast<const pgrazor_items_api_v1 *>(
      pgrazor_items_api(PGRAZOR_ITEMS_API_VERSION));
  if (it && it->service_handle) {
    return it->service_handle();
  }
  return nullptr;
}

static pgrazor_items_api_v1 kProxyItemsApi = {};

// ============================================================================
// Metadata Derivation Helpers (matching vgp.js / native-vgp-*.js)
// ============================================================================
static void *derived_return_class(void *method_info) {
  if (!method_info || !g_exp.meta.method_get_return_type || !g_exp.meta.class_from_type) {
    return nullptr;
  }
  auto get_ret = reinterpret_cast<void *(*)(void *)>(g_exp.meta.method_get_return_type);
  auto from_type = reinterpret_cast<void *(*)(void *)>(g_exp.meta.class_from_type);
  void *ret_type = get_ret(method_info);
  if (!ret_type) return nullptr;
  return from_type(ret_type);
}

static void *derived_field_parent(void *field_info) {
  if (!field_info || !g_exp.meta.field_get_parent) return nullptr;
  auto get_parent = reinterpret_cast<void *(*)(void *)>(g_exp.meta.field_get_parent);
  return get_parent(field_info);
}

static void *resolve_rpc_field(void *bag_class,void *rpc_class){
  if(!g_exp.meta.class_get_field_from_name || !g_exp.extra.class_get_parent || !g_exp.meta.field_get_offset ||
     !g_exp.meta.field_get_type || !g_exp.meta.class_from_type || !g_exp.meta.class_is_valuetype || !g_exp.meta.class_is_assignable_from)return nullptr;
  auto field=reinterpret_cast<void*(*)(void*,const char*)>(g_exp.meta.class_get_field_from_name);
  auto parent=reinterpret_cast<void*(*)(void*)>(g_exp.extra.class_get_parent);
  auto offset=reinterpret_cast<size_t(*)(void*)>(g_exp.meta.field_get_offset);
  auto type=reinterpret_cast<void*(*)(void*)>(g_exp.meta.field_get_type);
  auto klass=reinterpret_cast<void*(*)(void*)>(g_exp.meta.class_from_type);
  auto value=reinterpret_cast<bool(*)(void*)>(g_exp.meta.class_is_valuetype);
  auto accepts=reinterpret_cast<bool(*)(void*,void*)>(g_exp.meta.class_is_assignable_from);
  for(unsigned i=0;bag_class&&i<16;++i,bag_class=parent(bag_class)){
    void *f=field(bag_class,"cynr");if(!f)continue;void *k=klass(type(f));
    if(offset(f)<16||offset(f)>512||!k||value(k)||!accepts(k,rpc_class))return nullptr;
    return f;
  }return nullptr;
}

// ============================================================================
// Active VGP Transfer Helpers
// ============================================================================
static void clear_active_vgp_transfer() {
  if (g_exp.gchandle_free) {
    if (g_vgp_transfer.response_handle) g_exp.gchandle_free(g_vgp_transfer.response_handle);
    if (g_vgp_transfer.bag_handle) g_exp.gchandle_free(g_vgp_transfer.bag_handle);
    if (g_vgp_transfer.pokemon_handle) g_exp.gchandle_free(g_vgp_transfer.pokemon_handle);
    if (g_vgp_transfer.gm_handle) g_exp.gchandle_free(g_vgp_transfer.gm_handle);
  }
  memset(&g_vgp_transfer, 0, sizeof(g_vgp_transfer));
}

static void handle_vgp_transfer_ready(const PgrVgpControllerOut &out, uint64_t tid, uint64_t lease_ms) {
  clear_active_vgp_transfer();
  g_vgp_transfer.response_handle = reinterpret_cast<void *>(static_cast<uintptr_t>(out.response_handle));
  g_vgp_transfer.bag_handle = reinterpret_cast<void *>(static_cast<uintptr_t>(out.bag_handle));
  g_vgp_transfer.pokemon_handle = reinterpret_cast<void *>(static_cast<uintptr_t>(out.pokemon_handle));
  g_vgp_transfer.gm_handle = reinterpret_cast<void *>(static_cast<uintptr_t>(out.gm_handle));
  g_vgp_transfer.vgc_token = out.transfer_token;
  g_vgp_transfer.caught_id = out.caught_id;

  if (!(g_active_caps & PGR_EH_CAP_TRANSFER) || !g_tf || !g_tp || !g_broker ||
      !g_opts.auto_transfer_allowed) {
    if (g_vgc && out.transfer_token != 0) {
      g_vgc->transfer_feedback(out.transfer_token, PGR_VGC_FB_KEPT, tid);
    }
    clear_active_vgp_transfer();
    return;
  }

  g_sample.transfer_eval_serial++;

  void *pk_obj = g_exp.gchandle_get_target ? g_exp.gchandle_get_target(g_vgp_transfer.pokemon_handle) : nullptr;
  void *gm_obj = g_exp.gchandle_get_target ? g_exp.gchandle_get_target(g_vgp_transfer.gm_handle) : nullptr;
  uint32_t protect_mask = g_opts.transfer_protect >= 0 ? static_cast<uint32_t>(g_opts.transfer_protect) : 63u;

  PgrTransferFactsRequest req = {};
  req.version = PGR_TF_REQUEST_VERSION;
  req.size = sizeof(req);
  req.owner_tid = tid;
  req.lease_ms = lease_ms > 0 ? lease_ms : 2500;
  req.caught_id = out.caught_id;
  req.pokemon = pk_obj;
  req.gm = gm_obj;
  req.protect = protect_mask;

  PgrTransferFactsOut tf_out = {};
  int ins_rc = g_tf->inspect(&req, &tf_out);
  if (ins_rc != PGR_TF_VALID) {
    emit_eventf("transfer-error", "{\"id\":\"%llu\",\"reason\":\"facts-%d\"}",
                static_cast<unsigned long long>(out.caught_id), ins_rc);
    char reason[64];snprintf(reason,sizeof(reason),"facts-%d",ins_rc);notify_transfer_error(reason);
    if (g_vgc && out.transfer_token != 0) {
      g_vgc->transfer_feedback(out.transfer_token, PGR_VGC_FB_FAILED, tid);
    }
    clear_active_vgp_transfer();
    return;
  }

  PgrTransferInput tp_in = {};
  tp_in.version = 1;
  tp_in.size = sizeof(tp_in);
  tp_in.attack = tf_out.attack;
  tp_in.defense = tf_out.defense;
  tp_in.stamina = tf_out.stamina;
  tp_in.species = tf_out.species;
  tp_in.protect = protect_mask;
  tp_in.known = tf_out.known;
  tp_in.flags = tf_out.flags;
  tp_in.keep_all = g_opts.transfer_keep_all ? 1u : 0u;
  tp_in.keep_count = g_keep_species_count;
  tp_in.keep_ids = g_keep_species_count ? g_keep_species : nullptr;

  int reason_code = g_tp->decide(&tp_in);
  if (reason_code < 0) {
    if (g_vgc && out.transfer_token != 0) {
      g_vgc->transfer_feedback(out.transfer_token, PGR_VGC_FB_FAILED, tid);
    }
    clear_active_vgp_transfer();
    return;
  }

  if (reason_code > 0) {
    emit_eventf("transfer-kept",
                "{\"id\":\"%llu\",\"species\":%d,\"reasonCode\":%d}",
                static_cast<unsigned long long>(out.caught_id), tf_out.species, reason_code);
    char text[128];snprintf(text,sizeof(text),"Kept: %s",transfer_reason(reason_code));notify_transfer_result(text);
    if (g_vgc && out.transfer_token != 0) {
      g_vgc->transfer_feedback(out.transfer_token, PGR_VGC_FB_KEPT, tid);
    }
    clear_active_vgp_transfer();
    return;
  }

  uint64_t eff_lease = lease_ms > 0 ? lease_ms : 2500ULL;
  (void)eff_lease; // authoritative settings and lease come from61 at submit

  void *bag_obj = g_exp.gchandle_get_target ? g_exp.gchandle_get_target(g_vgp_transfer.bag_handle) : nullptr;
  pk_obj = g_exp.gchandle_get_target ? g_exp.gchandle_get_target(g_vgp_transfer.pokemon_handle) : nullptr;

  PgrTransferExecRequest tx_req = {};
  tx_req.version = PGR_TX_REQUEST_VERSION;
  tx_req.size = sizeof(tx_req);
  tx_req.captured_id = out.caught_id;
  tx_req.epoch = g_transfer_epoch;
  tx_req.tid = tid;
  tx_req.bag = bag_obj;
  tx_req.pokemon = pk_obj;
  tx_req.input = tp_in;

  uint64_t tx_tok = 0;
  int sub_rc = g_broker->submit(&tx_req, &tx_tok);
  if(sub_rc==0 && !tx_tok){
    notify_transfer_result("Kept: settings-changed");
    if(g_vgc && out.transfer_token)g_vgc->transfer_feedback(out.transfer_token,PGR_VGC_FB_KEPT,tid);
    clear_active_vgp_transfer();return;
  }
  if (sub_rc != PGR_TX_ISSUED || tx_tok == 0) {
    char reason[64];snprintf(reason,sizeof(reason),"submit-%d",sub_rc);notify_transfer_error(reason);
    emit_eventf("transfer-error",
                "{\"id\":\"%llu\",\"species\":%d,\"reason\":\"submit-%d\"}",
                static_cast<unsigned long long>(out.caught_id), tf_out.species, sub_rc);
    if (g_vgc && out.transfer_token != 0) {
      g_vgc->transfer_feedback(out.transfer_token, PGR_VGC_FB_FAILED, tid);
    }
    clear_active_vgp_transfer();
    return;
  }

  notify_transfer_result("Transfer requested; waiting for bag update");
  g_vgp_transfer.active = true;
  g_vgp_transfer.tx_token = tx_tok;
  g_vgp_transfer.species = tf_out.species;
  emit_eventf("transfer-requested", "{\"id\":\"%llu\",\"species\":%d}",
              static_cast<unsigned long long>(out.caught_id), tf_out.species);
  if (g_vgc && out.transfer_token != 0) {
    g_vgc->transfer_feedback(out.transfer_token, PGR_VGC_FB_REQUESTED, tid);
  }
}

static void pump_transfer_events(uint64_t tid, int stopping) {
  if (!g_broker || !(g_active_caps & PGR_EH_CAP_TRANSFER) || stopping) return;
  PgrTransferExecEvent ev = {};
  while (g_broker->next_event(&ev) == 1) {
    if (ev.kind == PGR_TX_EVENT_COMPLETE || ev.kind == PGR_TX_EVENT_TIMEOUT) {
      bool ok = (ev.kind == PGR_TX_EVENT_COMPLETE) && ((ev.aux & 1) != 0);
      uint64_t cid = (g_vgp_transfer.active && g_vgp_transfer.tx_token == ev.token)
                         ? g_vgp_transfer.caught_id
                         : 0ULL;
      if (ok) {
        emit_eventf("transfer-confirmed", "{\"id\":\"%llu\",\"token\":%llu}",
                    static_cast<unsigned long long>(cid),
                    static_cast<unsigned long long>(ev.token));
        notify_transfer_result("Transferred newly caught Pokemon");
        notify_transfer_outcome("transferred");
      } else {
        notify_transfer_error(ev.kind==PGR_TX_EVENT_TIMEOUT?"response-timeout":"server-result");
        emit_eventf("transfer-error",
                    "{\"id\":\"%llu\",\"token\":%llu,\"kind\":%u}",
                    static_cast<unsigned long long>(cid),
                    static_cast<unsigned long long>(ev.token), ev.kind);
      }
      if (g_vgp_transfer.active && g_vgp_transfer.tx_token == ev.token) {
        if (g_vgc && g_vgp_transfer.vgc_token != 0) {
          g_vgc->transfer_feedback(g_vgp_transfer.vgc_token,
                                   ok ? PGR_VGC_FB_SUCCESS : PGR_VGC_FB_FAILED, tid);
        }
        clear_active_vgp_transfer();
      }
    }
  }
}

// ============================================================================
// Berry Event & Resume Pumping
// ============================================================================
static const char *berry_name_for_id(int32_t id) {
  for (uint32_t i = 0; i < g_md.item_name_count && i < kItemNameMax; ++i) {
    if (g_md.item_names[i].value == id && g_md.item_names[i].name[0] != '\0') {
      return g_md.item_names[i].name;
    }
  }
  switch (id) {
    case 701: return "Razz Berry";
    case 702: return "Bluk Berry";
    case 703: return "Nanab Berry";
    case 704: return "Weepar Berry";
    case 705: return "Pinap Berry";
    case 706: return "Golden Razz Berry";
    case 707: return "Golden Nanab Berry";
    case 708: return "Silver Pinap Berry";
    default: return "Berry";
  }
}

static const char *berry_error_reason_str(int32_t aux) {
  switch (aux) {
    case PGR_BERRY_REASON_TIMEOUT:
      return "Berry response timed out; submitting catch without retry";
    case PGR_BERRY_REASON_CALLBACK_READBACK:
      return "Berry callback field readback mismatch";
    case PGR_BERRY_REASON_REQUEST_FAULT:
      return "Managed exception issuing berry feed request";
    case PGR_BERRY_REASON_BALLTYPE_FAULT:
      return "Managed exception reading BallType";
    case PGR_BERRY_REASON_INVENTORY:
      return "Berry bag quantity lookup failed";
    case PGR_BERRY_REASON_GCHANDLE:
      return "Berry GC handle allocation failed";
    default:
      return "Berry error";
  }
}

static void notify_inventory_poll() {
  const auto *inv = static_cast<const PgrInventoryHostApi *>(
      pgrazor_inventory_host_api(1));
  if (inv && inv->request_item_poll) {
    inv->request_item_poll();
  }
}

static void pump_berry_events_and_resume(uint64_t tid, int stopping) {
  if (!g_berries || !(g_active_caps & PGR_EH_CAP_BERRIES)) return;

  g_berries->tick(tid, stopping);

  pgr_berry_resume_v1 res = {};
  uint64_t ticket = g_berries->begin_resume(tid, &res);
  if (ticket != 0) {
    if (res.cause == PGR_BERRY_RESUME_DONE) {
      notify_inventory_poll();
    }
    struct ResumeFinishGuard {
      const pgr_berry_api_v1 *api;
      uint64_t t;
      uint64_t owner;
      ~ResumeFinishGuard() {
        if (api && api->finish_resume && t != 0) {
          api->finish_resume(t, owner);
        }
      }
    } guard{g_berries, ticket, tid};

    PinnedArg resumed_ball(res.ball);
    uint64_t first = res.first;
    uint64_t second = res.second;
    const auto *qb = static_cast<const PgrQuickballHostApi *>(
        pgrazor_quickball_host_api(1));
    if (resumed_ball.valid() && qb && qb->rewrite_submit) {
      PgrThrowValue tv = unpack_throw_value(first, second);
      qb->rewrite_submit(resumed_ball.get(), &tv);
      pack_throw_value(tv, &first, &second);
    }
    if (g_orig_pokeball_submit && res.ball) {
      g_orig_pokeball_submit(resumed_ball.get(), first, second, res.method);
    }
    g_sample.berry_resume_serial++;
  }

  pgr_berry_event_v1 ev = {};
  while (g_berries->next_event(&ev) == 1) {
    switch (ev.code) {
      case PGR_BERRY_EVT_READY:
        emit_eventf("berry-ready", "{\"ready\":%s}", ev.aux ? "true" : "false");
        notify_berry_ready(ev.aux ? 1 : 0);
        break;
      case PGR_BERRY_EVT_ENCOUNTER: {
        int active = ((ev.aux & 1) != 0 && (ev.aux & 4) != 0) ? 1 : 0;
        int busy = ((ev.aux & 2) != 0) ? 1 : 0;
        emit_eventf("encounter-berry", "{\"active\":%s,\"busy\":%s}",
                    active ? "true" : "false", busy ? "true" : "false");
        notify_encounter_berry(active, busy);
        break;
      }
      case PGR_BERRY_EVT_COOLDOWN_ENC: {
        int active = ev.aux ? 1 : 0;
        emit_eventf("cooldown-encounter", "{\"active\":%s}", active ? "true" : "false");
        notify_cooldown_encounter(active);
        break;
      }
      case PGR_BERRY_EVT_ACTION: {
        const char *act = (ev.aux == 0) ? "Ball throw"
                                        : ((ev.aux == 1) ? "Ball drop" : "Berry feeding");
        emit_eventf("cooldown-action", "{\"action\":\"%s\"}", act);
        notify_cooldown_action(act);
        break;
      }
      case PGR_BERRY_EVT_IMPACT_QUEUED:
        emit_eventf("berry-impact-queued", "{\"id\":%d,\"name\":\"%s\"}",
                    ev.id, berry_name_for_id(ev.id));
        break;
      case PGR_BERRY_EVT_FEED_REQUESTED:
        emit_eventf("berry-feed-requested", "{\"id\":%d,\"name\":\"%s\"}",
                    ev.id, berry_name_for_id(ev.id));
        break;
      case PGR_BERRY_EVT_ERROR:
        emit_eventf("berry-error", "{\"id\":%d,\"name\":\"%s\",\"reason\":\"%s\"}",
                    ev.id, berry_name_for_id(ev.id), berry_error_reason_str(ev.aux));
        break;
      default:
        break;
    }
  }
}

// ============================================================================
// VGP Controller Tick & Event Mapping
// ============================================================================
static const char *vgp_status_string(uint32_t code) {
  static const char *const kStatuses[32] = {
    "Off","Waiting for game readiness","Waiting for inventory","Out of red Poke Balls",
    "Pokemon storage full","Item bag full","Waiting for Pokemon retry / new targets",
    "Nearby Pokemon outside 40m catch range","Watching nearby Pokemon and PokeStops","Catching nearby Pokemon",
    "Spinning nearby PokeStop","Pokemon caught","Pokemon not caught","PokeStop spun","Spin unsuccessful",
    "Request rejected","Response timed out; toggle VGP to resume","Paused: native VGP error","Checking auto-transfer",
    "Transferring caught Pokemon","Transferred","Transfer error","Kept: protected","Caught; transfer inventory update timed out",
    "Cleaning inventory","Waiting for recent targets to expire","Initializing encounter support",
    "Pokemon catching disabled","VGP native actions unavailable",
    "Smart catch: waiting before throw","Smart catch: applying berry","Smart catch: no regular balls available"};
  return code<32?kStatuses[code]:"Paused: invalid VGP status";
}

static void tick_vgp_controller(uint64_t tid, uint64_t now_ms, uint64_t lease_ms) {
  if (!g_vgc || !(g_active_caps & PGR_EH_CAP_VGP)) return;

  pump_transfer_events(tid, 0);

  PgrVgpControllerTick in = {};
  in.version = PGR_VGC_TICK_VERSION;
  in.size = sizeof(in);
  in.owner = tid;
  in.epoch = g_vgp_epoch;
  in.lease_ms = (g_opts.vgp_allowed && g_opts.vgp_enabled) ? lease_ms : 0;
  in.allowed_ms = (g_opts.vgp_allowed && g_opts.vgp_enabled && lease_ms > 0)
                      ? lease_ms
                      : 0;
  const auto *cleanup = static_cast<const PgrInventoryCleanupApi *>(pgrazor_inventory_host_api(2));
  if (!g_opts.item_transfer_enabled || !g_opts.item_access_allowed ||
      !cleanup || !cleanup->pending || !cleanup->pending(g_vgp_cleanup_ticket))
    g_vgp_cleanup_until_ms = 0;
  in.cleanup_ms = g_vgp_cleanup_until_ms>now_ms ? g_vgp_cleanup_until_ms-now_ms : 0;
  if(in.cleanup_ms>20000)in.cleanup_ms=20000;
  in.generation = g_vgc->generation();
  in.lat = g_opts.lat;
  in.lon = g_opts.lon;
  in.mask = (g_opts.vgp_catch_allowed ? 1u : 0u) |
            (g_opts.vgp_spin_allowed ? 2u : 0u);

  uint32_t flags = 0;
  if (g_opts.vgp_map_visible) flags |= PGR_VGC_TF_MAP_VISIBLE;
  if ((g_active_caps & PGR_EH_CAP_TRANSFER) && g_opts.auto_transfer_allowed) {
    flags |= PGR_VGC_TF_TRANSFER_ENABLED;
  }
  if (g_opts.item_transfer_enabled && g_opts.item_access_allowed) {
    flags |= PGR_VGC_TF_ITEM_ENABLED;
  }
  const auto *quick=static_cast<const PgrQuickballHostApi*>(pgrazor_quickball_host_api(1));
  PgrQuickballHostSample quick_state{1,sizeof(quick_state)};
  if (quick && quick->sample && quick->sample(&quick_state)==0 && quick_state.ready && !quick_state.stopping)
    flags |= PGR_VGC_TF_MANUAL_READY;
  in.flags = flags;

  PgrVgpControllerOut out = {};
  int rc = g_vgc->tick(&in, &out);
  if (rc < 0) {
    g_sample.last_error = rc;
    return;
  }
  g_sample.vgp_tick_serial++;

  if (static_cast<int32_t>(out.status) != g_last_vgp_status) {
    g_last_vgp_status = static_cast<int32_t>(out.status);
    const char *st = vgp_status_string(out.status);
    emit_eventf("vgp-status", "{\"message\":\"%s\",\"code\":%u,\"error\":%d}", st, out.status,out.error);
    notify_vgp_status(st);
  }

  if (out.flags & PGR_VGC_EV_CATCH_ISSUED) {
    emit_event("vgp-request", "{\"action\":\"catch\"}");
    emit_event("cooldown-action", "{\"action\":\"VGP ball throw\"}");
    notify_cooldown_action("VGP ball throw");
  }
  if(out.flags & PGR_VGC_EV_SMART_BERRY){
    emit_event("cooldown-action","{\"action\":\"Smart catch berry\"}");
    notify_cooldown_action("Smart catch berry");
  }
  if (out.flags & PGR_VGC_EV_SPIN_ISSUED) {
    emit_event("vgp-request", "{\"action\":\"spin\"}");
  }
  if (out.flags & PGR_VGC_EV_CATCH_RESULT) {
    bool ok = out.success != 0;
    emit_eventf("vgp-result",
                "{\"action\":\"catch\",\"success\":%s,\"status\":%d,\"retryMs\":%llu}",
                ok ? "true" : "false", out.catch_status,
                static_cast<unsigned long long>(out.retry_ms));
    notify_vgp_result(ok ? 1 : 0, "catch");
  }
  if (out.flags & PGR_VGC_EV_SPIN_RESULT) {
    bool ok = out.success != 0;
    emit_eventf("vgp-result",
                "{\"action\":\"spin\",\"success\":%s,\"status\":%d,\"retryMs\":%llu}",
                ok ? "true" : "false", out.catch_status,
                static_cast<unsigned long long>(out.retry_ms));
    notify_vgp_result(ok ? 1 : 0, "spin");

  }
  if (out.flags & PGR_VGC_EV_ITEM_CLEANUP) {
    const auto *cleanup = static_cast<const PgrInventoryCleanupApi *>(pgrazor_inventory_host_api(2));
    g_vgp_cleanup_ticket = g_opts.item_transfer_enabled && g_opts.item_access_allowed &&
        cleanup && cleanup->request ? cleanup->request() : 0;
    g_vgp_cleanup_until_ms = g_vgp_cleanup_ticket ? now_ms + 20000ULL : 0;
  }
  if (out.flags & PGR_VGC_EV_TRANSFER_CANCEL) {
    clear_active_vgp_transfer();
  }
  if (out.flags & PGR_VGC_EV_TRANSFER_READY) {
    handle_vgp_transfer_ready(out, tid, lease_ms);
  }
  if (out.flags & PGR_VGC_EV_MARKER_UPDATE) {
    const char *act = (out.phase == PGR_VGC_PH_SPIN) ? "spin" : "catch";
    emit_eventf("vgp-marker", "{\"visible\":true,\"x\":%.2f,\"y\":%.2f,\"action\":\"%s\"}",
                out.marker_x, out.marker_y, act);
    notify_vgp_target(out.marker_visible && isfinite(out.marker_x) && isfinite(out.marker_y), out.marker_x, out.marker_y,
                      out.phase == PGR_VGC_PH_SPIN ? 1 : 0);
  } else if (out.flags & PGR_VGC_EV_MARKER_HIDE) {
    emit_event("vgp-marker", "{\"visible\":false}");
    notify_vgp_target(0, 0.0, 0.0, 0);
  }
}

// ============================================================================
// MapPOI Snapshot Tick & Enrichment (Feature 42)
// ============================================================================
static const char *lookup_team_name(int32_t team_id) {
  if ((g_md.enrichment_flags & 2u) != 0) {
    for (uint32_t i = 0; i < g_md.team_name_count && i < kTeamNameMax; ++i) {
      if (g_md.team_names[i].value == team_id && g_md.team_names[i].name[0] != '\0') {
        return g_md.team_names[i].name;
      }
    }
  }
  return nullptr;
}

static const char *lookup_raid_tier(int32_t level) {
  if ((g_md.enrichment_flags & 1u) != 0) {
    for (uint32_t i = 0; i < g_md.raid_tier_count && i < kRaidTierMax; ++i) {
      if (g_md.raid_tiers[i].value == level && g_md.raid_tiers[i].name[0] != '\0') {
        return g_md.raid_tiers[i].name;
      }
    }
  }
  return nullptr;
}

static size_t utf16_to_escaped_json(const uint16_t *src, uint32_t len, char *dst, size_t cap) {
  size_t pos = 0;
  for (uint32_t i = 0; i < len && pos + 8 < cap; ++i) {
    uint16_t c = src[i];
    if (c == '"' || c == '\\') {
      dst[pos++] = '\\';
      dst[pos++] = static_cast<char>(c);
    } else if (c >= 0x20 && c <= 0x7e) {
      dst[pos++] = static_cast<char>(c);
    } else {
      pos += static_cast<size_t>(snprintf(dst + pos, cap - pos, "\\u%04x", c));
    }
  }
  dst[pos] = '\0';
  return pos;
}

static void tick_map_poi(uint64_t tid, uint64_t now_ms, uint64_t lease_ms) {
  if (!g_map_poi || !(g_active_caps & PGR_EH_CAP_MAP_POI) || !g_map_poi_out) return;
  if (!g_opts.map_poi_allowed || lease_ms == 0) return;
  if (g_last_map_poi_ms != 0 && (now_ms - g_last_map_poi_ms) < 2000ULL) return;
  g_last_map_poi_ms = now_ms;

  PgrMapPoiRequest req = {};
  req.version = PGR_MP_REQUEST_VERSION;
  req.size = sizeof(PgrMapPoiRequest);
  req.tid = tid;
  req.remaining_ms = lease_ms;
  req.wall_ms = wall_clock_ms();

  int rc = g_map_poi->scan(&req, g_map_poi_out);
  if (rc != PGR_MP_OK) {
    if (rc < 0) g_sample.last_error = rc;
    return;
  }

  g_sample.map_poi_scan_serial++;
  g_sample.last_map_poi_count = g_map_poi_out->count;

  static char s_poi_json[1040000];
  static char s_poi_event[1040200];
  size_t pos = 0;
  s_poi_json[pos++] = '[';
  uint32_t count = g_map_poi_out->count;
  if (count > PGR_MP_MAX_POINTS) count = PGR_MP_MAX_POINTS;

  for (uint32_t i = 0; i < count && pos + 2048 < sizeof(s_poi_json); ++i) {
    const PgrMapPoiPoint &pt = g_map_poi_out->points[i];
    if (i > 0) s_poi_json[pos++] = ',';
    char id_buf[1600];
    uint32_t id_len = pt.id_length <= PGR_MP_MAX_ID ? pt.id_length : PGR_MP_MAX_ID;
    utf16_to_escaped_json(pt.id, id_len, id_buf, sizeof(id_buf));

    const char *kind_str = (pt.kind == PGR_MP_KIND_GYM) ? "gym" : "stop";
    pos += static_cast<size_t>(snprintf(
        s_poi_json + pos, sizeof(s_poi_json) - pos,
        "{\"id\":\"%s\",\"kind\":\"%s\",\"lat\":%.6f,\"lon\":%.6f",
        id_buf, kind_str, pt.lat, pt.lon));

    if (pt.kind == PGR_MP_KIND_GYM) {
      if (pt.flags & PGR_MP_FLAG_TEAM) {
        pos += static_cast<size_t>(snprintf(
            s_poi_json + pos, sizeof(s_poi_json) - pos, ",\"teamId\":%d", pt.team_id));
        const char *tn = lookup_team_name(pt.team_id);
        if (tn) {
          pos += static_cast<size_t>(snprintf(
              s_poi_json + pos, sizeof(s_poi_json) - pos, ",\"team\":\"%s\"", tn));
        }
      }
      if (pt.flags & PGR_MP_FLAG_SLOTS) {
        pos += static_cast<size_t>(snprintf(
            s_poi_json + pos, sizeof(s_poi_json) - pos, ",\"slots\":%d", pt.slots));
      }
      if (pt.raid_end > 0.0) {
        pos += static_cast<size_t>(snprintf(
            s_poi_json + pos, sizeof(s_poi_json) - pos,
            ",\"raidEnd\":%.0f,\"raidStart\":%.0f,\"egg\":%s",
            pt.raid_end, pt.raid_start,
            (pt.flags & PGR_MP_FLAG_EGG) ? "true" : "false"));
      }
      if (pt.flags & PGR_MP_FLAG_LEVEL) {
        pos += static_cast<size_t>(snprintf(
            s_poi_json + pos, sizeof(s_poi_json) - pos, ",\"level\":%d", pt.level));
        const char *rt = lookup_raid_tier(pt.level);
        if (rt) {
          pos += static_cast<size_t>(snprintf(
              s_poi_json + pos, sizeof(s_poi_json) - pos, ",\"tier\":\"%s\"", rt));
        }
      }
      if (pt.flags & PGR_MP_FLAG_BOSS) {
        pos += static_cast<size_t>(snprintf(
            s_poi_json + pos, sizeof(s_poi_json) - pos, ",\"boss\":%d", pt.boss));
      }
    }
    s_poi_json[pos++] = '}';
  }
  s_poi_json[pos++] = ']';
  s_poi_json[pos] = '\0';

  snprintf(s_poi_event, sizeof(s_poi_event), "{\"kind\":\"map-pois\",\"points\":%s}", s_poi_json);
  notify_map_poi_snapshot(s_poi_event);
  emit_event("map-pois", s_poi_event);
}

// ============================================================================
// Feature 52 Transport Settings Polling
// ============================================================================
static int32_t resolve_berry_token(const char *tok, size_t len) {
  if (len == 0) return 0;
  char buf[64];
  if (len >= sizeof(buf)) len = sizeof(buf) - 1;
  memcpy(buf, tok, len);
  buf[len] = '\0';
  char *endp = nullptr;
  long v = strtol(buf, &endp, 10);
  if (endp != buf && *endp == '\0' && v > 0) return static_cast<int32_t>(v);
  for (uint32_t i = 0; i < g_md.item_name_count && i < kItemNameMax; ++i) {
    if (strcasecmp(g_md.item_names[i].name, buf) == 0) {
      return g_md.item_names[i].value;
    }
  }
  if (strcasecmp(buf, "razz") == 0 || strcasecmp(buf, "ITEM_RAZZ_BERRY") == 0) return 701;
  if (strcasecmp(buf, "nanab") == 0 || strcasecmp(buf, "ITEM_NANAB_BERRY") == 0) return 703;
  if (strcasecmp(buf, "pinap") == 0 || strcasecmp(buf, "ITEM_PINAP_BERRY") == 0) return 705;
  if (strcasecmp(buf, "golden_razz") == 0 || strcasecmp(buf, "ITEM_GOLDEN_RAZZ_BERRY") == 0) return 706;
  if (strcasecmp(buf, "golden_nanab") == 0 || strcasecmp(buf, "ITEM_GOLDEN_NANAB_BERRY") == 0) return 707;
  if (strcasecmp(buf, "silver_pinap") == 0 || strcasecmp(buf, "ITEM_GOLDEN_PINAP_BERRY") == 0) return 708;
  return 0;
}

static void sync_from_transport(uint64_t tid) {
  const PgrRuntimeTransportApi *t = transport_api();
  if (!t || !t->sample || !t->get_bool || !t->get_int || !t->get_double || !t->get_string) return;

  PgrRuntimeTransportSample start = {PGR_TRANSPORT_SAMPLE_VERSION, sizeof(start)};
  if (t->sample(&start) != PGR_TRANSPORT_OK || start.snapshots_accepted == 0) {
    if (!g_direct_transport) return;
    start = {};
  }
  bool fresh = (start.snapshot_fresh != 0 && start.fault == 0);
  int active = 0;
  if (fresh && t->get_bool(3, &active) != PGR_TRANSPORT_OK) {
    fresh = false;
  }

  auto bval = [&](uint32_t idx) -> int {
    int v = 0;
    return (fresh && t->get_bool(idx, &v) == PGR_TRANSPORT_OK && v != 0) ? 1 : 0;
  };
  auto ival = [&](uint32_t idx) -> int32_t {
    int32_t v = 0;
    return (fresh && t->get_int(idx, &v) == PGR_TRANSPORT_OK) ? v : 0;
  };
  auto dval = [&](uint32_t idx, double fallback) -> double {
    double v = fallback;
    int has = 0;
    return (fresh && t->get_double(idx, &v, &has) == PGR_TRANSPORT_OK && has) ? v : fallback;
  };

  PgrEncounterHostOptions next_opts = g_opts;
  next_opts.berries_allowed = (active > 0 && bval(7) > 0) ? 1u : 0u;
  next_opts.auto_transfer_allowed = (active > 0 && bval(5) > 0) ? 1u : 0u;
  next_opts.item_access_allowed = (active > 0 && bval(16) > 0) ? 1u : 0u;
  next_opts.item_transfer_enabled = (active > 0 && bval(17) > 0) ? 1u : 0u;
  next_opts.map_poi_allowed = (active > 0 && bval(21) > 0) ? 1u : 0u;
  next_opts.transfer_keep_all = (bval(35) > 0) ? 1u : 0u;
  next_opts.vgp_allowed = (active > 0 && bval(36) > 0) ? 1u : 0u;
  next_opts.vgp_enabled = (bval(37) > 0) ? 1u : 0u;
  next_opts.vgp_map_visible = (bval(38) > 0) ? 1u : 0u;
  next_opts.transfer_protect = ival(8);

  int32_t vgp_mask = ival(9);
  next_opts.vgp_catch_allowed = (vgp_mask & 1) ? 1u : 0u;
  next_opts.vgp_spin_allowed = (vgp_mask & 2) ? 1u : 0u;
  next_opts.lat = dval(0, g_opts.lat);
  next_opts.lon = dval(1, g_opts.lon);

  // Parse berry priority string[2]
  char prio_buf[128] = {};
  uint32_t prio_len = 0;
  if (fresh && t->get_string(2, prio_buf, sizeof(prio_buf) - 1, &prio_len) == PGR_TRANSPORT_OK) {
    if (prio_len < sizeof(prio_buf)) prio_buf[prio_len] = '\0';
    { // Parse every coherent poll; a prior stale snapshot may have cleared options.
      snprintf(g_last_berry_prio_str, sizeof(g_last_berry_prio_str), "%s", prio_buf);
      uint32_t cnt = 0;
      int32_t ids[3] = {0, 0, 0};
      const char *p = prio_buf;
      while (*p && cnt < 3) {
        while (*p == ',' || *p == ' ' || *p == ';') ++p;
        if (!*p) break;
        const char *s_tok = p;
        while (*p && *p != ',' && *p != ';') ++p;
        int32_t id = resolve_berry_token(s_tok, static_cast<size_t>(p - s_tok));
        if (id > 0) {
          bool dup = false;
          for (uint32_t k = 0; k < cnt; ++k) {
            if (ids[k] == id) dup = true;
          }
          if (!dup) ids[cnt++] = id;
        }
      }
      next_opts.berry_priority_count = cnt;
      for (uint32_t k = 0; k < 3; ++k) next_opts.berry_priority_ids[k] = ids[k];
    }
  }

  // Keep policy is canonical CSV, matching61. Reject partial numbers, duplicates,
  // noncanonical leading zeros, overflow and truncation rather than preserving stale policy.
  char keep_buf[10000] = {};uint32_t keep_len=0, pending_count=0;
  int32_t pending_ids[2000]={};bool seen[2001]={};
  bool policy_valid=fresh && t->get_string(4,keep_buf,sizeof(keep_buf),&keep_len)==PGR_TRANSPORT_OK && keep_len<sizeof(keep_buf);
  if(policy_valid){
    size_t i=0;
    while(i<keep_len){
      if(keep_buf[i]<'1'||keep_buf[i]>'9'){policy_valid=false;break;}
      uint32_t value=0;
      while(i<keep_len && keep_buf[i]>='0' && keep_buf[i]<='9'){
        value=value*10+unsigned(keep_buf[i++]-'0');if(value>2000){policy_valid=false;break;}
      }
      if(!policy_valid || !value || seen[value] || pending_count==2000){policy_valid=false;break;}
      seen[value]=true;pending_ids[pending_count++]=int32_t(value);
      if(i<keep_len && (keep_buf[i++]!=',' || i==keep_len)){policy_valid=false;break;}
    }
  }
  if(!policy_valid)next_opts.auto_transfer_allowed=0;

  PgrRuntimeTransportSample end = {PGR_TRANSPORT_SAMPLE_VERSION, sizeof(end)};
  if (!fresh || t->sample(&end) != PGR_TRANSPORT_OK || end.snapshot_fresh == 0 ||
      end.fault != 0 || end.snapshot_seq != start.snapshot_seq ||
      end.generation != start.generation) {
    next_opts = {};
    next_opts.version = PGR_EH_API_VERSION;
    next_opts.size = sizeof(PgrEncounterHostOptions);
    next_opts.transfer_protect = 63;
    next_opts.lease_remaining_ms = 0ULL;
    g_opts = next_opts;
    g_lease_deadline_ms = 0ULL;
  } else {
    if(policy_valid){
      if(g_keep_species_count!=pending_count || memcmp(g_keep_species,pending_ids,pending_count*sizeof(int32_t))){
        memcpy(g_keep_species,pending_ids,pending_count*sizeof(int32_t));g_keep_species_count=pending_count;++g_transfer_epoch;
      }
    }
    next_opts.lease_remaining_ms = active > 0 ? 2500ULL : 0ULL;
    g_opts = next_opts;
    g_lease_deadline_ms = active > 0 ? (monotonic_ms() + 2500ULL) : 0ULL;
  }

  if (g_berries && (g_active_caps & PGR_EH_CAP_BERRIES)) {
    uint32_t cnt = g_opts.berries_allowed ? g_opts.berry_priority_count : 0u;
    uint64_t lease = (g_opts.berries_allowed && cnt > 0) ? g_opts.lease_remaining_ms : 0ULL;
    g_berries->update(cnt ? g_opts.berry_priority_ids : nullptr, cnt, lease, tid);
  }

  bool vgp_en = (g_opts.vgp_allowed && g_opts.vgp_enabled);
  uint32_t mask = (g_opts.vgp_catch_allowed ? 1u : 0u) | (g_opts.vgp_spin_allowed ? 2u : 0u);
  if (vgp_en != g_last_vgp_enabled || mask != g_last_vgp_mask) {
    if (((!vgp_en && g_last_vgp_enabled) || (g_last_vgp_mask & ~mask)) && g_vgc) {
      g_vgc->revoke();
      clear_active_vgp_transfer();
    }
    g_last_vgp_enabled = vgp_en;
    g_last_vgp_mask = mask;
    g_vgp_epoch++;
  }
}

// ============================================================================
// Leaf Installation Helpers
// ============================================================================
static int install_berries_leaf() {
  if (g_md.optional[OPT_BERRIES] != PGR_SHARED_OPT_AVAILABLE ||
      g_md.optional[OPT_ITEMS] != PGR_SHARED_OPT_AVAILABLE) {
    return PGR_EH_ERR_METADATA;
  }
  const auto *api = static_cast<const pgr_berry_api_v1 *>(
      pgrazor_encounter_berries_api(PGR_BERRY_API_VERSION));
  if (!api || !api->install) return PGR_EH_ERR_LEAF;

  if (!g_exp.meta.method_get_param || !g_exp.meta.class_from_type || !g_exp.extra.class_get_fields ||
      !g_exp.meta.field_get_offset || !g_exp.meta.field_get_flags || !g_exp.meta.field_get_type ||
      !g_exp.meta.class_is_enum || !g_exp.meta.class_value_size) return PGR_EH_ERR_METADATA;
  auto from_type=reinterpret_cast<void*(*)(void*)>(g_exp.meta.class_from_type);
  auto param=reinterpret_cast<void*(*)(void*,uint32_t)>(g_exp.meta.method_get_param);
  g_berry_response_class=from_type(param(md_h(H_BR_M_RESULT),0));
  auto fields=reinterpret_cast<void*(*)(void*,void**)>(g_exp.extra.class_get_fields);
  auto offset=reinterpret_cast<size_t(*)(void*)>(g_exp.meta.field_get_offset);
  auto flags=reinterpret_cast<uint32_t(*)(void*)>(g_exp.meta.field_get_flags);
  auto type=reinterpret_cast<void*(*)(void*)>(g_exp.meta.field_get_type);
  auto is_enum=reinterpret_cast<bool(*)(void*)>(g_exp.meta.class_is_enum);
  auto size=reinterpret_cast<int(*)(void*,int*)>(g_exp.meta.class_value_size);
  void *iter=nullptr;
  for(unsigned i=0;g_berry_response_class && i<128;++i){
    void *f=fields(g_berry_response_class,&iter);if(!f)break;
    if(offset(f)!=0x10 || (flags(f)&0x10))continue;
    void *klass=from_type(type(f));int align=0;
    if(!klass || !is_enum(klass) || size(klass,&align)!=4 || g_berry_status_field) return PGR_EH_ERR_METADATA;
    g_berry_status_field=f;
  }
  if(!g_berry_status_field) return PGR_EH_ERR_METADATA;
  void *class_ball_type = derived_return_class(md_h(H_BR_M_BALL_TYPE));
  if (!class_ball_type) class_ball_type = md_k(OC_ITEM);

  pgr_berry_config_v1 bcfg = {};
  bcfg.version = PGR_BERRY_API_VERSION;
  bcfg.size = sizeof(bcfg);
  bcfg.gchandle_new = reinterpret_cast<void *>(g_exp.gchandle_new);
  bcfg.gchandle_get_target = reinterpret_cast<void *>(g_exp.gchandle_get_target);
  bcfg.gchandle_free = reinterpret_cast<void *>(g_exp.gchandle_free);
  bcfg.object_get_class = reinterpret_cast<void *>(g_exp.object_get_class);
  bcfg.object_unbox = reinterpret_cast<void *>(g_exp.object_unbox);
  bcfg.object_new = reinterpret_cast<void *>(g_exp.object_new);
  bcfg.runtime_invoke = reinterpret_cast<void *>(g_exp.runtime_invoke);
  bcfg.field_get_value = reinterpret_cast<void *>(g_exp.field_get_value);
  bcfg.field_set_value = reinterpret_cast<void *>(g_exp.field_set_value);
  bcfg.field_set_value_object = reinterpret_cast<void *>(g_exp.field_set_value_object);

  bcfg.m_ball_type = md_h(H_BR_M_BALL_TYPE);
  bcfg.m_bread = md_h(H_BR_M_BREAD);
  bcfg.m_can = md_h(H_BR_M_CAN);
  bcfg.m_photo = md_h(H_BR_M_PHOTO);
  bcfg.m_quantity = md_h(H_IT_QUANTITY_M);
  bcfg.m_direct = md_h(H_BR_M_DIRECT);
  bcfg.m_alive = md_m(5);

  bcfg.class_pokeball = md_k(OC_POKEBALL);
  bcfg.class_scope = md_k(OC_BERRY);
  bcfg.class_bag = md_k(OC_IBAG);
  bcfg.class_bool = md_c(6);
  bcfg.class_int = md_k(OC_INT);
  bcfg.class_callback = md_h(H_BR_CALLBACK);

  bcfg.f_scope_bag = md_h(H_BR_F_BAG);
  bcfg.f_scope_ui = md_h(H_BR_F_UI);
  bcfg.f_ui_state = md_f(1);
  bcfg.f_callback_scope = md_h(H_BR_F_CB_SCOPE);
  bcfg.f_callback_berry = md_h(H_BR_F_CB_BERRY);

  bcfg.ball_type_master = 4;
  bcfg.state_open = 2;
  bcfg.reserved0 = 0;
  bcfg.reserved1 = 0;
  bcfg.class_ball_type = class_ball_type;

  int rc = api->install(&bcfg);
  if (rc != PGR_BERRY_OK) return PGR_EH_ERR_LEAF;
  g_berries = api;
  //18 only latches its owner on tick; latch before publishing encounter observers.
  if(g_berries->tick(g_owner_tid,0)!=PGR_BERRY_OK)return PGR_EH_ERR_LEAF;
  return PGR_EH_OK;
}

static int install_map_poi_leaf(bool *out_has_extra) {
  if (out_has_extra) *out_has_extra = false;
  if (g_md.optional[OPT_MAP_POI] != PGR_SHARED_OPT_AVAILABLE ||
      g_md.optional[OPT_VGP] != PGR_SHARED_OPT_AVAILABLE) {
    return PGR_EH_ERR_METADATA;
  }
  const auto *api = static_cast<const PgrMapPoiApi *>(
      pgrazor_map_poi_api(PGR_MP_API_VERSION));
  if (!api || !api->install) return PGR_EH_ERR_LEAF;

  PgrMapPoiConfig mcfg = {};
  mcfg.version = PGR_MP_CONFIG_VERSION;
  mcfg.size = sizeof(PgrMapPoiConfig);
  mcfg.runtime_invoke = reinterpret_cast<void *>(g_exp.runtime_invoke);
  mcfg.object_get_class = reinterpret_cast<void *>(g_exp.object_get_class);
  mcfg.field_get_value = reinterpret_cast<void *>(g_exp.field_get_value);
  mcfg.object_unbox = reinterpret_cast<void *>(g_exp.object_unbox);
  mcfg.gchandle_new = reinterpret_cast<void *>(g_exp.gchandle_new);
  mcfg.gchandle_get_target = reinterpret_cast<void *>(g_exp.gchandle_get_target);
  mcfg.gchandle_free = reinterpret_cast<void *>(g_exp.gchandle_free);
  mcfg.thread_current = reinterpret_cast<void *>(g_exp.thread_current);
  mcfg.class_get_type = reinterpret_cast<void *>(g_exp.meta.class_get_type);
  mcfg.type_get_object = g_exp.extra.type_get_object;
  mcfg.array_length = g_exp.extra.array_length;
  mcfg.object_get_virtual_method = reinterpret_cast<void *>(g_exp.object_get_virtual_method);
  mcfg.string_length = reinterpret_cast<void *>(g_exp.string_length);
  mcfg.string_chars = reinterpret_cast<void *>(g_exp.string_chars);

  mcfg.class_stop = md_k(OC_VGP_STOP);
  mcfg.class_gym = md_h(H_MP_GYM) ? md_h(H_MP_GYM) : md_k(OC_MAP_GYM);
  mcfg.class_fort = md_k(OC_VGP_FORT);
  mcfg.class_raid = md_h(H_MP_RAID) ? md_h(H_MP_RAID) : md_k(OC_RAID_INFO);
  mcfg.class_bool = md_c(6);
  mcfg.class_string = md_k(OC_STRING);

  mcfg.m_find = md_h(H_VGP_M_FIND_ALL);
  mcfg.m_alive = md_m(5);
  mcfg.m_fort = md_h(H_VGP_M_FORT);

  mcfg.f_fortId = md_h(H_VGP_F_FORT_ID);
  mcfg.f_lat = md_h(H_VGP_F_FORT_LAT);
  mcfg.f_lon = md_h(H_VGP_F_FORT_LON);
  mcfg.f_raidInfo = md_h(H_MP_INFO);
  mcfg.f_spawn = md_h(H_MP_SPAWN);
  mcfg.f_end = md_h(H_MP_END);
  mcfg.f_battle = md_h(H_MP_BATTLE);
  mcfg.f_hidden = md_h(H_MP_HIDDEN);

  bool has_extra = false;
  if (g_md.optional[OPT_MAP_POI_EXTRA] == PGR_SHARED_OPT_AVAILABLE) {
    void *m_team = md_h(H_MPE_TEAM_M);
    void *c_team = derived_return_class(m_team);
    if (m_team && c_team) {
      mcfg.m_team = m_team;
      mcfg.class_teamBox = c_team;
      has_extra = true;
    }
    void *f_gd = md_h(H_MPE_GYM_DISP);
    void *c_gd = md_h(H_MPE_GYM_DISP_CLASS) ? md_h(H_MPE_GYM_DISP_CLASS) : md_k(OC_GYM_DISPLAY);
    void *f_slots = md_h(H_MPE_SLOTS);
    if (f_gd && c_gd && f_slots) {
      mcfg.f_gymDisplay = f_gd;
      mcfg.class_gymDisplay = c_gd;
      mcfg.f_slots = f_slots;
      has_extra = true;
    }
    if (md_h(H_MPE_LEVEL)) {
      mcfg.f_level = md_h(H_MPE_LEVEL);
      has_extra = true;
    }
    void *f_boss = md_h(H_MPE_BOSS);
    void *c_boss = md_h(H_MPE_BOSS_CLASS);
    void *f_boss_id = md_h(H_MPE_BOSS_ID);
    if (f_boss && c_boss && f_boss_id) {
      mcfg.f_boss = f_boss;
      mcfg.class_boss = c_boss;
      mcfg.f_bossId = f_boss_id;
      has_extra = true;
    }
  }

  int rc = api->install(&mcfg);
  if (rc != 0) return PGR_EH_ERR_LEAF;

  if (!g_map_poi_out) {
    g_map_poi_out = static_cast<PgrMapPoiOutput *>(calloc(1, sizeof(PgrMapPoiOutput)));
    if (!g_map_poi_out) return PGR_EH_ERR_LEAF;
  }
  g_map_poi = api;
  if (out_has_extra) *out_has_extra = has_extra;
  return PGR_EH_OK;
}

static int install_transfer_leaves() {
  auto broker = static_cast<const TransferBroker*>(pgrazor_shared_host_api(2));
  if (!broker || broker->version!=2 || broker->size!=sizeof(TransferBroker) ||
      !broker->ready || !broker->submit || !broker->next_event || broker->ready()!=1)
    return PGR_EH_RETRY;
  g_tp=static_cast<const PgrTransferApi*>(pgrazor_transfer_policy_api(1));
  g_tf=static_cast<const PgrTransferFactsApi*>(pgrazor_transfer_facts_api(PGR_TF_API_VERSION));
  if(!g_tp || !g_tf) return PGR_EH_ERR_LEAF;
  g_broker=broker;
  return PGR_EH_OK;
}

static int install_vgp_leaves(bool *out_has_marker) {
  const auto *items=static_cast<const pgrazor_items_api_v1*>(pgrazor_items_api(PGRAZOR_ITEMS_API_VERSION));
  if(!items || items->version!=PGRAZOR_ITEMS_API_VERSION || items->size!=sizeof(*items))return PGR_EH_ERR_LEAF;
  kProxyItemsApi=*items;kProxyItemsApi.service_handle=&proxy_items_service_handle;kProxyItemsApi.service_acquire=&proxy_items_service_acquire;
  if (out_has_marker) *out_has_marker = false;
  if (g_md.optional[OPT_VGP] != PGR_SHARED_OPT_AVAILABLE ||
      g_md.optional[OPT_ITEMS] != PGR_SHARED_OPT_AVAILABLE ||
      g_md.optional[OPT_ITEMS_LOOKUP] != PGR_SHARED_OPT_AVAILABLE) {
    return PGR_EH_ERR_METADATA;
  }

  const auto *vp = static_cast<const PgrVgpPolicyApi *>(
      pgrazor_vgp_policy_api(PGR_VP_API_VERSION));
  const auto *va = static_cast<const PgrVgpActionsApi *>(
      pgrazor_vgp_actions_api(PGR_VA_API_VERSION));
  const auto *vc = static_cast<const PgrVgpCandidateApi *>(
      pgrazor_vgp_candidates_api(PGR_VC_API_VERSION));
  const auto *vr = static_cast<const PgrVgpResultsApi *>(
      pgrazor_vgp_results_api(PGR_VR_API_VERSION));
  const auto *vd = static_cast<const PgrVgpDiscoveryApi *>(
      pgrazor_vgp_discovery_api(PGR_VD_API_VERSION));
  const auto *vm = static_cast<const PgrVgpMaintenanceApi *>(
      pgrazor_vgp_maintenance_api(PGR_VM_API_VERSION));
  const auto *vx = static_cast<const PgrVgpAuxApi *>(
      pgrazor_vgp_aux_api(PGR_VX_API_VERSION));
  const auto *vgc = static_cast<const PgrVgpControllerApi *>(
      pgrazor_vgp_controller_api(PGR_VGC_API_VERSION));
  if (!vp || !va || !vc || !vr || !vd || !vm || !vx || !vgc) return PGR_EH_ERR_LEAF;

  // 1. Feature 25: VGP Policy
  PgrVgpPolicyConfig vp_cfg = {};
  vp_cfg.version = PGR_VP_CONFIG_VERSION;
  vp_cfg.size = sizeof(vp_cfg);
  vp_cfg.scan_interval_ms = 2500;
  vp_cfg.catch_seen_ms = 30000;
  vp_cfg.spin_seen_ms = 300000;
  vp_cfg.reject_delay_ms = 30000;
  vp_cfg.terminal_delay_ms = 3600000;
  vp_cfg.retry_delay_ms = 5000;
  vp_cfg.default_delay_ms = 30000;
  vp_cfg.max_keys = 2048;
  vp_cfg.status_terminal[0] = g_md.opt_ints[I_VGP_CS_SUCCESS];
  vp_cfg.status_terminal[1] = g_md.opt_ints[I_VGP_CS_FLEE];
  vp_cfg.status_terminal[2] = -1;
  vp_cfg.status_terminal[3] = -1;
  vp_cfg.status_retry[0] = g_md.opt_ints[I_VGP_CS_ESCAPE];
  pgr_vgp_smart_escape_status(g_md.opt_ints[I_VGP_CS_ESCAPE]);
  vp_cfg.status_retry[1] = g_md.opt_ints[I_VGP_CS_MISSED];
  vp_cfg.status_retry[2] = g_md.opt_ints[I_VGP_CS_ITEM_REPL];
  vp_cfg.status_retry[3] = -1;
  if (vp->install(&vp_cfg) != 0) return PGR_EH_ERR_LEAF;

  // 2. Feature 26: VGP Actions
  PgrVgpActionsConfig va_cfg = {};
  va_cfg.version = PGR_VA_CONFIG_VERSION;
  va_cfg.size = sizeof(va_cfg);
  va_cfg.gchandle_new = reinterpret_cast<void *>(g_exp.gchandle_new);
  va_cfg.gchandle_get_target = reinterpret_cast<void *>(g_exp.gchandle_get_target);
  va_cfg.gchandle_free = reinterpret_cast<void *>(g_exp.gchandle_free);
  va_cfg.object_get_class = reinterpret_cast<void *>(g_exp.object_get_class);
  va_cfg.object_new = reinterpret_cast<void *>(g_exp.object_new);
  va_cfg.runtime_invoke = reinterpret_cast<void *>(g_exp.runtime_invoke);
  va_cfg.field_get_value = reinterpret_cast<void *>(g_exp.field_get_value);
  va_cfg.field_set_value = reinterpret_cast<void *>(g_exp.field_set_value);
  va_cfg.field_set_value_obj = reinterpret_cast<void *>(g_exp.field_set_value_object);
  va_cfg.m_encounter = md_h(H_VGP_M_ENCOUNTER);
  va_cfg.m_capture = md_h(H_VGP_M_CAPTURE);
  va_cfg.m_ctor = md_h(H_VGP_M_CTOR);
  va_cfg.class_wild = md_k(OC_VGP_WILD);
  va_cfg.class_fort = md_k(OC_VGP_FORT);
  va_cfg.class_rpc = md_k(OC_VGP_RPC);
  va_cfg.class_request = md_k(OC_VGP_REQUEST);
  va_cfg.f_fort_id = md_h(H_VGP_F_FORT_ID);
  va_cfg.f_fort_lat = md_h(H_VGP_F_FORT_LAT);
  va_cfg.f_fort_lon = md_h(H_VGP_F_FORT_LON);
  va_cfg.f_req_id = md_h(H_VGP_F_ID);
  va_cfg.f_req_lat = md_h(H_VGP_F_LAT);
  va_cfg.f_req_lon = md_h(H_VGP_F_LON);
  va_cfg.f_req_target_lat = md_h(H_VGP_F_TARGET_LAT);
  va_cfg.f_req_target_lon = md_h(H_VGP_F_TARGET_LON);
  va_cfg.class_stop = md_k(OC_VGP_STOP);
  if (va->install(&va_cfg) != 0) return PGR_EH_ERR_LEAF;

  // 3. Feature 29: VGP Candidates
  void *class_latlng = derived_return_class(md_h(H_VGP_M_LOCATION));
  void *class_uint64 = derived_return_class(md_h(H_VGP_M_ID));
  if (!class_uint64) class_uint64 = const_cast<void *>(g_md.field_type_classes[0]);

  PgrVgpCandidateConfig vc_cfg = {};
  vc_cfg.version = PGR_VC_CONFIG_VERSION;
  vc_cfg.size = sizeof(vc_cfg);
  vc_cfg.gchandle_new = reinterpret_cast<void *>(g_exp.gchandle_new);
  vc_cfg.gchandle_get_target = reinterpret_cast<void *>(g_exp.gchandle_get_target);
  vc_cfg.gchandle_free = reinterpret_cast<void *>(g_exp.gchandle_free);
  vc_cfg.object_get_class = reinterpret_cast<void *>(g_exp.object_get_class);
  vc_cfg.runtime_invoke = reinterpret_cast<void *>(g_exp.runtime_invoke);
  vc_cfg.object_unbox = reinterpret_cast<void *>(g_exp.object_unbox);
  vc_cfg.object_get_virtual_method = reinterpret_cast<void *>(g_exp.object_get_virtual_method);
  vc_cfg.field_get_value = reinterpret_cast<void *>(g_exp.field_get_value);
  vc_cfg.m_alive = md_m(5);
  vc_cfg.m_location = md_h(H_VGP_M_LOCATION);
  vc_cfg.m_id = md_h(H_VGP_M_ID);
  vc_cfg.m_visible = md_h(H_VGP_M_VISIBLE);
  vc_cfg.m_active = md_h(H_VGP_M_ACTIVE);
  vc_cfg.m_range = md_h(H_VGP_M_RANGE);
  vc_cfg.m_closed = md_h(H_VGP_M_CLOSED);
  vc_cfg.m_cooling = md_h(H_VGP_M_COOLING);
  vc_cfg.m_fort = md_h(H_VGP_M_FORT);
  vc_cfg.f_fort_id = md_h(H_VGP_F_FORT_ID);
  vc_cfg.class_wild = md_k(OC_VGP_WILD);
  vc_cfg.class_stop = md_k(OC_VGP_STOP);
  vc_cfg.class_fort = md_k(OC_VGP_FORT);
  vc_cfg.class_bool = md_c(6);
  vc_cfg.class_latlng = class_latlng;
  vc_cfg.class_uint64 = class_uint64;
  if (vc->install(&vc_cfg) != 0) return PGR_EH_ERR_LEAF;

  // 4. Feature 30: VGP Results
  PgrVgpResultsConfig vr_cfg = {};
  vr_cfg.version = PGR_VR_CONFIG_VERSION;
  vr_cfg.size = sizeof(vr_cfg);
  vr_cfg.gchandle_new = reinterpret_cast<void *>(g_exp.gchandle_new);
  vr_cfg.gchandle_get_target = reinterpret_cast<void *>(g_exp.gchandle_get_target);
  vr_cfg.gchandle_free = reinterpret_cast<void *>(g_exp.gchandle_free);
  vr_cfg.object_get_class = reinterpret_cast<void *>(g_exp.object_get_class);
  vr_cfg.class_get_parent = g_exp.extra.class_get_parent ? g_exp.extra.class_get_parent
                                                         : md_h(H_VGP_EXP_PARENT);
  vr_cfg.class_get_field_from_name = reinterpret_cast<void *>(g_exp.meta.class_get_field_from_name);
  vr_cfg.field_get_offset = reinterpret_cast<void *>(g_exp.meta.field_get_offset);
  vr_cfg.field_get_type = reinterpret_cast<void *>(g_exp.meta.field_get_type);
  vr_cfg.class_from_type = reinterpret_cast<void *>(g_exp.meta.class_from_type);
  vr_cfg.class_is_valuetype = reinterpret_cast<void *>(g_exp.meta.class_is_valuetype);
  vr_cfg.field_get_value = reinterpret_cast<void *>(g_exp.field_get_value);
  vr_cfg.class_bool = md_c(6);
  vr_cfg.class_resp = md_c(3);
  vr_cfg.class_spinresult = md_k(OC_SPINRESULT);
  vr_cfg.name_complete = const_cast<char *>("completeCalled");
  vr_cfg.name_error = const_cast<char *>("errorCalled");
  vr_cfg.name_value = const_cast<char *>("completedValue");
  vr_cfg.name_status = const_cast<char *>("statusErrorCalled");
  vr_cfg.f_catch_status = md_h(H_VGP_F_CATCH_STATUS);
  vr_cfg.f_spin_result = md_h(H_VGP_F_RESULT);
  vr_cfg.catch_success = g_md.success;
  vr_cfg.spin_success = g_md.opt_ints[I_VGP_SPIN_SUCCESS];
  if (vr->install(&vr_cfg) != 0) return PGR_EH_ERR_LEAF;

  // 5. Feature 31: VGP Discovery
  void *class_item_service = derived_field_parent(md_h(H_IT_BAG));
  if (!class_item_service) class_item_service = md_k(OC_ISERVICE);
  void *class_obj_array = derived_return_class(md_h(H_VGP_M_FIND_ALL));
  void *class_container = derived_return_class(md_h(H_IL_CONTAINER_M));
  if (!class_container) class_container = md_k(OC_DICONTAINER);

  PgrVgpDiscoveryConfig vd_cfg = {};
  vd_cfg.version = PGR_VD_CONFIG_VERSION;
  vd_cfg.size = sizeof(vd_cfg);
  vd_cfg.gchandle_new = reinterpret_cast<void *>(g_exp.gchandle_new);
  vd_cfg.gchandle_get_target = reinterpret_cast<void *>(g_exp.gchandle_get_target);
  vd_cfg.gchandle_free = reinterpret_cast<void *>(g_exp.gchandle_free);
  vd_cfg.object_get_class = reinterpret_cast<void *>(g_exp.object_get_class);
  vd_cfg.runtime_invoke = reinterpret_cast<void *>(g_exp.runtime_invoke);
  vd_cfg.object_unbox = reinterpret_cast<void *>(g_exp.object_unbox);
  vd_cfg.field_get_value = reinterpret_cast<void *>(g_exp.field_get_value);
  vd_cfg.class_get_type = reinterpret_cast<void *>(g_exp.meta.class_get_type);
  vd_cfg.type_get_object = g_exp.extra.type_get_object;
  vd_cfg.array_length = g_exp.extra.array_length;
  vd_cfg.m_find = md_h(H_IL_FIND_M);
  vd_cfg.m_container = md_h(H_IL_CONTAINER_M);
  vd_cfg.m_resolve = md_h(H_IL_RESOLVE_M);
  vd_cfg.m_item_count = md_h(H_VGP_M_ITEM_COUNT);
  vd_cfg.m_pokemon_count = md_h(H_VGP_M_PK_COUNT);
  vd_cfg.m_max_pokemon = md_h(H_VGP_M_MAX_PK);
  vd_cfg.m_total_items = md_h(H_VGP_M_TOTAL_ITEMS);
  vd_cfg.m_max_items = md_h(H_VGP_M_MAX_ITEMS);
  vd_cfg.m_find_all = md_h(H_VGP_M_FIND_ALL);
  vd_cfg.class_map = md_h(H_IL_MAP);
  vd_cfg.class_player_contract = md_k(OC_VGP_PLAYER_CONTRACT);
  vd_cfg.class_bag_contract = md_k(OC_VGP_BAG_CONTRACT);
  vd_cfg.class_player = md_k(OC_VGP_PLAYER);
  vd_cfg.class_pokemon_bag = md_k(OC_BAG);
  vd_cfg.class_item_bag = md_k(OC_IBAG);
  vd_cfg.class_item_service = class_item_service;
  vd_cfg.class_int32 = md_k(OC_INT);
  vd_cfg.class_wild = md_k(OC_VGP_WILD);
  vd_cfg.class_stop = md_k(OC_VGP_STOP);
  vd_cfg.class_object_array = class_obj_array;
  vd_cfg.class_container = class_container;
  vd_cfg.f_item_bag = md_h(H_IT_BAG);
  vd_cfg.class_is_assignable_from = reinterpret_cast<void *>(g_exp.meta.class_is_assignable_from);
  if (vd->install(&vd_cfg) != 0) return PGR_EH_ERR_LEAF;

  // 6. Feature 32: VGP Maintenance
  PgrVgpMaintenanceConfig vm_cfg = {};
  vm_cfg.version = PGR_VM_CONFIG_VERSION;
  vm_cfg.size = sizeof(vm_cfg);
  vm_cfg.gchandle_new = reinterpret_cast<void *>(g_exp.gchandle_new);
  vm_cfg.gchandle_get_target = reinterpret_cast<void *>(g_exp.gchandle_get_target);
  vm_cfg.gchandle_free = reinterpret_cast<void *>(g_exp.gchandle_free);
  vm_cfg.object_get_class = reinterpret_cast<void *>(g_exp.object_get_class);
  vm_cfg.runtime_invoke = reinterpret_cast<void *>(g_exp.runtime_invoke);
  vm_cfg.object_unbox = reinterpret_cast<void *>(g_exp.object_unbox);
  vm_cfg.object_get_virtual_method = reinterpret_cast<void *>(g_exp.object_get_virtual_method);
  vm_cfg.field_get_value = reinterpret_cast<void *>(g_exp.field_get_value);
  vm_cfg.m_item_count = md_h(H_VGP_M_ITEM_COUNT);
  vm_cfg.m_alive = md_m(5);
  vm_cfg.m_cooldown = md_h(H_VGP_M_COOLDOWN);
  vm_cfg.f_item_bag = md_h(H_IT_BAG);
  vm_cfg.f_until = md_h(H_VGP_F_UNTIL);
  vm_cfg.f_result = md_h(H_VGP_F_RESULT);
  vm_cfg.class_service = class_item_service;
  vm_cfg.class_item_bag = md_k(OC_IBAG);
  vm_cfg.class_int32 = md_k(OC_INT);
  vm_cfg.class_bool = md_c(6);
  vm_cfg.class_stop = md_k(OC_VGP_STOP);
  vm_cfg.class_spin_result = md_k(OC_SPINRESULT);
  vm_cfg.spin_success = g_md.opt_ints[I_VGP_SPIN_SUCCESS];
  if (vm->install(&vm_cfg) != 0) return PGR_EH_ERR_LEAF;

  // 7. Feature 34: VGP Aux
  void *f_rpc = resolve_rpc_field(md_k(OC_IBAG), md_k(OC_VGP_RPC));
  PgrVgpAuxConfig vx_cfg = {};
  vx_cfg.version = PGR_VX_CONFIG_VERSION;
  vx_cfg.size = sizeof(vx_cfg);
  vx_cfg.gc_new = reinterpret_cast<void *>(g_exp.gchandle_new);
  vx_cfg.gc_get = reinterpret_cast<void *>(g_exp.gchandle_get_target);
  vx_cfg.gc_free = reinterpret_cast<void *>(g_exp.gchandle_free);
  vx_cfg.object_get_class = reinterpret_cast<void *>(g_exp.object_get_class);
  vx_cfg.runtime_invoke = reinterpret_cast<void *>(g_exp.runtime_invoke);
  vx_cfg.field_get_value = reinterpret_cast<void *>(g_exp.field_get_value);
  vx_cfg.class_get_method_from_name = reinterpret_cast<void *>(g_exp.meta.class_get_method_from_name);
  vx_cfg.method_is_generic = reinterpret_cast<void *>(g_exp.meta.method_is_generic);
  vx_cfg.method_get_param_count = reinterpret_cast<void *>(g_exp.meta.method_get_param_count);
  vx_cfg.method_get_object = g_exp.extra.method_get_object;
  vx_cfg.array_new = g_exp.extra.array_new ? g_exp.extra.array_new : md_h(H_VGP_EXP_ARRAY);
  vx_cfg.class_get_type = reinterpret_cast<void *>(g_exp.meta.class_get_type);
  vx_cfg.type_get_object = g_exp.extra.type_get_object;
  vx_cfg.gc_wbarrier_set_field = g_exp.extra.gc_wbarrier_set_field
                                     ? g_exp.extra.gc_wbarrier_set_field
                                     : md_h(H_VGP_EXP_BARRIER);
  vx_cfg.method_get_from_reflection = g_exp.extra.method_get_from_reflection
                                          ? g_exp.extra.method_get_from_reflection
                                          : md_h(H_VGP_EXP_UNREFLECT);
  vx_cfg.method_is_inflated = reinterpret_cast<void *>(g_exp.meta.method_is_inflated);
  vx_cfg.class_rpc = md_k(OC_VGP_RPC);
  vx_cfg.class_request = md_k(OC_VGP_REQUEST);
  vx_cfg.class_spin = md_k(OC_SPINRESULT);
  vx_cfg.class_type = md_k(OC_VGP_TYPE);
  vx_cfg.class_item_bag = md_k(OC_IBAG);
  vx_cfg.class_pokemon = md_k(OC_PK);
  vx_cfg.class_response = md_c(3);
  vx_cfg.f_rpc = f_rpc;
  vx_cfg.f_caught_id = md_f(0);
  vx_cfg.f_pokemon_id = md_h(H_DET_ID);
  vx_cfg.f_bag_gm = md_h(H_VGP_F_BAG_GM);
  vx_cfg.m_get_pokemon = md_h(H_TR_GET_M);
  if (vx->install(&vx_cfg) != 0) return PGR_EH_ERR_LEAF;

  // 8. Optional Feature 28: VGP Marker
  const PgrVgpMarkerApi *vmk = static_cast<const PgrVgpMarkerApi*>(pgrazor_vgp_marker_api(1));
  bool has_marker = false;
  if (g_md.optional[OPT_VGP_MARKER] == PGR_SHARED_OPT_AVAILABLE) {
    const auto *mk = static_cast<const PgrVgpMarkerApi *>(
        pgrazor_vgp_marker_api(1));
    if (mk && mk->install) {
      PgrVgpMarkerConfig mk_cfg = {};
      mk_cfg.version = 1;
      mk_cfg.size = sizeof(mk_cfg);
      mk_cfg.gc_new = reinterpret_cast<void *>(g_exp.gchandle_new);
      mk_cfg.gc_get = reinterpret_cast<void *>(g_exp.gchandle_get_target);
      mk_cfg.gc_free = reinterpret_cast<void *>(g_exp.gchandle_free);
      mk_cfg.object_class = reinterpret_cast<void *>(g_exp.object_get_class);
      mk_cfg.invoke = reinterpret_cast<void *>(g_exp.runtime_invoke);
      mk_cfg.unbox = reinterpret_cast<void *>(g_exp.object_unbox);
      mk_cfg.assignable = reinterpret_cast<void *>(g_exp.meta.class_is_assignable_from);
      mk_cfg.alive = md_m(5);
      mk_cfg.camera = md_h(H_VGPM_CAMERA);
      mk_cfg.transform = md_h(H_VGPM_TRANSFORM);
      mk_cfg.position = md_h(H_VGPM_POSITION);
      mk_cfg.project = md_h(H_VGPM_PROJECT);
      mk_cfg.width = md_h(H_VGPM_WIDTH);
      mk_cfg.height = md_h(H_VGPM_HEIGHT);
      mk_cfg.bool_class = md_c(6);
      mk_cfg.int_class = md_k(OC_INT);
      mk_cfg.vector_class = md_k(OC_VECTOR3);
      mk_cfg.camera_class = md_k(OC_VGP_CAMERA);
      mk_cfg.transform_class = md_k(OC_TRANSFORM);
      mk_cfg.wild_class = md_k(OC_VGP_WILD);
      mk_cfg.stop_class = md_k(OC_VGP_STOP);
      if (mk->install(&mk_cfg) == 0) {
        vmk = mk;
        has_marker = true;
      }
    }
  }

  // 9. Feature 33: VGP Controller
  PgrVgpControllerConfig vgc_cfg = {};
  vgc_cfg.version = PGR_VGC_CONFIG_VERSION;
  vgc_cfg.size = sizeof(vgc_cfg);
  vgc_cfg.policy_table = const_cast<PgrVgpPolicyApi *>(vp);
  vgc_cfg.actions_table = const_cast<PgrVgpActionsApi *>(va);
  vgc_cfg.candidates_table = const_cast<PgrVgpCandidateApi *>(vc);
  vgc_cfg.results_table = const_cast<PgrVgpResultsApi *>(vr);
  vgc_cfg.discovery_table = const_cast<PgrVgpDiscoveryApi *>(vd);
  vgc_cfg.maintenance_table = const_cast<PgrVgpMaintenanceApi *>(vm);
  vgc_cfg.marker_table = const_cast<PgrVgpMarkerApi *>(vmk);
  vgc_cfg.items_table = const_cast<pgrazor_items_api_v1 *>(&kProxyItemsApi);
  vgc_cfg.aux_table = const_cast<PgrVgpAuxApi *>(vx);
  vgc_cfg.gc_new = reinterpret_cast<void *>(g_exp.gchandle_new);
  vgc_cfg.gc_get = reinterpret_cast<void *>(g_exp.gchandle_get_target);
  vgc_cfg.gc_free = reinterpret_cast<void *>(g_exp.gchandle_free);
  vgc_cfg.object_class = reinterpret_cast<void *>(g_exp.object_get_class);
  vgc_cfg.runtime_invoke = reinterpret_cast<void *>(g_exp.runtime_invoke);
  vgc_cfg.object_unbox = reinterpret_cast<void *>(g_exp.object_unbox);
  vgc_cfg.m_alive = md_m(5);
  vgc_cfg.class_wild = md_k(OC_VGP_WILD);
  vgc_cfg.class_nameplate = md_k(OC_VGP_NAMEPLATE);
  vgc_cfg.class_bool = md_c(6);
  vgc_cfg.rpc_id = g_md.opt_ints[I_VGP_RPC_ID];
  if (vgc->install(&vgc_cfg) != 0) return PGR_EH_ERR_LEAF;

  g_vp = vp;
  g_va = va;
  g_vc = vc;
  g_vr = vr;
  g_vd = vd;
  g_vm = vm;
  g_vx = vx;
  g_vmk = vmk;
  g_vgc = vgc;
  if (out_has_marker) *out_has_marker = has_marker;
  return PGR_EH_OK;
}

// ============================================================================
// Hook Installation & Rollback
// ============================================================================
static void rollback_hooks() {
  // A published trampoline must remain callable for frames already in flight.
  g_hook_poisoned = true;
  g_stopped = true;
  g_stop_requested.store(true, std::memory_order_release);
  g_active_caps = 0;
}

static int install_one_hook(void *target, void *replacement, void **orig_out) {
  if (!target || !replacement || !orig_out || g_hook_poisoned || g_hooked_count >= 7) return PGR_EH_ERR_HOOK;
  for (uint32_t i = 0; i < g_hooked_count; ++i) {
    if (g_hooked_addrs[i] == target) return *orig_out ? PGR_EH_OK : PGR_EH_ERR_HOOK;
  }
  const int rc = pgrazor_hook(target, replacement, orig_out);
  if (*orig_out) g_hooked_addrs[g_hooked_count++] = target;
  if (rc != 0 || !*orig_out) { rollback_hooks(); return PGR_EH_ERR_HOOK; }
  return PGR_EH_OK;
}

// ============================================================================
// API Implementation
// ============================================================================
static int api_configure(const PgrEncounterHostConfig *config) {
  StateGuard state; if (!state) return PGR_EH_RETRY;
  if (g_hook_poisoned) return PGR_EH_ERR_HOOK;
  if (g_installed) {

    return PGR_EH_OK;
  }

  ResolvedExports exp = {};
  PgrSharedMetadataOutput md = {};
  PgrEncounterHostConfig local_cfg = {};
  bool have_md = false;

  if (config != nullptr) {
    if (config->version != PGR_EH_API_VERSION ||
        config->size != sizeof(PgrEncounterHostConfig)) {

      return PGR_EH_ERR_BAD_CONFIG;
    }
    local_cfg = *config;
    if (config->metadata_config.binding.exports) {
      exp.meta = *config->metadata_config.binding.exports;
    }
    if (config->extra_exports) {
      exp.extra = *config->extra_exports;
    } else if (config->metadata_config.extra) {
      exp.extra = *config->metadata_config.extra;
    }
    exp.thread_current = reinterpret_cast<void *(*)(void)>(config->metadata_config.thread_current);
    exp.runtime_invoke = reinterpret_cast<decltype(exp.runtime_invoke)>(config->runtime_invoke);
    exp.object_get_class = reinterpret_cast<decltype(exp.object_get_class)>(config->object_get_class);
    exp.object_unbox = reinterpret_cast<decltype(exp.object_unbox)>(config->object_unbox);
    exp.object_new = reinterpret_cast<decltype(exp.object_new)>(config->object_new);
    exp.field_get_value = reinterpret_cast<decltype(exp.field_get_value)>(config->field_get_value);
    exp.field_set_value = reinterpret_cast<decltype(exp.field_set_value)>(config->field_set_value);
    exp.field_set_value_object = reinterpret_cast<decltype(exp.field_set_value_object)>(config->field_set_value_object);
    exp.string_length = reinterpret_cast<decltype(exp.string_length)>(config->string_length);
    exp.string_chars = reinterpret_cast<decltype(exp.string_chars)>(config->string_chars);
    exp.object_get_virtual_method = reinterpret_cast<decltype(exp.object_get_virtual_method)>(config->object_get_virtual_method);
    exp.gchandle_new = reinterpret_cast<decltype(exp.gchandle_new)>(exp.extra.gchandle_new);
    exp.gchandle_get_target = reinterpret_cast<decltype(exp.gchandle_get_target)>(exp.extra.gchandle_get_target);
    exp.gchandle_free = reinterpret_cast<decltype(exp.gchandle_free)>(exp.extra.gchandle_free);

    if (config->resolved_metadata) {
      if (config->resolved_metadata->version != 2u ||
          config->resolved_metadata->size != sizeof(PgrSharedMetadataOutput)) {

        return PGR_EH_ERR_METADATA;
      }
      md = *config->resolved_metadata;
      have_md = true;
    }
  } else {
    local_cfg.version = PGR_EH_API_VERSION;
    local_cfg.size = sizeof(PgrEncounterHostConfig);
  }

  // Fill any missing runtime/meta exports from Feature 54 (runtime_bootstrap) if available.
  const auto *boot = static_cast<const PgrBootApi *>(
      pgrazor_runtime_bootstrap_api(PGR_BOOT_API_VERSION));
  PgrBootBinding b = {};
  b.version = PGR_BOOT_BINDING_VERSION;
  b.size = sizeof(PgrBootBinding);
  if (boot && boot->snapshot && boot->snapshot(&b) == PGR_BOOT_OK && b.status == PGR_BOOT_OK) {
    if (!exp.meta.domain_get) exp.meta = b.meta;
    if (!exp.thread_current) {
      exp.thread_current = reinterpret_cast<decltype(exp.thread_current)>(b.runtime.thread_current);
    }
    if (!exp.runtime_invoke) {
      exp.runtime_invoke = reinterpret_cast<decltype(exp.runtime_invoke)>(b.runtime.runtime_invoke);
    }
    if (!exp.object_get_class) {
      exp.object_get_class = reinterpret_cast<decltype(exp.object_get_class)>(b.runtime.object_get_class);
    }
    if (!exp.object_unbox) {
      exp.object_unbox = reinterpret_cast<decltype(exp.object_unbox)>(b.runtime.object_unbox);
    }
    if (!exp.object_new) {
      exp.object_new = reinterpret_cast<decltype(exp.object_new)>(b.runtime.object_new);
    }
    if (!exp.field_get_value) {
      exp.field_get_value = reinterpret_cast<decltype(exp.field_get_value)>(b.runtime.field_get_value);
    }
    if (!exp.field_set_value) {
      exp.field_set_value = reinterpret_cast<decltype(exp.field_set_value)>(b.runtime.field_set_value);
    }
    if (!exp.string_chars) {
      exp.string_chars = reinterpret_cast<decltype(exp.string_chars)>(b.runtime.string_chars);
    }
    if (!exp.gchandle_new) {
      exp.gchandle_new = reinterpret_cast<decltype(exp.gchandle_new)>(b.runtime.gchandle_new);
      exp.extra.gchandle_new = reinterpret_cast<void *>(b.runtime.gchandle_new);
    }
    if (!exp.gchandle_get_target) {
      exp.gchandle_get_target = reinterpret_cast<decltype(exp.gchandle_get_target)>(b.runtime.gchandle_get_target);
      exp.extra.gchandle_get_target = reinterpret_cast<void *>(b.runtime.gchandle_get_target);
    }
    if (!exp.gchandle_free) {
      exp.gchandle_free = reinterpret_cast<decltype(exp.gchandle_free)>(b.runtime.gchandle_free);
      exp.extra.gchandle_free = reinterpret_cast<void *>(b.runtime.gchandle_free);
    }
    if (!exp.extra.field_static_get_value) {
      exp.extra.field_static_get_value = reinterpret_cast<void *>(b.runtime.field_static_get_value);
    }
    if (!exp.extra.array_new) {
      exp.extra.array_new = reinterpret_cast<void *>(b.runtime.array_new);
    }
    if (b.handle) {
      auto sym = [&](const char *n) {
        void *p=dlsym(b.handle,n);
        const auto *mdapi=static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));
        PgrMetaCodeQuery q{1,sizeof(q),p,4,nullptr,0,0};
        return p && mdapi && mdapi->validate_code(&q)==0 ? p : nullptr;
      };
      if (!exp.field_set_value_object) {
        exp.field_set_value_object = reinterpret_cast<decltype(exp.field_set_value_object)>(
            sym("il2cpp_field_set_value_object"));
      }
      if (!exp.string_length) {
        exp.string_length = reinterpret_cast<decltype(exp.string_length)>(sym("il2cpp_string_length"));
      }
      if (!exp.object_get_virtual_method) {
        exp.object_get_virtual_method = reinterpret_cast<decltype(exp.object_get_virtual_method)>(
            sym("il2cpp_object_get_virtual_method"));
      }
      if (!exp.extra.class_get_fields) exp.extra.class_get_fields = sym("il2cpp_class_get_fields");
      if (!exp.extra.field_get_name) exp.extra.field_get_name = sym("il2cpp_field_get_name");
      if (!exp.extra.type_get_object) exp.extra.type_get_object = sym("il2cpp_type_get_object");
      if (!exp.extra.array_length) exp.extra.array_length = sym("il2cpp_array_length");
      if (!exp.extra.method_get_object) exp.extra.method_get_object = sym("il2cpp_method_get_object");
      if (!exp.extra.runtime_class_init) exp.extra.runtime_class_init = sym("il2cpp_runtime_class_init");
      if (!exp.extra.class_get_parent) exp.extra.class_get_parent = sym("il2cpp_class_get_parent");
      if (!exp.extra.gc_wbarrier_set_field) exp.extra.gc_wbarrier_set_field = sym("il2cpp_gc_wbarrier_set_field");
      if (!exp.extra.method_get_from_reflection) {
        exp.extra.method_get_from_reflection = sym("il2cpp_method_get_from_reflection");
      }
    }
  }

  // Resolve Feature 56 metadata if not already provided: first check Feature 61 shared_host,
  // then fall back to Feature 56 pgrazor_shared_metadata_api(2)->resolve().
  if (!have_md) {
    const auto *sh = static_cast<const PgrSharedHostApi *>(pgrazor_shared_host_api(1));
    const PgrSharedMetadataOutput *sh_md = (sh && sh->metadata) ? sh->metadata() : nullptr;
    if (sh_md && sh_md->version == 2u && sh_md->size == sizeof(PgrSharedMetadataOutput)) {
      md = *sh_md;
      have_md = true;
    }
  }
  if (!have_md) {
    const auto *sm = static_cast<const PgrSharedMetadataApi *>(
        pgrazor_shared_metadata_api(2u));
    if (!sm || !sm->resolve) {

      return PGR_EH_ERR_METADATA;
    }
    PgrSharedMetadataConfig mcfg = {};
    if (config && config->metadata_config.version == 2u &&
        config->metadata_config.size == sizeof(PgrSharedMetadataConfig) &&
        config->metadata_config.binding.exports != nullptr) {
      mcfg = config->metadata_config;
      if (!mcfg.extra) mcfg.extra = &exp.extra;
    } else {
      mcfg.version = 2u;
      mcfg.size = sizeof(PgrSharedMetadataConfig);
      mcfg.binding.version = PGR_MD_CONFIG_VERSION;
      mcfg.binding.size = sizeof(PgrMetaBindConfig);
      mcfg.binding.exports = &exp.meta;
      mcfg.binding.module_base = b.module_base;
      mcfg.binding.module_size = b.module_size;
      mcfg.thread_current = reinterpret_cast<void *>(exp.thread_current);
      mcfg.owner_tid = current_os_tid();
      mcfg.extra = &exp.extra;
    }
    if (sm->resolve(&mcfg, &md) != PGR_SHARED_OK) {

      return PGR_EH_ERR_METADATA;
    }
  }

  if (!exp.thread_current || !exp.runtime_invoke || !exp.object_get_class ||
      !exp.object_unbox || !exp.object_new || !exp.field_get_value ||
      !exp.field_set_value || !exp.field_set_value_object || !exp.gchandle_new ||
      !exp.gchandle_get_target || !exp.gchandle_free) {

    return PGR_EH_ERR_BAD_CONFIG;
  }

  if (!exp.thread_current()) return PGR_EH_RETRY;
  g_owner_tid = current_os_tid();
  g_direct_transport = config == nullptr;
  g_cfg = local_cfg;
  g_exp = exp;
  g_md = md;
  g_configured = true;

  return PGR_EH_OK;
}

static int api_install(void) {
  StateGuard state; if (!state) return PGR_EH_RETRY;
  if (g_hook_poisoned) return PGR_EH_ERR_HOOK;
  if (g_installed) {

    return PGR_EH_OK;
  }
  if (!g_configured) {

    int cfg_rc = api_configure(nullptr);
    if (cfg_rc != PGR_EH_OK) return cfg_rc;

  }
  if (monotonic_ms() == 0) {

    return PGR_EH_ERR_NO_CLOCK;
  }

  uint32_t req_mask = g_cfg.capability_mask ? g_cfg.capability_mask : 0x7Fu;
  bool require_all = g_cfg.require_all != 0;
  uint32_t active_caps = 0;

  // Register before publishing any63 hooks:49 owns the shared btxu trampoline.
  if(req_mask&PGR_EH_CAP_BERRIES){
    auto f=static_cast<const BerryFilterApi*>(pgrazor_catch_observers_api(2));
    if(!f || f->version!=2 || f->size!=sizeof(BerryFilterApi) || !f->register_filter) return PGR_EH_RETRY;
    int rc=f->register_filter(&berry_hold_filter,g_owner_tid);
    if(rc!=0) return rc==6?PGR_EH_RETRY:PGR_EH_ERR_HOOK;
  }

  // 1. Berries (Feature 18)
  if (req_mask & PGR_EH_CAP_BERRIES) {
    int rc = install_berries_leaf();
    if (rc == PGR_EH_OK) {
      active_caps |= PGR_EH_CAP_BERRIES;
      if (g_md.optional[OPT_ITEMS] == PGR_SHARED_OPT_AVAILABLE) {
        active_caps |= PGR_EH_CAP_ITEMS;
      }
      emit_event("berry-ready", "{\"ready\":true}");
      notify_berry_ready(1);
    } else if (require_all) {

      return rc;
    } else {
      emit_event("berry-ready", "{\"ready\":false}");
      notify_berry_ready(0);
    }
  }

  // 2. MapPOI (Feature 42)
  if (req_mask & PGR_EH_CAP_MAP_POI) {
    bool has_extra = false;
    int rc = install_map_poi_leaf(&has_extra);
    if (rc == PGR_EH_OK) {
      active_caps |= PGR_EH_CAP_MAP_POI;
      if (has_extra) active_caps |= PGR_EH_CAP_MAP_POI_EXTRA;
    } else if (require_all) {

      return rc;
    }
  }

  // 3. Auto-Transfer (Features 4, 23, 35, 57)
  if (req_mask & PGR_EH_CAP_TRANSFER) {
    int rc = install_transfer_leaves();
    if (rc == PGR_EH_OK) {
      active_caps |= PGR_EH_CAP_TRANSFER;
    } else if (require_all) {

      return rc;
    }
  }

  // 4. VGP (Features 25..34)
  if (req_mask & PGR_EH_CAP_VGP) {
    bool has_marker = false;
    int rc = install_vgp_leaves(&has_marker);
    if (rc == PGR_EH_OK) {
      active_caps |= PGR_EH_CAP_VGP | PGR_EH_CAP_ITEMS;
      if (has_marker) active_caps |= PGR_EH_CAP_VGP_MARKER;
      emit_event("vgp-ready", "{\"ready\":true,\"reason\":\"\"}");
      notify_vgp_ready(1, "");
    } else if (require_all) {

      return rc;
    } else {
      emit_event("vgp-ready", "{\"ready\":false,\"reason\":\"VGP metadata unavailable\"}");
      notify_vgp_ready(0, "VGP metadata unavailable");
    }
  }

  // 5. Install Dobby hooks (using *_CODE handles!)
  uint32_t qb_shared_mask = 0;
  if (active_caps & PGR_EH_CAP_BERRIES) {
    if (install_one_hook(md_h(H_BR_M_ENTER_CODE),
                         reinterpret_cast<void *>(&hook_berry_enter),
                         reinterpret_cast<void **>(&g_orig_berry_enter)) != PGR_EH_OK ||
        install_one_hook(md_h(H_BR_M_EXIT_CODE),
                         reinterpret_cast<void *>(&hook_berry_exit),
                         reinterpret_cast<void **>(&g_orig_berry_exit)) != PGR_EH_OK ||
        install_one_hook(md_h(H_BR_M_RESULT_CODE),
                         reinterpret_cast<void *>(&hook_berry_result),
                         reinterpret_cast<void **>(&g_orig_berry_result)) != PGR_EH_OK ||
        install_one_hook(md_h(H_BR_M_ERROR_CODE),
                         reinterpret_cast<void *>(&hook_berry_error),
                         reinterpret_cast<void **>(&g_orig_berry_error)) != PGR_EH_OK ||
        install_one_hook(md_h(H_BR_M_SUBMIT_CODE),
                         reinterpret_cast<void *>(&hook_pokeball_submit),
                         reinterpret_cast<void **>(&g_orig_pokeball_submit)) != PGR_EH_OK) {
      rollback_hooks();

      return PGR_EH_ERR_HOOK;
    }
    qb_shared_mask |= PGR_QBH_SUBMIT;
  }

  if (active_caps & PGR_EH_CAP_VGP) {
    if (install_one_hook(md_h(H_VGP_M_MANUAL_UI_CODE),
                         reinterpret_cast<void *>(&hook_nameplate_set_pokemon_ui),
                         reinterpret_cast<void **>(&g_orig_nameplate_ui)) != PGR_EH_OK) {
      rollback_hooks();

      return PGR_EH_ERR_HOOK;
    }
    qb_shared_mask |= PGR_QBH_NAMEPLATE;
  }

  if (qb_shared_mask != 0) {
    const auto *qb = static_cast<const PgrQuickballHostApi *>(
        pgrazor_quickball_host_api(1));
    if (qb && qb->shared_sites) {
      qb->shared_sites(qb_shared_mask);
    }
  }

  g_active_caps = active_caps;
  g_installed = true;
  g_stopped = false;
  g_stop_requested.store(false, std::memory_order_release);
  g_sample.installed = 1;
  g_sample.active_capabilities = active_caps;

  return PGR_EH_OK;
}

static int api_update_options(const PgrEncounterHostOptions *options, uint64_t proven_tid) {
  if (!options) return PGR_EH_ERR_NULL_ARG;
  if (options->version != PGR_EH_API_VERSION ||
      options->size != sizeof(PgrEncounterHostOptions)) {
    return PGR_EH_ERR_BAD_CONFIG;
  }
  StateGuard state; if (!state) return PGR_EH_RETRY;
  if (!g_installed) {

    return PGR_EH_ERR_NOT_INSTALLED;
  }
  uint64_t tid = proven_unity_tid();
  if (!tid || (proven_tid && proven_tid != tid)) return PGR_EH_RETRY;
  g_opts = *options;
  uint64_t lease = options->lease_remaining_ms > 2500ULL ? 2500ULL : options->lease_remaining_ms;
  g_opts.lease_remaining_ms = lease;
  g_lease_deadline_ms = lease > 0 ? (monotonic_ms() + lease) : 0ULL;

  if (g_berries && (g_active_caps & PGR_EH_CAP_BERRIES)) {
    uint32_t cnt = (options->berries_allowed && options->berry_priority_count <= 3)
                       ? options->berry_priority_count
                       : 0u;
    g_berries->update(cnt ? options->berry_priority_ids : nullptr, cnt,
                      options->berries_allowed ? lease : 0ULL, tid);
  }

  bool vgp_en = (options->vgp_allowed && options->vgp_enabled);
  uint32_t mask = (options->vgp_catch_allowed ? 1u : 0u) | (options->vgp_spin_allowed ? 2u : 0u);
  if (vgp_en != g_last_vgp_enabled || mask != g_last_vgp_mask) {
    if (((!vgp_en && g_last_vgp_enabled) || (g_last_vgp_mask & ~mask)) && g_vgc && tid != 0) {
      g_vgc->revoke();
      clear_active_vgp_transfer();
    }
    g_last_vgp_enabled = vgp_en;
    g_last_vgp_mask = mask;
    g_vgp_epoch++;
  }

  return PGR_EH_OK;
}

static int api_set_keep_species(const int32_t *species_ids, uint32_t count) {
  if (count > 0 && !species_ids) return PGR_EH_ERR_NULL_ARG;
  if (count > 2000) return PGR_EH_ERR_BAD_CONFIG;
  StateGuard state; if (!state) return PGR_EH_RETRY;
  g_keep_species_count = count;
  for (uint32_t i = 0; i < count; ++i) {
    g_keep_species[i] = species_ids[i];
  }
  g_transfer_epoch++;

  return PGR_EH_OK;
}

static int api_stop(uint64_t proven_tid);
static int api_tick(uint64_t proven_tid) {
  if (g_stop_requested.load(std::memory_order_acquire)) return api_stop(proven_tid);
  StateGuard state; if (!state) return PGR_EH_RETRY;
  if (!g_installed) {

    return PGR_EH_ERR_NOT_INSTALLED;
  }
  if (g_stopped) {
    return PGR_EH_OK;
  }
  uint64_t os_tid = proven_unity_tid();
  if (os_tid == 0 || (proven_tid != 0 && proven_tid != os_tid)) {

    return PGR_EH_OK;
  }

  if (g_ticking || g_hook_frames) return PGR_EH_RETRY;
  struct TickFrame {TickFrame(){g_ticking=true;} ~TickFrame(){g_ticking=false;}} tickframe;
  // Sync settings from Feature 52 transport if active.
  sync_from_transport(os_tid);

  uint64_t now = monotonic_ms();
  uint64_t rem_lease = (g_lease_deadline_ms > now) ? (g_lease_deadline_ms - now) : 0ULL;
  if (rem_lease > 2500ULL) rem_lease = 2500ULL;

  g_sample.tick_serial++;

  // 1. Berries tick, resume & event drain
  pump_berry_events_and_resume(os_tid, 0);

  // 2. VGP controller tick & transfer handoff
  tick_vgp_controller(os_tid, now, rem_lease);

  // 3. MapPOI snapshot scan
  tick_map_poi(os_tid, now, rem_lease);
  pgr_gym_tick(os_tid, &g_md);
  pgr_evolution_tick(os_tid, &g_md);
  pgr_buddy_emotion_tick(os_tid, &g_md);


  return PGR_EH_OK;
}

static int api_stop(uint64_t proven_tid) {
  g_stop_requested.store(true, std::memory_order_release);
  StateGuard state; if (!state) return PGR_EH_RETRY;
  if (!g_installed) {

    return PGR_EH_ERR_NOT_INSTALLED;
  }
  if (g_stopped) return PGR_EH_OK;
  if (g_hook_frames || g_ticking) return PGR_EH_RETRY;
  uint64_t tid = proven_unity_tid();
  if (!tid || (proven_tid && proven_tid != tid)) return PGR_EH_RETRY;
  g_stopped = true;
  if (tid != 0) {
    if (g_berries && (g_active_caps & PGR_EH_CAP_BERRIES)) {
      g_berries->update(nullptr, 0, 0, tid);
      pump_berry_events_and_resume(tid, 1);
    }
    if (g_vgc && (g_active_caps & PGR_EH_CAP_VGP)) {
      pgr_gym_stop(tid);
      pgr_evolution_stop(tid);
      pgr_buddy_emotion_stop(tid);
      g_vgc->stop(tid);
      notify_vgp_target(0, 0.0, 0.0, 0);
    }
    if (g_tx && (g_active_caps & PGR_EH_CAP_TRANSFER)) {
      pump_transfer_events(tid, 1);
    }
    clear_active_vgp_transfer();
  }

  return PGR_EH_OK;
}

static int api_sample(PgrEncounterHostSample *out) {
  if (!out) return PGR_EH_ERR_NULL_ARG;
  if (out->version != PGR_EH_API_VERSION || out->size != sizeof(PgrEncounterHostSample)) {
    return PGR_EH_ERR_BAD_CONFIG;
  }
  StateGuard state; if (!state) return PGR_EH_RETRY;
  *out = g_sample;

  return PGR_EH_OK;
}

static int api_poll_event(char *type_buf, uint32_t type_cap, char *json_buf, uint32_t json_cap) {
  if (!type_buf || type_cap == 0 || !json_buf || json_cap == 0) return PGR_EH_ERR_NULL_ARG;
  pthread_mutex_lock(&g_event_mu);
  if (g_event_count == 0) {
    pthread_mutex_unlock(&g_event_mu);
    return 0;
  }
  const QueuedEvent &ev = g_event_ring[g_event_head];
  snprintf(type_buf, type_cap, "%s", ev.type);
  snprintf(json_buf, json_cap, "%s", ev.json);
  g_event_head = (g_event_head + 1) % kEventQueueCap;
  g_event_count--;
  pthread_mutex_unlock(&g_event_mu);
  return 1;
}

static const PgrEncounterHostApi kEncounterHostApi = {
    PGR_EH_API_VERSION,
    sizeof(PgrEncounterHostApi),
    &api_configure,
    &api_install,
    &api_update_options,
    &api_set_keep_species,
    &api_tick,
    &api_stop,
    &api_sample,
    &api_poll_event,
};

}  // namespace

extern "C" const void *pgrazor_encounter_host_api(uint32_t version) {
  if (version != PGR_EH_API_VERSION) return nullptr;
  return &kEncounterHostApi;
}
