#include "shared_catch.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <time.h>
#include <unistd.h>

// Feature 51 native shared catch orchestration. See shared_catch.h for the ABI and the division of
// labour with the host JS and the leaf features (24 / 36-40 / 4 / 35 / 23). Built -fno-exceptions /
// -fno-rtti like every sibling. This module installs NO hook, makes NO managed/il2cpp call, and STORES
// NO MANAGED POINTER: it is pure bounded bookkeeping over opaque host slot/gui keys, so there is no
// deadlock window across a managed callback (there is none) and no GC rooting here (the host owns every
// root and frees it on the owner thread when it drains a CANCELLED/REAPED event).

static_assert(sizeof(PgrSharedCatchConfig) == 32, "config must be 32 bytes");
static_assert(sizeof(PgrSharedCatchDecision) == 40, "decision must be 40 bytes");
static_assert(sizeof(PgrSharedCatchEvent) == 24, "event must be 24 bytes");
static_assert(sizeof(PgrSharedCatchSample) == 88, "sample must be 88 bytes");
static_assert(sizeof(PgrSharedCatchApi) == 112, "table must be 112 bytes");

namespace {

constexpr int kEventCap = 16;  // bounded drain FIFO (>= MAX_RECORDS + overflow headroom)

struct Record {
  bool used = false;
  uint64_t slot = 0;
  uint64_t gui_key = 0;
  uint32_t kind = PGR_SH_KIND_SUMMARY;
  uint64_t epoch = 0;        // armed epoch at creation (JS r.epoch = s.epoch)
  bool init_done = false, set_scores_done = false, open_complete_done = false, present_done = false;
  bool success = false, captured_id = false, invoked = false;
  int64_t ready_match = -1;  // -1 == "null"; else the unity tick when first seen ready
  uint64_t deadline_ms = 0;  // created + timeout_ms
};

struct Binding {
  bool used = false;
  uint64_t token = 0;
  uint64_t slot = 0;
};

// --- lifetime / publication --------------------------------------------------------
std::mutex g_install_mutex;
std::mutex g_state_mutex;
PgrSharedCatchConfig g_cfg{};
std::atomic<bool> g_installed{false};
bool g_install_attempted = false;
int g_install_error = PGR_SH_ERR_NOT_INSTALLED;

// Permanent owner, latched once from a genuine on-thread call.
std::atomic<uint64_t> g_owner_tid{0};

// --- gate (guarded by g_state_mutex) -----------------------------------------------
int g_enabled = 0;
bool g_stopping = false;
uint64_t g_lease_until_ms = 0;
uint64_t g_epoch = 0;
uint64_t g_slot_serial = 0;  // monotonic slot id source

// --- tables (guarded by g_state_mutex) ---------------------------------------------
Record g_records[PGR_SH_MAX_RECORDS];
Binding g_bindings[PGR_SH_XFER_SLOTS];

PgrSharedCatchEvent g_events[kEventCap];
int g_event_head = 0, g_event_tail = 0, g_event_count = 0;

// --- telemetry (atomic so sample is lock-free for the counters) --------------------
std::atomic<uint64_t> g_created{0}, g_armed{0}, g_dismiss{0}, g_cancel{0}, g_reap{0};
std::atomic<uint64_t> g_bind{0}, g_resolve{0}, g_last_tid{0};
std::atomic<uint32_t> g_fault{0};

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

// Caller holds g_state_mutex.
bool gate_live_locked(uint64_t now) {
  if (!g_enabled || g_stopping || g_fault.load(std::memory_order_relaxed)) return false;
  return g_lease_until_ms != 0 && now < g_lease_until_ms;
}

// Caller holds g_state_mutex. Push a drain record; fail closed (latch fault) on overflow.
void push_event_locked(uint32_t kind, int32_t reason, const Record &r) {
  if (g_event_count >= kEventCap) { g_fault.store(1, std::memory_order_relaxed); return; }
  PgrSharedCatchEvent &e = g_events[g_event_tail];
  e.kind = kind;
  e.reason = reason;
  e.slot = r.slot;
  e.gui_key = r.gui_key;
  g_event_tail = (g_event_tail + 1) % kEventCap;
  ++g_event_count;
}

// Caller holds g_state_mutex. Drop a slot: emit the event, release any binding to it, clear the record.
void drop_record_locked(Record &r, uint32_t event_kind, int32_t reason) {
  push_event_locked(event_kind, reason, r);
  if (event_kind == PGR_SH_EVENT_CANCELLED) g_cancel.fetch_add(1, std::memory_order_relaxed);
  else g_reap.fetch_add(1, std::memory_order_relaxed);
  for (auto &b : g_bindings) if (b.used && b.slot == r.slot) b.used = false;
  r = Record{};
}

Record *find_by_gui_locked(uint64_t gui_key) {
  for (auto &r : g_records) if (r.used && r.gui_key == gui_key) return &r;
  return nullptr;
}
Record *find_by_slot_locked(uint64_t slot) {
  if (slot == 0) return nullptr;
  for (auto &r : g_records) if (r.used && r.slot == slot) return &r;
  return nullptr;
}

bool record_ready_locked(const Record &r) {
  if (r.kind == PGR_SH_KIND_DETAILS)
    return r.init_done && r.present_done && r.open_complete_done;
  return r.init_done && r.set_scores_done && r.open_complete_done && r.success && r.captured_id;
}

uint32_t live_record_count_locked() {
  uint32_t n = 0;
  for (auto &r : g_records) if (r.used) ++n;
  return n;
}

// Caller holds g_state_mutex. Prune stale/off/expired/timed-out records with the exact JS reason order:
// permission-revoked > stop > off > lease-expired > timeout. A dead gate drops every non-invoked record.
void prune_locked(uint64_t now) {
  const bool gate = gate_live_locked(now);
  for (auto &r : g_records) {
    if (!r.used || r.invoked) continue;
    int32_t reason = 0;
    if (r.epoch != g_epoch) reason = PGR_SH_REASON_PERMISSION_REVOKED;
    else if (g_stopping) reason = PGR_SH_REASON_STOP;
    else if (!g_enabled) reason = PGR_SH_REASON_OFF;
    else if (!gate) reason = PGR_SH_REASON_LEASE_EXPIRED;  // enabled but lease dead
    else if (now >= r.deadline_ms) reason = PGR_SH_REASON_TIMEOUT;
    if (reason != 0) drop_record_locked(r, PGR_SH_EVENT_CANCELLED, reason);
  }
}

// --- config validation -------------------------------------------------------------
bool validate_config(const PgrSharedCatchConfig *c, int *rc) {
  if (c == nullptr) { *rc = PGR_SH_ERR_NULL_ARG; return false; }
  if (c->version != PGR_SH_CONFIG_VERSION) { *rc = PGR_SH_ERR_BAD_VERSION; return false; }
  if (c->size != sizeof(PgrSharedCatchConfig)) { *rc = PGR_SH_ERR_BAD_SIZE; return false; }
  if(c->reserved0 || c->reserved1) { *rc = PGR_SH_ERR_MISSING_PTR; return false; }
  if (c->max_records < 1 || c->max_records > PGR_SH_MAX_RECORDS) { *rc = PGR_SH_ERR_MISSING_PTR; return false; }
  if (c->timeout_ms < 1 || c->timeout_ms > PGR_SH_TIMEOUT_MS) { *rc = PGR_SH_ERR_MISSING_PTR; return false; }
  if (c->lease_ceiling_ms < 1 || c->lease_ceiling_ms > PGR_SH_MAX_LEASE_MS) { *rc = PGR_SH_ERR_MISSING_PTR; return false; }
  if (c->xfer_slots < 1 || c->xfer_slots > PGR_SH_XFER_SLOTS) { *rc = PGR_SH_ERR_MISSING_PTR; return false; }
  *rc = PGR_SH_OK;
  return true;
}

// --- public API --------------------------------------------------------------------
int api_install(const PgrSharedCatchConfig *config) {
  int rc = PGR_SH_OK;
  if (!validate_config(config, &rc)) return rc;
  uint64_t probe = 0;
  if (!read_boottime_ms(&probe)) return PGR_SH_ERR_CLOCK_FAILURE;

  std::lock_guard<std::mutex> install_lock(g_install_mutex);
  if (g_installed.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    if (std::memcmp(&g_cfg, config, sizeof(*config)) != 0) return PGR_SH_ERR_RECONFIG;
    return PGR_SH_OK;
  }
  if (g_install_attempted) return g_install_error;
  g_install_attempted = true;
  {
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    g_cfg = *config;
    g_enabled = 0;
    g_stopping = false;
    g_lease_until_ms = 0;
    g_epoch = 0;
    g_slot_serial = 0;
    for (auto &r : g_records) r = Record{};
    for (auto &b : g_bindings) b = Binding{};
    g_event_head = g_event_tail = g_event_count = 0;
  }
  g_installed.store(true, std::memory_order_release);
  return g_install_error = PGR_SH_OK;
}

int api_update(int enabled, uint64_t remaining, uint64_t epoch, uint64_t proven_tid) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_SH_ERR_NOT_INSTALLED;
  uint64_t now = 0;
  const bool clock_ok = read_boottime_ms(&now);
  std::lock_guard<std::mutex> lock(g_state_mutex);
  const uint64_t owner = g_owner_tid.load(std::memory_order_acquire);
  const bool on_owner = clock_ok && proven_tid != 0 && current_tid() == proven_tid && (owner == 0 || owner == proven_tid);
  // A foreign thread, a bad clock/tid, or a disable can only REVOKE. Arming requires the owner thread.
  if (!on_owner || !enabled) {
    g_enabled = 0;
    g_lease_until_ms = 0;
    if (on_owner) g_epoch = epoch;  // the owner may record a new epoch even while disabling
    return clock_ok ? PGR_SH_OK : PGR_SH_ERR_CLOCK_FAILURE;
  }
  const uint64_t ceiling = g_cfg.lease_ceiling_ms;
  remaining = remaining > ceiling ? ceiling : remaining;
  g_enabled = 1;
  g_stopping = false;  // a fresh owner arm clears a prior stop
  g_lease_until_ms = remaining ? now + remaining : 0;
  g_epoch = epoch;
  return PGR_SH_OK;
}

int api_cancel_all(int32_t reason, uint64_t proven_tid) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_SH_ERR_NOT_INSTALLED;
  const uint64_t tid = current_tid();
  if (proven_tid == 0 || tid != proven_tid || !owner_ready(tid)) return PGR_SH_OK;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  for (auto &r : g_records) if (r.used && !r.invoked) drop_record_locked(r, PGR_SH_EVENT_CANCELLED, reason);
  g_last_tid.store(tid, std::memory_order_relaxed);
  return PGR_SH_OK;
}

int api_upsert(uint64_t gui_key, uint32_t kind, uint64_t proven_tid, uint64_t *out_slot) {
  if (out_slot != nullptr) *out_slot = 0;
  if (!g_installed.load(std::memory_order_acquire)) return PGR_SH_ERR_NOT_INSTALLED;
  if (out_slot == nullptr) return PGR_SH_ERR_NULL_ARG;
  if (gui_key == 0 || (kind != PGR_SH_KIND_SUMMARY && kind != PGR_SH_KIND_DETAILS)) return PGR_SH_ERR_NULL_ARG;
  const uint64_t tid = current_tid();
  if (proven_tid == 0 || tid != proven_tid || !owner_ready(tid)) return PGR_SH_OK;
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) return PGR_SH_ERR_CLOCK_FAILURE;

  std::lock_guard<std::mutex> lock(g_state_mutex);
  if(g_fault.load(std::memory_order_relaxed))return PGR_SH_ERR_OVERFLOW;
  if (Record *existing = find_by_gui_locked(gui_key)) { *out_slot = existing->slot; return PGR_SH_OK; }

  const uint32_t limit = g_cfg.max_records;
  Record *target = nullptr;
  uint32_t live = 0;
  for (auto &r : g_records) if (r.used) ++live;
  if (live >= limit) {
    // Evict the first non-invoked record (JS record-overflow). If none is evictable, refuse.
    Record *oldest=nullptr;
    for(auto &r:g_records)if(r.used&&!r.invoked&&(!oldest||r.slot<oldest->slot))oldest=&r;
    if(oldest)drop_record_locked(*oldest,PGR_SH_EVENT_CANCELLED,PGR_SH_REASON_RECORD_OVERFLOW);
  }
  for (auto &r : g_records) if (!r.used) { target = &r; break; }
  if (target == nullptr) return PGR_SH_OK;  // table still full of invoked/non-evictable records

  *target = Record{};
  target->used = true;
  target->slot = ++g_slot_serial;
  target->gui_key = gui_key;
  target->kind = kind;
  target->epoch = g_epoch;
  target->deadline_ms = now + g_cfg.timeout_ms;
  *out_slot = target->slot;
  g_created.fetch_add(1, std::memory_order_relaxed);
  g_last_tid.store(tid, std::memory_order_relaxed);
  return PGR_SH_OK;
}

int api_mark(uint64_t slot, uint32_t stage, uint32_t aux, uint64_t proven_tid) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_SH_ERR_NOT_INSTALLED;
  const uint64_t tid = current_tid();
  if (proven_tid == 0 || tid != proven_tid || !owner_ready(tid)) return PGR_SH_OK;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  Record *r = find_by_slot_locked(slot);
  if (r == nullptr || r->invoked) return PGR_SH_OK;
  switch (stage) {
    case PGR_SH_STAGE_INIT:
      r->init_done = true;
      r->success = (aux & 0x1u) != 0;
      r->captured_id = (aux & 0x2u) != 0;
      break;
    case PGR_SH_STAGE_SET_SCORES:    r->set_scores_done = true; break;
    case PGR_SH_STAGE_OPEN_COMPLETE: r->open_complete_done = true; break;
    case PGR_SH_STAGE_PRESENT:       r->present_done = true; break;
    default: return PGR_SH_ERR_NULL_ARG;
  }
  r->ready_match = -1;  // JS r.readyMatch = null on every progress event
  g_last_tid.store(tid, std::memory_order_relaxed);
  return PGR_SH_OK;
}

int api_cancel(uint64_t slot, int32_t reason, uint64_t proven_tid) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_SH_ERR_NOT_INSTALLED;
  const uint64_t tid = current_tid();
  if (proven_tid == 0 || tid != proven_tid || !owner_ready(tid)) return PGR_SH_OK;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  Record *r = find_by_slot_locked(slot);
  if (r == nullptr || r->invoked) return PGR_SH_OK;
  drop_record_locked(*r, PGR_SH_EVENT_CANCELLED, reason);
  g_last_tid.store(tid, std::memory_order_relaxed);
  return PGR_SH_OK;
}

int api_select(uint64_t unity_ticks, uint64_t proven_tid, PgrSharedCatchDecision *out) {
  if (out == nullptr) return PGR_SH_ERR_NULL_ARG;
  out->version = PGR_SH_DECISION_VERSION;
  out->size = sizeof(PgrSharedCatchDecision);
  out->action = PGR_SH_ACT_NONE;
  out->kind = PGR_SH_KIND_SUMMARY;
  out->slot = 0;
  out->gui_key = 0;
  out->tick = unity_ticks;
  if (!g_installed.load(std::memory_order_acquire)) return PGR_SH_ERR_NOT_INSTALLED;
  const uint64_t tid = current_tid();
  if (proven_tid == 0 || tid != proven_tid || !owner_ready(tid)) return PGR_SH_OK;
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) return PGR_SH_ERR_CLOCK_FAILURE;

  std::lock_guard<std::mutex> lock(g_state_mutex);
  prune_locked(now);
  if (!gate_live_locked(now)) return PGR_SH_OK;  // dead gate: queue drained, no decision
  // Tick zero is cleanup only, including during opener frames and shutdown.
  if(!unity_ticks||unity_ticks>static_cast<uint64_t>(INT64_MAX))return PGR_SH_OK;

  // First ready, non-invoked, non-cancelled candidate (records are only live if not cancelled -- a
  // cancel drops the slot immediately -- so "used && !invoked && ready" is the JS predicate).
  Record *cand = nullptr;
  for (auto &r : g_records) {
    if (!r.used || r.invoked) continue;
    if(record_ready_locked(r)&&(!cand||r.slot<cand->slot))cand=&r;
  }
  if (cand == nullptr) return PGR_SH_OK;

  const int64_t ticks = static_cast<int64_t>(unity_ticks);
  if (cand->ready_match < 0) {
    cand->ready_match = ticks;  // JS: first ready tick -> candidate-armed, wait
    out->action = PGR_SH_ACT_ARM;
    out->kind = cand->kind;
    out->slot = cand->slot;
    out->gui_key = cand->gui_key;
    g_armed.fetch_add(1, std::memory_order_relaxed);
    g_last_tid.store(tid, std::memory_order_relaxed);
    return PGR_SH_OK;
  }
  if (ticks <= cand->ready_match) return PGR_SH_OK;  // not a strictly later proven tick yet
  out->action = PGR_SH_ACT_DISMISS;
  out->kind = cand->kind;
  out->slot = cand->slot;
  out->gui_key = cand->gui_key;
  g_dismiss.fetch_add(1, std::memory_order_relaxed);
  g_last_tid.store(tid, std::memory_order_relaxed);
  return PGR_SH_OK;
}

int api_finish(uint64_t slot, int /*advanced*/, uint64_t proven_tid) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_SH_ERR_NOT_INSTALLED;
  const uint64_t tid = current_tid();
  if (proven_tid == 0 || tid != proven_tid || !owner_ready(tid)) return PGR_SH_OK;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  Record *r = find_by_slot_locked(slot);
  if (r == nullptr) return PGR_SH_OK;
  r->invoked = true;  // latch so the decision is never re-issued even on a failed dismiss
  drop_record_locked(*r, PGR_SH_EVENT_REAPED, 0);
  g_last_tid.store(tid, std::memory_order_relaxed);
  return PGR_SH_OK;
}

int api_transfer_bind(uint64_t slot, uint64_t token, uint64_t proven_tid) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_SH_ERR_NOT_INSTALLED;
  if (slot == 0 || token == 0) return PGR_SH_ERR_NULL_ARG;
  const uint64_t tid = current_tid();
  if (proven_tid == 0 || tid != proven_tid || !owner_ready(tid)) return PGR_SH_OK;
  uint64_t now=0;if(!read_boottime_ms(&now))return PGR_SH_ERR_CLOCK_FAILURE;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  auto *record=find_by_slot_locked(slot);if(!record)return PGR_SH_ERR_NULL_ARG;
  // transfer_execution owns a ten-second response window. Preserve the host's
  // one-second UI cleanup margin instead of pruning at the initial eight seconds.
  record->deadline_ms=now+11000;
  const uint32_t slots = g_cfg.xfer_slots;
  // Replace an existing binding for the same token, else take a free slot; overflow fails closed.
  for (uint32_t i = 0; i < slots; ++i) if (g_bindings[i].used && g_bindings[i].token == token) { g_bindings[i].slot = slot; g_bind.fetch_add(1, std::memory_order_relaxed); return PGR_SH_OK; }
  for (uint32_t i = 0; i < slots; ++i) if (!g_bindings[i].used) { g_bindings[i].used = true; g_bindings[i].token = token; g_bindings[i].slot = slot; g_bind.fetch_add(1, std::memory_order_relaxed); g_last_tid.store(tid, std::memory_order_relaxed); return PGR_SH_OK; }
  g_fault.store(1, std::memory_order_relaxed);
  return PGR_SH_ERR_OVERFLOW;
}

int api_transfer_resolve(uint64_t token, uint64_t proven_tid, uint64_t *out_slot) {
  if (out_slot != nullptr) *out_slot = 0;
  if (!g_installed.load(std::memory_order_acquire)) return PGR_SH_ERR_NOT_INSTALLED;
  if (out_slot == nullptr) return PGR_SH_ERR_NULL_ARG;
  const uint64_t tid = current_tid();
  if (proven_tid == 0 || tid != proven_tid || !owner_ready(tid)) return PGR_SH_OK;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  const uint32_t slots = g_cfg.xfer_slots;
  for (uint32_t i = 0; i < slots; ++i) {
    if (g_bindings[i].used && g_bindings[i].token == token) {
      *out_slot = g_bindings[i].slot;  // one-shot consume: a late/duplicate event cannot re-attribute
      g_bindings[i] = Binding{};
      g_resolve.fetch_add(1, std::memory_order_relaxed);
      g_last_tid.store(tid, std::memory_order_relaxed);
      return PGR_SH_OK;
    }
  }
  return PGR_SH_OK;  // unknown/already-consumed token -> out_slot stays 0
}

int api_stop(uint64_t /*proven_tid*/) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_SH_ERR_NOT_INSTALLED;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  g_enabled = 0;
  g_stopping = true;
  g_lease_until_ms = 0;
  return PGR_SH_OK;
}

int api_sample(PgrSharedCatchSample *out) {
  if (out == nullptr) return PGR_SH_ERR_NULL_ARG;
  uint32_t records = 0, pending = 0, open_xfer = 0;
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    records = live_record_count_locked();
    pending = static_cast<uint32_t>(g_event_count);
    for (auto &b : g_bindings) if (b.used) ++open_xfer;
  }
  out->version = PGR_SH_SAMPLE_VERSION;
  out->size = sizeof(PgrSharedCatchSample);
  out->created_serial = g_created.load(std::memory_order_relaxed);
  out->armed_serial = g_armed.load(std::memory_order_relaxed);
  out->dismiss_serial = g_dismiss.load(std::memory_order_relaxed);
  out->cancel_serial = g_cancel.load(std::memory_order_relaxed);
  out->reap_serial = g_reap.load(std::memory_order_relaxed);
  out->xfer_bind_serial = g_bind.load(std::memory_order_relaxed);
  out->xfer_resolve_serial = g_resolve.load(std::memory_order_relaxed);
  out->last_tid = g_last_tid.load(std::memory_order_relaxed);
  out->records = records;
  out->pending_events = pending;
  out->open_xfer = open_xfer;
  out->fault = g_fault.load(std::memory_order_relaxed);
  return PGR_SH_OK;
}

int api_next_event(PgrSharedCatchEvent *out) {
  if (out == nullptr) return PGR_SH_ERR_NULL_ARG;
  if (!g_installed.load(std::memory_order_acquire)) return PGR_SH_ERR_NOT_INSTALLED;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  if (g_event_count <= 0) return PGR_SH_OK;  // empty
  *out = g_events[g_event_head];
  g_event_head = (g_event_head + 1) % kEventCap;
  --g_event_count;
  return 1;
}

const PgrSharedCatchApi kApiTable = {
    PGR_SH_API_VERSION, static_cast<uint32_t>(sizeof(PgrSharedCatchApi)),
    &api_install,  &api_update,   &api_cancel_all,      &api_upsert,
    &api_mark,     &api_cancel,   &api_select,          &api_finish,
    &api_transfer_bind, &api_transfer_resolve, &api_stop, &api_sample,
    &api_next_event,
};

}  // namespace

extern "C" const void *pgrazor_shared_catch_api(uint32_t version) {
  if (version != PGR_SH_API_VERSION) return nullptr;
  return &kApiTable;
}
