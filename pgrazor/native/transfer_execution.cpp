#include "transfer_execution.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <time.h>
#include <unistd.h>

// Feature 23 native transfer request lifecycle. See transfer_execution.h for the ABI and the division
// of labour with the host JS. Built -fno-exceptions / -fno-rtti like every sibling: the JS "throw"
// paths become fail-closed error returns / records; every GC root is freed exactly once on the owner
// thread; the ReleasePokemon invoke is issued at most once per request. No mutex is ever held across a
// managed call or a GC-handle call. This module installs NO hook and NEVER calls back into JS.
//
// Reentrancy model (same conservatism as outgoing_gifts.cpp -- a gc call / field read / invoke may
// re-enter the owner thread through il2cpp instrumentation or a relocating collector):
//   * Every managed reference held across a managed call is strongly rooted and re-resolved from its
//     root afterwards; no raw pointer is reused across a potential safepoint.
//   * "seen" (the captured id) and "issued" (a pending transaction with its durable roots) are latched
//     BEFORE the ReleasePokemon invoke, so an uncertain release is never retried and the id is
//     quarantined process-wide. *out_token is written before the invoke too.
//   * The result/error observers only LATCH a match (observe_begin) and SET a completion flag + emit
//     one record (observe_end); they NEVER free. The actual root free is deferred to the outermost
//     owner frame (leave_frame). A callback that fires synchronously inside ReleasePokemon therefore
//     cannot free the issuing frame's locals, and -- because the observers do not drain the FIFO --
//     cannot beat the host's token->record registration.
//   * STOP revokes the gate immediately (any thread); the owner-thread teardown of every tracked
//     transaction is deferred to the outermost frame. submit() and re-arm are blocked while a stop or a
//     cleanup is latched. The seen quarantine persists across a stop (no uncertain release is resent).
//   * sample() reads atomic mirrors published on the owner thread.

static_assert(sizeof(PgrTransferExecConfig) == 144, "config must be 144 bytes");
static_assert(sizeof(PgrTransferExecRequest) == 104, "request must be 104 bytes");
static_assert(sizeof(PgrTransferExecSample) == 88, "sample must be 88 bytes");
static_assert(sizeof(PgrTransferExecEvent) == 24, "event must be 24 bytes");
static_assert(sizeof(PgrTransferExecApi) == 72, "table must be 72 bytes");
static_assert(offsetof(PgrTransferExecConfig, gchandle_new) == 8, "layout");
static_assert(offsetof(PgrTransferExecConfig, m_get) == 56, "layout");
static_assert(offsetof(PgrTransferExecConfig, class_bag) == 72, "layout");
static_assert(offsetof(PgrTransferExecConfig, f_id) == 104, "layout");
static_assert(offsetof(PgrTransferExecConfig, success_literal) == 136, "layout");
static_assert(offsetof(PgrTransferExecRequest, input) == 48, "layout");

namespace {

constexpr uint64_t kMaxLeaseMs = PGR_TX_MAX_LEASE_MS;

// --- il2cpp runtime ABI typedefs ---------------------------------------------------
using GcNew = uintptr_t (*)(void *obj, int pinned);
using GcGet = void *(*)(uintptr_t handle);
using GcFree = void (*)(uintptr_t handle);
using ObjClass = void *(*)(void *obj);
using Invoke = void *(*)(const void *method, void *obj, void **params, void **exc);
using FieldGet = void (*)(void *obj, void *field, void *out);

// --- lifetime / publication --------------------------------------------------------
std::mutex g_install_mutex;
std::mutex g_state_mutex;
PgrTransferExecConfig g_cfg{};
const PgrTransferApi *g_policy = nullptr;  // feature-4 transfer_policy, resolved once at install
std::atomic<bool> g_installed{false};
bool g_install_attempted = false;
int g_install_error = PGR_TX_ERR_NOT_INSTALLED;

// Permanent Unity owner, latched once from a genuine on-thread submit()/observe/tick().
std::atomic<uint64_t> g_owner_tid{0};

// Root / frame accounting (atomic so sample is lock-free for these).
std::atomic<uint32_t> g_roots{0};
std::atomic<uint32_t> g_active_depth{0};

// Cross-thread sample mirrors (published on the owner thread).
std::atomic<uint32_t> g_m_pending{0};
std::atomic<uint32_t> g_m_seen{0};
std::atomic<uint32_t> g_m_tickets{0};

// --- gate / telemetry (guarded by g_state_mutex) -----------------------------------
int g_enabled = 0;
uint64_t g_lease_until_ms = 0;
uint64_t g_epoch = 0;        // the armed epoch (submit requires request.epoch == this)
uint64_t g_gen = 1;          // revocation generation; bumped on disable / epoch / dead-lease refresh
bool g_fault = false;
bool g_overflow = false;
uint64_t g_request_serial = 0, g_complete_serial = 0, g_timeout_serial = 0, g_error_serial = 0,
         g_token_serial = 0, g_last_tid = 0, g_last_token = 0;

PgrTransferExecEvent g_fifo[PGR_TX_EVENT_CAP];
uint32_t g_fifo_head = 0, g_fifo_count = 0;

// --- owner-thread reentrancy guards ------------------------------------------------
std::atomic<bool> g_pending_stop{false};
std::atomic<bool> g_cleaning{false};

// --- process-wide captured-id quarantine (owner-thread only) -----------------------
uint64_t g_seen[PGR_TX_SEEN_MAX];
uint32_t g_seen_count = 0;

// --- bounded pending transactions (owner-thread only) ------------------------------
struct Txn {
  uint64_t token = 0;
  uint64_t captured_id = 0;
  uint64_t epoch = 0;
  uint64_t deadline_ms = 0;
  uintptr_t bag_root = 0;
  uintptr_t pokemon_root = 0;
  uintptr_t promise_root = 0;
  bool finished = false;
  bool success = false;
  bool uncertain = false;  // release threw / null promise
  bool timed_out = false;
};
Txn g_pending[PGR_TX_PENDING_MAX];
uint32_t g_pending_count = 0;

// --- bounded observe_begin/observe_end tickets (owner-thread only) ------------------
struct TicketSlot {
  bool in_use = false;
  uint64_t token = 0;
  bool success = false;
  uint64_t nonce = 0;
};
TicketSlot g_tickets[PGR_TX_TICKET_SLOTS];
uint64_t g_ticket_nonce = 0;
uint32_t g_ticket_open = 0;

// --- clock / tid -------------------------------------------------------------------
uint64_t current_tid() { return static_cast<uint64_t>(static_cast<pid_t>(gettid())); }

bool read_boottime_ms(uint64_t *out_ms) {
  struct timespec ts {};
  if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0 || ts.tv_sec < 0 || ts.tv_nsec < 0) return false;
  *out_ms = static_cast<uint64_t>(ts.tv_sec) * 1000ULL + static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
  return true;
}

// Typed accessors onto the immutable published config.
GcNew gc_new() { return reinterpret_cast<GcNew>(g_cfg.gchandle_new); }
GcGet gc_get() { return reinterpret_cast<GcGet>(g_cfg.gchandle_get_target); }
GcFree gc_free_raw() { return reinterpret_cast<GcFree>(g_cfg.gchandle_free); }
ObjClass obj_class() { return reinterpret_cast<ObjClass>(g_cfg.object_get_class); }
FieldGet field_get() { return reinterpret_cast<FieldGet>(g_cfg.field_get_value); }
void *managed_invoke(const void *method, void *obj, void **params, void **exc) {
  *exc = nullptr;
  return reinterpret_cast<Invoke>(g_cfg.runtime_invoke)(method, obj, params, exc);
}

bool is_exact(void *obj, void *klass) { return obj != nullptr && obj_class()(obj) == klass; }

// --- root bookkeeping --------------------------------------------------------------
uintptr_t hold(void *obj) {
  if (obj == nullptr) return 0;
  uintptr_t h = gc_new()(obj, 0);
  if (h != 0) g_roots.fetch_add(1, std::memory_order_relaxed);
  return h;
}
void drop(uintptr_t *h) {
  if (*h == 0) return;
  uintptr_t v = *h;
  *h = 0;  // detach before the (possibly reentrant) free
  gc_free_raw()(v);
  g_roots.fetch_sub(1, std::memory_order_relaxed);
}

// --- field readers (object must be rooted by the caller) ---------------------------
void *field_ptr(void *obj, void *field) {
  void *out = nullptr;
  if (obj != nullptr) field_get()(obj, field, &out);
  return out;
}
int32_t field_i32(void *obj, void *field) {
  int32_t v = 0;
  if (obj != nullptr) field_get()(obj, field, &v);
  return v;
}
uint64_t field_u64(void *obj, void *field) {
  uint64_t v = 0;
  if (obj != nullptr) field_get()(obj, field, &v);
  return v;
}

void publish() {
  g_m_pending.store(g_pending_count, std::memory_order_relaxed);
  g_m_seen.store(g_seen_count, std::memory_order_relaxed);
  g_m_tickets.store(g_ticket_open, std::memory_order_relaxed);
}

uint32_t root_total() {
  return g_roots.load(std::memory_order_relaxed) + g_active_depth.load(std::memory_order_relaxed);
}

// --- gate / telemetry --------------------------------------------------------------
void push_event(uint32_t kind, int32_t id, uint64_t token, int32_t aux, uint64_t tid) {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  g_last_tid = tid;
  g_last_token = token;
  if (kind == PGR_TX_EVENT_REQUEST) ++g_request_serial;
  else if (kind == PGR_TX_EVENT_COMPLETE) { ++g_complete_serial; if (!(aux & 1)) ++g_error_serial; }
  else if (kind == PGR_TX_EVENT_TIMEOUT) ++g_timeout_serial;
  if (g_fifo_count >= PGR_TX_EVENT_CAP) {
    g_overflow = true;
    g_fault = true;  // fail closed: no further issue while faulted
    return;
  }
  uint32_t idx = (g_fifo_head + g_fifo_count) % PGR_TX_EVENT_CAP;
  g_fifo[idx] = PgrTransferExecEvent{kind, id, token, aux, 0};
  ++g_fifo_count;
}

// Fresh-clock gate: enabled, lease live, epoch matches, gen unchanged, owner thread, not faulted /
// stopping / cleaning. Fails closed.
uint64_t pin_generation() {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  return g_gen;
}
bool gate_live(uint64_t req_epoch, uint64_t tid, uint64_t generation) {
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) return false;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  if (g_fault || g_gen != generation || g_pending_stop.load() || g_cleaning.load()) return false;
  if (!g_enabled) return false;
  if (!(g_lease_until_ms != 0 && now < g_lease_until_ms)) return false;
  if (g_epoch != req_epoch) return false;
  return current_tid() == tid && g_owner_tid.load(std::memory_order_acquire) == tid;
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

// --- seen quarantine (owner thread) ------------------------------------------------
bool seen_contains(uint64_t id) {
  for (uint32_t i = 0; i < g_seen_count; ++i) if (g_seen[i] == id) return true;
  return false;
}
void seen_add(uint64_t id) {
  if (g_seen_count < PGR_TX_SEEN_MAX && !seen_contains(id)) g_seen[g_seen_count++] = id;
}

// --- pending table (owner thread) --------------------------------------------------
int find_pending_by_token(uint64_t token) {
  for (uint32_t i = 0; i < g_pending_count; ++i) if (g_pending[i].token == token) return static_cast<int>(i);
  return -1;
}
// Match an onEnter callback to an active (unfinished) issued transaction by captured id AND rooted
// pokemon/bag identity. Re-resolves roots; the caller holds the owner thread with no intervening
// managed call, so the raw comparison is valid.
int find_pending_match(uint64_t cb_id, void *cb_pokemon, void *cb_bag) {
  for (uint32_t i = 0; i < g_pending_count; ++i) {
    Txn &t = g_pending[i];
    if (t.finished || t.captured_id != cb_id) continue;
    if (gc_get()(t.pokemon_root) == cb_pokemon && gc_get()(t.bag_root) == cb_bag) return static_cast<int>(i);
  }
  return -1;
}

// Free + remove tracked transactions. only_finished removes just completed ones; otherwise every one
// (stop teardown). Handles are collected and the table compacted BEFORE any free, so a reentrant
// observe_end/submit fired from inside a gc_free finds a consistent, shorter table and no live slot to
// double-free. The seen quarantine is NOT touched.
void collect_and_free(bool only_finished) {
  g_cleaning.store(true);
  uintptr_t handles[PGR_TX_PENDING_MAX * 3];
  uint32_t hc = 0;
  uint32_t w = 0;
  for (uint32_t r = 0; r < g_pending_count; ++r) {
    const bool remove = !only_finished || g_pending[r].finished;
    if (remove) {
      if (g_pending[r].bag_root) handles[hc++] = g_pending[r].bag_root;
      if (g_pending[r].pokemon_root) handles[hc++] = g_pending[r].pokemon_root;
      if (g_pending[r].promise_root) handles[hc++] = g_pending[r].promise_root;
    } else {
      if (w != r) g_pending[w] = g_pending[r];
      ++w;
    }
  }
  g_pending_count = w;
  publish();
  for (uint32_t i = 0; i < hc; ++i) drop(&handles[i]);
  g_cleaning.store(false);
}

// Outermost owner frame: perform the deferred stop teardown (then clear the latch so a later session
// can re-arm) or reap completed transactions. The free happens BEFORE the depth decrement so
// root_total stays nonzero until done.
void leave_frame() {
  if (g_active_depth.load(std::memory_order_relaxed) == 1) {
    if (!g_pending_stop.load()) collect_and_free(true);
    if (g_pending_stop.load()) {
      // Invalidate abandoned observer tickets before freeing. Nonces never reset.
      for (auto &ticket : g_tickets) ticket = TicketSlot{};
      g_ticket_open = 0;
      collect_and_free(false);
      g_pending_stop.store(false);
    }
  }
  g_active_depth.fetch_sub(1, std::memory_order_relaxed);
}

// --- ticket slots ------------------------------------------------------------------
uint64_t ticket_alloc(uint64_t token, bool success) {
  if (g_ticket_nonce >= ((uint64_t{1} << 45) - 1)) return 0;
  for (int i = 0; i < PGR_TX_TICKET_SLOTS; ++i) {
    if (!g_tickets[i].in_use) {
      g_tickets[i].in_use = true;
      g_tickets[i].token = token;
      g_tickets[i].success = success;
      g_tickets[i].nonce = ++g_ticket_nonce;
      ++g_ticket_open;
      publish();
      return (g_tickets[i].nonce << 8) | static_cast<uint64_t>(i + 1);
    }
  }
  return 0;  // fail closed: no free slot
}
// Resolve + consume a one-shot ticket. Returns the slot index, or -1 for a stale/unknown/duplicate
// ticket (no effect).
int ticket_consume(uint64_t ticket, uint64_t *out_token, bool *out_success) {
  if (ticket == 0) return -1;
  uint32_t idx = static_cast<uint32_t>(ticket & 0xFF);
  if (idx == 0 || idx > PGR_TX_TICKET_SLOTS) return -1;
  int i = static_cast<int>(idx - 1);
  uint64_t nonce = ticket >> 8;
  if (!g_tickets[i].in_use || g_tickets[i].nonce != nonce) return -1;
  *out_token = g_tickets[i].token;
  *out_success = g_tickets[i].success;
  g_tickets[i].in_use = false;
  g_tickets[i].token = 0;
  if (g_ticket_open) --g_ticket_open;
  publish();
  return i;
}

// --- submit ------------------------------------------------------------------------
// Runs inside an active owner frame. Locals bag_root/pokemon_root are dropped at the single exit; on
// the success path their ownership is moved into the transaction (locals zeroed) before the invoke.
int do_submit(const PgrTransferExecRequest *req, uint64_t tid, uint64_t *out_token) {
  uintptr_t bag_root = 0, pokemon_root = 0;
  int rc = PGR_TX_OK;
  const uint64_t generation = pin_generation();
  if (!gate_live(req->epoch, tid, generation) || g_policy->decide(&req->input) != 0 ||
      seen_contains(req->captured_id) || g_seen_count >= PGR_TX_SEEN_MAX) return PGR_TX_OK;

  if (req->captured_id == 0) { rc = PGR_TX_ERR_IDENTITY; goto done; }

  bag_root = hold(req->bag);
  pokemon_root = hold(req->pokemon);
  if (bag_root == 0 || pokemon_root == 0) { rc = PGR_TX_ERR_GCHANDLE; goto done; }  // fail closed, no release

  {
    void *bag = gc_get()(bag_root);
    void *pokemon = gc_get()(pokemon_root);
    if (!is_exact(bag, g_cfg.class_bag) || !is_exact(pokemon, g_cfg.class_pokemon)) { rc = PGR_TX_ERR_IDENTITY; goto done; }
    if (field_u64(gc_get()(pokemon_root), g_cfg.f_id) != req->captured_id) { rc = PGR_TX_ERR_IDENTITY; goto done; }

    // Current bag identity: bag.GetPokemon(id) must be the SAME rooted pokemon with the SAME id.
    uint64_t id = req->captured_id;
    void *params[1] = {&id};
    void *exc = nullptr;
    bag = gc_get()(bag_root);
    void *current = bag != nullptr ? managed_invoke(g_cfg.m_get, bag, params, &exc) : nullptr;
    void *pk = gc_get()(pokemon_root);
    if (exc != nullptr || current == nullptr || current != pk || !is_exact(current, g_cfg.class_pokemon) ||
        field_u64(current, g_cfg.f_id) != req->captured_id) {
      rc = PGR_TX_ERR_IDENTITY;
      goto done;
    }

    // Resolve all managed arguments before the final gate. No callback-capable
    // lookup may occur between authorization and ReleasePokemon.
    void *release_pokemon = gc_get()(pokemon_root);
    void *release_bag = gc_get()(bag_root);
    uint64_t now = 0;
    if (!read_boottime_ms(&now)) { rc = PGR_TX_ERR_CLOCK_FAILURE; goto done; }
    if (!release_pokemon || !release_bag || g_policy->decide(&req->input) != 0 ||
        !gate_live(req->epoch, tid, generation) || seen_contains(req->captured_id) ||
        g_seen_count >= PGR_TX_SEEN_MAX) goto done;
    {
      std::lock_guard<std::mutex> lock(g_state_mutex);
      if (g_pending_count >= PGR_TX_PENDING_MAX || g_fifo_count >= PGR_TX_EVENT_CAP ||
          g_token_serial >= ((uint64_t{1} << 53) - 1)) {
        g_overflow = true; g_fault = true; rc = PGR_TX_ERR_OVERFLOW; goto done;
      }
    }

    // Latch seen + issued BEFORE the invoke; move the roots into the transaction; publish the token.
    seen_add(req->captured_id);
    uint32_t slot = g_pending_count++;
    Txn &t = g_pending[slot];
    t = Txn{};
    uint64_t token;
    { std::lock_guard<std::mutex> lock(g_state_mutex); token = ++g_token_serial; }
    t.token = token;
    t.captured_id = req->captured_id;
    t.epoch = req->epoch;
    t.deadline_ms = now + PGR_TX_TIMEOUT_MS;
    t.bag_root = bag_root;
    t.pokemon_root = pokemon_root;
    bag_root = pokemon_root = 0;  // ownership moved to the transaction
    *out_token = token;
    publish();
    push_event(PGR_TX_EVENT_REQUEST, 0, token, 0, tid);

    // Issue the release. A synchronous result/error callback may fire inside this invoke; the observers
    // only set flags + emit a record (the reap is deferred to leave_frame), so the slot below is safe
    // to consult. Re-resolve bag/pokemon from their roots.
    void *rel_params[1] = {release_pokemon};
    void *rel_exc = nullptr;
    void *promise = managed_invoke(g_cfg.m_release, release_bag, rel_params, &rel_exc);

    int idx = find_pending_by_token(token);
    if (idx >= 0 && !g_pending[idx].finished && !g_pending_stop.load()) {
      if (rel_exc != nullptr || promise == nullptr) {
        g_pending[idx].uncertain = true;  // uncertain: retained, quarantined, never retried
      } else {
        g_pending[idx].promise_root = hold(promise);
        if (!g_pending[idx].promise_root) g_pending[idx].uncertain = true;
      }
    }
    rc = PGR_TX_ISSUED;
  }

done:
  drop(&bag_root);
  drop(&pokemon_root);
  return rc;
}

// --- observe halves ----------------------------------------------------------------
uint64_t do_observe_begin(void *callback, void *response, int is_error, uint64_t) {
  uintptr_t cb_root = hold(callback);
  if (!cb_root) return 0;
  uintptr_t response_root = response ? hold(response) : 0;
  uintptr_t pokemon_root = 0, bag_root = 0;
  uint64_t ticket = 0;
  if (!is_exact(gc_get()(cb_root), g_cfg.class_callback) || (response && !response_root)) goto done;
  pokemon_root = hold(field_ptr(gc_get()(cb_root), g_cfg.f_callback_pokemon));
  if (!pokemon_root || !is_exact(gc_get()(pokemon_root), g_cfg.class_pokemon)) goto done;
  bag_root = hold(field_ptr(gc_get()(cb_root), g_cfg.f_callback_bag));
  if (!bag_root) goto done;
  {
    const uint64_t id = field_u64(gc_get()(pokemon_root), g_cfg.f_id);
    const int idx = find_pending_match(id, gc_get()(pokemon_root), gc_get()(bag_root));
    if (idx >= 0 && !g_pending_stop.load()) {
      const uint64_t token = g_pending[idx].token;
      const bool success = !is_error && response_root &&
          is_exact(gc_get()(response_root), g_cfg.class_release_out) &&
          field_i32(gc_get()(response_root), g_cfg.f_status) == g_cfg.success_literal;
      if (!g_pending_stop.load()) ticket = ticket_alloc(token, success);
    }
  }
done:
  drop(&bag_root); drop(&pokemon_root); drop(&response_root); drop(&cb_root);
  return ticket;
}

void do_observe_end(uint64_t ticket, uint64_t tid) {
  uint64_t token = 0;
  bool success = false;
  if (ticket_consume(ticket, &token, &success) < 0) return;  // stale / unknown: no effect
  int idx = find_pending_by_token(token);
  if (idx < 0 || g_pending[idx].finished) return;  // already reaped / completed
  g_pending[idx].finished = true;
  g_pending[idx].success = success;
  publish();
  push_event(PGR_TX_EVENT_COMPLETE, 0, token, success ? 1 : 0, tid);
}

// --- public API --------------------------------------------------------------------
bool validate_config(const PgrTransferExecConfig *c, int *rc) {
  if (c == nullptr) { *rc = PGR_TX_ERR_NULL_ARG; return false; }
  if (c->version != PGR_TX_CONFIG_VERSION) { *rc = PGR_TX_ERR_BAD_VERSION; return false; }
  if (c->size != sizeof(PgrTransferExecConfig)) { *rc = PGR_TX_ERR_BAD_SIZE; return false; }
  void *const *ptrs = reinterpret_cast<void *const *>(&c->gchandle_new);
  for (int i = 0; i < 16; ++i) {
    if (ptrs[i] == nullptr) { *rc = PGR_TX_ERR_MISSING_PTR; return false; }
  }
  *rc = PGR_TX_OK;
  return true;
}

int api_install(const PgrTransferExecConfig *config) {
  int rc = PGR_TX_OK;
  if (!validate_config(config, &rc)) return rc;
  uint64_t probe = 0;
  if (!read_boottime_ms(&probe)) return PGR_TX_ERR_CLOCK_FAILURE;
  const PgrTransferApi *policy = static_cast<const PgrTransferApi *>(pgrazor_transfer_policy_api(1));
  if (policy == nullptr || policy->decide == nullptr) return PGR_TX_ERR_NO_POLICY;

  std::lock_guard<std::mutex> install_lock(g_install_mutex);
  if (g_installed.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    if (std::memcmp(&g_cfg, config, sizeof(*config)) != 0) return PGR_TX_ERR_RECONFIG;
    return PGR_TX_OK;
  }
  if (g_install_attempted) return g_install_error;
  g_install_attempted = true;
  {
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    g_cfg = *config;
    g_policy = policy;
    g_enabled = 0;
    g_lease_until_ms = 0;
    g_epoch = 0;
    g_fault = false;
  }
  g_pending_stop.store(false);
  g_pending_count = 0;
  g_seen_count = 0;
  publish();
  g_installed.store(true, std::memory_order_release);
  return g_install_error = PGR_TX_OK;
}

int api_update(int enabled, uint64_t remaining, uint64_t epoch, uint64_t proven_tid) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_TX_ERR_NOT_INSTALLED;
  uint64_t now = 0;
  const bool clock_ok = read_boottime_ms(&now);
  std::lock_guard<std::mutex> lock(g_state_mutex);
  const uint64_t owner = g_owner_tid.load(std::memory_order_acquire);
  const bool on_owner = clock_ok && proven_tid != 0 && current_tid() == proven_tid && (owner == 0 || owner == proven_tid);
  // A foreign thread (owner latched elsewhere), a bad clock/tid, or a stop/cleanup in flight can only
  // REVOKE. Arming requires the owner thread and enabled != 0.
  if (!on_owner || !enabled || g_pending_stop.load() || g_cleaning.load()) {
    if (g_enabled || g_lease_until_ms) ++g_gen;
    g_enabled = 0;
    g_lease_until_ms = 0;
    if (on_owner) g_epoch = epoch;  // the owner may record a new epoch even while disabling
    return clock_ok ? PGR_TX_OK : PGR_TX_ERR_CLOCK_FAILURE;
  }
  remaining = remaining > kMaxLeaseMs ? kMaxLeaseMs : remaining;
  if (!g_enabled || g_epoch != epoch || !remaining || !g_lease_until_ms || now >= g_lease_until_ms) ++g_gen;
  g_enabled = 1;
  g_lease_until_ms = remaining ? now + remaining : 0;
  g_epoch = epoch;
  return PGR_TX_OK;
}

int api_submit(const PgrTransferExecRequest *request, uint64_t *out_token) {
  if (out_token != nullptr) *out_token = 0;
  if (!g_installed.load(std::memory_order_acquire)) return PGR_TX_ERR_NOT_INSTALLED;
  if (request == nullptr || out_token == nullptr) return PGR_TX_ERR_NULL_ARG;
  if (request->version != PGR_TX_REQUEST_VERSION) return PGR_TX_ERR_BAD_VERSION;
  if (request->size != sizeof(PgrTransferExecRequest)) return PGR_TX_ERR_BAD_SIZE;

  const uint64_t tid = current_tid();
  if (request->tid == 0 || tid != request->tid) return PGR_TX_OK;  // wrong thread: refused
  if (!owner_ready(tid)) return PGR_TX_OK;
  if (g_pending_stop.load() || g_cleaning.load()) return PGR_TX_OK;  // stop / cleanup rearm denied
  if (g_active_depth.load(std::memory_order_relaxed) != 0) return PGR_TX_OK;  // no reentrant submit

  g_active_depth.fetch_add(1, std::memory_order_relaxed);
  int rc = do_submit(request, tid, out_token);
  leave_frame();
  return rc;
}

uint64_t api_observe_begin(void *callback, void *response, int is_error, uint64_t proven_tid) {
  if (!g_installed.load(std::memory_order_acquire)) return 0;
  const uint64_t tid = current_tid();
  if (proven_tid == 0 || tid != proven_tid || callback == nullptr) return 0;
  if (!owner_ready(tid)) return 0;

  g_active_depth.fetch_add(1, std::memory_order_relaxed);
  uint64_t ticket = do_observe_begin(callback, response, is_error, tid);
  leave_frame();
  return ticket;
}

int api_observe_end(uint64_t ticket, uint64_t proven_tid) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_TX_ERR_NOT_INSTALLED;
  const uint64_t tid = current_tid();
  if (proven_tid == 0 || tid != proven_tid) return PGR_TX_OK;
  if (!owner_ready(tid)) return PGR_TX_OK;

  g_active_depth.fetch_add(1, std::memory_order_relaxed);
  do_observe_end(ticket, tid);
  leave_frame();
  return PGR_TX_OK;
}

int api_tick(uint64_t proven_tid, int stopping) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_TX_ERR_NOT_INSTALLED;
  if (stopping) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_pending_stop.store(true);
    g_enabled = 0;
    g_lease_until_ms = 0;
    ++g_gen;
  }
  const uint64_t tid = current_tid();
  if (proven_tid == 0 || tid != proven_tid || !owner_ready(tid)) return PGR_TX_OK;  // revoke latched; no teardown off-owner
  if (g_active_depth.load(std::memory_order_relaxed) != 0) {
    if (stopping) g_pending_stop.store(true);  // defer teardown to the outermost frame
    return PGR_TX_OK;
  }

  g_active_depth.fetch_add(1, std::memory_order_relaxed);
  if (g_pending_stop.load()) { leave_frame(); return PGR_TX_OK; }  // teardown happens in leave_frame

  uint64_t now = 0;
  if (read_boottime_ms(&now)) {
    for (uint32_t i = 0; i < g_pending_count; ++i) {
      Txn &t = g_pending[i];
      if (!t.finished && !t.timed_out && now >= t.deadline_ms) {
        t.timed_out = true;  // uncertain, once; the request is retained until a callback or a stop
        push_event(PGR_TX_EVENT_TIMEOUT, 0, t.token, 0, tid);
      }
    }
  }
  leave_frame();  // reaps completed transactions
  return PGR_TX_OK;
}

int api_sample(PgrTransferExecSample *out) {
  if (out == nullptr) return PGR_TX_ERR_NULL_ARG;
  const uint32_t roots = root_total();
  const uint32_t pending = g_m_pending.load(std::memory_order_relaxed);
  const uint32_t seen = g_m_seen.load(std::memory_order_relaxed);
  const uint32_t tickets = g_m_tickets.load(std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(g_state_mutex);
  out->version = PGR_TX_SAMPLE_VERSION;
  out->size = sizeof(PgrTransferExecSample);
  out->request_serial = g_request_serial;
  out->complete_serial = g_complete_serial;
  out->timeout_serial = g_timeout_serial;
  out->error_serial = g_error_serial;
  out->last_tid = g_last_tid;
  out->last_token = g_last_token;
  out->roots = roots;
  out->pending = pending;
  out->seen = seen;
  out->fault = g_fault ? 1u : 0u;
  out->overflow = g_overflow ? 1u : 0u;
  out->pending_events = g_fifo_count;
  out->open_tickets = tickets;
  out->reserved = 0;
  return PGR_TX_OK;
}

int api_next_event(PgrTransferExecEvent *out) {
  if (out == nullptr) return PGR_TX_ERR_NULL_ARG;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  if (g_fifo_count == 0) return 0;
  *out = g_fifo[g_fifo_head];
  g_fifo_head = (g_fifo_head + 1) % PGR_TX_EVENT_CAP;
  --g_fifo_count;
  return 1;
}

const PgrTransferExecApi kApiTable = {
    PGR_TX_API_VERSION, static_cast<uint32_t>(sizeof(PgrTransferExecApi)),
    &api_install,       &api_update,
    &api_submit,        &api_observe_begin,
    &api_observe_end,   &api_tick,
    &api_sample,        &api_next_event,
};

}  // namespace

extern "C" const void *pgrazor_transfer_execution_api(uint32_t version) {
  if (version != PGR_TX_API_VERSION) return nullptr;
  return &kApiTable;
}
