#include "accounts.h"
#include "hook_engine.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <time.h>
#include <unistd.h>

namespace {

typedef Il2CppObject* (*AuthLogoutFn)(Il2CppObject* self, const MethodInfo* method);

constexpr const char* kDobKey1 = "4b9b44d8-66f5-46dc-86bb-0215bfa90f5e";
constexpr const char* kDobKey2 = "80a6f7e6-1a8d-442e-bd95-74c795242b3f";

int32_t current_os_tid() noexcept {
  // Use the libc entry point, as the other native controllers do. It preserves
  // the host thread identity when ARM64 code runs through an emulator bridge.
  return static_cast<int32_t>(gettid());
}

uint64_t clock_boottime_ms() noexcept {
  struct timespec ts {};
  if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0) {
    return 0;
  }
  return static_cast<uint64_t>(ts.tv_sec) * 1000ULL +
         static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
}

struct ControllerState {
  std::mutex policy_mu;
  std::mutex hook_install_mu;

  std::atomic<AuthLogoutFn> orig_auth_logout{nullptr};
  std::atomic<bool> config_bound{false};
  std::atomic<bool> enabled{false};
  std::atomic<bool> hook_installed{false};
  std::atomic<bool> hook_failed_latched{false};
  std::atomic<int32_t> owner_tid{0};
  std::atomic<uint32_t> active_depth{0};
  std::atomic<uint32_t> active_gc_roots{0};
  std::atomic<bool> logging_out{false};

  pgrazor_accounts_config_v1_t cfg{};

  // Policy & command fields (guarded by policy_mu)
  uint64_t lease_until_ms = 0;
  uint32_t pending_cmd_kind = PGR_ACC_CMD_NONE;
  uint64_t pending_cmd_token = 0;
  uint64_t last_accepted_token = 0;
  uint64_t last_consumed_token = 0;
  bool uncertain_op_tainted = false;
  bool deferred_clear_requested = false;
  uint64_t mutation_epoch = 1;

  // Bounded FIFO event ring (guarded by policy_mu, fail-closed on overflow)
  pgrazor_account_event_v1_t ring[PGR_ACCOUNTS_EVENT_QUEUE_CAP]{};
  size_t ring_head = 0;
  size_t ring_count = 0;
  uint64_t next_seq = 1;
  bool queue_overflow_latched = false;

  // Snapshot identity mirror (guarded by policy_mu)
  bool snap_has_identity = false;
  uint16_t snap_id_len = 0;
  uint16_t snap_name_len = 0;
  uint16_t snap_id[PGR_ACCOUNTS_MAX_UTF16]{};
  uint16_t snap_name[PGR_ACCOUNTS_MAX_UTF16]{};
  uint64_t snap_last_identity_ms = 0;

  // Unity-owner-only runtime state (touched exclusively when current_os_tid() == owner_tid)
  uint64_t next_tick_ms = 0;
  uint64_t logout_started_ms = 0;
  bool in_tick = false;
  bool in_wrapper_logout_call = false;
  uintptr_t future_handle = 0;
  uintptr_t auth_handle = 0;
};

ControllerState g_state;

bool validate_config(const pgrazor_accounts_config_v1_t* c) noexcept {
  if (!c) return false;
  if (!c->runtime_invoke || !c->gchandle_new || !c->gchandle_get_target ||
      !c->gchandle_free || !c->object_get_class || !c->object_unbox ||
      !c->class_get_type || !c->type_get_object || !c->string_new ||
      !c->string_length || !c->string_chars || !c->field_get_value) {
    return false;
  }
  if (!c->boolean_class || !c->string_class || !c->future_class ||
      !c->map_scene_context_class || !c->di_container_class ||
      !c->player_class || !c->player_contract_class || !c->auth_class ||
      !c->auth_contract_class || !c->wrapper_class ||
      !c->wrapper_contract_class || !c->prefs_class) {
    return false;
  }
  if (!c->auth_active_provider_field || !c->future_status_field ||
      !c->future_disposed_field) {
    return false;
  }
  if (!c->find_object_of_type_method || !c->map_get_container_method ||
      !c->container_resolve_method || !c->player_get_id_method ||
      !c->player_get_name_method || !c->auth_get_connected_method ||
      !c->auth_get_cached_method || !c->wrapper_logout_method ||
      !c->auth_logout_method || !c->prefs_has_key_method ||
      !c->prefs_delete_key_method || !c->prefs_save_method ||
      !c->auth_logout_target_fn) {
    return false;
  }
  if (c->promise_succeeded_value == c->promise_failed_value) {
    return false;
  }
  return true;
}

void clear_identity_locked() noexcept {
  g_state.snap_has_identity = false;
  g_state.snap_id_len = 0;
  g_state.snap_name_len = 0;
  std::memset(g_state.snap_id, 0, sizeof(g_state.snap_id));
  std::memset(g_state.snap_name, 0, sizeof(g_state.snap_name));
}

void push_event_locked(uint32_t kind, uint32_t reason, bool available,
                       bool future_captured, const uint16_t* id, uint16_t id_len,
                       const uint16_t* name, uint16_t name_len) noexcept {
  if (g_state.queue_overflow_latched) {
    return;
  }
  if (g_state.ring_count >= PGR_ACCOUNTS_EVENT_QUEUE_CAP) {
    g_state.queue_overflow_latched = true;
    g_state.enabled.store(false, std::memory_order_release);
    g_state.mutation_epoch++;
    g_state.lease_until_ms = 0;
    g_state.pending_cmd_kind = PGR_ACC_CMD_NONE;
    g_state.pending_cmd_token = 0;
    g_state.deferred_clear_requested = true;
    clear_identity_locked();
    return;
  }
  const size_t idx = (g_state.ring_head + g_state.ring_count) % PGR_ACCOUNTS_EVENT_QUEUE_CAP;
  pgrazor_account_event_v1_t& ev = g_state.ring[idx];
  std::memset(&ev, 0, sizeof(ev));
  ev.seq = g_state.next_seq++;
  ev.timestamp_ms = clock_boottime_ms();
  ev.kind = kind;
  ev.reason = reason;
  ev.available = available ? 1u : 0u;
  ev.future_captured = future_captured ? 1u : 0u;
  if (id && id_len > 0 && id_len <= PGR_ACCOUNTS_MAX_UTF16) {
    ev.id_len = id_len;
    std::memcpy(ev.id, id, static_cast<size_t>(id_len) * sizeof(uint16_t));
  }
  if (name && name_len > 0 && name_len <= PGR_ACCOUNTS_MAX_UTF16) {
    ev.name_len = name_len;
    std::memcpy(ev.name, name, static_cast<size_t>(name_len) * sizeof(uint16_t));
  }
  g_state.ring_count++;
}

void emit_simple_event(uint32_t kind, uint32_t reason = PGR_ACC_REASON_NONE,
                       bool available = false, bool future_captured = false) noexcept {
  std::lock_guard<std::mutex> lock(g_state.policy_mu);
  push_event_locked(kind, reason, available, future_captured, nullptr, 0, nullptr, 0);
}

void emit_logout_failed_and_clear_identity(uint32_t reason) noexcept {
  std::lock_guard<std::mutex> lock(g_state.policy_mu);
  // This helper is used only after Wrapper.Logout was invoked. Its effects may
  // have started even when a getter, future, or wrapper later fails.
  g_state.uncertain_op_tainted = true;
  clear_identity_locked();
  push_event_locked(PGR_ACC_EVT_LOGOUT_FAILED, reason, false, false, nullptr, 0, nullptr, 0);
}

uintptr_t alloc_gc_handle_owner(Il2CppObject* obj) noexcept {
  if (!obj) return 0;
  const uintptr_t h = g_state.cfg.gchandle_new(obj, false);
  if (h != 0) {
    g_state.active_gc_roots.fetch_add(1, std::memory_order_acq_rel);
  }
  return h;
}

void free_gc_handle_slot_owner(uintptr_t* slot) noexcept {
  if (!slot) return;
  const uintptr_t h = *slot;
  if (h != 0) {
    *slot = 0;
    g_state.cfg.gchandle_free(h);
    g_state.active_gc_roots.fetch_sub(1, std::memory_order_acq_rel);
  }
}

void free_logout_handles_owner() noexcept {
  free_gc_handle_slot_owner(&g_state.future_handle);
  free_gc_handle_slot_owner(&g_state.auth_handle);
}

void finalize_deferred_clear_owner() noexcept {
  free_logout_handles_owner();
  const bool was_busy = g_state.logging_out.load(std::memory_order_relaxed) ||
                        g_state.in_wrapper_logout_call;
  g_state.logging_out.store(false, std::memory_order_release);
  g_state.in_wrapper_logout_call = false;
  g_state.logout_started_ms = 0;
  g_state.next_tick_ms = 0;
  {
    std::lock_guard<std::mutex> lock(g_state.policy_mu);
    if (was_busy) {
      g_state.uncertain_op_tainted = true;
    }
    clear_identity_locked();
    g_state.deferred_clear_requested = false;
  }
}

struct ActiveFrameGuard {
  ActiveFrameGuard() noexcept {
    g_state.active_depth.fetch_add(1, std::memory_order_acq_rel);
  }
  ActiveFrameGuard(const ActiveFrameGuard&) = delete;
  ActiveFrameGuard& operator=(const ActiveFrameGuard&) = delete;
  ~ActiveFrameGuard() noexcept {
    if (g_state.active_depth.load(std::memory_order_acquire) == 1) {
      bool need_cleanup = false;
      {
        std::lock_guard<std::mutex> lock(g_state.policy_mu);
        need_cleanup = g_state.deferred_clear_requested;
      }
      if (need_cleanup) {
        finalize_deferred_clear_owner();
      }
    }
    g_state.active_depth.fetch_sub(1, std::memory_order_acq_rel);
  }
};

struct ScopedTickFlag {
  ScopedTickFlag() noexcept { g_state.in_tick = true; }
  ~ScopedTickFlag() noexcept { g_state.in_tick = false; }
};

struct ScopedGcRoot {
  uintptr_t handle = 0;
  ScopedGcRoot() noexcept = default;
  explicit ScopedGcRoot(Il2CppObject* obj) noexcept {
    reset(obj);
  }
  ScopedGcRoot(const ScopedGcRoot&) = delete;
  ScopedGcRoot& operator=(const ScopedGcRoot&) = delete;
  ~ScopedGcRoot() noexcept {
    reset(nullptr);
  }
  void reset(Il2CppObject* obj) noexcept {
    free_gc_handle_slot_owner(&handle);
    if (obj != nullptr) {
      handle = alloc_gc_handle_owner(obj);
    }
  }
  Il2CppObject* target() const noexcept {
    return handle != 0 ? g_state.cfg.gchandle_get_target(handle) : nullptr;
  }
};

bool is_exact_class(Il2CppObject* obj, Il2CppClass* expected) noexcept {
  if (!obj || !expected) return false;
  return g_state.cfg.object_get_class(obj) == expected;
}

bool is_allowed_static_method(const MethodInfo* method) noexcept {
  const auto& c = g_state.cfg;
  return method != nullptr &&
         (method == c.prefs_has_key_method ||
          method == c.prefs_delete_key_method ||
          method == c.prefs_save_method ||
          method == c.find_object_of_type_method);
}

bool is_allowed_instance_method(const MethodInfo* method) noexcept {
  const auto& c = g_state.cfg;
  return method != nullptr &&
         (method == c.map_get_container_method ||
          method == c.container_resolve_method ||
          method == c.player_get_id_method ||
          method == c.player_get_name_method ||
          method == c.auth_get_connected_method ||
          method == c.auth_get_cached_method ||
          method == c.wrapper_logout_method);
}

bool invoke_checked(const MethodInfo* method, Il2CppObject* instance,
                    void** params, Il2CppObject** out_ret) noexcept {
  if (out_ret) *out_ret = nullptr;
  if (!method) return false;
  if (instance == nullptr) {
    if (!is_allowed_static_method(method)) return false;
  } else {
    if (!is_allowed_instance_method(method)) return false;
  }
  Il2CppObject* exc = nullptr;
  Il2CppObject* ret = g_state.cfg.runtime_invoke(method, instance, params, &exc);
  if (exc != nullptr) {
    return false;
  }
  if (out_ret) *out_ret = ret;
  return true;
}

bool invoke_bool_getter(const MethodInfo* method, Il2CppObject* instance,
                        void** params, bool* out_val) noexcept {
  if (!out_val) return false;
  Il2CppObject* boxed = nullptr;
  if (!invoke_checked(method, instance, params, &boxed) || !boxed) {
    return false;
  }
  if (!is_exact_class(boxed, g_state.cfg.boolean_class)) {
    return false;
  }
  void* raw = g_state.cfg.object_unbox(boxed);
  if (!raw) return false;
  *out_val = (*reinterpret_cast<const uint8_t*>(raw) != 0);
  return true;
}

bool read_utf16_bounded(Il2CppObject* str_obj, uint16_t* out_buf,
                        uint16_t* out_len) noexcept {
  if (!str_obj || !out_buf || !out_len) return false;
  if (!is_exact_class(str_obj, g_state.cfg.string_class)) return false;
  auto* s = reinterpret_cast<Il2CppString*>(str_obj);
  const int32_t len = g_state.cfg.string_length(s);
  if (len < 1 || len > static_cast<int32_t>(PGR_ACCOUNTS_MAX_UTF16)) {
    return false;
  }
  const uint16_t* chars = g_state.cfg.string_chars(s);
  if (!chars) return false;
  std::memcpy(out_buf, chars, static_cast<size_t>(len) * sizeof(uint16_t));
  *out_len = static_cast<uint16_t>(len);
  return true;
}

bool can_mutate_now(uint64_t expected_epoch) noexcept {
  const uint64_t now_bt = clock_boottime_ms();
  if (now_bt == 0 || !g_state.enabled.load(std::memory_order_acquire)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_state.policy_mu);
  if (g_state.deferred_clear_requested ||
      g_state.mutation_epoch != expected_epoch ||
      now_bt >= g_state.lease_until_ms) {
    return false;
  }
  return true;
}

bool resolve_service_owner(Il2CppClass* contract_class,
                           Il2CppClass* expected_impl_class,
                           ScopedGcRoot* out_service_root) noexcept {
  if (!contract_class || !expected_impl_class || !out_service_root) return false;

  const Il2CppType* map_type = g_state.cfg.class_get_type(g_state.cfg.map_scene_context_class);
  if (!map_type) return false;
  Il2CppObject* map_type_obj = g_state.cfg.type_get_object(map_type);
  if (!map_type_obj) return false;
  ScopedGcRoot map_type_root(map_type_obj);
  if (!map_type_root.handle) return false;

  Il2CppObject* map_type_target = map_type_root.target();
  if (!map_type_target) return false;
  void* find_args[1] = {map_type_target};
  Il2CppObject* map_ctx = nullptr;
  if (!invoke_checked(g_state.cfg.find_object_of_type_method, nullptr, find_args, &map_ctx) ||
      !map_ctx || !is_exact_class(map_ctx, g_state.cfg.map_scene_context_class)) {
    return false;
  }
  ScopedGcRoot map_root(map_ctx);
  if (!map_root.handle) return false;

  Il2CppObject* map_target = map_root.target();
  if (!map_target) return false;
  Il2CppObject* container = nullptr;
  if (!invoke_checked(g_state.cfg.map_get_container_method, map_target, nullptr, &container) ||
      !container || !is_exact_class(container, g_state.cfg.di_container_class)) {
    return false;
  }
  ScopedGcRoot container_root(container);
  if (!container_root.handle) return false;

  const Il2CppType* contract_type = g_state.cfg.class_get_type(contract_class);
  if (!contract_type) return false;
  Il2CppObject* contract_type_obj = g_state.cfg.type_get_object(contract_type);
  if (!contract_type_obj) return false;
  ScopedGcRoot contract_type_root(contract_type_obj);
  if (!contract_type_root.handle) return false;

  Il2CppObject* container_target = container_root.target();
  Il2CppObject* contract_target = contract_type_root.target();
  if (!container_target || !contract_target) return false;
  void* resolve_args[1] = {contract_target};
  Il2CppObject* service = nullptr;
  if (!invoke_checked(g_state.cfg.container_resolve_method, container_target, resolve_args, &service) ||
      !service || !is_exact_class(service, expected_impl_class)) {
    return false;
  }
  out_service_root->reset(service);
  return out_service_root->handle != 0;
}

Il2CppObject* hook_auth_logout(Il2CppObject* self, const MethodInfo* method) noexcept {
  AuthLogoutFn orig = g_state.orig_auth_logout.load(std::memory_order_acquire);
  if (!orig) {
    std::lock_guard<std::mutex> lock(g_state.hook_install_mu);
    orig = g_state.orig_auth_logout.load(std::memory_order_acquire);
  }

  const int32_t tid = current_os_tid();
  const int32_t owner = g_state.owner_tid.load(std::memory_order_acquire);
  const bool is_owner = (owner != 0 && tid == owner);
  const bool active_enabled = g_state.enabled.load(std::memory_order_acquire);

  if (!is_owner || !active_enabled) {
    return orig ? orig(self, method) : nullptr;
  }

  ActiveFrameGuard frame_guard;
  uint64_t entry_epoch = 0;
  {
    std::lock_guard<std::mutex> lock(g_state.policy_mu);
    entry_epoch = g_state.mutation_epoch;
  }

  const bool valid_self = is_exact_class(self, g_state.cfg.auth_class);
  const bool capture_for_our_request =
      valid_self && g_state.logging_out.load(std::memory_order_relaxed) &&
      g_state.in_wrapper_logout_call;

  if (valid_self) {
    std::lock_guard<std::mutex> lock(g_state.policy_mu);
    if (!g_state.deferred_clear_requested) {
      clear_identity_locked();
      push_event_locked(PGR_ACC_EVT_SESSION_ENDED, PGR_ACC_REASON_NONE,
                        false, false, nullptr, 0, nullptr, 0);
    }
  }

  Il2CppObject* ret = orig ? orig(self, method) : nullptr;

  bool allow_post_mutation = false;
  {
    std::lock_guard<std::mutex> lock(g_state.policy_mu);
    allow_post_mutation = g_state.enabled.load(std::memory_order_acquire) &&
                          !g_state.deferred_clear_requested &&
                          (g_state.mutation_epoch == entry_epoch);
  }

  if (allow_post_mutation && capture_for_our_request &&
      is_exact_class(ret, g_state.cfg.future_class)) {
    free_gc_handle_slot_owner(&g_state.future_handle);
    g_state.future_handle = alloc_gc_handle_owner(ret);
  }

  return ret;
}

void poll_logout_owner(uint64_t now_ms) noexcept {
  if (!g_state.future_handle && g_state.auth_handle) {
    Il2CppObject* a = g_state.cfg.gchandle_get_target(g_state.auth_handle);
    if (!is_exact_class(a, g_state.cfg.auth_class)) {
      free_logout_handles_owner();
      g_state.logging_out.store(false, std::memory_order_release);
      emit_logout_failed_and_clear_identity(PGR_ACC_REASON_LOGOUT_AUTH_INVALID);
      return;
    }
    bool is_conn = true;
    if (!invoke_bool_getter(g_state.cfg.auth_get_connected_method, a, nullptr, &is_conn)) {
      free_logout_handles_owner();
      g_state.logging_out.store(false, std::memory_order_release);
      emit_logout_failed_and_clear_identity(PGR_ACC_REASON_MISSING_AUTH_STATE);
      return;
    }
    Il2CppObject* refreshed_a = g_state.cfg.gchandle_get_target(g_state.auth_handle);
    if (!is_exact_class(refreshed_a, g_state.cfg.auth_class)) {
      free_logout_handles_owner();
      g_state.logging_out.store(false, std::memory_order_release);
      emit_logout_failed_and_clear_identity(PGR_ACC_REASON_LOGOUT_AUTH_INVALID);
      return;
    }
    void* active_provider = nullptr;
    g_state.cfg.field_get_value(refreshed_a, g_state.cfg.auth_active_provider_field, &active_provider);
    bool has_cached = true;
    if (!invoke_bool_getter(g_state.cfg.auth_get_cached_method, refreshed_a, nullptr, &has_cached)) {
      free_logout_handles_owner();
      g_state.logging_out.store(false, std::memory_order_release);
      emit_logout_failed_and_clear_identity(PGR_ACC_REASON_MISSING_AUTH_STATE);
      return;
    }
    if (!is_conn && active_provider == nullptr && !has_cached) {
      free_logout_handles_owner();
      g_state.logging_out.store(false, std::memory_order_release);
      std::lock_guard<std::mutex> lock(g_state.policy_mu);
      clear_identity_locked();
      push_event_locked(PGR_ACC_EVT_LOGOUT_COMPLETE, PGR_ACC_REASON_CLEARED_AUTH_STATE,
                        false, false, nullptr, 0, nullptr, 0);
      return;
    }
  }

  if (g_state.future_handle && g_state.auth_handle) {
    Il2CppObject* f = g_state.cfg.gchandle_get_target(g_state.future_handle);
    Il2CppObject* a = g_state.cfg.gchandle_get_target(g_state.auth_handle);
    if (!is_exact_class(f, g_state.cfg.future_class) ||
        !is_exact_class(a, g_state.cfg.auth_class)) {
      free_logout_handles_owner();
      g_state.logging_out.store(false, std::memory_order_release);
      emit_logout_failed_and_clear_identity(PGR_ACC_REASON_LOGOUT_FUTURE_INVALID);
      return;
    }
    uint8_t is_disposed = 1;
    g_state.cfg.field_get_value(f, g_state.cfg.future_disposed_field, &is_disposed);
    if (is_disposed != 0) {
      free_logout_handles_owner();
      g_state.logging_out.store(false, std::memory_order_release);
      emit_logout_failed_and_clear_identity(PGR_ACC_REASON_LOGOUT_FUTURE_INVALID);
      return;
    }
    int32_t status_val = 0;
    g_state.cfg.field_get_value(f, g_state.cfg.future_status_field, &status_val);
    if (status_val == g_state.cfg.promise_failed_value) {
      free_logout_handles_owner();
      g_state.logging_out.store(false, std::memory_order_release);
      emit_logout_failed_and_clear_identity(PGR_ACC_REASON_LOGOUT_FAILED);
      return;
    }
    if (status_val == g_state.cfg.promise_succeeded_value) {
      bool is_conn = true;
      if (!invoke_bool_getter(g_state.cfg.auth_get_connected_method, a, nullptr, &is_conn)) {
        free_logout_handles_owner();
        g_state.logging_out.store(false, std::memory_order_release);
        emit_logout_failed_and_clear_identity(PGR_ACC_REASON_MISSING_AUTH_STATE);
        return;
      }
  if (!is_conn) {
        free_logout_handles_owner();
        g_state.logging_out.store(false, std::memory_order_release);
        emit_simple_event(PGR_ACC_EVT_LOGOUT_COMPLETE, PGR_ACC_REASON_NONE);
        return;
      }
    }
  }

  if (now_ms >= g_state.logout_started_ms &&
      (now_ms - g_state.logout_started_ms) > PGR_ACCOUNTS_LOGOUT_TIMEOUT_MS) {
    free_logout_handles_owner();
    g_state.logging_out.store(false, std::memory_order_release);
    emit_logout_failed_and_clear_identity(PGR_ACC_REASON_LOGOUT_TIMEOUT);
  }
}

// Local mutations may partially apply before an exception or reentrant stop.
struct MutationOutcome {
  bool attempted = false;
  bool completed = false;
  ~MutationOutcome() noexcept {
    if (attempted && !completed) {
      std::lock_guard<std::mutex> lock(g_state.policy_mu);
      g_state.uncertain_op_tainted = true;
    }
  }
};

void execute_save_session_owner(uint64_t epoch) noexcept {
  MutationOutcome outcome;
  if (!can_mutate_now(epoch)) {
    emit_simple_event(PGR_ACC_EVT_LOGOUT_FAILED, PGR_ACC_REASON_SESSION_SAVE_UNAVAILABLE);
    return;
  }
  outcome.attempted = true;
  if (!invoke_checked(g_state.cfg.prefs_save_method, nullptr, nullptr, nullptr)) {
    emit_simple_event(PGR_ACC_EVT_LOGOUT_FAILED, PGR_ACC_REASON_SESSION_SAVE_UNAVAILABLE);
    return;
  }
  if (!can_mutate_now(epoch)) return;
  outcome.completed = true;
  emit_simple_event(PGR_ACC_EVT_SESSION_SAVED, PGR_ACC_REASON_NONE);
}

void execute_reset_login_owner(uint64_t epoch) noexcept {
  MutationOutcome outcome;
  const char* const keys[2] = {kDobKey1, kDobKey2};
  for (size_t i = 0; i < 2; ++i) {
    Il2CppString* s = g_state.cfg.string_new(keys[i]);
    if (!s) {
      emit_simple_event(PGR_ACC_EVT_LOGOUT_FAILED, PGR_ACC_REASON_LOGIN_RESET_UNAVAILABLE);
      return;
    }
    ScopedGcRoot key_root(reinterpret_cast<Il2CppObject*>(s));
    if (!key_root.handle) {
      emit_simple_event(PGR_ACC_EVT_LOGOUT_FAILED, PGR_ACC_REASON_LOGIN_RESET_UNAVAILABLE);
      return;
    }
    Il2CppObject* key_target = key_root.target();
    if (!key_target || !can_mutate_now(epoch)) {
      emit_simple_event(PGR_ACC_EVT_LOGOUT_FAILED, PGR_ACC_REASON_LOGIN_RESET_UNAVAILABLE);
      return;
    }
    void* args[1] = {key_target};
    outcome.attempted = true;
    if (!invoke_checked(g_state.cfg.prefs_delete_key_method, nullptr, args, nullptr)) {
      emit_simple_event(PGR_ACC_EVT_LOGOUT_FAILED, PGR_ACC_REASON_LOGIN_RESET_UNAVAILABLE);
      return;
    }
    Il2CppObject* refreshed_key = key_root.target();
    if (!refreshed_key) {
      emit_simple_event(PGR_ACC_EVT_LOGOUT_FAILED, PGR_ACC_REASON_LOGIN_RESET_UNAVAILABLE);
      return;
    }
    void* has_args[1] = {refreshed_key};
    bool still_has = true;
    if (!invoke_bool_getter(g_state.cfg.prefs_has_key_method, nullptr, has_args, &still_has) ||
        still_has) {
      emit_simple_event(PGR_ACC_EVT_LOGOUT_FAILED, PGR_ACC_REASON_LOGIN_RESET_NOT_SAVED);
      return;
    }
  }
  if (!can_mutate_now(epoch) ||
      !invoke_checked(g_state.cfg.prefs_save_method, nullptr, nullptr, nullptr)) {
    emit_simple_event(PGR_ACC_EVT_LOGOUT_FAILED, PGR_ACC_REASON_LOGIN_RESET_UNAVAILABLE);
    return;
  }
  if (!can_mutate_now(epoch)) return;
  outcome.completed = true;
  emit_simple_event(PGR_ACC_EVT_LOGIN_RESET_COMPLETE, PGR_ACC_REASON_NONE);
}

bool api_init_impl(const pgrazor_accounts_config_v1_t* cfg) noexcept {
  if (g_state.hook_failed_latched.load(std::memory_order_acquire) ||
      !validate_config(cfg)) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(g_state.policy_mu);
    if (!g_state.config_bound.load(std::memory_order_relaxed)) {
      g_state.cfg = *cfg;
      g_state.config_bound.store(true, std::memory_order_release);
    } else if (std::memcmp(&g_state.cfg, cfg, sizeof(*cfg)) != 0) {
      return false;
    }
  }

  if (!g_state.hook_installed.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> hook_lock(g_state.hook_install_mu);
    if (g_state.hook_failed_latched.load(std::memory_order_acquire)) {
      return false;
    }
    if (!g_state.hook_installed.load(std::memory_order_relaxed)) {
      void* orig_fn = nullptr;
      const int rc = pgrazor_hook(
          g_state.cfg.auth_logout_target_fn,
          reinterpret_cast<void*>(&hook_auth_logout),
          &orig_fn);
      if (rc != 0 || orig_fn == nullptr) {
        g_state.hook_failed_latched.store(true, std::memory_order_release);
        return false;
      }
      g_state.orig_auth_logout.store(reinterpret_cast<AuthLogoutFn>(orig_fn),
                                     std::memory_order_release);
      g_state.hook_installed.store(true, std::memory_order_release);
    }
  }

  {
    std::lock_guard<std::mutex> lock(g_state.policy_mu);
    if (g_state.queue_overflow_latched ||
        g_state.hook_failed_latched.load(std::memory_order_acquire)) {
      return false;
    }
    // Preserve deferred_clear_requested if true so owner thread finalizes stale roots first.
    g_state.mutation_epoch++;
    g_state.enabled.store(true, std::memory_order_release);
    push_event_locked(PGR_ACC_EVT_READY, PGR_ACC_REASON_NONE, true, false,
                      nullptr, 0, nullptr, 0);
  }
  return true;
}

bool api_update_impl(bool allowed, uint32_t lease_ms,
                     uint32_t cmd_kind, uint64_t cmd_token) noexcept {
  if (!g_state.config_bound.load(std::memory_order_acquire) ||
      !g_state.hook_installed.load(std::memory_order_acquire) ||
      g_state.hook_failed_latched.load(std::memory_order_acquire)) {
    return false;
  }
  const uint64_t now_bt = clock_boottime_ms();
  if (now_bt == 0) return false;
  const uint32_t clamped_lease =
      lease_ms > PGR_ACCOUNTS_MAX_LEASE_MS ? PGR_ACCOUNTS_MAX_LEASE_MS : lease_ms;

  std::lock_guard<std::mutex> lock(g_state.policy_mu);
  if (g_state.queue_overflow_latched || !g_state.enabled.load(std::memory_order_acquire)) {
    return false;
  }
  if (!allowed || clamped_lease == 0) {
    g_state.mutation_epoch++;
    g_state.lease_until_ms = 0;
    g_state.pending_cmd_kind = PGR_ACC_CMD_NONE;
    g_state.pending_cmd_token = 0;
    return true;
  }
  g_state.lease_until_ms = now_bt + clamped_lease;
  if (cmd_kind == PGR_ACC_CMD_NONE) {
    g_state.pending_cmd_kind = PGR_ACC_CMD_NONE;
    g_state.pending_cmd_token = 0;
  } else if (cmd_token > g_state.last_accepted_token && !g_state.uncertain_op_tainted) {
    g_state.last_accepted_token = cmd_token;
    g_state.pending_cmd_kind = cmd_kind;
    g_state.pending_cmd_token = cmd_token;
  }
  return true;
}

void api_tick_impl(uint64_t /*caller_now_ms*/, int32_t caller_tid) noexcept {
  if (!g_state.config_bound.load(std::memory_order_acquire) ||
      !g_state.hook_installed.load(std::memory_order_acquire)) {
    return;
  }
  const int32_t real_tid = current_os_tid();
  if (real_tid <= 0 || caller_tid != real_tid) {
    return;
  }
  int32_t expected_owner = 0;
  if (!g_state.owner_tid.compare_exchange_strong(
          expected_owner, real_tid, std::memory_order_acq_rel)) {
    if (expected_owner != real_tid) {
      return;
    }
  }

  // Reject nested ticks on the owner thread.
  if (g_state.in_tick || g_state.active_depth.load(std::memory_order_acquire) > 0) {
    return;
  }

  ActiveFrameGuard frame_guard;
  ScopedTickFlag tick_flag;

  bool need_cleanup = false;
  {
    std::lock_guard<std::mutex> lock(g_state.policy_mu);
    need_cleanup = g_state.deferred_clear_requested;
  }
  if (need_cleanup) {
    finalize_deferred_clear_owner();
  }

  if (!g_state.enabled.load(std::memory_order_acquire)) {
    return;
  }

  const uint64_t now_bt = clock_boottime_ms();
  if (now_bt == 0 || now_bt < g_state.next_tick_ms) {
    return;
  }
  g_state.next_tick_ms = now_bt + PGR_ACCOUNTS_TICK_INTERVAL_MS;

  if (g_state.logging_out.load(std::memory_order_relaxed)) {
    poll_logout_owner(now_bt);
    return;
  }

  uint64_t lease_until = 0;
  uint32_t cmd_kind = PGR_ACC_CMD_NONE;
  uint64_t cmd_token = 0;
  uint64_t epoch = 0;
  {
    std::lock_guard<std::mutex> lock(g_state.policy_mu);
    lease_until = g_state.lease_until_ms;
    epoch = g_state.mutation_epoch;
    if (now_bt < lease_until && !g_state.uncertain_op_tainted &&
        !g_state.deferred_clear_requested &&
        g_state.pending_cmd_kind != PGR_ACC_CMD_NONE &&
        g_state.pending_cmd_token > g_state.last_consumed_token) {
      cmd_kind = g_state.pending_cmd_kind;
      cmd_token = g_state.pending_cmd_token;
      if (cmd_kind == PGR_ACC_CMD_SAVE_SESSION ||
          cmd_kind == PGR_ACC_CMD_RESET_LOGIN) {
        g_state.last_consumed_token = cmd_token;
        g_state.pending_cmd_kind = PGR_ACC_CMD_NONE;
        g_state.pending_cmd_token = 0;
      }
    }
  }

  if (now_bt >= lease_until) {
    return;
  }

  if (cmd_kind == PGR_ACC_CMD_SAVE_SESSION) {
    execute_save_session_owner(epoch);
    return;
  }

  if (cmd_kind == PGR_ACC_CMD_RESET_LOGIN) {
    execute_reset_login_owner(epoch);
    return;
  }

  ScopedGcRoot auth_root;
  if (!resolve_service_owner(g_state.cfg.auth_contract_class,
                             g_state.cfg.auth_class, &auth_root)) {
    return;
  }
  Il2CppObject* auth_obj = auth_root.target();
  bool is_conn = false;
  if (!auth_obj || !invoke_bool_getter(g_state.cfg.auth_get_connected_method,
                                       auth_obj, nullptr, &is_conn)) {
    g_state.next_tick_ms = now_bt + PGR_ACCOUNTS_ERROR_BACKOFF_MS;
    emit_simple_event(PGR_ACC_EVT_READ_ERROR, PGR_ACC_REASON_MISSING_AUTH_STATE);
    return;
  }
  if (!is_conn) {
    return;
  }

  ScopedGcRoot player_root;
  if (!resolve_service_owner(g_state.cfg.player_contract_class,
                             g_state.cfg.player_class, &player_root)) {
    return;
  }

  Il2CppObject* player_obj = player_root.target();
  Il2CppObject* id_obj = nullptr;
  if (!player_obj || !invoke_checked(g_state.cfg.player_get_id_method,
                                     player_obj, nullptr, &id_obj)) {
    g_state.next_tick_ms = now_bt + PGR_ACCOUNTS_ERROR_BACKOFF_MS;
    emit_simple_event(PGR_ACC_EVT_READ_ERROR, PGR_ACC_REASON_INVOCATION_EXCEPTION);
    return;
  }
  ScopedGcRoot id_root(id_obj);

  player_obj = player_root.target();
  Il2CppObject* name_obj = nullptr;
  if (!player_obj || !invoke_checked(g_state.cfg.player_get_name_method,
                                     player_obj, nullptr, &name_obj)) {
    g_state.next_tick_ms = now_bt + PGR_ACCOUNTS_ERROR_BACKOFF_MS;
    emit_simple_event(PGR_ACC_EVT_READ_ERROR, PGR_ACC_REASON_INVOCATION_EXCEPTION);
    return;
  }
  ScopedGcRoot name_root(name_obj);

  uint16_t id_buf[PGR_ACCOUNTS_MAX_UTF16]{};
  uint16_t name_buf[PGR_ACCOUNTS_MAX_UTF16]{};
  uint16_t id_len = 0;
  uint16_t name_len = 0;
  if (!read_utf16_bounded(id_root.target(), id_buf, &id_len) ||
      !read_utf16_bounded(name_root.target(), name_buf, &name_len)) {
    return;
  }

  if (cmd_kind == PGR_ACC_CMD_LOGOUT) {
    ScopedGcRoot wrapper_root;
    if (!resolve_service_owner(g_state.cfg.wrapper_contract_class,
                               g_state.cfg.wrapper_class, &wrapper_root)) {
      g_state.next_tick_ms = now_bt + PGR_ACCOUNTS_ERROR_BACKOFF_MS;
      emit_simple_event(PGR_ACC_EVT_READ_ERROR, PGR_ACC_REASON_LOGOUT_WRAPPER_INVALID);
      return;
    }
    Il2CppObject* refreshed_auth = auth_root.target();
    if (!refreshed_auth || !wrapper_root.target()) {
      g_state.next_tick_ms = now_bt + PGR_ACCOUNTS_ERROR_BACKOFF_MS;
      emit_simple_event(PGR_ACC_EVT_READ_ERROR, PGR_ACC_REASON_LOGOUT_WRAPPER_INVALID);
      return;
    }
    free_logout_handles_owner();
    g_state.auth_handle = alloc_gc_handle_owner(refreshed_auth);
    if (!g_state.auth_handle) {
      g_state.next_tick_ms = now_bt + PGR_ACCOUNTS_ERROR_BACKOFF_MS;
      emit_simple_event(PGR_ACC_EVT_READ_ERROR, PGR_ACC_REASON_LOGOUT_AUTH_INVALID);
      return;
    }

    Il2CppObject* refreshed_wrapper = wrapper_root.target();
    const uint64_t pre_call_bt = clock_boottime_ms();
    bool can_proceed = false;
    {
      std::lock_guard<std::mutex> lock(g_state.policy_mu);
      if (refreshed_wrapper != nullptr &&
          g_state.enabled.load(std::memory_order_acquire) &&
          !g_state.deferred_clear_requested &&
          !g_state.uncertain_op_tainted &&
          g_state.mutation_epoch == epoch &&
          g_state.pending_cmd_kind == PGR_ACC_CMD_LOGOUT &&
          g_state.pending_cmd_token == cmd_token &&
          cmd_token > g_state.last_consumed_token &&
          pre_call_bt != 0 && pre_call_bt < g_state.lease_until_ms) {
        g_state.last_consumed_token = cmd_token;
        g_state.pending_cmd_kind = PGR_ACC_CMD_NONE;
        g_state.pending_cmd_token = 0;
        can_proceed = true;
      }
    }
    if (!can_proceed) {
      free_logout_handles_owner();
      emit_simple_event(PGR_ACC_EVT_LOGOUT_FAILED, PGR_ACC_REASON_LEASE_EXPIRED);
      return;
    }

    g_state.logging_out.store(true, std::memory_order_release);
    g_state.logout_started_ms = pre_call_bt;
    g_state.in_wrapper_logout_call = true;
    const bool ok = invoke_checked(g_state.cfg.wrapper_logout_method,
                                   refreshed_wrapper, nullptr, nullptr);
    g_state.in_wrapper_logout_call = false;

    bool revoked = false;
    {
      std::lock_guard<std::mutex> lock(g_state.policy_mu);
      revoked = g_state.deferred_clear_requested || (g_state.mutation_epoch != epoch);
    }
    if (revoked) {
      return;
    }
    if (!ok) {
      free_logout_handles_owner();
      g_state.logging_out.store(false, std::memory_order_release);
      emit_logout_failed_and_clear_identity(PGR_ACC_REASON_INVOCATION_EXCEPTION);
      return;
    }
    emit_simple_event(PGR_ACC_EVT_LOGOUT_REQUESTED, PGR_ACC_REASON_NONE,
                      false, g_state.future_handle != 0);
    return;
  }

  {
    const uint64_t publish_bt = clock_boottime_ms();
    std::lock_guard<std::mutex> lock(g_state.policy_mu);
    if (!g_state.enabled.load(std::memory_order_acquire) ||
        g_state.deferred_clear_requested ||
        g_state.mutation_epoch != epoch ||
        publish_bt == 0 || publish_bt >= g_state.lease_until_ms) {
      return;
    }
    const bool same_id =
        g_state.snap_has_identity && g_state.snap_id_len == id_len &&
        std::memcmp(g_state.snap_id, id_buf, static_cast<size_t>(id_len) * sizeof(uint16_t)) == 0;
    if (!same_id || (publish_bt - g_state.snap_last_identity_ms) >= PGR_ACCOUNTS_IDENTITY_HEARTBEAT_MS) {
      g_state.snap_has_identity = true;
      g_state.snap_id_len = id_len;
      g_state.snap_name_len = name_len;
      std::memset(g_state.snap_id, 0, sizeof(g_state.snap_id));
      std::memset(g_state.snap_name, 0, sizeof(g_state.snap_name));
      std::memcpy(g_state.snap_id, id_buf, static_cast<size_t>(id_len) * sizeof(uint16_t));
      std::memcpy(g_state.snap_name, name_buf, static_cast<size_t>(name_len) * sizeof(uint16_t));
      g_state.snap_last_identity_ms = publish_bt;
      push_event_locked(PGR_ACC_EVT_IDENTITY, PGR_ACC_REASON_NONE, true, false,
                        id_buf, id_len, name_buf, name_len);
    }
  }
}

bool api_clear_impl() noexcept {
  const int32_t tid = current_os_tid();
  const int32_t owner = g_state.owner_tid.load(std::memory_order_acquire);
  if (owner == 0 || tid != owner) {
    return false;
  }

  g_state.enabled.store(false, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(g_state.policy_mu);
    g_state.mutation_epoch++;
    g_state.lease_until_ms = 0;
    g_state.pending_cmd_kind = PGR_ACC_CMD_NONE;
    g_state.pending_cmd_token = 0;
    clear_identity_locked();
    if (g_state.logging_out.load(std::memory_order_relaxed) ||
        g_state.in_wrapper_logout_call ||
        g_state.active_depth.load(std::memory_order_acquire) > 0) {
      g_state.uncertain_op_tainted = true;
    }
    g_state.deferred_clear_requested = true;
    if (g_state.active_depth.load(std::memory_order_acquire) > 0) {
      // Outermost ActiveFrameGuard epilogue on the owner thread will finalize.
      return true;
    }
  }

  ActiveFrameGuard cleanup_frame;
  return true;
}

void api_sample_impl(pgrazor_account_snapshot_v1_t* out_snapshot) noexcept {
  if (!out_snapshot) return;
  std::memset(out_snapshot, 0, sizeof(*out_snapshot));
  std::lock_guard<std::mutex> lock(g_state.policy_mu);
  out_snapshot->seq = g_state.next_seq - 1;
  out_snapshot->last_identity_ms = g_state.snap_last_identity_ms;
  out_snapshot->last_accepted_token = g_state.last_accepted_token;
  out_snapshot->last_consumed_token = g_state.last_consumed_token;
  out_snapshot->owner_tid = g_state.owner_tid.load(std::memory_order_acquire);
  out_snapshot->active_depth = g_state.active_depth.load(std::memory_order_acquire);
  out_snapshot->active_gc_roots = g_state.active_gc_roots.load(std::memory_order_acquire);
  out_snapshot->configured = g_state.config_bound.load(std::memory_order_acquire) ? 1u : 0u;
  out_snapshot->hook_installed = g_state.hook_installed.load(std::memory_order_acquire) ? 1u : 0u;
  out_snapshot->hook_latched_fail = g_state.hook_failed_latched.load(std::memory_order_acquire) ? 1u : 0u;
  out_snapshot->logging_out = g_state.logging_out.load(std::memory_order_acquire) ? 1u : 0u;
  out_snapshot->queue_overflow_latched = g_state.queue_overflow_latched ? 1u : 0u;
  out_snapshot->has_identity = g_state.snap_has_identity ? 1u : 0u;
  out_snapshot->id_len = g_state.snap_id_len;
  out_snapshot->name_len = g_state.snap_name_len;
  std::memcpy(out_snapshot->id, g_state.snap_id, sizeof(out_snapshot->id));
  std::memcpy(out_snapshot->name, g_state.snap_name, sizeof(out_snapshot->name));
}

size_t api_drain_impl(pgrazor_account_event_v1_t* out_events, size_t max_events) noexcept {
  if (!out_events || max_events == 0) return 0;
  std::lock_guard<std::mutex> lock(g_state.policy_mu);
  const size_t n = g_state.ring_count < max_events ? g_state.ring_count : max_events;
  for (size_t i = 0; i < n; ++i) {
    const size_t idx = (g_state.ring_head + i) % PGR_ACCOUNTS_EVENT_QUEUE_CAP;
    out_events[i] = g_state.ring[idx];
  }
  g_state.ring_head = (g_state.ring_head + n) % PGR_ACCOUNTS_EVENT_QUEUE_CAP;
  g_state.ring_count -= n;
  return n;
}

const pgrazor_accounts_api_v1_t kAccountsApiV1 = {
    PGR_ACCOUNTS_API_VERSION,
    static_cast<uint32_t>(sizeof(pgrazor_accounts_api_v1_t)),
    &api_init_impl,
    &api_update_impl,
    &api_tick_impl,
    &api_clear_impl,
    &api_sample_impl,
    &api_drain_impl,
};

}  // namespace

extern "C" const pgrazor_accounts_api_v1_t* pgrazor_accounts_api(uint32_t version) {
  if (version != PGR_ACCOUNTS_API_VERSION) {
    return nullptr;
  }
  return &kAccountsApiV1;
}
