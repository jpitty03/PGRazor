#include "routes_discovery.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <time.h>
#include <unistd.h>

// Feature 44 native Routes discovery reader + rooted route cache. See routes_discovery.h for the ABI and
// the division of labour with routes.js and feature 43. Built -fno-exceptions / -fno-rtti like every
// sibling: a JS "throw" path becomes a fail-closed return; every temporary GC root is freed exactly once
// on the owner thread; cache roots are freed only on the owner thread (via the transactional swap in
// discover() or drain()). No mutex is held across a managed call. This module installs NO hook and NEVER
// calls back into JS.

static_assert(sizeof(PgrRoutesDiscoveryConfig) == 312, "config 312");
static_assert(sizeof(PgrRoutesDiscoveryRequest) == 48, "request 48");
static_assert(sizeof(PgrRoutesDiscoveryLookup) == 304, "lookup 304");
static_assert(sizeof(PgrRoutesDiscoveryRoute) == 832, "route 832");
static_assert(sizeof(PgrRoutesDiscoveryOutput) == 83216, "output 83216");
static_assert(sizeof(PgrRoutesDiscoveryPoints) == 80016, "points 80016");
static_assert(sizeof(PgrRoutesDiscoveryApi) == 64, "table 64");

namespace {

constexpr uint64_t kMaxLeaseMs = PGR_RD_MAX_LEASE_MS;

// --- il2cpp runtime ABI typedefs ---------------------------------------------------
using Invoke    = void *(*)(const void *method, void *obj, void **params, void **exc);
using ObjClass  = void *(*)(void *obj);
using Unbox     = void *(*)(void *boxed);
using GcNew     = uintptr_t (*)(void *obj, int pinned);
using GcGet     = void *(*)(uintptr_t handle);
using GcFree    = void (*)(uintptr_t handle);
using ThreadCur = void *(*)();
using FieldGet  = void (*)(void *obj, void *field, void *out);
using StrLen    = int32_t (*)(void *string);
using StrChars  = uint16_t *(*)(void *string);

// --- lifetime / publication --------------------------------------------------------
std::mutex g_install_mutex;
std::mutex g_state_mutex;
PgrRoutesDiscoveryConfig g_cfg{};
std::atomic<bool> g_installed{false};

// Permanent Unity owner, latched once from a genuine on-thread call.
std::atomic<uint64_t> g_owner_tid{0};

// Revocation generation (initially 1). Bumped only by revoke(); compared against request.generation and
// the published cache generation.
std::atomic<uint64_t> g_gen{1};

// Reentrancy busy flag (guarded by g_state_mutex); a scan INCLUDING root cleanup is in flight.
bool g_busy = false;

// --- the retained route cache (owner-thread only) ----------------------------------
// Every entry holds ONE owned GC root to its SharedRouteProto, decoded scalar fields, and a heap point
// array. The cache is bound to an owned root on the RoutesService object and the generation at publish.
struct CacheEntry {
  uintptr_t handle;        // owned GC root to the SharedRouteProto (borrowed out by lookup, never freed there)
  uint32_t flags;
  uint32_t id_length;
  uint32_t name_length;
  uint32_t point_count;
  double distance;
  double cooldown;
  double start_lat, start_lon, end_lat, end_lon;
  uint16_t id[PGR_RD_ID_MAX];
  uint16_t name[PGR_RD_NAME_MAX];
  PgrRoutesDiscoveryPoint *points;  // heap, point_count entries (nullptr when point_count == 0)
};
struct Cache {
  uintptr_t service_handle;  // owned GC root to the RoutesService the routes came from (0 when empty)
  uint64_t generation;       // generation value current at publication
  uint32_t count;
  CacheEntry entries[PGR_RD_MAX_ROUTES];
};
Cache g_cache{};  // ~85 KB BSS; only ever mutated on the owner thread under g_busy

// --- clock / tid -------------------------------------------------------------------
uint64_t current_tid() { return static_cast<uint64_t>(static_cast<pid_t>(gettid())); }

bool read_boottime_ms(uint64_t *out_ms) {
  struct timespec ts {};
  if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0 || ts.tv_sec < 0 || ts.tv_nsec < 0) return false;
  *out_ms = static_cast<uint64_t>(ts.tv_sec) * 1000ULL + static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
  return true;
}

ObjClass obj_class() { return reinterpret_cast<ObjClass>(g_cfg.object_get_class); }
bool is_exact(void *obj, void *klass) { return obj != nullptr && obj_class()(obj) == klass; }
bool attached() { return reinterpret_cast<ThreadCur>(g_cfg.thread_current)() != nullptr; }

// Owning temporary root. The destructor frees any live handle, so an early return always balances every
// root allocated on that path. No handle ever leaves this module through a Root.
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
struct BusyExit { ~BusyExit() { std::lock_guard<std::mutex> lock(g_state_mutex); g_busy = false; } };

// Lease + attached-thread + generation gate between managed steps and once more before publication /
// cache swap. Sets *status to a benign SKIP (expiry / detached / revoked) or a CLOCK fault.
bool within_window(uint64_t deadline, uint64_t generation, int *status) {
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) { *status = PGR_RD_ERR_CLOCK; return false; }
  if (now >= deadline) { *status = PGR_RD_SKIP; return false; }
  if (!attached()) { *status = PGR_RD_SKIP; return false; }
  if (g_gen.load(std::memory_order_acquire) != generation) { *status = PGR_RD_SKIP; return false; }
  return true;
}

// Raw field readers. field_get_value has no error return; the caller re-resolves the base object from its
// root immediately before the read (no allocation happens inside).
void *read_ref(void *obj, void *field) {
  void *out = nullptr;
  reinterpret_cast<FieldGet>(g_cfg.field_get_value)(obj, field, &out);
  return out;
}
int read_bool(void *obj, void *field) {
  uint8_t v = 0;
  reinterpret_cast<FieldGet>(g_cfg.field_get_value)(obj, field, &v);
  return v <= 1 ? v : -1;  // byte > 1 fails closed
}
int64_t read_i64(void *obj, void *field) {
  int64_t v = 0;
  reinterpret_cast<FieldGet>(g_cfg.field_get_value)(obj, field, &v);
  return v;
}
double read_f64(void *obj, void *field) {
  double v = 0;
  reinterpret_cast<FieldGet>(g_cfg.field_get_value)(obj, field, &v);
  return v;
}

// Invoke a 0/1-arg method on `recv` (re-resolved from its root) and root the (possibly null) return.
// Mirrors map_poi's call(): OK / THROW / GCHANDLE (alloc or unexpected null) / DEAD (receiver collected).
enum CallResult { CALL_OK, CALL_THROW, CALL_GCHANDLE, CALL_DEAD, CALL_EXPIRED };
CallResult call(void *method, const Root &recv, void **args, Root &out, bool allowNull, uint64_t deadline, uint64_t generation) {
  int guard=0; if (!within_window(deadline,generation,&guard)) return CALL_EXPIRED;
  void *self = recv.get();
  if (!self) return CALL_DEAD;
  void *exc = nullptr;
  void *value = reinterpret_cast<Invoke>(g_cfg.runtime_invoke)(method, self, args, &exc);
  if (exc) return CALL_THROW;
  if (!value) return allowNull ? CALL_OK : CALL_GCHANDLE;
  return out.set(value) ? CALL_OK : CALL_GCHANDLE;
}

// Invoke a boxed-Int32 collection count getter on `recv`. Returns the count in *out. Result: CALL_OK on a
// validated Int32 >= 0; CALL_THROW on a managed throw; CALL_GCHANDLE / CALL_DEAD on a root/receiver fault;
// a non-Int32 box or a negative value is reported CALL_GCHANDLE (treated as a fault by the caller).
CallResult invoke_count(void *method, const Root &recv, int32_t *out, uint64_t deadline, uint64_t generation) {
  Root box;
  CallResult rc = call(method, recv, nullptr, box, /*allowNull=*/false, deadline, generation);
  if (rc != CALL_OK) return rc;
  if (!is_exact(box.get(), g_cfg.class_int32)) return CALL_GCHANDLE;
  void *value = reinterpret_cast<Unbox>(g_cfg.object_unbox)(box.get());
  if (!value) return CALL_GCHANDLE;
  int32_t n = 0;
  std::memcpy(&n, value, sizeof(n));
  if (n < 0) return CALL_GCHANDLE;
  *out = n;
  return CALL_OK;
}

// Copy a UTF16 string object's chars (proven exact class_string by the caller) into `out` (cap units).
// Returns the copied length, or -1 on a bad length. For id the caller enforces 1..cap; for name it caps.
int copy_utf16(void *strObj, uint16_t *out, uint32_t cap, bool truncate) {
  int32_t len = reinterpret_cast<StrLen>(g_cfg.string_length)(strObj);
  if (len < 0) return -1;
  uint32_t n = static_cast<uint32_t>(len);
  if (n > cap) { if (!truncate) return -1; n = cap; }
  if (n == 0) return 0;
  uint16_t *chars = reinterpret_cast<StrChars>(g_cfg.string_chars)(strObj);
  if (!chars) return -1;
  std::memcpy(out, chars, static_cast<size_t>(n) * sizeof(uint16_t));
  return static_cast<int>(n);
}

// Read an anchor (RoutePoiAnchor.Anchor -> RouteWaypointProto lat/lon) with NO allocation between the
// three raw reads, so the child pointers stay stable. `protoObj` is the freshly re-resolved proto. Returns
// true and fills lat/lon only when the anchor + waypoint exist with the expected classes.
bool read_anchor(void *protoObj, void *poiField, double *lat, double *lon) {
  void *anchor = read_ref(protoObj, poiField);
  if (!is_exact(anchor, g_cfg.class_anchor)) return false;
  void *wp = read_ref(anchor, g_cfg.f_anchor_anchor);
  if (!is_exact(wp, g_cfg.class_waypoint)) return false;
  *lat = read_f64(wp, g_cfg.f_wp_lat);
  *lon = read_f64(wp, g_cfg.f_wp_lon);
  return std::isfinite(*lat) && std::isfinite(*lon) && std::fabs(*lat) <= 90 && std::fabs(*lon) <= 180;
}

void free_cache(Cache &c) {
  for (uint32_t i = 0; i < c.count; ++i) {
    if (c.entries[i].handle) reinterpret_cast<GcFree>(g_cfg.gchandle_free)(c.entries[i].handle);
    delete[] c.entries[i].points;
    c.entries[i].points = nullptr;
    c.entries[i].handle = 0;
  }
  if (c.service_handle) reinterpret_cast<GcFree>(g_cfg.gchandle_free)(c.service_handle);
  c.service_handle = 0;
  c.count = 0;
  c.generation = 0;
}

// --- owner latch -------------------------------------------------------------------
bool owner_ready(uint64_t tid) {
  if (tid == 0 || current_tid() != tid) return false;
  uint64_t owner = g_owner_tid.load(std::memory_order_acquire);
  if (!(owner == 0 || owner == tid)) return false;
  uint64_t expected = 0;
  g_owner_tid.compare_exchange_strong(expected, tid, std::memory_order_acq_rel);
  return g_owner_tid.load(std::memory_order_acquire) == tid;
}

// --- per-route decode --------------------------------------------------------------
// Decode one SharedRouteProto (raw, just returned by get_Item) into a staging CacheEntry. Returns 1 ADMIT
// (entry filled, entry.handle a persistent cache root; entry.points a heap array), 0 SKIP (no persistent
// root retained; the route is dropped like a JS continue / readShared return-null), or NEGATIVE ABORT the
// whole discover -- in which case *status carries the value to publish (SKIP 0 on an expiry / detached /
// revoked lease, a negative on a clock / root / managed fault). `staged` already contains the routes
// admitted so far (for id dedup). The caller publishes *status on any abort, never the return value.
int decode_route(void *protoRaw, uint64_t deadline, uint64_t generation, const Cache *staged,
                 CacheEntry *entry, int *status) {
  std::memset(entry, 0, sizeof(*entry));

  Root proto;
  if (!proto.set(protoRaw)) { *status = PGR_RD_ERR_GCHANDLE; return PGR_RD_ERR_GCHANDLE; }
  if (!is_exact(proto.get(), g_cfg.class_shared)) return 0;  // wrong class: skip (JS boundSnapshot drops)

  // Id: System.String, exact, UTF16 length 1..128. Root it, re-resolve, validate, copy.
  if (!within_window(deadline, generation, status)) return -1;
  void *idRaw = read_ref(proto.get(), g_cfg.f_shared_id);
  if (!idRaw) return 0;  // no id: skip (JS readShared returns null)
  {
    Root idStr;
    if (!idStr.set(idRaw)) { *status = PGR_RD_ERR_GCHANDLE; return PGR_RD_ERR_GCHANDLE; }
    if (!is_exact(idStr.get(), g_cfg.class_string)) return 0;
    int n = copy_utf16(idStr.get(), entry->id, PGR_RD_ID_MAX, /*truncate=*/false);
    if (n < 1) return 0;  // empty id or length > 128: skip (long ids unreachable by START)
    entry->id_length = static_cast<uint32_t>(n);
  }

  // Dedup against the already-admitted ids (first wins, like the JS `seen` set).
  for (uint32_t i = 0; i < staged->count; ++i) {
    if (staged->entries[i].id_length == entry->id_length &&
        std::memcmp(staged->entries[i].id, entry->id,
                    static_cast<size_t>(entry->id_length) * sizeof(uint16_t)) == 0)
      return 0;  // duplicate id: skip
  }

  // Name (optional, truncated to 256). A missing / non-string name is length 0.
  if (!within_window(deadline, generation, status)) return -1;
  void *nameRaw = read_ref(proto.get(), g_cfg.f_shared_name);
  if (nameRaw) {
    Root nameStr;
    if (!nameStr.set(nameRaw)) { *status = PGR_RD_ERR_GCHANDLE; return PGR_RD_ERR_GCHANDLE; }
    if (is_exact(nameStr.get(), g_cfg.class_string)) {
      int n = copy_utf16(nameStr.get(), entry->name, PGR_RD_NAME_MAX, /*truncate=*/true);
      if (n > 0) entry->name_length = static_cast<uint32_t>(n);
    }
  }

  // Reversible bool (byte 0/1; a byte > 1 fails closed -> skip).
  if (!within_window(deadline, generation, status)) return -1;
  {
    int rev = read_bool(proto.get(), g_cfg.f_shared_reversible);
    if (rev < 0) return 0;
    if (rev == 1) entry->flags |= PGR_RD_FLAG_REVERSIBLE;
  }

  // Distance (int64 -> double).
  entry->distance = static_cast<double>(read_i64(proto.get(), g_cfg.f_shared_distance));

  // Cooldown: PlayerStats present (exact) -> CooldownFinishMs; else 0.
  {
    void *stats = read_ref(proto.get(), g_cfg.f_shared_stats);
    if (is_exact(stats, g_cfg.class_playerStats))
      entry->cooldown = static_cast<double>(read_i64(stats, g_cfg.f_stats_cooldown));
  }

  // Start / End anchors: omit absent, mistyped or non-finite/out-of-range coordinates.
  {
    double lat = 0, lon = 0;
    if (read_anchor(proto.get(), g_cfg.f_shared_startPoi, &lat, &lon)) {
      entry->start_lat = lat; entry->start_lon = lon; entry->flags |= PGR_RD_FLAG_HAS_START;
    }
    if (read_anchor(proto.get(), g_cfg.f_shared_endPoi, &lat, &lon)) {
      entry->end_lat = lat; entry->end_lon = lon; entry->flags |= PGR_RD_FLAG_HAS_END;
    }
  }

  // Waypoints: a valid cached path requires 2..5000 finite in-range points, or the
  // route is skipped; a managed throw while reading this route's waypoints skips the route only.
  if (!within_window(deadline, generation, status)) return -1;
  void *wpsRaw = read_ref(proto.get(), g_cfg.f_shared_waypoints);
  if (wpsRaw) {
    Root wps;
    if (!wps.set(wpsRaw)) { *status = PGR_RD_ERR_GCHANDLE; return PGR_RD_ERR_GCHANDLE; }
    if (!is_exact(wps.get(),g_cfg.class_waypoints)) return 0;
    int32_t count = 0;
    switch (invoke_count(g_cfg.m_wpCount, wps, &count, deadline, generation)) {
      case CALL_OK:       break;
      case CALL_EXPIRED:  *status=PGR_RD_SKIP; return 0;
      case CALL_THROW:    return 0;  // JS try/catch around the waypoint loop: drop this route only
      case CALL_GCHANDLE: *status = PGR_RD_ERR_GCHANDLE; return PGR_RD_ERR_GCHANDLE;
      case CALL_DEAD:     *status = PGR_RD_ERR_GCHANDLE; return PGR_RD_ERR_GCHANDLE;
    }
    if (count < 2 || count > PGR_RD_MAX_POINTS) return 0;  // JS readShared returns null

    PgrRoutesDiscoveryPoint *pts = new (std::nothrow) PgrRoutesDiscoveryPoint[count];
    if (!pts) { *status = PGR_RD_ERR_ALLOCATION; return PGR_RD_ERR_ALLOCATION; }
    uint32_t stored = 0;
    for (int32_t i = 0; i < count; ++i) {
      if (!within_window(deadline, generation, status)) { delete[] pts; return -1; }
      Root wp;
      int32_t idx = i;
      void *args[1] = {&idx};
      switch (call(g_cfg.m_wpItem, wps, args, wp, /*allowNull=*/true, deadline, generation)) {
        case CALL_OK:       break;
        case CALL_EXPIRED:  delete[] pts; *status=PGR_RD_SKIP; return 0;
        case CALL_THROW:    delete[] pts; return 0;  // route-level throw: drop this route only
        case CALL_GCHANDLE: delete[] pts; *status = PGR_RD_ERR_GCHANDLE; return PGR_RD_ERR_GCHANDLE;
        case CALL_DEAD:     delete[] pts; *status = PGR_RD_ERR_GCHANDLE; return PGR_RD_ERR_GCHANDLE;
      }
      void *w = wp.get();
      if (!w) continue;                               // null element: skip point (JS continue)
      if (!is_exact(w, g_cfg.class_waypoint)) continue;
      pts[stored].lat = read_f64(w, g_cfg.f_wp_lat);
      pts[stored].lon = read_f64(w, g_cfg.f_wp_lon);
      if (!std::isfinite(pts[stored].lat) || !std::isfinite(pts[stored].lon) || std::fabs(pts[stored].lat)>90 || std::fabs(pts[stored].lon)>180) { delete[] pts; return 0; }
      ++stored;
    }
    if (stored < 2) { delete[] pts; return 0; }
    else { entry->points = pts; entry->point_count = stored; }
  }

  if (entry->point_count < 2) { delete[] entry->points; entry->points=nullptr; return 0; }

  // Admit: take a PERSISTENT cache root on the (re-resolved) proto. This outlives decode and the swap.
  if (!within_window(deadline, generation, status)) { delete[] entry->points; entry->points = nullptr; return -1; }
  void *live = proto.get();
  if (!live) { delete[] entry->points; entry->points = nullptr; *status = PGR_RD_ERR_GCHANDLE; return PGR_RD_ERR_GCHANDLE; }
  uintptr_t handle = reinterpret_cast<GcNew>(g_cfg.gchandle_new)(live, 0);
  if (!handle) { delete[] entry->points; entry->points = nullptr; *status = PGR_RD_ERR_GCHANDLE; return PGR_RD_ERR_GCHANDLE; }
  entry->handle = handle;
  return 1;
}

void init_output(PgrRoutesDiscoveryOutput *out) {
  std::memset(out, 0, sizeof(*out));
  out->version = PGR_RD_OUTPUT_VERSION;
  out->size = sizeof(PgrRoutesDiscoveryOutput);
}
void init_points(PgrRoutesDiscoveryPoints *out) {
  std::memset(out, 0, sizeof(*out));
  out->version = PGR_RD_POINTS_VERSION;
  out->size = sizeof(PgrRoutesDiscoveryPoints);
}

// --- config validation / install ---------------------------------------------------
bool validate_config(const PgrRoutesDiscoveryConfig *c, int *rc) {
  if (c == nullptr) { *rc = PGR_RD_ERR_NULL_ARG; return false; }
  if (c->version != PGR_RD_CONFIG_VERSION) { *rc = PGR_RD_ERR_BAD_VERSION; return false; }
  if (c->size != sizeof(PgrRoutesDiscoveryConfig)) { *rc = PGR_RD_ERR_BAD_SIZE; return false; }
  void *const *ptrs = reinterpret_cast<void *const *>(
      reinterpret_cast<const unsigned char *>(c) + offsetof(PgrRoutesDiscoveryConfig, runtime_invoke));
  for (int i = 0; i < PGR_RD_PTR_COUNT; ++i)
    if (ptrs[i] == nullptr) { *rc = PGR_RD_ERR_MISSING_PTR; return false; }
  *rc = 0;
  return true;
}

int api_install(const PgrRoutesDiscoveryConfig *config) {
  int rc = 0;
  if (!validate_config(config, &rc)) return rc;
  uint64_t probe = 0;
  if (!read_boottime_ms(&probe)) return PGR_RD_ERR_CLOCK;

  std::lock_guard<std::mutex> install_lock(g_install_mutex);
  if (g_installed.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    return std::memcmp(&g_cfg, config, sizeof(*config)) == 0 ? 0 : PGR_RD_ERR_RECONFIG;
  }
  {
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    g_cfg = *config;
    g_busy = false;
  }
  g_installed.store(true, std::memory_order_release);
  return 0;
}

uint64_t api_generation() { return g_gen.load(std::memory_order_acquire); }
uint64_t api_revoke() { return g_gen.fetch_add(1, std::memory_order_acq_rel) + 1; }

// Shared gate for the owner-thread entry points. Returns a benign SKIP / fault status in *rc on failure;
// on success computes the clamped deadline (lease entry points) and the request generation.
bool enter_owner(uint64_t tid, uint64_t residual_ms, uint64_t generation, bool need_lease,
                 uint64_t *deadline, int *rc) {
  if (!g_installed.load(std::memory_order_acquire)) { *rc = PGR_RD_ERR_NOT_INSTALLED; return false; }
  if (!attached() || !owner_ready(tid)) { *rc = PGR_RD_SKIP; return false; }
  if (need_lease) {
    const uint64_t residual = residual_ms > kMaxLeaseMs ? kMaxLeaseMs : residual_ms;
    if (residual == 0) { *rc = PGR_RD_SKIP; return false; }
    if (generation != g_gen.load(std::memory_order_acquire)) { *rc = PGR_RD_SKIP; return false; }
    uint64_t now = 0;
    if (!read_boottime_ms(&now)) { *rc = PGR_RD_ERR_CLOCK; return false; }
    *deadline = now + residual;
  }
  return true;
}

int api_discover(const PgrRoutesDiscoveryRequest *req, PgrRoutesDiscoveryOutput *out) {
  if (!out) return PGR_RD_ERR_NULL_ARG;
  init_output(out);
  if (!req) { out->status = PGR_RD_ERR_NULL_ARG; return out->status; }
  if (req->version != PGR_RD_REQUEST_VERSION) { out->status = PGR_RD_ERR_BAD_VERSION; return out->status; }
  if (req->size != sizeof(*req)) { out->status = PGR_RD_ERR_BAD_SIZE; return out->status; }
  const PgrRoutesDiscoveryRequest r = *req;

  uint64_t deadline = 0;
  int rc = 0;
  if (!enter_owner(r.tid, r.residual_ms, r.generation, /*need_lease=*/true, &deadline, &rc)) {
    out->status = rc; return rc;
  }
  const uint64_t generation = r.generation;

  { std::lock_guard<std::mutex> lock(g_state_mutex); if (g_busy) { out->status = PGR_RD_ERR_BUSY; return out->status; } g_busy = true; }
  BusyExit busy_exit;

  Cache *staging = new (std::nothrow) Cache();
  if (!staging) { out->status = PGR_RD_ERR_ALLOCATION; return out->status; }
  staging->generation = generation;

  // All managed work (and its temporary roots) lives in this block; every temporary Root is destroyed
  // before the final gate. The persistent cache roots live in `staging` and are freed on any abort.
  int status = PGR_RD_OK;
  {
    if (!within_window(deadline, generation, &status)) goto done;

    // Resolve + root the borrowed routes service; bind the staging cache to it with an OWNED root.
    void *svcRaw = reinterpret_cast<GcGet>(g_cfg.gchandle_get_target)(static_cast<uintptr_t>(r.service_handle));
    if (!svcRaw) { status = PGR_RD_SKIP; goto done; }
    Root service;
    if (!service.set(svcRaw)) { status = PGR_RD_ERR_GCHANDLE; goto done; }
    if (!is_exact(service.get(), g_cfg.class_routes)) { status = PGR_RD_SKIP; goto done; }
    {
      void *live = service.get();
      uintptr_t sh = reinterpret_cast<GcNew>(g_cfg.gchandle_new)(live, 0);
      if (!sh) { status = PGR_RD_ERR_GCHANDLE; goto done; }
      staging->service_handle = sh;
    }

    // didn -> NearbyRoutesInfo; a null / wrong class is a valid EMPTY snapshot (clears the cache).
    if (!within_window(deadline, generation, &status)) goto done;
    void *nearby = read_ref(service.get(), g_cfg.f_routes_didn);
    if (!is_exact(nearby, g_cfg.class_nearby)) goto publish;  // empty snapshot

    void *listRaw = read_ref(nearby, g_cfg.f_nearby_routes);
    if (!listRaw) goto publish;  // empty snapshot
    Root list;
    if (!list.set(listRaw)) { status = PGR_RD_ERR_GCHANDLE; goto done; }

    if (!within_window(deadline, generation, &status)) goto done;
    if (!is_exact(list.get(),g_cfg.class_list)) { status=PGR_RD_ERR_METADATA; goto done; }
    int32_t total = 0;
    switch (invoke_count(g_cfg.m_listCount, list, &total, deadline, generation)) {
      case CALL_OK:       break;
      case CALL_EXPIRED:  status=PGR_RD_SKIP; goto done;
      case CALL_THROW:    status = PGR_RD_ERR_MANAGED; goto done;   // list-level throw is a fault
      case CALL_GCHANDLE: status = PGR_RD_ERR_GCHANDLE; goto done;
      case CALL_DEAD:     status = PGR_RD_ERR_GCHANDLE; goto done;
    }
    if (total > PGR_RD_MAX_ROUTES) total = PGR_RD_MAX_ROUTES;  // clamp, matching JS readDiscovery

    for (int32_t i = 0; i < total && staging->count < PGR_RD_MAX_ROUTES; ++i) {
      if (!within_window(deadline, generation, &status)) goto done;
      Root item;
      int32_t idx = i;
      void *args[1] = {&idx};
      switch (call(g_cfg.m_listItem, list, args, item, /*allowNull=*/true, deadline, generation)) {
        case CALL_OK:       break;
        case CALL_EXPIRED:  status=PGR_RD_SKIP; goto done;
        case CALL_THROW:    status = PGR_RD_ERR_MANAGED; goto done;
        case CALL_GCHANDLE: status = PGR_RD_ERR_GCHANDLE; goto done;
        case CALL_DEAD:     status = PGR_RD_ERR_GCHANDLE; goto done;
      }
      void *protoRaw = item.get();
      if (!protoRaw) continue;  // null element: skip (JS continue)

      CacheEntry entry;
      int dr = decode_route(protoRaw, deadline, generation, staging, &entry, &status);
      if (status != PGR_RD_OK || dr < 0) goto done;          // fault: abort (status already set)
      if (dr == 0) continue;          // skip this route
      staging->entries[staging->count++] = entry;  // ADMIT (shallow copy: handle + points transferred)
    }
  }

publish:
  // Fresh pre-publication gate, AFTER every temporary root is released (the block above has closed).
  if (!within_window(deadline, generation, &status)) goto done;
  // Transactional swap: owner-thread only and serialized by g_busy, so no mutex is needed (and none is
  // held across gchandle_free). An off-thread revoke only bumps g_gen; it never touches cache memory.
  free_cache(g_cache);            // drop the previous cache roots on the owner thread
  if (!within_window(deadline, generation, &status)) goto done;
  g_cache = *staging;             // transfer entries (handles + point arrays) + service root + generation
  delete staging;                 // free the staging container ONLY; the point arrays moved into g_cache
  staging = nullptr;

  init_output(out);
  out->count = g_cache.count;
  for (uint32_t i = 0; i < g_cache.count; ++i) {
    const CacheEntry &e = g_cache.entries[i];
    PgrRoutesDiscoveryRoute &h = out->routes[i];
    h.flags = e.flags;
    h.id_length = e.id_length;
    h.name_length = e.name_length;
    h.point_count = e.point_count;
    h.distance_meters = e.distance;
    h.cooldown_finish_ms = e.cooldown;
    h.start_lat = e.start_lat; h.start_lon = e.start_lon;
    h.end_lat = e.end_lat; h.end_lon = e.end_lon;
    std::memcpy(h.id, e.id, static_cast<size_t>(e.id_length) * sizeof(uint16_t));
    std::memcpy(h.name, e.name, static_cast<size_t>(e.name_length) * sizeof(uint16_t));
  }
  out->status = PGR_RD_OK;
  return PGR_RD_OK;

done:
  // Fault / expiry: discard the staging cache (free its roots + point arrays on the owner thread) and keep
  // the previous cache unchanged. No partial publish.
  if (staging) { free_cache(*staging); delete staging; }
  init_output(out);
  out->status = status;
  return status;
}

int api_points(const PgrRoutesDiscoveryRequest *req, uint32_t index, PgrRoutesDiscoveryPoints *out) {
  if (!out) return PGR_RD_ERR_NULL_ARG;
  init_points(out);
  if (!req) { out->status = PGR_RD_ERR_NULL_ARG; return out->status; }
  if (req->version != PGR_RD_REQUEST_VERSION) { out->status = PGR_RD_ERR_BAD_VERSION; return out->status; }
  if (req->size != sizeof(*req)) { out->status = PGR_RD_ERR_BAD_SIZE; return out->status; }
  const PgrRoutesDiscoveryRequest r = *req;
  const uint32_t idx = index ? index : r.index;  // header: index arg takes precedence, else request.index

  uint64_t deadline = 0;
  int rc = 0;
  if (!enter_owner(r.tid, r.residual_ms, r.generation, /*need_lease=*/true, &deadline, &rc)) {
    out->status = rc; return rc;
  }
  { std::lock_guard<std::mutex> lock(g_state_mutex); if (g_busy) { out->status = PGR_RD_ERR_BUSY; return out->status; } g_busy = true; }
  BusyExit busy_exit;

  // Generation must still match the published cache (an off-thread revoke invalidates it).
  if (r.generation != g_gen.load(std::memory_order_acquire) || g_cache.generation != g_gen.load(std::memory_order_acquire)) {
    out->status = PGR_RD_SKIP; return out->status;
  }
  if (idx >= g_cache.count) { out->status = PGR_RD_ERR_BAD_INDEX; return out->status; }
  const CacheEntry &e = g_cache.entries[idx];
  out->count = e.point_count;
  if (e.point_count) std::memcpy(out->points, e.points, static_cast<size_t>(e.point_count) * sizeof(PgrRoutesDiscoveryPoint));
  if (!within_window(deadline, r.generation, &rc)) { init_points(out); out->status = rc; return rc; }
  out->status = PGR_RD_OK;
  return PGR_RD_OK;
}

int api_lookup(const PgrRoutesDiscoveryLookup *lk, uint64_t *out_handle) {
  if (out_handle) *out_handle = 0;
  if (!out_handle) return PGR_RD_ERR_NULL_ARG;
  if (!lk) return PGR_RD_ERR_NULL_ARG;
  if (lk->version != PGR_RD_LOOKUP_VERSION) return PGR_RD_ERR_BAD_VERSION;
  if (lk->size != sizeof(*lk)) return PGR_RD_ERR_BAD_SIZE;
  if (lk->id_length < 1 || lk->id_length > PGR_RD_ID_MAX || lk->reserved != 0) return PGR_RD_SKIP;
  const PgrRoutesDiscoveryLookup l = *lk;

  uint64_t deadline = 0;
  int rc = 0;
  if (!enter_owner(l.tid, l.residual_ms, l.generation, /*need_lease=*/true, &deadline, &rc)) return rc;

  { std::lock_guard<std::mutex> lock(g_state_mutex); if (g_busy) return PGR_RD_ERR_BUSY; g_busy = true; }
  BusyExit busy_exit;

  // Cache must be published under the current generation (never revived across a revoke) and bound to the
  // same RoutesService object the caller is acting through.
  const uint64_t gen = g_gen.load(std::memory_order_acquire);
  if (l.generation != gen || g_cache.generation != gen || g_cache.count == 0 || !g_cache.service_handle)
    return PGR_RD_SKIP;
  void *bound = reinterpret_cast<GcGet>(g_cfg.gchandle_get_target)(g_cache.service_handle);
  void *reqSvc = reinterpret_cast<GcGet>(g_cfg.gchandle_get_target)(static_cast<uintptr_t>(l.service_handle));
  if (!bound || !reqSvc || bound != reqSvc) return PGR_RD_SKIP;

  for (uint32_t i = 0; i < g_cache.count; ++i) {
    const CacheEntry &e = g_cache.entries[i];
    if (e.id_length != l.id_length) continue;
    if (std::memcmp(e.id, l.id, static_cast<size_t>(l.id_length) * sizeof(uint16_t)) != 0) continue;
    if (!e.handle) return PGR_RD_SKIP;
    if (!reinterpret_cast<GcGet>(g_cfg.gchandle_get_target)(e.handle)) return PGR_RD_SKIP;  // collected
    if (!within_window(deadline, l.generation, &rc)) return rc;
    *out_handle = static_cast<uint64_t>(e.handle);  // BORROWED: the caller must not free it
    return PGR_RD_OK;
  }
  return PGR_RD_SKIP;  // id not found
}

int api_drain(const PgrRoutesDiscoveryRequest *req) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_RD_ERR_NOT_INSTALLED;
  if (!req) return PGR_RD_ERR_NULL_ARG;
  if (req->version != PGR_RD_REQUEST_VERSION) return PGR_RD_ERR_BAD_VERSION;
  if (req->size != sizeof(*req)) return PGR_RD_ERR_BAD_SIZE;
  // Owner + attached, but NO lease: cleanup must work after the lease has expired. An off-owner call is a
  // benign SKIP that frees nothing (a foreign thread must never free managed roots).
  if (!attached() || !owner_ready(req->tid)) return PGR_RD_SKIP;

  { std::lock_guard<std::mutex> lock(g_state_mutex); if (g_busy) return PGR_RD_ERR_BUSY; g_busy = true; }
  BusyExit busy_exit;
  free_cache(g_cache);  // owner-thread only, serialized by g_busy; no mutex held across gchandle_free
  return PGR_RD_OK;
}

const PgrRoutesDiscoveryApi kApiTable = {
    PGR_RD_API_VERSION, static_cast<uint32_t>(sizeof(PgrRoutesDiscoveryApi)),
    &api_install, &api_generation, &api_revoke, &api_discover, &api_points, &api_lookup, &api_drain,
};

}  // namespace

extern "C" const void *pgrazor_routes_discovery_api(uint32_t version) {
  if (version != PGR_RD_API_VERSION) return nullptr;
  return &kApiTable;
}
