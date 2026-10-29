#include "vgp_policy.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <time.h>
#include <unistd.h>

// Feature 25 native VGP scheduling / retry policy. See vgp_policy.h for the ABI and the division of
// labour with the host JS. Built -fno-exceptions / -fno-rtti like every sibling: a JS "throw" path
// becomes a fail-closed negative return. This module installs NO hook and NEVER calls back into JS.
//
// State (all guarded by g_state_mutex unless atomic):
//   * g_seen[]  : a bounded, fixed-capacity quarantine of recent target keys with per-key monotonic
//                 expiry deadlines. A plain compacting array (<= 2048 entries); sweep() rebuilds it in
//                 place dropping expired keys, so lookups never chase deletion tombstones.
//   * g_next_ms : the scan-cadence deadline (arm()); due() is YES only once CLOCK_BOOTTIME passes it.
//   * g_turn    : the catch/spin alternation counter (order()).
//   * g_blocked : the uncertain-response latch; set by block(), cleared only by clear_block()/reset().
// A VGP mask toggle clears g_blocked but PRESERVES g_seen (clear_block does not touch the quarantine);
// only reset() drops it. All deadlines are now + delay on the single native CLOCK_BOOTTIME.

static_assert(sizeof(PgrVgpPolicyConfig) == 128, "config must be 128 bytes");
static_assert(sizeof(PgrVgpPolicySample) == 64, "sample must be 64 bytes");
static_assert(sizeof(PgrVgpPolicyApi) == 112, "table must be 112 bytes");
static_assert(offsetof(PgrVgpPolicyConfig, scan_interval_ms) == 8, "layout");
static_assert(offsetof(PgrVgpPolicyConfig, max_keys) == 64, "layout");
static_assert(offsetof(PgrVgpPolicyConfig, status_terminal) == 72, "layout");
static_assert(offsetof(PgrVgpPolicyConfig, status_retry) == 88, "layout");

namespace {

// --- lifetime / publication --------------------------------------------------------
std::mutex g_install_mutex;
std::mutex g_state_mutex;
PgrVgpPolicyConfig g_cfg{};
std::atomic<bool> g_installed{false};
bool g_install_attempted = false;

// Permanent Unity owner, latched once from a genuine on-thread call.
std::atomic<uint64_t> g_owner_tid{0};

// --- scheduling state (guarded by g_state_mutex) -----------------------------------
struct SeenKey {
  uint64_t deadline_ms;
  uint32_t len;
  char key[PGR_VP_MAX_KEY_LEN];
};
SeenKey g_seen[PGR_VP_MAX_KEYS];
uint32_t g_seen_size = 0;
uint64_t g_next_ms = 0;  // scan-cadence deadline
uint32_t g_turn = 0;     // catch/spin alternation
bool g_blocked = false;  // uncertain-response latch
uint64_t g_epoch = 0;

// --- telemetry mirrors (atomic so sample is lock-free) -----------------------------
std::atomic<uint64_t> g_scans{0}, g_marks{0}, g_evictions{0}, g_blocks{0}, g_drops{0};
std::atomic<uint64_t> g_full_rejects{0};

// --- clock / tid -------------------------------------------------------------------
uint64_t current_tid() { return static_cast<uint64_t>(static_cast<pid_t>(gettid())); }

bool read_boottime_ms(uint64_t *out_ms) {
  struct timespec ts {};
  if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0 || ts.tv_sec < 0 || ts.tv_nsec < 0) return false;
  *out_ms = static_cast<uint64_t>(ts.tv_sec) * 1000ULL + static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
  return true;
}

// --- owner latching ----------------------------------------------------------------
bool owner_ready(uint64_t tid) {
  if (tid == 0) return false;
  uint64_t owner = g_owner_tid.load(std::memory_order_acquire);
  if (!(owner == 0 || owner == tid)) return false;
  uint64_t expected = 0;
  g_owner_tid.compare_exchange_strong(expected, tid, std::memory_order_acq_rel);
  return current_tid() == tid && g_owner_tid.load(std::memory_order_acquire) == tid;
}

// --- quarantine helpers (caller holds g_state_mutex) -------------------------------
int find_key(const char *key, uint32_t len) {
  for (uint32_t i = 0; i < g_seen_size; ++i)
    if (g_seen[i].len == len && std::memcmp(g_seen[i].key, key, len) == 0) return static_cast<int>(i);
  return -1;
}

void sweep_locked(uint64_t now) {
  uint32_t w = 0;
  for (uint32_t r = 0; r < g_seen_size; ++r) {
    if (g_seen[r].deadline_ms > now) {
      if (w != r) g_seen[w] = g_seen[r];
      ++w;
    } else {
      g_evictions.fetch_add(1, std::memory_order_relaxed);
    }
  }
  g_seen_size = w;
}

bool valid_key(const char *key, uint32_t len) {
  return key != nullptr && len != 0 && len <= PGR_VP_MAX_KEY_LEN;
}

// --- config validation -------------------------------------------------------------
bool validate_config(const PgrVgpPolicyConfig *c, int *rc) {
  if (c == nullptr) { *rc = PGR_VP_ERR_NULL_ARG; return false; }
  if (c->version != PGR_VP_CONFIG_VERSION) { *rc = PGR_VP_ERR_BAD_VERSION; return false; }
  if (c->size != sizeof(PgrVgpPolicyConfig)) { *rc = PGR_VP_ERR_BAD_SIZE; return false; }
  if (c->max_keys == 0 || c->max_keys > PGR_VP_MAX_KEYS) { *rc = PGR_VP_ERR_BAD_CONFIG; return false; }
  if (c->scan_interval_ms == 0) { *rc = PGR_VP_ERR_BAD_CONFIG; return false; }
  *rc = PGR_VP_OK;
  return true;
}

// --- public API --------------------------------------------------------------------
int api_install(const PgrVgpPolicyConfig *config) {
  int rc = PGR_VP_OK;
  if (!validate_config(config, &rc)) return rc;
  uint64_t probe = 0;
  if (!read_boottime_ms(&probe)) return PGR_VP_ERR_CLOCK_FAILURE;

  std::lock_guard<std::mutex> install_lock(g_install_mutex);
  if (g_installed.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    if (std::memcmp(&g_cfg, config, sizeof(*config)) != 0) return PGR_VP_ERR_RECONFIG;
    return PGR_VP_OK;
  }
  if (g_install_attempted) return PGR_VP_ERR_NOT_INSTALLED;
  g_install_attempted = true;
  {
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    g_cfg = *config;
    g_seen_size = 0;
    g_next_ms = 0;
    g_turn = 0;
    g_blocked = false;
    g_epoch = 0;
  }
  g_installed.store(true, std::memory_order_release);
  return PGR_VP_OK;
}

// Owner-gate preamble shared by every owner-only call. Returns PGR_VP_OK when the caller owns the
// controller, else the appropriate negative error.
int owner_gate(uint64_t proven_tid) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_VP_ERR_NOT_INSTALLED;
  const uint64_t tid = current_tid();
  if (proven_tid == 0 || tid != proven_tid || !owner_ready(tid)) return PGR_VP_ERR_OFF_OWNER;
  return PGR_VP_OK;
}

int api_update(uint64_t epoch, uint64_t proven_tid) {
  const int g = owner_gate(proven_tid);
  if (g != PGR_VP_OK) return g;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  g_epoch = epoch;
  return PGR_VP_OK;
}

int api_reset(uint64_t proven_tid) {
  const int g = owner_gate(proven_tid);
  if (g != PGR_VP_OK) return g;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  g_seen_size = 0;
  g_next_ms = 0;
  g_turn = 0;
  g_blocked = false;
  g_drops.fetch_add(1, std::memory_order_relaxed);
  return PGR_VP_OK;
}

int api_due(uint64_t proven_tid) {
  const int g = owner_gate(proven_tid);
  if (g != PGR_VP_OK) return g;
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) return PGR_VP_ERR_CLOCK_FAILURE;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  if (g_blocked || now < g_next_ms) return PGR_VP_OK;
  g_scans.fetch_add(1, std::memory_order_relaxed);
  return PGR_VP_YES;
}

int api_order(uint64_t proven_tid, uint32_t *out_order) {
  if (out_order == nullptr) return PGR_VP_ERR_NULL_ARG;
  const int g = owner_gate(proven_tid);
  if (g != PGR_VP_OK) return g;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  *out_order = (g_turn++ & 1u) ? 1u : 0u;  // odd turn -> spin-first, mirrors (r.turn++%2)?['spin',...]
  return PGR_VP_OK;
}

int api_sweep(uint64_t proven_tid, uint32_t *out_size) {
  const int g = owner_gate(proven_tid);
  if (g != PGR_VP_OK) return g;
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) return PGR_VP_ERR_CLOCK_FAILURE;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  sweep_locked(now);
  if (out_size != nullptr) *out_size = g_seen_size;
  return (g_seen_size >= g_cfg.max_keys) ? PGR_VP_YES : PGR_VP_OK;
}

int api_seen(uint64_t proven_tid, const char *key, uint32_t key_len) {
  const int g = owner_gate(proven_tid);
  if (g != PGR_VP_OK) return g;
  if (!valid_key(key, key_len)) return PGR_VP_ERR_BAD_KEY;
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) return PGR_VP_ERR_CLOCK_FAILURE;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  const int idx = find_key(key, key_len);
  return (idx >= 0 && g_seen[idx].deadline_ms > now) ? PGR_VP_YES : PGR_VP_OK;
}

int api_touch(uint64_t proven_tid, const char *key, uint32_t key_len, uint64_t delay_ms) {
  const int g = owner_gate(proven_tid);
  if (g != PGR_VP_OK) return g;
  if (!valid_key(key, key_len)) return PGR_VP_ERR_BAD_KEY;
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) return PGR_VP_ERR_CLOCK_FAILURE;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  const int idx = find_key(key, key_len);
  if (idx >= 0) {
    g_seen[idx].deadline_ms = now + delay_ms;
    g_marks.fetch_add(1, std::memory_order_relaxed);
    return PGR_VP_OK;
  }
  if (g_seen_size >= g_cfg.max_keys) {
    g_full_rejects.fetch_add(1, std::memory_order_relaxed);
    return PGR_VP_FULL;
  }
  SeenKey &slot = g_seen[g_seen_size++];
  slot.deadline_ms = now + delay_ms;
  slot.len = key_len;
  std::memcpy(slot.key, key, key_len);
  g_marks.fetch_add(1, std::memory_order_relaxed);
  return PGR_VP_OK;
}

int api_arm(uint64_t proven_tid, uint64_t delay_ms) {
  const int g = owner_gate(proven_tid);
  if (g != PGR_VP_OK) return g;
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) return PGR_VP_ERR_CLOCK_FAILURE;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  g_next_ms = now + delay_ms;
  return PGR_VP_OK;
}

int api_block(uint64_t proven_tid) {
  const int g = owner_gate(proven_tid);
  if (g != PGR_VP_OK) return g;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  g_blocked = true;
  g_blocks.fetch_add(1, std::memory_order_relaxed);
  return PGR_VP_OK;
}

int api_clear_block(uint64_t proven_tid) {
  const int g = owner_gate(proven_tid);
  if (g != PGR_VP_OK) return g;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  g_blocked = false;  // quarantine (g_seen) deliberately preserved across a mask toggle
  return PGR_VP_OK;
}

int api_catch_delay(int32_t status, uint64_t *out_ms) {
  if (out_ms == nullptr) return PGR_VP_ERR_NULL_ARG;
  if (!g_installed.load(std::memory_order_acquire)) return PGR_VP_ERR_NOT_INSTALLED;
  for (int i = 0; i < 4; ++i)
    if (g_cfg.status_terminal[i] >= 0 && status == g_cfg.status_terminal[i]) {
      *out_ms = g_cfg.terminal_delay_ms;
      return PGR_VP_OK;
    }
  for (int i = 0; i < 4; ++i)
    if (g_cfg.status_retry[i] >= 0 && status == g_cfg.status_retry[i]) {
      *out_ms = g_cfg.retry_delay_ms;
      return PGR_VP_OK;
    }
  *out_ms = g_cfg.default_delay_ms;
  return PGR_VP_OK;
}

int api_sample(PgrVgpPolicySample *out) {
  if (out == nullptr) return PGR_VP_ERR_NULL_ARG;
  uint32_t size, turn, blocked;
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    size = g_seen_size;
    turn = g_turn;
    blocked = g_blocked ? 1u : 0u;
  }
  out->version = PGR_VP_SAMPLE_VERSION;
  out->size = sizeof(PgrVgpPolicySample);
  out->scans = g_scans.load(std::memory_order_relaxed);
  out->marks = g_marks.load(std::memory_order_relaxed);
  out->evictions = g_evictions.load(std::memory_order_relaxed);
  out->blocks = g_blocks.load(std::memory_order_relaxed);
  out->drops = g_drops.load(std::memory_order_relaxed);
  out->seen_size = size;
  out->turn = turn;
  out->blocked = blocked;
  out->full_rejects = static_cast<uint32_t>(g_full_rejects.load(std::memory_order_relaxed));
  return PGR_VP_OK;
}

const PgrVgpPolicyApi kApiTable = {
    PGR_VP_API_VERSION, static_cast<uint32_t>(sizeof(PgrVgpPolicyApi)),
    &api_install, &api_update, &api_reset,       &api_due,         &api_order,
    &api_sweep,   &api_seen,   &api_touch,        &api_arm,         &api_block,
    &api_clear_block, &api_catch_delay, &api_sample,
};

}  // namespace

extern "C" const void *pgrazor_vgp_policy_api(uint32_t version) {
  if (version != PGR_VP_API_VERSION) return nullptr;
  return &kApiTable;
}
