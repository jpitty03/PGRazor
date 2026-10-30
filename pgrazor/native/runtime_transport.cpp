#include "runtime_transport.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <time.h>

// Feature 52 native runtime transport. See runtime_transport.h for the ABI and the division of labour with
// the Java host (NativeRuntimeTransport.java) and the future compiled gameplay consumers. Built
// -fno-exceptions / -fno-rtti like every sibling. This module installs NO hook, makes NO managed/il2cpp call,
// stores NO pointer across the boundary, and NEVER holds a lock across a callback (there is no callback). All
// cross-boundary data is compiled primitive fields plus length-prefixed, UTF-8-validated bytes.

static_assert(sizeof(PgrRuntimeTransportConfig) == 32, "config must be 32 bytes");
static_assert(sizeof(PgrRuntimeTransportSample) == 88, "sample must be 88 bytes");
static_assert(sizeof(PgrRuntimeTransportApi) == 96, "table must be 96 bytes");

namespace {

// Compiled action schema: per opcode, the scalar and string arg counts AND the ordered scalar types (legacy
// translated.js order). emit() validates both arity and each scalar's type/domain against this table -- a bad
// native producer can never poison the host with an out-of-type/out-of-domain scalar. Scalar types: B boolean
// (0/1), I int (signed-32), L long (exact 64-bit), D double (raw IEEE-754 bits, must decode finite).
enum ScalarType : uint8_t { ST_B = 0, ST_I = 1, ST_L = 2, ST_D = 3 };
struct ActionSchema { uint8_t scalars; uint8_t strings; uint8_t types[PGR_TRANSPORT_MAX_SCALARS]; };
constexpr ActionSchema kActions[PGR_TRANSPORT_ACTIONS] = {
  {0, 2, {}},                       // 0  accountIdentity(String,String)
  {1, 0, {ST_B}},                   // 1  setAccountsReady(boolean)
  {0, 0, {}},                       // 2  accountSessionEnded()
  {0, 0, {}},                       // 3  accountLogoutComplete()
  {0, 0, {}},                       // 4  accountLogoutFailed()
  {0, 0, {}},                       // 5  accountSignInResetComplete()
  {0, 0, {}},                       // 6  accountSessionSaved()
  {1, 1, {ST_B}},                   // 7  setGameRoutesReady(boolean,String)
  {0, 1, {}},                       // 8  setGameRoutesSnapshot(String)
  {0, 1, {}},                       // 9  gameRoutesAction(String)
  {5, 0, {ST_D, ST_D, ST_I, ST_I, ST_D}}, // 10 applyMapTap(double,double,int,int,double)
  {0, 1, {}},                       // 11 cameraDiagnostic(String)
  {1, 0, {ST_B}},                   // 12 configureCameraDiagnostics(boolean)
  {1, 0, {ST_B}},                   // 13 configureCameraSteering(boolean)
  {1, 0, {ST_B}},                   // 14 configureInteractionDiagnostics(boolean)
  {1, 0, {ST_B}},                   // 15 configureQuickBall(boolean)
  {1, 0, {ST_B}},                   // 16 configureQuickSummary(boolean)
  {0, 1, {}},                       // 17 cooldownAction(String)
  {0, 1, {}},                       // 18 embedded(String)
  {0, 2, {}},                       // 19 event(String,String)
  {0, 1, {}},                       // 20 interactionDiagnostic(String)
  {0, 1, {}},                       // 21 invalidateCameraHeading(String)
  {0, 1, {}},                       // 22 quickBallOutcome(String) -> boolean (return ignored by the host drain)
  {1, 1, {ST_B}},                   // 23 setAutoTransferReady(boolean,String)
  {0, 1, {}},                       // 24 setBattleStatus(String)
  {1, 1, {ST_B}},                   // 25 setBattlesReady(boolean,String)
  {1, 0, {ST_B}},                   // 26 setBerryReady(boolean)
  {1, 1, {ST_B}},                   // 27 setBuddyReady(boolean,String)
  {0, 1, {}},                       // 28 setBuddyStatus(String)
  {1, 0, {ST_B}},                   // 29 setCameraDiagnosticRunning(boolean)
  {0, 1, {}},                       // 30 setCooldownEncounter(String)
  {2, 0, {ST_B, ST_B}},             // 31 setEncounterBerry(boolean,boolean)
  {0, 1, {}},                       // 32 setGiftStatus(String)
  {1, 1, {ST_B}},                   // 33 setGiftsReady(boolean,String)
  {1, 1, {ST_B}},                   // 34 setIncubatorReady(boolean,String)
  {0, 1, {}},                       // 35 setIncubatorStatus(String)
  {1, 0, {ST_B}},                   // 36 setInteractionDiagnosticRunning(boolean)
  {0, 1, {}},                       // 37 setItemCatalog(String)
  {1, 1, {ST_B}},                   // 38 setItemReady(boolean,String)
  {0, 1, {}},                       // 39 setItemStatus(String)
  {1, 0, {ST_B}},                   // 40 setIvOwnedReady(boolean)
  {0, 1, {}},                       // 41 setMapPoiSnapshot(String)
  {1, 0, {ST_B}},                   // 42 setMapTapReady(boolean)
  {1, 1, {ST_B}},                   // 43 setQuickBallReady(boolean,String)
  {1, 1, {ST_B}},                   // 44 setQuickSpinReady(boolean,String)
  {1, 1, {ST_B}},                   // 45 setQuickSummaryReady(boolean,String)
  {1, 1, {ST_B}},                   // 46 setRocketReady(boolean,String)
  {0, 1, {}},                       // 47 setRocketStatus(String)
  {2, 0, {ST_D, ST_D}},             // 48 setSpawnRange(double,double)
  {1, 0, {ST_B}},                   // 49 setSpawnReady(boolean)
  {1, 0, {ST_B}},                   // 50 setThrowArReady(boolean)
  {1, 0, {ST_B}},                   // 51 setThrowAlwaysHitReady(boolean)
  {1, 0, {ST_B}},                   // 52 setThrowReady(boolean)
  {1, 1, {ST_B}},                   // 53 setVgpReady(boolean,String)
  {0, 1, {}},                       // 54 setVgpStatus(String)
  {4, 0, {ST_B, ST_D, ST_D, ST_B}}, // 55 setVgpTarget(boolean,double,double,boolean)
  {0, 1, {}},                       // 56 transferResult(String)
  {4, 0, {ST_D, ST_I, ST_B, ST_L}}, // 57 updateCameraHeadingAt(double,int,boolean,long)
  {1, 1, {ST_B}}                    // 58 vgpResult(String,boolean)
};

// Validate one scalar against its compiled type. Returns PGR_TRANSPORT_OK or PGR_TRANSPORT_ERR_DOMAIN. Longs are NOT bounded
// here (the typed native wire carries the exact 64-bit value -- no JS safe-integer clamp); any per-opcode long
// domain is enforced separately below.
int validate_scalar(uint8_t type, int64_t v) {
  switch (type) {
    case ST_B: return (v == 0 || v == 1) ? PGR_TRANSPORT_OK : PGR_TRANSPORT_ERR_DOMAIN;
    case ST_I: return (v >= INT32_MIN && v <= INT32_MAX) ? PGR_TRANSPORT_OK : PGR_TRANSPORT_ERR_DOMAIN;
    case ST_L: return PGR_TRANSPORT_OK;  // exact 64-bit; Long.MAX_VALUE is valid
    case ST_D: {
      double d;
      uint64_t bits = static_cast<uint64_t>(v);
      std::memcpy(&d, &bits, sizeof(d));
      return std::isfinite(d) ? PGR_TRANSPORT_OK : PGR_TRANSPORT_ERR_DOMAIN;
    }
    default: return PGR_TRANSPORT_ERR_DOMAIN;
  }
}

double scalar_as_double(int64_t v) {
  double d;
  uint64_t bits = static_cast<uint64_t>(v);
  std::memcpy(&d, &bits, sizeof(d));
  return d;
}

// Per-opcode value domain, mirroring the Java host drain exactly. Scalars already passed validate_scalar.
int validate_domain(uint32_t opcode, const int64_t *s) {
  if (opcode == 10) {  // applyMapTap: |lat|<=90, |lon|<=180, mode 0..3, sampledAt>=0
    if (std::fabs(scalar_as_double(s[0])) > 90.0) return PGR_TRANSPORT_ERR_DOMAIN;
    if (std::fabs(scalar_as_double(s[1])) > 180.0) return PGR_TRANSPORT_ERR_DOMAIN;
    int32_t mode = static_cast<int32_t>(s[2]);
    if (mode < 0 || mode > 3) return PGR_TRANSPORT_ERR_DOMAIN;
    if (scalar_as_double(s[4]) < 0.0) return PGR_TRANSPORT_ERR_DOMAIN;
  } else if (opcode == 57) {  // updateCameraHeadingAt: timestamp (long arg3) >= 0
    if (s[3] < 0) return PGR_TRANSPORT_ERR_DOMAIN;
  }
  return PGR_TRANSPORT_OK;
}

// Guarded booleans (active, vgpEnabled, *Allowed, *Requested) return 0 on a stale snapshot instead of STALE,
// reproducing the legacy translated.js client guard. Indexed by bool reader id (see the contract).
constexpr bool kGuardedBool[PGR_TRANSPORT_BOOLS] = {
  true,  true,  true,  true,  false, true,  true,  true,  true,  true,  // 0..9
  true,  false, true,  false, true,  false, true,  false, false, true,  // 10..19
  false, true,  true,  true,  true,  true,  true,  true,  false, true,  // 20..29
  false, false, false, false, false, false, true,  true,  false, true, // 30..39 (eventBoxesAllowed)
};

// --- little-endian frame accessors (the wire is LE; ARM64 is LE, so these are plain loads/stores) ---------
uint32_t load_u32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t load_u64(const uint8_t *p) {
  return static_cast<uint64_t>(load_u32(p)) | (static_cast<uint64_t>(load_u32(p + 4)) << 32);
}
void store_u32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
void store_u64(uint8_t *p, uint64_t v) { store_u32(p, static_cast<uint32_t>(v)); store_u32(p + 4, static_cast<uint32_t>(v >> 32)); }

// Strict UTF-8 validation (no overlong forms, no surrogates, no > U+10FFFF). Matches the host's strict decoder.
bool valid_utf8(const char *s, uint32_t len) {
  uint32_t i = 0;
  while (i < len) {
    uint8_t c = static_cast<uint8_t>(s[i]);
    uint32_t extra, min_cp, cp;
    if (c < 0x80) { ++i; continue; }
    else if ((c & 0xE0) == 0xC0) { extra = 1; min_cp = 0x80; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; min_cp = 0x800; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; min_cp = 0x10000; cp = c & 0x07; }
    else return false;
    if (i + extra >= len) return false;
    for (uint32_t k = 1; k <= extra; ++k) {
      uint8_t cc = static_cast<uint8_t>(s[i + k]);
      if ((cc & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cc & 0x3F);
    }
    if (cp < min_cp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    i += extra + 1;
  }
  return true;
}

struct Snapshot {
  bool valid = false;
  bool inert = false;
  uint64_t seq = 0;
  uint64_t accept_ms = 0;   // CLOCK_BOOTTIME at accept
  uint8_t bools[PGR_TRANSPORT_BOOLS] = {};
  int32_t ints[PGR_TRANSPORT_INTS] = {};
  double doubles[PGR_TRANSPORT_DOUBLES] = {};
  uint64_t pulses[PGR_TRANSPORT_PULSES] = {};
  std::string strings[PGR_TRANSPORT_STRINGS];
};

struct Event {
  uint32_t opcode = 0;
  uint32_t n_scalars = 0;
  uint32_t n_strings = 0;
  uint64_t seq = 0;
  int64_t scalars[PGR_TRANSPORT_MAX_SCALARS] = {};
  std::string strings[PGR_TRANSPORT_MAX_STR_ARGS];
};

// --- lifetime / publication --------------------------------------------------------
std::mutex g_install_mutex;
std::mutex g_state_mutex;
PgrRuntimeTransportConfig g_cfg{};
std::atomic<bool> g_installed{false};
bool g_install_attempted = false;
int g_install_error = PGR_TRANSPORT_ERR_NOT_INSTALLED;

// --- state (guarded by g_state_mutex) ----------------------------------------------
bool g_stopped = false;
Snapshot g_snapshot;
std::deque<Event> g_queue;
uint32_t g_queue_bytes = 0;
uint64_t g_event_seq = 0;
uint64_t g_generation = 1;
uint64_t g_pulse_consumed[PGR_TRANSPORT_PULSES] = {};

// --- telemetry (atomic so sample is lock-free for the counters) --------------------
std::atomic<uint64_t> g_snap_ok{0}, g_snap_bad{0}, g_emitted{0}, g_drained{0}, g_dropped{0};
std::atomic<uint32_t> g_fault{0};

bool read_boottime_ms(uint64_t *out_ms) {
  struct timespec ts {};
  if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0 || ts.tv_sec < 0 || ts.tv_nsec < 0) return false;
  *out_ms = static_cast<uint64_t>(ts.tv_sec) * 1000ULL + static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
  return true;
}

// Caller holds g_state_mutex. Fresh == installed && !stopped && a valid, non-inert snapshot within the lease.
bool snapshot_fresh_locked(uint64_t now) {
  if (g_stopped || !g_snapshot.valid || g_snapshot.inert) return false;
  return now >= g_snapshot.accept_ms && (now - g_snapshot.accept_ms) <= g_cfg.lease_ms;
}

bool config_ok(const PgrRuntimeTransportConfig *c, int *rc) {
  if (c == nullptr) { *rc = PGR_TRANSPORT_ERR_NULL_ARG; return false; }
  if (c->version != PGR_TRANSPORT_CONFIG_VERSION) { *rc = PGR_TRANSPORT_ERR_BAD_VERSION; return false; }
  if (c->size != sizeof(PgrRuntimeTransportConfig)) { *rc = PGR_TRANSPORT_ERR_BAD_SIZE; return false; }
  if (c->lease_ms < 1 || c->lease_ms > PGR_TRANSPORT_MAX_LEASE_MS) { *rc = PGR_TRANSPORT_ERR_BAD_BOUND; return false; }
  if (c->max_events < 1 || c->max_events > PGR_TRANSPORT_MAX_EVENTS) { *rc = PGR_TRANSPORT_ERR_BAD_BOUND; return false; }
  if (c->max_string_bytes < 1 || c->max_string_bytes > PGR_TRANSPORT_MAX_STRING) { *rc = PGR_TRANSPORT_ERR_BAD_BOUND; return false; }
  if (c->queue_byte_budget < 1 || c->queue_byte_budget > PGR_TRANSPORT_MAX_FRAME) { *rc = PGR_TRANSPORT_ERR_BAD_BOUND; return false; }
  if (c->reserved0 != 0 || c->reserved1 != 0) { *rc = PGR_TRANSPORT_ERR_BAD_BOUND; return false; }  // reserved must be 0
  *rc = PGR_TRANSPORT_OK;
  return true;
}

// Parse + validate a snapshot frame into `out`. Does not touch global state. Returns a status. max_string
// bounds each string's byte length (the host's configured per-string ceiling), strictly, not just the 1 MiB
// absolute ceiling.
int parse_snapshot(const uint8_t *p, uint32_t len, uint32_t max_string, Snapshot *out) {
  if (p == nullptr) return PGR_TRANSPORT_ERR_NULL_ARG;
  if (len < PGR_TRANSPORT_SNAPSHOT_FIXED + PGR_TRANSPORT_STRINGS * 4u || len > PGR_TRANSPORT_MAX_FRAME) return PGR_TRANSPORT_ERR_BAD_FRAME;
  if (load_u32(p + PGR_TRANSPORT_OFF_MAGIC) != PGR_TRANSPORT_SNAPSHOT_MAGIC) return PGR_TRANSPORT_ERR_BAD_FRAME;
  if (load_u32(p + PGR_TRANSPORT_OFF_VERSION) != PGR_TRANSPORT_FRAME_VERSION) return PGR_TRANSPORT_ERR_BAD_VERSION;
  if (load_u32(p + PGR_TRANSPORT_OFF_FRAME_SIZE) != len) return PGR_TRANSPORT_ERR_BAD_SIZE;

  uint32_t flags = load_u32(p + PGR_TRANSPORT_OFF_FLAGS);
  if ((flags & ~static_cast<uint32_t>(PGR_TRANSPORT_FLAG_INERT)) != 0) return PGR_TRANSPORT_ERR_BAD_FRAME;  // no unknown flags
  out->seq = load_u64(p + PGR_TRANSPORT_OFF_SEQ);
  out->inert = (flags & PGR_TRANSPORT_FLAG_INERT) != 0;
  for (uint32_t i = 0; i < PGR_TRANSPORT_BOOLS; ++i) {
    uint8_t b = p[PGR_TRANSPORT_OFF_BOOLS + i];
    if (b > 1) return PGR_TRANSPORT_ERR_BAD_FRAME;
    out->bools[i] = b;
  }
  // Validate padding if a future layout leaves any; v2 uses all forty boolean bytes.
  for (uint32_t i = PGR_TRANSPORT_OFF_BOOLS + PGR_TRANSPORT_BOOLS; i < PGR_TRANSPORT_OFF_INTS; ++i)
    if (p[i] != 0) return PGR_TRANSPORT_ERR_BAD_FRAME;
  for (uint32_t i = 0; i < PGR_TRANSPORT_INTS; ++i)
    out->ints[i] = static_cast<int32_t>(load_u32(p + PGR_TRANSPORT_OFF_INTS + i * 4));
  for (uint32_t i = 0; i < PGR_TRANSPORT_DOUBLES; ++i) {
    uint64_t bits = load_u64(p + PGR_TRANSPORT_OFF_DOUBLES + i * 8);
    double d;
    std::memcpy(&d, &bits, sizeof(d));
    out->doubles[i] = d;  // NaN sentinel for a "null" coord is accepted verbatim
  }
  for (uint32_t i = 0; i < PGR_TRANSPORT_PULSES; ++i)
    out->pulses[i] = load_u64(p + PGR_TRANSPORT_OFF_PULSES + i * 8);

  uint32_t at = PGR_TRANSPORT_OFF_STRINGS;
  for (uint32_t i = 0; i < PGR_TRANSPORT_STRINGS; ++i) {
    if (at + 4 > len) return PGR_TRANSPORT_ERR_BAD_FRAME;
    uint32_t slen = load_u32(p + at);
    at += 4;
    if (slen > max_string || at + slen > len) return PGR_TRANSPORT_ERR_BAD_FRAME;
    if (!valid_utf8(reinterpret_cast<const char *>(p + at), slen)) return PGR_TRANSPORT_ERR_BAD_UTF8;
    out->strings[i].assign(reinterpret_cast<const char *>(p + at), slen);
    at += slen;
  }
  if (at != len) return PGR_TRANSPORT_ERR_BAD_FRAME;  // no trailing bytes
  out->valid = true;
  return PGR_TRANSPORT_OK;
}

// Caller holds g_state_mutex. Serialize up to the drain cap of queued events into [out, out+cap).
int drain_locked(uint8_t *out, uint32_t cap, uint32_t *out_len) {
  if (cap < PGR_TRANSPORT_EVENT_HEADER) return PGR_TRANSPORT_ERR_RANGE;
  uint32_t at = PGR_TRANSPORT_EVENT_HEADER;
  uint32_t count = 0;
  uint64_t base_seq = g_queue.empty() ? 0 : g_queue.front().seq;
  while (count < PGR_TRANSPORT_DRAIN_CAP && !g_queue.empty()) {
    const Event &e = g_queue.front();
    uint32_t need = 12 + e.n_scalars * 8;  // opcode,n_scalars,n_strings + scalars
    for (uint32_t s = 0; s < e.n_strings; ++s) need += 4 + static_cast<uint32_t>(e.strings[s].size());
    if (at + need > cap) break;  // leave the rest queued for the next exchange (not a drop)
    store_u32(out + at, e.opcode); at += 4;
    store_u32(out + at, e.n_scalars); at += 4;
    store_u32(out + at, e.n_strings); at += 4;
    for (uint32_t s = 0; s < e.n_scalars; ++s) { store_u64(out + at, static_cast<uint64_t>(e.scalars[s])); at += 8; }
    for (uint32_t s = 0; s < e.n_strings; ++s) {
      uint32_t slen = static_cast<uint32_t>(e.strings[s].size());
      store_u32(out + at, slen); at += 4;
      std::memcpy(out + at, e.strings[s].data(), slen); at += slen;
    }
    for (uint32_t s = 0; s < e.n_strings; ++s) g_queue_bytes -= static_cast<uint32_t>(e.strings[s].size());
    g_queue.pop_front();
    ++count;
  }
  // The head event could not fit even this output buffer and nothing was drained: fail explicitly instead of
  // returning an empty frame on every call forever (emit() already rejects events too big for ANY frame).
  if (count == 0 && !g_queue.empty()) return PGR_TRANSPORT_ERR_RANGE;
  store_u32(out + PGR_TRANSPORT_OFF_MAGIC, PGR_TRANSPORT_EVENTS_MAGIC);
  store_u32(out + PGR_TRANSPORT_OFF_VERSION, PGR_TRANSPORT_FRAME_VERSION);
  store_u32(out + PGR_TRANSPORT_OFF_FRAME_SIZE, at);
  store_u32(out + 12, count);
  store_u64(out + 16, base_seq);
  store_u64(out + 24, g_generation);
  *out_len = at;
  g_drained.fetch_add(count, std::memory_order_relaxed);
  return PGR_TRANSPORT_OK;
}

// --- public API --------------------------------------------------------------------
int api_install(const PgrRuntimeTransportConfig *config) {
  int rc = PGR_TRANSPORT_OK;
  if (!config_ok(config, &rc)) return rc;
  uint64_t probe = 0;
  if (!read_boottime_ms(&probe)) return PGR_TRANSPORT_ERR_CLOCK;

  std::lock_guard<std::mutex> install_lock(g_install_mutex);
  if (g_installed.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    if (std::memcmp(&g_cfg, config, sizeof(*config)) != 0) return PGR_TRANSPORT_ERR_RECONFIG;
    return PGR_TRANSPORT_OK;
  }
  if (g_install_attempted) return g_install_error;
  g_install_attempted = true;
  {
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    g_cfg = *config;
    g_stopped = false;
    g_snapshot = Snapshot{};
    g_queue.clear();
    g_queue_bytes = 0;
    g_event_seq = 0;
    for (auto &c : g_pulse_consumed) c = 0;
  }
  g_installed.store(true, std::memory_order_release);
  return g_install_error = PGR_TRANSPORT_OK;
}

int api_exchange(const uint8_t *snapshot, uint32_t snap_len, uint8_t *out, uint32_t out_cap, uint32_t *out_len) {
  if (out == nullptr || out_len == nullptr) return PGR_TRANSPORT_ERR_NULL_ARG;
  *out_len = 0;
  if (!g_installed.load(std::memory_order_acquire)) return PGR_TRANSPORT_ERR_NOT_INSTALLED;
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) return PGR_TRANSPORT_ERR_CLOCK;

  int snap_rc = PGR_TRANSPORT_OK;
  Snapshot parsed;
  if (snap_len != 0) {
    snap_rc = parse_snapshot(snapshot, snap_len, g_cfg.max_string_bytes, &parsed);
    if (snap_rc == PGR_TRANSPORT_OK) parsed.accept_ms = now;
  }

  std::lock_guard<std::mutex> lock(g_state_mutex);
  if (snap_len != 0) {
    // A well-formed frame still has to be STRICTLY NEWER than the stored snapshot: seq must advance and no
    // monotonic pulse count may regress. A stale/replayed frame is rejected -- it does NOT overwrite the
    // stored snapshot and does NOT refresh the lease (accepting it would revive expired gameplay state).
    if (snap_rc == PGR_TRANSPORT_OK && g_snapshot.valid) {
      bool replay = parsed.seq <= g_snapshot.seq;
      for (uint32_t i = 0; !replay && i < PGR_TRANSPORT_PULSES; ++i)
        if (parsed.pulses[i] < g_snapshot.pulses[i]) replay = true;
      if (replay) snap_rc = PGR_TRANSPORT_ERR_REPLAY;
    }
    if (snap_rc == PGR_TRANSPORT_OK) {
      // Pulse counts are monotonic across snapshots (the host keeps cumulative counts); the per-pulse
      // consumed baselines persist and only reset on install(), so take_pulse() keeps edge semantics.
      g_snapshot = std::move(parsed);
      g_snap_ok.fetch_add(1, std::memory_order_relaxed);
    } else {
      g_snap_bad.fetch_add(1, std::memory_order_relaxed);
      g_fault.store(1, std::memory_order_relaxed);
      // The previously stored snapshot is left intact; the drain still runs below.
    }
  }
  int rc = drain_locked(out, out_cap, out_len);
  if (rc != PGR_TRANSPORT_OK) return rc;
  return snap_rc;  // surfaces a rejected snapshot while still delivering the drained events
}

int api_emit(uint32_t opcode, const int64_t *scalars, uint32_t n_scalars,
             const PgrRuntimeTransportString *strings, uint32_t n_strings) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_TRANSPORT_ERR_NOT_INSTALLED;
  if (opcode >= PGR_TRANSPORT_ACTIONS) return PGR_TRANSPORT_ERR_BAD_OPCODE;
  const ActionSchema &a = kActions[opcode];
  if (n_scalars != a.scalars || n_strings != a.strings) return PGR_TRANSPORT_ERR_BAD_SCHEMA;
  if (n_scalars > 0 && scalars == nullptr) return PGR_TRANSPORT_ERR_NULL_ARG;
  if (n_strings > 0 && strings == nullptr) return PGR_TRANSPORT_ERR_NULL_ARG;
  // Full typed validation (not arity alone): each scalar must match its compiled type, then the per-opcode
  // value domain must hold. A bad native producer cannot push a poisoned value through to the host.
  for (uint32_t i = 0; i < n_scalars; ++i) {
    int srv = validate_scalar(a.types[i], scalars[i]);
    if (srv != PGR_TRANSPORT_OK) return srv;
  }
  { int drv = validate_domain(opcode, scalars); if (drv != PGR_TRANSPORT_OK) return drv; }

  Event e;
  e.opcode = opcode;
  e.n_scalars = n_scalars;
  e.n_strings = n_strings;
  for (uint32_t i = 0; i < n_scalars; ++i) e.scalars[i] = scalars[i];
  uint32_t add_bytes = 0;
  uint64_t serialized = 12 + static_cast<uint64_t>(n_scalars) * 8;  // opcode,n_scalars,n_strings + scalars
  for (uint32_t i = 0; i < n_strings; ++i) {
    const PgrRuntimeTransportString &s = strings[i];
    if (s.len > 0 && s.ptr == nullptr) return PGR_TRANSPORT_ERR_NULL_ARG;
    if (s.len > g_cfg.max_string_bytes) return PGR_TRANSPORT_ERR_RANGE;
    if (!valid_utf8(s.ptr, s.len)) return PGR_TRANSPORT_ERR_BAD_UTF8;
    e.strings[i].assign(s.ptr ? s.ptr : "", s.len);
    add_bytes += s.len;
    serialized += 4ULL + s.len;
  }
  // Reject up front any event whose serialized form could never fit a bounded frame; otherwise it would sit at
  // the head of the queue and wedge every future drain (the queue could never make progress).
  if (PGR_TRANSPORT_EVENT_HEADER + serialized > PGR_TRANSPORT_MAX_FRAME) return PGR_TRANSPORT_ERR_TOO_LARGE;

  std::lock_guard<std::mutex> lock(g_state_mutex);
  if (g_stopped) return PGR_TRANSPORT_STOPPED;  // clean shutdown no-op, not a fault
  if (g_queue.size() >= g_cfg.max_events || g_queue_bytes + add_bytes > g_cfg.queue_byte_budget) {
    g_dropped.fetch_add(1, std::memory_order_relaxed);
    g_fault.store(1, std::memory_order_relaxed);
    return PGR_TRANSPORT_ERR_OVERFLOW;  // explicit backpressure; never a silent drop
  }
  e.seq = ++g_event_seq;
  g_queue_bytes += add_bytes;
  g_queue.push_back(std::move(e));
  g_emitted.fetch_add(1, std::memory_order_relaxed);
  return PGR_TRANSPORT_OK;
}

int api_get_bool(uint32_t field, int *out) {
  if (out == nullptr) return PGR_TRANSPORT_ERR_NULL_ARG;
  if (field >= PGR_TRANSPORT_BOOLS) return PGR_TRANSPORT_ERR_RANGE;
  if (!g_installed.load(std::memory_order_acquire)) return PGR_TRANSPORT_ERR_NOT_INSTALLED;
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) return PGR_TRANSPORT_ERR_CLOCK;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  if (!snapshot_fresh_locked(now)) {
    if (kGuardedBool[field]) { *out = 0; return PGR_TRANSPORT_OK; }  // legacy guarded false default
    return PGR_TRANSPORT_STALE;
  }
  *out = g_snapshot.bools[field] ? 1 : 0;
  return PGR_TRANSPORT_OK;
}

int api_get_int(uint32_t field, int32_t *out) {
  if (out == nullptr) return PGR_TRANSPORT_ERR_NULL_ARG;
  if (field >= PGR_TRANSPORT_INTS) return PGR_TRANSPORT_ERR_RANGE;
  if (!g_installed.load(std::memory_order_acquire)) return PGR_TRANSPORT_ERR_NOT_INSTALLED;
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) return PGR_TRANSPORT_ERR_CLOCK;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  if (!snapshot_fresh_locked(now)) return PGR_TRANSPORT_STALE;
  *out = g_snapshot.ints[field];
  return PGR_TRANSPORT_OK;
}

int api_get_double(uint32_t field, double *out, int *has_value) {
  if (out == nullptr || has_value == nullptr) return PGR_TRANSPORT_ERR_NULL_ARG;
  if (field >= PGR_TRANSPORT_DOUBLES) return PGR_TRANSPORT_ERR_RANGE;
  if (!g_installed.load(std::memory_order_acquire)) return PGR_TRANSPORT_ERR_NOT_INSTALLED;
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) return PGR_TRANSPORT_ERR_CLOCK;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  if (!snapshot_fresh_locked(now)) return PGR_TRANSPORT_STALE;
  double d = g_snapshot.doubles[field];
  bool finite = std::isfinite(d);
  *has_value = finite ? 1 : 0;  // NaN/inf sentinel == legacy JSON null coord
  *out = finite ? d : 0.0;
  return PGR_TRANSPORT_OK;
}

int copy_out(const std::string &s, char *buf, uint32_t cap, uint32_t *out_len) {
  uint32_t n = static_cast<uint32_t>(s.size());
  if (out_len != nullptr) *out_len = n;
  if (buf == nullptr) return PGR_TRANSPORT_ERR_NULL_ARG;
  if (cap < n) return PGR_TRANSPORT_ERR_RANGE;
  std::memcpy(buf, s.data(), n);
  return PGR_TRANSPORT_OK;
}

int api_get_string(uint32_t field, char *buf, uint32_t cap, uint32_t *out_len) {
  if (out_len == nullptr) return PGR_TRANSPORT_ERR_NULL_ARG;
  *out_len = 0;
  if (field >= PGR_TRANSPORT_STRINGS) return PGR_TRANSPORT_ERR_RANGE;
  if (!g_installed.load(std::memory_order_acquire)) return PGR_TRANSPORT_ERR_NOT_INSTALLED;
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) return PGR_TRANSPORT_ERR_CLOCK;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  if (!snapshot_fresh_locked(now)) return PGR_TRANSPORT_STALE;
  return copy_out(g_snapshot.strings[field], buf, cap, out_len);
}

int api_get_command_snapshot(char *cmd, uint32_t cmd_cap, uint32_t *cmd_len,
                             char *tok, uint32_t tok_cap, uint32_t *tok_len) {
  if (cmd_len == nullptr || tok_len == nullptr) return PGR_TRANSPORT_ERR_NULL_ARG;
  *cmd_len = 0; *tok_len = 0;
  if (!g_installed.load(std::memory_order_acquire)) return PGR_TRANSPORT_ERR_NOT_INSTALLED;
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) return PGR_TRANSPORT_ERR_CLOCK;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  if (!snapshot_fresh_locked(now)) return PGR_TRANSPORT_STALE;
  const std::string &c = g_snapshot.strings[5];
  const std::string &t = g_snapshot.strings[6];
  // Atomic pair: report BOTH required lengths and validate BOTH capacities BEFORE writing either, so a failure
  // on the token can never leave a partially written command behind (the pair is all-or-nothing).
  uint32_t cn = static_cast<uint32_t>(c.size());
  uint32_t tn = static_cast<uint32_t>(t.size());
  *cmd_len = cn;
  *tok_len = tn;
  if (cmd == nullptr || tok == nullptr) return PGR_TRANSPORT_ERR_NULL_ARG;
  if (cmd_cap < cn || tok_cap < tn) return PGR_TRANSPORT_ERR_RANGE;  // neither copied
  std::memcpy(cmd, c.data(), cn);
  std::memcpy(tok, t.data(), tn);
  return PGR_TRANSPORT_OK;
}

int api_take_pulse(uint32_t pulse, int *out_fired) {
  if (out_fired == nullptr) return PGR_TRANSPORT_ERR_NULL_ARG;
  *out_fired = 0;
  if (pulse >= PGR_TRANSPORT_PULSES) return PGR_TRANSPORT_ERR_RANGE;
  if (!g_installed.load(std::memory_order_acquire)) return PGR_TRANSPORT_ERR_NOT_INSTALLED;
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) return PGR_TRANSPORT_ERR_CLOCK;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  if (!snapshot_fresh_locked(now)) return PGR_TRANSPORT_OK;  // legacy client: stale -> no edge
  uint64_t count = g_snapshot.pulses[pulse];
  if (count > g_pulse_consumed[pulse]) { g_pulse_consumed[pulse] = count; *out_fired = 1; }
  return PGR_TRANSPORT_OK;
}

int api_sample(PgrRuntimeTransportSample *out) {
  if (out == nullptr) return PGR_TRANSPORT_ERR_NULL_ARG;
  uint32_t depth = 0, bytes = 0, fresh = 0;
  uint64_t gen = 0, snap_seq = 0;
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    uint64_t now = 0;
    bool clock_ok = read_boottime_ms(&now);
    depth = static_cast<uint32_t>(g_queue.size());
    bytes = g_queue_bytes;
    fresh = (clock_ok && snapshot_fresh_locked(now)) ? 1 : 0;
    gen = g_generation;
    snap_seq = g_snapshot.valid ? g_snapshot.seq : 0;
  }
  out->version = PGR_TRANSPORT_SAMPLE_VERSION;
  out->size = sizeof(PgrRuntimeTransportSample);
  out->snapshots_accepted = g_snap_ok.load(std::memory_order_relaxed);
  out->snapshots_rejected = g_snap_bad.load(std::memory_order_relaxed);
  out->events_emitted = g_emitted.load(std::memory_order_relaxed);
  out->events_drained = g_drained.load(std::memory_order_relaxed);
  out->events_dropped = g_dropped.load(std::memory_order_relaxed);
  out->last_event_seq = g_event_seq;
  out->generation = gen;
  out->snapshot_seq = snap_seq;
  out->queue_depth = depth;
  out->queue_bytes = bytes;
  out->snapshot_fresh = fresh;
  out->fault = g_fault.load(std::memory_order_relaxed);
  return PGR_TRANSPORT_OK;
}

int api_stop(void) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_TRANSPORT_ERR_NOT_INSTALLED;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  g_stopped = true;
  g_snapshot.valid = false;  // getters go STALE immediately
  g_queue.clear();
  g_queue_bytes = 0;
  ++g_generation;
  return PGR_TRANSPORT_OK;
}

const PgrRuntimeTransportApi kApiTable = {
    PGR_TRANSPORT_API_VERSION, static_cast<uint32_t>(sizeof(PgrRuntimeTransportApi)),
    &api_install, &api_exchange, &api_emit,
    &api_get_bool, &api_get_int, &api_get_double, &api_get_string,
    &api_get_command_snapshot, &api_take_pulse, &api_sample, &api_stop,
};

}  // namespace

extern "C" const void *pgrazor_runtime_transport_api(uint32_t version) {
  if (version != PGR_TRANSPORT_API_VERSION) return nullptr;
  return &kApiTable;
}
