#include "quick_spin.h"
#include "hook_engine.h"
#include "interaction_observer.h"

#include <atomic>
#include <mutex>
#include <vector>
#include <cstring>
#include <time.h>
#include <unistd.h>

// Feature 11 quick-spin. See quick_spin.h for the ownership contract.
//
// Threading model (the invariants this file enforces):
//   * enter/exit/result/spin hooks and tick() run on the game's Unity thread.
//     The first on-thread observation whose tid matches the policy's proven_tid
//     latches g_owner_tid once, permanently.
//   * Only the latched owner thread, and only when no managed call is in flight
//     (g_call_depth == 0), ever frees a GC handle. update()/clear()/sample() may
//     run on any thread, so they never free -- they only mark the record for
//     cancellation (its handles move to g_retired) and the owner drains later.
//   * Every managed call (field_get_value, runtime_invoke, original callbacks)
//     runs with g_state_mutex released, with temporary GC roots on the objects it
//     touches, and with the record generation re-validated before and after.

namespace {

constexpr uint64_t kMaxLeaseMs = 2500ULL;
constexpr uint64_t kInitialDeadlineMs = 15000ULL;
constexpr uint64_t kRequestDeadlineMs = 12000ULL;
constexpr uint32_t kMaxReentrantDepth = 8U;
constexpr int32_t kGuiStateOpening = 1;

using GcHandleNewFn = uintptr_t (*)(void *obj, bool pinned);
using GcHandleGetTargetFn = void *(*)(uintptr_t handle);
using GcHandleFreeFn = void (*)(uintptr_t handle);
using ObjectGetClassFn = void *(*)(void *obj);
using FieldGetValueFn = void (*)(void *obj, void *field, void *out_value);
using RuntimeInvokeFn = void *(*)(const void *method, void *obj, void **params, void **exc);
using ObjectUnboxFn = void *(*)(void *obj);

using EnterHookFn = void (*)(void *self, const void *method);
using ExitHookFn = void (*)(void *self, const void *method);
using ResultHookFn = void (*)(void *self, void *proto, const void *method);
using SpinHookFn = void (*)(void *self, float speed, const void *method);

struct ResolvedConfig {
  int32_t success_value;
  int32_t state_open_value;

  GcHandleNewFn gchandle_new;
  GcHandleGetTargetFn gchandle_get_target;
  GcHandleFreeFn gchandle_free;
  ObjectGetClassFn object_get_class;
  FieldGetValueFn field_get_value;
  RuntimeInvokeFn runtime_invoke;
  ObjectUnboxFn object_unbox;

  void *enter_address;
  void *exit_address;
  void *result_address;
  void *spin_address;

  const void *spin_method;
  const void *request_method;
  const void *can_method;
  const void *active_method;
  const void *close_method;
  const void *alive_method;

  void *spin_state_class;
  void *spin_gui_class;
  void *gesture_class;
  void *spinner_class;
  void *spin_result_class;
  void *bool_class;

  void *gui_field;
  void *gesture_field;
  void *spinner_field;
  void *result_field;
  void *current_state_field;
};

struct SpinRecord {
  bool active;
  bool enter_complete;
  bool sent;
  bool has_result;
  bool closed;
  uint64_t generation;
  uintptr_t state_handle;
  uintptr_t gui_handle;
  uint64_t epoch;
  uint64_t opened_at_ms;
  uint64_t deadline_ms;
  uint64_t enter_tick;
  uint64_t result_tick;
  int32_t result_value;
};

std::mutex g_install_mutex;
std::mutex g_state_mutex;

std::atomic<bool> g_installed{false};
bool g_install_attempted = false; // install mutex; partially installed hooks stay alive
int g_install_error = PGR_QUICK_SPIN_ERR_NOT_INSTALLED;
PgrQuickSpinConfig g_bound_config{};
std::atomic<EnterHookFn> g_orig_enter{nullptr};
std::atomic<ExitHookFn> g_orig_exit{nullptr};
std::atomic<ResultHookFn> g_orig_result{nullptr};
std::atomic<SpinHookFn> g_orig_spin{nullptr};

// The permanent cleanup owner, latched once from a genuine on-thread observation.
// Separate from the policy hint (g_proven_tid) and from the reentrancy busy guard
// (g_call_depth) so a policy update from another thread can never reassign it.
std::atomic<uint64_t> g_owner_tid{0};

// --- Everything below is guarded by g_state_mutex. ---
ResolvedConfig g_cfg{};
bool g_enabled = false;
uint64_t g_lease_until_ms = 0;
uint64_t g_proven_tid = 0;
uint64_t g_epoch = 0;
uint64_t g_last_tick_number = 0;
uint64_t g_next_generation = 0;
uint32_t g_call_depth = 0;
uint32_t g_temp_root_count = 0;

SpinRecord g_record{};

// GC handles orphaned by a cancellation, awaiting a free on the owner thread.
// Unbounded so an off-thread cancellation burst can never drop (leak) a handle.
std::vector<uintptr_t> g_retired;

uint32_t g_request_serial = 0;
uint32_t g_closed_serial = 0;
uint32_t g_error_serial = 0;
uint32_t g_cooldown_serial = 0;
int32_t g_last_result = 0;
uint32_t g_last_error_code = PGR_QUICK_SPIN_RUNTIME_NONE;
int32_t g_last_gui_state = 0;
uint64_t g_last_opened_ms = 0;
uint64_t g_last_tid = 0;

uint64_t current_tid() {
  return static_cast<uint64_t>(static_cast<pid_t>(gettid()));
}

bool read_boottime_ms(uint64_t *out_ms) {
  struct timespec ts {};
  if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0 || ts.tv_sec < 0 || ts.tv_nsec < 0) {
    return false;
  }
  *out_ms = static_cast<uint64_t>(ts.tv_sec) * 1000ULL +
            static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
  return true;
}

// Publish an original trampoline pointer, or wait for install to publish it if a
// call races in before the install mutex is released. Never skips the original.
template <typename Fn>
Fn load_orig(std::atomic<Fn> &slot) {
  Fn fn = slot.load(std::memory_order_acquire);
  if (fn != nullptr) {
    return fn;
  }
  std::lock_guard<std::mutex> lock(g_install_mutex);
  return slot.load(std::memory_order_acquire);
}

// --- helpers; all require g_state_mutex held by the caller ---

void publish_error_locked(uint32_t code, int32_t result_val, uint64_t tid) {
  g_last_error_code = code;
  g_last_result = result_val;
  g_last_tid = tid;
  ++g_error_serial;
}

void queue_retired_locked(uintptr_t handle) {
  if (handle != 0) {
    g_retired.push_back(handle);
  }
}

void detach_record_locked() {
  if (!g_record.active) {
    return;
  }
  queue_retired_locked(g_record.state_handle);
  queue_retired_locked(g_record.gui_handle);
  g_record = SpinRecord{};
}

// Latch the cleanup owner. Only ever called once we have confirmed the caller is
// running on the policy-proven Unity thread.
void latch_owner_locked(uint64_t tid) {
  uint64_t expected = 0;
  g_owner_tid.compare_exchange_strong(expected, tid, std::memory_order_acq_rel);
}

bool spin_permitted_locked(uint64_t now_ms) {
  return g_enabled && g_proven_tid != 0 && now_ms < g_lease_until_ms;
}

bool record_matches_and_permitted_locked(uint64_t generation, uint64_t now_ms) {
  if (!g_record.active || g_record.generation != generation) {
    return false;
  }
  if (!spin_permitted_locked(now_ms) || g_record.epoch != g_epoch ||
      now_ms >= g_record.deadline_ms) {
    return false;
  }
  return true;
}

uint32_t owned_root_count_locked() {
  uint32_t count = g_temp_root_count + static_cast<uint32_t>(g_retired.size()) + (g_call_depth ? 1U : 0U);
  if (g_record.active) {
    if (g_record.state_handle != 0) ++count;
    if (g_record.gui_handle != 0) ++count;
  }
  return count;
}

// Free retired handles, but ONLY on the latched owner thread and ONLY when no
// managed call is in flight, so no outer frame's handle copy is freed under it.
void drain_retired_if_owner_idle() {
  std::vector<uintptr_t> to_free;
  GcHandleFreeFn free_fn = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    const uint64_t owner = g_owner_tid.load(std::memory_order_acquire);
    if (owner == 0 || current_tid() != owner || g_call_depth != 0 ||
        g_retired.empty()) {
      return;
    }
    to_free.swap(g_retired);
    free_fn = g_cfg.gchandle_free;
  }
  if (free_fn != nullptr) {
    for (uintptr_t h : to_free) {
      if (h != 0) free_fn(h);
    }
  }
}

// Reentrancy busy guard for owner-thread managed work. While any scope is live the
// retired list is never freed; the outermost scope drains on unwind.
class UnityCallScope {
 public:
  explicit UnityCallScope(uint64_t tid) : active_(false) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_call_depth >= kMaxReentrantDepth) {
      publish_error_locked(PGR_QUICK_SPIN_RUNTIME_REENTRANT_OVERFLOW, 0, tid);
      detach_record_locked();
      return;
    }
    ++g_call_depth;
    active_ = true;
  }

  ~UnityCallScope() {
    if (!active_) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(g_state_mutex);
      if (g_call_depth > 0) --g_call_depth;
    }
    drain_retired_if_owner_idle();
  }

  bool ok() const { return active_; }

 private:
  bool active_;
};

// A temporary GC root held for the lifetime of a single owner-thread frame. The
// handle is created/freed only on the owner thread, so direct free is safe.
class ScopedTempRoot {
 public:
  ScopedTempRoot(const ScopedTempRoot &) = delete;
  ScopedTempRoot &operator=(const ScopedTempRoot &) = delete;
  ScopedTempRoot(const ResolvedConfig &cfg, void *obj) : cfg_(cfg), handle_(0) {
    if (obj != nullptr && cfg_.gchandle_new != nullptr) {
      handle_ = cfg_.gchandle_new(obj, false);
      if (handle_ != 0) {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        ++g_temp_root_count;
      }
    }
  }

  ~ScopedTempRoot() { reset(); }

  uintptr_t handle() const { return handle_; }

  void *target() const {
    if (handle_ == 0 || cfg_.gchandle_get_target == nullptr) {
      return nullptr;
    }
    return cfg_.gchandle_get_target(handle_);
  }

  // Hand ownership to the persistent record WITHOUT taking g_state_mutex (the
  // caller already holds it). Avoids the self-deadlock the draft had here.
  uintptr_t detach_locked() {
    uintptr_t h = handle_;
    if (h != 0) {
      if (g_temp_root_count > 0) --g_temp_root_count;
      handle_ = 0;
    }
    return h;
  }

  void reset() {
    if (handle_ == 0) {
      return;
    }
    uintptr_t h = handle_;
    handle_ = 0;
    {
      std::lock_guard<std::mutex> lock(g_state_mutex);
      if (g_temp_root_count > 0) --g_temp_root_count;
    }
    if (cfg_.gchandle_free != nullptr) {
      cfg_.gchandle_free(h);
    }
  }

 private:
  const ResolvedConfig &cfg_;
  uintptr_t handle_;
};

// runtime_invoke a parameterless/one-arg managed getter returning a boxed bool.
bool invoke_unboxed_bool(const ResolvedConfig &cfg, const void *method, void *obj,
                         void **params, uint32_t exc_error_code, bool *out_value,
                         uint32_t *out_err) {
  void *exc = nullptr;
  void *boxed = cfg.runtime_invoke(method, obj, params, &exc);
  if (exc != nullptr || boxed == nullptr) {
    *out_err = exc_error_code;
    return false;
  }
  ScopedTempRoot boxed_root(cfg, boxed);
  if (boxed_root.handle() == 0) {
    *out_err = PGR_QUICK_SPIN_RUNTIME_GCHANDLE_TEMP_FAILED;
    return false;
  }
  void *target = boxed_root.target();
  if (target == nullptr || cfg.object_get_class(target) != cfg.bool_class) {
    *out_err = PGR_QUICK_SPIN_RUNTIME_BOOL_TYPE_MISMATCH;
    return false;
  }
  void *raw = cfg.object_unbox(target);
  if (raw == nullptr) {
    *out_err = PGR_QUICK_SPIN_RUNTIME_BOOL_TYPE_MISMATCH;
    return false;
  }
  const uint8_t value = *static_cast<const uint8_t *>(raw);
  if (value > 1) { *out_err = PGR_QUICK_SPIN_RUNTIME_BOOL_TYPE_MISMATCH; return false; }
  *out_value = value == 1U;
  *out_err = PGR_QUICK_SPIN_RUNTIME_NONE;
  return true;
}

// ---------------------------------------------------------------------------
// Hooks. Each chains its original exactly once with unmodified arguments.
// ---------------------------------------------------------------------------

void hook_enter(void *self, const void *method) {
  EnterHookFn orig = load_orig(g_orig_enter);
  const uint64_t tid = current_tid();

  ResolvedConfig cfg{};
  bool is_unity = false;
  bool pre_permitted = false;
  uint64_t now_ms = 0;
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    cfg = g_cfg;
    is_unity = (g_proven_tid != 0 && tid == g_proven_tid && (g_owner_tid.load(std::memory_order_acquire) == 0 || g_owner_tid.load(std::memory_order_acquire) == tid));
    if (is_unity) {
      latch_owner_locked(tid);
      if (read_boottime_ms(&now_ms)) pre_permitted = spin_permitted_locked(now_ms);
    }
  }

  pgr_interaction_owner::Phase observer(6);
  pgr_interaction_owner::Pins pins(observer.wanted(),reinterpret_cast<void*>(cfg.gchandle_new),reinterpret_cast<void*>(cfg.gchandle_get_target),reinterpret_cast<void*>(cfg.gchandle_free),self);
  pgr_interaction_owner::AbortBeforeRoots unwind{observer};
  self=pins.at(0);
  auto invoke_original=[&](){
    if(pins.valid)observer.enter(pins.at(0));
    orig(pins.at(0),method);
    observer.leave();
  };
  if(!pins.valid){invoke_original();return;}

  if (!is_unity) {
    invoke_original();
    return;
  }

  UnityCallScope call_scope(tid);
  if (!call_scope.ok()) {
    invoke_original();
    return;
  }

  uint64_t created_gen = 0;
  if (pre_permitted && self != nullptr &&
      cfg.object_get_class(self) == cfg.spin_state_class) {
    uintptr_t state_handle = cfg.gchandle_new(self, false);
    if (state_handle == 0) {
      std::lock_guard<std::mutex> lock(g_state_mutex);
      publish_error_locked(PGR_QUICK_SPIN_RUNTIME_GCHANDLE_STATE_FAILED, 0, tid);
      detach_record_locked();
    } else {
      bool keep_handle = false;
      {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        uint64_t confirm_now = 0;
        if (read_boottime_ms(&confirm_now) && spin_permitted_locked(confirm_now)) {
          detach_record_locked();
          created_gen = ++g_next_generation;
          g_record.active = true;
          g_record.generation = created_gen;
          g_record.state_handle = state_handle;
          g_record.epoch = g_epoch;
          g_record.opened_at_ms = confirm_now;
          g_record.deadline_ms = confirm_now + kInitialDeadlineMs;
          g_record.enter_tick = g_last_tick_number;
          keep_handle = true;
        }
      }
      if (!keep_handle) {
        cfg.gchandle_free(state_handle);
      }
    }
  }

  // Call original EnterState before marking enter_tick (same as JS onLeave).
  invoke_original();

  if (created_gen != 0) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_record.active && g_record.generation == created_gen) {
      g_record.enter_tick = g_last_tick_number;
      g_record.enter_complete = true;
    }
  }
}

void hook_exit(void *self, const void *method) {
  ExitHookFn orig = load_orig(g_orig_exit);
  const uint64_t tid = current_tid();

  ResolvedConfig cfg{};
  uint64_t active_gen = 0;
  uintptr_t state_handle = 0;
  bool is_unity = false;
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    cfg = g_cfg;
    is_unity = (g_proven_tid != 0 && tid == g_proven_tid && (g_owner_tid.load(std::memory_order_acquire) == 0 || g_owner_tid.load(std::memory_order_acquire) == tid));
    if (is_unity) {
      latch_owner_locked(tid);
      if (g_record.active) {
        active_gen = g_record.generation;
        state_handle = g_record.state_handle;
      }
    }
  }

  pgr_interaction_owner::Phase observer(7);
  pgr_interaction_owner::Pins pins(observer.wanted(),reinterpret_cast<void*>(cfg.gchandle_new),reinterpret_cast<void*>(cfg.gchandle_get_target),reinterpret_cast<void*>(cfg.gchandle_free),self);
  pgr_interaction_owner::AbortBeforeRoots unwind{observer};
  self=pins.at(0);
  auto invoke_original=[&](){
    if(pins.valid)observer.enter(pins.at(0));
    orig(pins.at(0),method);
    observer.leave();
  };
  if(!pins.valid){invoke_original();return;}

  if (!is_unity) {
    invoke_original();
    return;
  }

  UnityCallScope call_scope(tid);
  if (call_scope.ok() && active_gen != 0 && state_handle != 0) {
    void *state_obj = cfg.gchandle_get_target(state_handle);
    if (state_obj == self) {
      std::lock_guard<std::mutex> lock(g_state_mutex);
      if (g_record.active && g_record.generation == active_gen) {
        detach_record_locked();
      }
    }
  }

  invoke_original();
}

void hook_result(void *self, void *proto, const void *method) {
  ResultHookFn orig = load_orig(g_orig_result);
  const uint64_t tid = current_tid();

  ResolvedConfig cfg{};
  bool is_unity = false;
  uint64_t candidate_gen = 0;
  uintptr_t state_handle = 0;
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    cfg = g_cfg;
    is_unity = (g_proven_tid != 0 && tid == g_proven_tid && (g_owner_tid.load(std::memory_order_acquire) == 0 || g_owner_tid.load(std::memory_order_acquire) == tid));
    if (is_unity) {
      latch_owner_locked(tid);
      uint64_t now_ms = 0;
      if (g_record.active && g_record.sent && read_boottime_ms(&now_ms) &&
          spin_permitted_locked(now_ms) && g_record.epoch == g_epoch &&
          now_ms < g_record.deadline_ms) {
        candidate_gen = g_record.generation;
        state_handle = g_record.state_handle;
      }
    }
  }

  pgr_interaction_owner::Phase observer(13);
  pgr_interaction_owner::Pins pins(observer.wanted(),reinterpret_cast<void*>(cfg.gchandle_new),reinterpret_cast<void*>(cfg.gchandle_get_target),reinterpret_cast<void*>(cfg.gchandle_free),self,proto);
  pgr_interaction_owner::AbortBeforeRoots unwind{observer};
  self=pins.at(0);proto=pins.at(1);
  auto invoke_original=[&](){
    if(pins.valid)observer.enter(pins.at(0),pins.at(1));
    orig(pins.at(0),pins.at(1),method);
    observer.leave();
  };
  if(!pins.valid){invoke_original();return;}

  if (!is_unity) {
    invoke_original();
    return;
  }

  UnityCallScope call_scope(tid);
  const bool valid_proto =
      (proto != nullptr && cfg.object_get_class(proto) == cfg.spin_result_class);

  // A valid spin result always bumps the cooldown counter on the verified Unity
  // thread, even while quick-spin is disabled (parity with the old JS behaviour).
  if (valid_proto) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    ++g_cooldown_serial;
    g_last_tid = tid;
  }

  bool has_captured = false;
  int32_t captured_result = 0;
  if (call_scope.ok() && valid_proto && candidate_gen != 0 && state_handle != 0) {
    void *state_obj = cfg.gchandle_get_target(state_handle);
    if (state_obj != nullptr && state_obj == self) {
      cfg.field_get_value(proto, cfg.result_field, &captured_result);
      has_captured = true;
    }
  }

  // Original result handler runs before publishing the result to record state.
  invoke_original();

  if (has_captured) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    uint64_t now_ms = 0;
    if (read_boottime_ms(&now_ms) &&
        record_matches_and_permitted_locked(candidate_gen, now_ms) &&
        g_record.sent) {
      g_record.has_result = true;
      g_record.result_value = captured_result;
      g_record.result_tick = g_last_tick_number;
      g_last_result = captured_result;
    }
  }
}

void hook_spin(void *self, float speed, const void *method) {
  SpinHookFn orig = load_orig(g_orig_spin);
  const uint64_t tid = current_tid();

  ResolvedConfig cfg{};
  bool is_unity = false;
  uint64_t candidate_gen = 0;
  uintptr_t state_handle = 0;
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    cfg = g_cfg;
    is_unity = (g_proven_tid != 0 && tid == g_proven_tid && (g_owner_tid.load(std::memory_order_acquire) == 0 || g_owner_tid.load(std::memory_order_acquire) == tid));
    if (is_unity) {
      latch_owner_locked(tid);
      uint64_t now_ms = 0;
      if (g_record.active && !g_record.sent && read_boottime_ms(&now_ms) &&
          spin_permitted_locked(now_ms) && g_record.epoch == g_epoch &&
          now_ms < g_record.deadline_ms) {
        candidate_gen = g_record.generation;
        state_handle = g_record.state_handle;
      }
    }
  }

  if (!is_unity || candidate_gen == 0 || state_handle == 0 || self == nullptr) {
    orig(self, speed, method);
    return;
  }

  UnityCallScope call_scope(tid);
  if (call_scope.ok()) {
    void *state_obj = cfg.gchandle_get_target(state_handle);
    if (state_obj != nullptr &&
        cfg.object_get_class(state_obj) == cfg.spin_state_class) {
      void *gesture = nullptr;
      cfg.field_get_value(state_obj, cfg.gesture_field, &gesture);
      if (gesture != nullptr &&
          cfg.object_get_class(gesture) == cfg.gesture_class) {
        ScopedTempRoot gesture_root(cfg, gesture);
        void *rooted_gesture = gesture_root.target();
        if (rooted_gesture != nullptr) {
          void *spinner = nullptr;
          cfg.field_get_value(rooted_gesture, cfg.spinner_field, &spinner);
          if (spinner == self) {
            // The original Spin triggered the game's own request: mark sent so our
            // tick never issues a duplicate one.
            std::lock_guard<std::mutex> lock(g_state_mutex);
            uint64_t now_ms = 0;
            if (read_boottime_ms(&now_ms) &&
                record_matches_and_permitted_locked(candidate_gen, now_ms) &&
                !g_record.sent) {
              g_record.sent = true;
              g_record.deadline_ms = now_ms + kRequestDeadlineMs;
            }
          }
        }
      }
    }
  }

  orig(self, speed, method);
}

// ---------------------------------------------------------------------------
// Install.
// ---------------------------------------------------------------------------

bool validate_config(const PgrQuickSpinConfig *config, int *out_rc) {
  if (config == nullptr) {
    *out_rc = PGR_QUICK_SPIN_ERR_INVALID_ARG;
    return false;
  }
  if (config->version != PGR_QUICK_SPIN_CONFIG_VERSION) {
    *out_rc = PGR_QUICK_SPIN_ERR_BAD_VERSION;
    return false;
  }
  if (config->size != sizeof(PgrQuickSpinConfig)) {
    *out_rc = PGR_QUICK_SPIN_ERR_BAD_SIZE;
    return false;
  }
  void *const ptrs[] = {
      config->gchandle_new,    config->gchandle_get_target, config->gchandle_free,
      config->object_get_class, config->field_get_value,    config->runtime_invoke,
      config->object_unbox,    config->enter_address,       config->exit_address,
      config->result_address,  config->spin_address,        config->spin_method,
      config->request_method,  config->can_method,          config->active_method,
      config->close_method,    config->alive_method,        config->spin_state_class,
      config->spin_gui_class,  config->gesture_class,       config->spinner_class,
      config->spin_result_class, config->bool_class,        config->gui_field,
      config->gesture_field,   config->spinner_field,       config->result_field,
      config->current_state_field,
  };
  for (void *p : ptrs) {
    if (p == nullptr) {
      *out_rc = PGR_QUICK_SPIN_ERR_MISSING_PTR;
      return false;
    }
  }
  void *const hooks[] = {config->enter_address, config->exit_address,
                         config->result_address, config->spin_address};
  for (size_t i = 0; i < 4; ++i) {
    for (size_t j = i + 1; j < 4; ++j) {
      if (hooks[i] == hooks[j]) {
        *out_rc = PGR_QUICK_SPIN_ERR_DUPLICATE_TARGET;
        return false;
      }
    }
  }
  *out_rc = PGR_QUICK_SPIN_OK;
  return true;
}

ResolvedConfig resolve_config(const PgrQuickSpinConfig *c) {
  ResolvedConfig r{};
  r.success_value = c->success_value;
  r.state_open_value = c->state_open_value;
  r.gchandle_new = reinterpret_cast<GcHandleNewFn>(c->gchandle_new);
  r.gchandle_get_target = reinterpret_cast<GcHandleGetTargetFn>(c->gchandle_get_target);
  r.gchandle_free = reinterpret_cast<GcHandleFreeFn>(c->gchandle_free);
  r.object_get_class = reinterpret_cast<ObjectGetClassFn>(c->object_get_class);
  r.field_get_value = reinterpret_cast<FieldGetValueFn>(c->field_get_value);
  r.runtime_invoke = reinterpret_cast<RuntimeInvokeFn>(c->runtime_invoke);
  r.object_unbox = reinterpret_cast<ObjectUnboxFn>(c->object_unbox);
  r.enter_address = c->enter_address;
  r.exit_address = c->exit_address;
  r.result_address = c->result_address;
  r.spin_address = c->spin_address;
  r.spin_method = c->spin_method;
  r.request_method = c->request_method;
  r.can_method = c->can_method;
  r.active_method = c->active_method;
  r.close_method = c->close_method;
  r.alive_method = c->alive_method;
  r.spin_state_class = c->spin_state_class;
  r.spin_gui_class = c->spin_gui_class;
  r.gesture_class = c->gesture_class;
  r.spinner_class = c->spinner_class;
  r.spin_result_class = c->spin_result_class;
  r.bool_class = c->bool_class;
  r.gui_field = c->gui_field;
  r.gesture_field = c->gesture_field;
  r.spinner_field = c->spinner_field;
  r.result_field = c->result_field;
  r.current_state_field = c->current_state_field;
  return r;
}

int api_install(const PgrQuickSpinConfig *config) {
  int validation_rc = PGR_QUICK_SPIN_OK;
  if (!validate_config(config, &validation_rc)) {
    return validation_rc;
  }

  uint64_t boot_probe = 0;
  if (!read_boottime_ms(&boot_probe)) {
    return PGR_QUICK_SPIN_ERR_CLOCK_FAILURE;
  }

  std::lock_guard<std::mutex> install_lock(g_install_mutex);
  const ResolvedConfig resolved = resolve_config(config);

  if (g_installed.load(std::memory_order_acquire)) {
    // Hooks live for the process; re-install of the same targets is idempotent,
    // but the four hook addresses may never be rebound.
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    if (std::memcmp(&g_bound_config, config, sizeof(*config)) != 0) {
      return PGR_QUICK_SPIN_ERR_RECONFIG_ADDRESS_MISMATCH;
    }
    return PGR_QUICK_SPIN_OK;
  }
  if (g_install_attempted) return g_install_error;
  g_install_attempted = true;
  g_bound_config = *config;

  {
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    g_cfg = resolved;
    g_enabled = false;
    g_lease_until_ms = 0;
    g_proven_tid = 0;
    g_epoch = 0;
    g_last_tick_number = 0;
    detach_record_locked();
  }

  // Install hooks under g_install_mutex, publishing each original before the next.
  // On partial failure retain published trampolines: another thread may already
  // be using one. The failed attempt is latched until process restart.
  void *orig_enter = nullptr;
  if (pgrazor_hook(resolved.enter_address, reinterpret_cast<void *>(&hook_enter),
                   &orig_enter) != 0 ||
      orig_enter == nullptr) {
    return g_install_error = PGR_QUICK_SPIN_ERR_HOOK_ENTER;
  }
  g_orig_enter.store(reinterpret_cast<EnterHookFn>(orig_enter), std::memory_order_release);

  void *orig_exit = nullptr;
  if (pgrazor_hook(resolved.exit_address, reinterpret_cast<void *>(&hook_exit),
                   &orig_exit) != 0 ||
      orig_exit == nullptr) {
    return g_install_error = PGR_QUICK_SPIN_ERR_HOOK_EXIT;
  }
  g_orig_exit.store(reinterpret_cast<ExitHookFn>(orig_exit), std::memory_order_release);

  void *orig_result = nullptr;
  if (pgrazor_hook(resolved.result_address, reinterpret_cast<void *>(&hook_result),
                   &orig_result) != 0 ||
      orig_result == nullptr) {
    return g_install_error = PGR_QUICK_SPIN_ERR_HOOK_RESULT;
  }
  g_orig_result.store(reinterpret_cast<ResultHookFn>(orig_result), std::memory_order_release);

  void *orig_spin = nullptr;
  if (pgrazor_hook(resolved.spin_address, reinterpret_cast<void *>(&hook_spin),
                   &orig_spin) != 0 ||
      orig_spin == nullptr) {
    return g_install_error = PGR_QUICK_SPIN_ERR_HOOK_SPIN;
  }
  g_orig_spin.store(reinterpret_cast<SpinHookFn>(orig_spin), std::memory_order_release);

  g_installed.store(true, std::memory_order_release);
  return g_install_error = PGR_QUICK_SPIN_OK;
}

// ---------------------------------------------------------------------------
// Policy update / clear. Safe on any thread: they only mark cancellation; the
// owner thread frees retired handles via drain_retired_if_owner_idle().
// ---------------------------------------------------------------------------

int api_update(int enabled, uint64_t remaining_lease_ms, uint64_t proven_tid,
               uint64_t epoch) {
  if (!g_installed.load(std::memory_order_acquire)) {
    return PGR_QUICK_SPIN_ERR_NOT_INSTALLED;
  }

  uint64_t now_ms = 0;
  const bool clock_ok = read_boottime_ms(&now_ms);
  bool wrong_thread = false;
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    const uint64_t owner = g_owner_tid.load(std::memory_order_acquire);
    wrong_thread = owner != 0 && proven_tid != 0 && proven_tid != owner;
    if (wrong_thread) {
      enabled = 0;
    } else if (proven_tid != 0) {
      g_proven_tid = proven_tid;
    }
    g_epoch = epoch;

    if (!clock_ok) {
      g_enabled = false;
      g_lease_until_ms = 0;
      publish_error_locked(PGR_QUICK_SPIN_RUNTIME_CLOCK_BOOTTIME_FAILED, 0, proven_tid);
      detach_record_locked();
    } else {
      const uint64_t clamped =
          remaining_lease_ms > kMaxLeaseMs ? kMaxLeaseMs : remaining_lease_ms;
      const bool allow = (enabled != 0 && clamped > 0 && proven_tid != 0 && !wrong_thread);
      g_enabled = allow;
      g_lease_until_ms = allow ? (now_ms + clamped) : 0;
      if (!allow || (g_record.active && (g_record.epoch != epoch ||
                                         now_ms >= g_record.deadline_ms))) {
        detach_record_locked();
      }
    }
  }

  drain_retired_if_owner_idle();
  return wrong_thread ? PGR_QUICK_SPIN_ERR_INVALID_ARG : clock_ok ? PGR_QUICK_SPIN_OK : PGR_QUICK_SPIN_ERR_CLOCK_FAILURE;
}

int api_clear(void) {
  if (!g_installed.load(std::memory_order_acquire)) {
    return PGR_QUICK_SPIN_ERR_NOT_INSTALLED;
  }
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    detach_record_locked();
  }
  drain_retired_if_owner_idle();
  return PGR_QUICK_SPIN_OK;
}

// ---------------------------------------------------------------------------
// Tick: mirrors spinTick(). Runs on the owner thread and performs the managed
// getters/invokes with the record generation re-validated before every call.
// ---------------------------------------------------------------------------

int api_tick(uint64_t proven_tid, uint64_t tick_number, uint64_t opener_depth) {
  if (!g_installed.load(std::memory_order_acquire)) {
    return PGR_QUICK_SPIN_ERR_NOT_INSTALLED;
  }

  // Only the real verified Unity thread may advance tick state or act on a record.
  const uint64_t caller_tid = current_tid();
  if (proven_tid == 0 || caller_tid != proven_tid) {
    return PGR_QUICK_SPIN_OK;
  }
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    const uint64_t owner = g_owner_tid.load(std::memory_order_acquire);
    if (g_proven_tid != proven_tid || (owner && owner != proven_tid) || g_call_depth != 0)
      return PGR_QUICK_SPIN_OK;
    latch_owner_locked(proven_tid);
  }

  UnityCallScope call_scope(proven_tid);
  if (!call_scope.ok()) {
    return PGR_QUICK_SPIN_OK;
  }

  uint64_t now_ms = 0;
  if (!read_boottime_ms(&now_ms)) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    publish_error_locked(PGR_QUICK_SPIN_RUNTIME_CLOCK_BOOTTIME_FAILED, 0, proven_tid);
    detach_record_locked();
    return PGR_QUICK_SPIN_ERR_CLOCK_FAILURE;
  }

  ResolvedConfig cfg{};
  SpinRecord snap{};
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_proven_tid != 0 && g_proven_tid != proven_tid) {
      return PGR_QUICK_SPIN_OK;
    }
    g_proven_tid = proven_tid;
    latch_owner_locked(proven_tid);
    g_last_tick_number = tick_number;

    if (!g_record.active) {
      return PGR_QUICK_SPIN_OK;
    }
    if (!spin_permitted_locked(now_ms) || g_record.epoch != g_epoch ||
        now_ms >= g_record.deadline_ms) {
      detach_record_locked();
      return PGR_QUICK_SPIN_OK;
    }
    if (opener_depth != 0 || !g_record.enter_complete ||
        tick_number <= g_record.enter_tick) {
      return PGR_QUICK_SPIN_OK;
    }
    cfg = g_cfg;
    snap = g_record;
  }

  const uint64_t gen = snap.generation;

  auto clear_if_same_gen = [&](uint32_t err_code, int32_t result_val) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (err_code != PGR_QUICK_SPIN_RUNTIME_NONE) {
      publish_error_locked(err_code, result_val, proven_tid);
    }
    if (g_record.active && g_record.generation == gen) {
      detach_record_locked();
    }
  };

  // Re-validate the record (same generation, still permitted, unexpired) before
  // each managed call. Returns false if the record is gone/stale.
  auto recheck = [&](uint64_t *out_now) -> bool {
    uint64_t check_now = 0;
    if (!read_boottime_ms(&check_now)) {
      clear_if_same_gen(PGR_QUICK_SPIN_RUNTIME_CLOCK_BOOTTIME_FAILED, 0);
      return false;
    }
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (!g_record.active || g_record.generation != gen) {
      return false;
    }
    if (!spin_permitted_locked(check_now) || g_record.epoch != g_epoch ||
        check_now >= g_record.deadline_ms) {
      detach_record_locked();
      return false;
    }
    if (out_now != nullptr) *out_now = check_now;
    return true;
  };

  void *state_obj = cfg.gchandle_get_target(snap.state_handle);
  if (state_obj == nullptr ||
      cfg.object_get_class(state_obj) != cfg.spin_state_class) {
    clear_if_same_gen(PGR_QUICK_SPIN_RUNTIME_NONE, 0);
    return PGR_QUICK_SPIN_OK;
  }

  void *gui_obj = nullptr;
  cfg.field_get_value(state_obj, cfg.gui_field, &gui_obj);
  if (gui_obj == nullptr) {
    return PGR_QUICK_SPIN_OK;  // gui not materialised yet; try again next tick
  }
  if (cfg.object_get_class(gui_obj) != cfg.spin_gui_class) {
    clear_if_same_gen(PGR_QUICK_SPIN_RUNTIME_NONE, 0);
    return PGR_QUICK_SPIN_OK;
  }

  ScopedTempRoot gui_root(cfg, gui_obj);
  if (gui_root.handle() == 0) {
    clear_if_same_gen(PGR_QUICK_SPIN_RUNTIME_GCHANDLE_GUI_FAILED, 0);
    return PGR_QUICK_SPIN_OK;
  }

  if (!recheck(&now_ms)) return PGR_QUICK_SPIN_OK;

  void *rooted_gui = gui_root.target();
  if (rooted_gui == nullptr) {
    clear_if_same_gen(PGR_QUICK_SPIN_RUNTIME_NONE, 0);
    return PGR_QUICK_SPIN_OK;
  }

  void *alive_args[1] = {rooted_gui};
  bool gui_alive = false;
  uint32_t bool_err = PGR_QUICK_SPIN_RUNTIME_NONE;
  if (!invoke_unboxed_bool(cfg, cfg.alive_method, nullptr, alive_args,
                           PGR_QUICK_SPIN_RUNTIME_ALIVE_EXCEPTION, &gui_alive,
                           &bool_err)) {
    clear_if_same_gen(bool_err, 0);
    return PGR_QUICK_SPIN_OK;
  }
  if (!gui_alive) {
    clear_if_same_gen(PGR_QUICK_SPIN_RUNTIME_NONE, 0);
    return PGR_QUICK_SPIN_OK;
  }

  if (!recheck(&now_ms)) return PGR_QUICK_SPIN_OK;
  rooted_gui = gui_root.target();
  if (rooted_gui == nullptr) {
    clear_if_same_gen(PGR_QUICK_SPIN_RUNTIME_NONE, 0);
    return PGR_QUICK_SPIN_OK;
  }

  // If we already pinned a gui on a previous tick, it must still be the same one.
  if (snap.gui_handle != 0) {
    void *expected_gui = cfg.gchandle_get_target(snap.gui_handle);
    if (expected_gui != rooted_gui) {
      clear_if_same_gen(PGR_QUICK_SPIN_RUNTIME_NONE, 0);
      return PGR_QUICK_SPIN_OK;
    }
  }

  int32_t gui_state = 0;
  cfg.field_get_value(rooted_gui, cfg.current_state_field, &gui_state);
  if (gui_state != kGuiStateOpening && gui_state != cfg.state_open_value) {
    return PGR_QUICK_SPIN_OK;
  }

  // Re-snapshot in case a result callback fired between the gate above and here.
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (!g_record.active || g_record.generation != gen) {
      return PGR_QUICK_SPIN_OK;
    }
    snap = g_record;
  }

  if (snap.has_result) {
    if (gui_state != cfg.state_open_value || tick_number <= snap.result_tick) {
      return PGR_QUICK_SPIN_OK;
    }
    if (snap.result_value == cfg.success_value) {
      if (!recheck(&now_ms)) return PGR_QUICK_SPIN_OK;
      {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        if (!g_record.active || g_record.generation != gen || g_record.closed) {
          return PGR_QUICK_SPIN_OK;
        }
        g_record.closed = true;
      }
      rooted_gui = gui_root.target();
      if (rooted_gui == nullptr) {
        clear_if_same_gen(PGR_QUICK_SPIN_RUNTIME_NONE, 0);
        return PGR_QUICK_SPIN_OK;
      }
      void *exc = nullptr;
      cfg.runtime_invoke(cfg.close_method, rooted_gui, nullptr, &exc);
      if (exc != nullptr) {
        clear_if_same_gen(PGR_QUICK_SPIN_RUNTIME_CLOSE_EXCEPTION, 0);
        return PGR_QUICK_SPIN_OK;
      }
      uint64_t done_ms = now_ms;
      read_boottime_ms(&done_ms);
      {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_last_opened_ms = done_ms >= snap.opened_at_ms ? (done_ms - snap.opened_at_ms) : 0;
        g_last_tid = proven_tid;
        ++g_closed_serial;
        if (g_record.active && g_record.generation == gen) {
          detach_record_locked();
        }
      }
      return PGR_QUICK_SPIN_OK;
    }
    // Non-success result: surface it and drop the record.
    clear_if_same_gen(PGR_QUICK_SPIN_RUNTIME_RESULT_NON_SUCCESS, snap.result_value);
    return PGR_QUICK_SPIN_OK;
  }

  if (snap.sent) {
    return PGR_QUICK_SPIN_OK;  // request already issued; wait for the result
  }

  // No result yet and not sent: walk SpinState -> Gesture -> Spinner, gated on
  // IsActive / CanSpin, then issue Spin(360) + request.
  state_obj = cfg.gchandle_get_target(snap.state_handle);
  if (state_obj == nullptr) {
    clear_if_same_gen(PGR_QUICK_SPIN_RUNTIME_NONE, 0);
    return PGR_QUICK_SPIN_OK;
  }

  void *gesture_obj = nullptr;
  cfg.field_get_value(state_obj, cfg.gesture_field, &gesture_obj);
  if (gesture_obj == nullptr ||
      cfg.object_get_class(gesture_obj) != cfg.gesture_class) {
    return PGR_QUICK_SPIN_OK;
  }

  ScopedTempRoot gesture_root(cfg, gesture_obj);
  if (gesture_root.handle() == 0) {
    clear_if_same_gen(PGR_QUICK_SPIN_RUNTIME_GCHANDLE_TEMP_FAILED, 0);
    return PGR_QUICK_SPIN_OK;
  }

  if (!recheck(&now_ms)) return PGR_QUICK_SPIN_OK;
  void *rooted_gesture = gesture_root.target();
  if (rooted_gesture == nullptr) return PGR_QUICK_SPIN_OK;

  bool is_active = false;
  if (!invoke_unboxed_bool(cfg, cfg.active_method, rooted_gesture, nullptr,
                           PGR_QUICK_SPIN_RUNTIME_ACTIVE_EXCEPTION, &is_active,
                           &bool_err)) {
    clear_if_same_gen(bool_err, 0);
    return PGR_QUICK_SPIN_OK;
  }
  if (!is_active) return PGR_QUICK_SPIN_OK;

  if (!recheck(&now_ms)) return PGR_QUICK_SPIN_OK;
  rooted_gesture = gesture_root.target();
  if (rooted_gesture == nullptr) return PGR_QUICK_SPIN_OK;

  void *spinner_obj = nullptr;
  cfg.field_get_value(rooted_gesture, cfg.spinner_field, &spinner_obj);
  if (spinner_obj == nullptr ||
      cfg.object_get_class(spinner_obj) != cfg.spinner_class) {
    return PGR_QUICK_SPIN_OK;
  }

  ScopedTempRoot spinner_root(cfg, spinner_obj);
  if (spinner_root.handle() == 0) {
    clear_if_same_gen(PGR_QUICK_SPIN_RUNTIME_GCHANDLE_TEMP_FAILED, 0);
    return PGR_QUICK_SPIN_OK;
  }

  if (!recheck(&now_ms)) return PGR_QUICK_SPIN_OK;
  void *rooted_spinner = spinner_root.target();
  if (rooted_spinner == nullptr) return PGR_QUICK_SPIN_OK;

  bool can_spin = false;
  if (!invoke_unboxed_bool(cfg, cfg.can_method, rooted_spinner, nullptr,
                           PGR_QUICK_SPIN_RUNTIME_CAN_EXCEPTION, &can_spin,
                           &bool_err)) {
    clear_if_same_gen(bool_err, 0);
    return PGR_QUICK_SPIN_OK;
  }
  if (!can_spin) return PGR_QUICK_SPIN_OK;

  if (!recheck(&now_ms)) return PGR_QUICK_SPIN_OK;

  // Mark sent, promote the gui temp root to the persistent record root, and set the
  // 12s request deadline BEFORE invoking Spin(360)/request, so a synchronous result
  // callback or an uncertain failure can never produce a duplicate request.
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (!record_matches_and_permitted_locked(gen, now_ms) || g_record.sent ||
        g_record.has_result) {
      return PGR_QUICK_SPIN_OK;
    }
    queue_retired_locked(g_record.gui_handle);
    g_record.gui_handle = gui_root.detach_locked();  // no nested lock: detach_locked
    g_record.sent = true;
    g_record.deadline_ms = now_ms + kRequestDeadlineMs;
  }

  rooted_spinner = spinner_root.target();
  if (rooted_spinner == nullptr) {
    clear_if_same_gen(PGR_QUICK_SPIN_RUNTIME_SPIN_EXCEPTION, 0);
    return PGR_QUICK_SPIN_OK;
  }

  float speed = 360.0f;
  void *spin_args[1] = {&speed};
  void *spin_exc = nullptr;
  cfg.runtime_invoke(cfg.spin_method, rooted_spinner, spin_args, &spin_exc);
  if (spin_exc != nullptr) {
    clear_if_same_gen(PGR_QUICK_SPIN_RUNTIME_SPIN_EXCEPTION, 0);
    return PGR_QUICK_SPIN_OK;
  }

  if (!recheck(&now_ms)) return PGR_QUICK_SPIN_OK;

  bool result_already_arrived = false;
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (!g_record.active || g_record.generation != gen) {
      return PGR_QUICK_SPIN_OK;
    }
    result_already_arrived = g_record.has_result;
    g_last_gui_state = gui_state;
    g_last_opened_ms = now_ms >= snap.opened_at_ms ? now_ms - snap.opened_at_ms : 0;
    g_last_tid = proven_tid;
    ++g_request_serial; // Count issuance even if the managed request subsequently throws.
  }

  // Skip the explicit request if the original Spin already drove the normal
  // request and synchronously delivered a result.
  if (!result_already_arrived) {
    rooted_spinner = spinner_root.target();
    if (rooted_spinner == nullptr) {
      clear_if_same_gen(PGR_QUICK_SPIN_RUNTIME_REQUEST_EXCEPTION, 0);
      return PGR_QUICK_SPIN_OK;
    }
    void *req_exc = nullptr;
    cfg.runtime_invoke(cfg.request_method, rooted_spinner, nullptr, &req_exc);
    if (req_exc != nullptr) {
      clear_if_same_gen(PGR_QUICK_SPIN_RUNTIME_REQUEST_EXCEPTION, 0);
      return PGR_QUICK_SPIN_OK;
    }
  }

  return PGR_QUICK_SPIN_OK;
}

// ---------------------------------------------------------------------------
// Sample: pure pull. Never frees handles, never calls back into JS.
// ---------------------------------------------------------------------------

int api_sample(PgrQuickSpinSample *out_sample) {
  if (out_sample == nullptr) {
    return PGR_QUICK_SPIN_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(g_state_mutex);
  out_sample->version = PGR_QUICK_SPIN_SAMPLE_VERSION;
  out_sample->size = sizeof(PgrQuickSpinSample);
  out_sample->owned_root_count = owned_root_count_locked();
  out_sample->request_serial = g_request_serial;
  out_sample->closed_serial = g_closed_serial;
  out_sample->error_serial = g_error_serial;
  out_sample->cooldown_serial = g_cooldown_serial;
  out_sample->last_result = g_last_result;
  out_sample->last_error_code = g_last_error_code;
  out_sample->last_gui_state = g_last_gui_state;
  out_sample->last_opened_ms = g_last_opened_ms;
  out_sample->last_tid = g_last_tid;
  out_sample->active_generation = g_record.active ? g_record.generation : 0ULL;
  return PGR_QUICK_SPIN_OK;
}

const PgrQuickSpinApi kApiTable = {
    PGR_QUICK_SPIN_API_VERSION,
    static_cast<uint32_t>(sizeof(PgrQuickSpinApi)),
    &api_install,
    &api_update,
    &api_tick,
    &api_sample,
    &api_clear,
};

}  // namespace

extern "C" const void *pgrazor_quick_spin_api(uint32_t version) {
  if (version != PGR_QUICK_SPIN_API_VERSION) return nullptr;
  return &kApiTable;
}
