#include "map_poi.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <memory>
#include <new>
#include <time.h>
#include <unistd.h>

// Feature 42: synchronous, read-only map POI snapshot. No hooks, no callbacks, no retained managed state.
// Every temporary managed value is rooted on the owner thread and freed before the output is published; the
// output header/count are stamped only AFTER all roots are released, with the busy guard still held, and a
// partial snapshot is never published. A private heap snapshot protects provisional rows from reentrant
// callers; there is no large stack object or per-point allocation. Built -fno-exceptions / -fno-rtti:
// managed exceptions surface through runtime_invoke's exc out-param, never a C++ throw. No mutex is held
// across a managed call.

static_assert(sizeof(PgrMapPoiConfig) == 328, "config 328");
static_assert(sizeof(PgrMapPoiRequest) == 32, "request 32");
static_assert(sizeof(PgrMapPoiPoint) == 576, "point 576");
static_assert(sizeof(PgrMapPoiOutput) == 294928, "output 294928");
static_assert(sizeof(PgrMapPoiApi) == 24, "table 24");
static_assert(offsetof(PgrMapPoiConfig, runtime_invoke) == 8, "config body start");
static_assert(offsetof(PgrMapPoiConfig, class_stop) == 120, "config classes");   // 8 + 14*8
static_assert(offsetof(PgrMapPoiConfig, f_fortId) == 192, "config mandatory fields"); // 8 + 23*8
static_assert(offsetof(PgrMapPoiConfig, m_team) == 256, "config optional start");     // 8 + 31*8
static_assert(offsetof(PgrMapPoiConfig, f_bossId) == 320, "config optional end");     // 8 + 39*8
static_assert(offsetof(PgrMapPoiRequest, tid) == 8, "request tid");
static_assert(offsetof(PgrMapPoiRequest, remaining_ms) == 16, "request remaining");
static_assert(offsetof(PgrMapPoiRequest, wall_ms) == 24, "request wall");
static_assert(offsetof(PgrMapPoiPoint, lat) == 16, "point lat");
static_assert(offsetof(PgrMapPoiPoint, raid_end) == 32, "point raid_end");
static_assert(offsetof(PgrMapPoiPoint, team_id) == 48, "point team_id");
static_assert(offsetof(PgrMapPoiPoint, id) == 64, "point id");
static_assert(offsetof(PgrMapPoiOutput, status) == 8, "output status");
static_assert(offsetof(PgrMapPoiOutput, count) == 12, "output count");
static_assert(offsetof(PgrMapPoiOutput, points) == 16, "output points");

namespace {

constexpr uint64_t kMaxLeaseMs = PGR_MP_MAX_LEASE_MS;
constexpr int kMandatoryPtrCount = 31;        // slots 0..30, all non-null
constexpr int kTotalPtrCount = 40;            // slots 0..39
constexpr double kMaxSafeInteger = 9007199254740991.0;  // 2^53 - 1 (JS Number.MAX_SAFE_INTEGER)

using New        = uintptr_t (*)(void *, int);
using Get        = void *(*)(uintptr_t);
using FreeH      = void (*)(uintptr_t);
using ObjClass   = void *(*)(void *);
using Invoke     = void *(*)(const void *, void *, void **, void **);
using Unbox      = void *(*)(void *);
using FieldGet   = void (*)(void *, void *, void *);
using ClassType  = void *(*)(void *);
using TypeObject = void *(*)(void *);
using ArrayLen   = uintptr_t (*)(void *);
using VirtMethod = void *(*)(void *, void *);
using StrLen     = int32_t (*)(void *);
using StrChars   = uint16_t *(*)(void *);
using ThreadCur  = void *(*)();

std::mutex g_install_mutex;
std::mutex g_state_mutex;
PgrMapPoiConfig g_cfg{};
std::atomic<bool> g_installed{false};

// Which optional groups the installed config enables (computed once at install).
bool g_has_team = false;
bool g_has_slots = false;
bool g_has_level = false;
bool g_has_boss = false;

// Permanent Unity owner, latched once from a genuine on-thread call.
std::atomic<uint64_t> g_owner_tid{0};
bool g_busy = false;  // a scan, INCLUDING root cleanup, is in flight (guarded by g_state_mutex)

uint64_t current_tid() { return static_cast<uint64_t>(static_cast<pid_t>(gettid())); }

bool read_boottime_ms(uint64_t *out_ms) {
  struct timespec ts {};
  if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0 || ts.tv_sec < 0 || ts.tv_nsec < 0) return false;
  *out_ms = static_cast<uint64_t>(ts.tv_sec) * 1000ULL + static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
  return true;
}

ObjClass obj_class() { return reinterpret_cast<ObjClass>(g_cfg.object_get_class); }
bool is_exact(void *obj, void *klass) { return obj != nullptr && obj_class()(obj) == klass; }

// Owning temporary root. set() roots a raw managed object; the destructor frees any live handle so an early
// return always balances every root allocated on that path. No handle ever leaves this module.
struct Root {
  uintptr_t handle = 0;
  Root() = default;
  Root(const Root &) = delete;
  Root &operator=(const Root &) = delete;
  ~Root() { if (handle) reinterpret_cast<FreeH>(g_cfg.gchandle_free)(handle); }
  void *get() const { return handle ? reinterpret_cast<Get>(g_cfg.gchandle_get_target)(handle) : nullptr; }
  void reset() { if (handle) { reinterpret_cast<FreeH>(g_cfg.gchandle_free)(handle); handle = 0; } }
  bool set(void *object) {
    reset();
    if (!object) return false;
    handle = reinterpret_cast<New>(g_cfg.gchandle_new)(object, 0);
    return handle && get();
  }
};

// Busy reset runs AFTER the Roots declared below it are destroyed, so reentrant root cleanup still observes
// busy == true (the guard spans destruction/root cleanup).
struct BusyExit { ~BusyExit() { std::lock_guard<std::mutex> lock(g_state_mutex); g_busy = false; } };

// Invoke a method and root its (possibly null) return value. receiver null => static call; otherwise the
// receiver is re-resolved from its root (relocating collector). Returns one of CALL_*.
enum CallResult { CALL_OK, CALL_THROW, CALL_GCHANDLE, CALL_DEAD };

CallResult call(void *method, const Root *receiver, void **args, Root &out, bool allowNull) {
  void *self = nullptr;
  if (receiver) {
    self = receiver->get();
    if (!self) return CALL_DEAD;
  }
  void *exc = nullptr;
  void *value = reinterpret_cast<Invoke>(g_cfg.runtime_invoke)(method, self, args, &exc);
  if (exc) return CALL_THROW;
  if (!value) return allowNull ? CALL_OK : CALL_GCHANDLE;
  return out.set(value) ? CALL_OK : CALL_GCHANDLE;
}

// Field reads. field_get_value has no error return; the object is re-resolved from its root immediately
// before the read (no allocation happens inside), so a relocation between records never leaves a stale base.
bool field_ptr(const Root &r, void *field, void **out) {
  void *o = r.get();
  if (!o) return false;
  reinterpret_cast<FieldGet>(g_cfg.field_get_value)(o, field, out);
  return true;
}
bool field_f64(const Root &r, void *field, double *out) {
  void *o = r.get();
  if (!o) return false;
  reinterpret_cast<FieldGet>(g_cfg.field_get_value)(o, field, out);
  return true;
}
bool field_u64(const Root &r, void *field, uint64_t *out) {
  void *o = r.get();
  if (!o) return false;
  reinterpret_cast<FieldGet>(g_cfg.field_get_value)(o, field, out);
  return true;
}
bool field_i32(const Root &r, void *field, int32_t *out) {
  void *o = r.get();
  if (!o) return false;
  reinterpret_cast<FieldGet>(g_cfg.field_get_value)(o, field, out);
  return true;
}
bool field_u8(const Root &r, void *field, uint8_t *out) {
  void *o = r.get();
  if (!o) return false;
  reinterpret_cast<FieldGet>(g_cfg.field_get_value)(o, field, out);
  return true;
}

// Unbox a boxed value rooted in `box`, re-resolving its target, and copy `n` bytes of the value. false on a
// null/unboxable value. Caller validates the box class first.
bool unbox_copy(const Root &box, void *out, size_t n) {
  void *obj = box.get();
  if (!obj) return false;
  void *value = reinterpret_cast<Unbox>(g_cfg.object_unbox)(obj);
  if (!value) return false;
  std::memcpy(out, value, n);
  return true;
}

// Build a managed System.Type for `klass` and root it. type_get_object's result must be rooted before any
// further managed call.
bool type_object(void *klass, Root &out, int *err) {
  void *type = reinterpret_cast<ClassType>(g_cfg.class_get_type)(klass);
  if (!type) { *err = PGR_MP_ERR_METADATA; return false; }
  void *obj = reinterpret_cast<TypeObject>(g_cfg.type_get_object)(type);
  if (!obj) { *err = PGR_MP_ERR_METADATA; return false; }
  if (!out.set(obj)) { *err = PGR_MP_ERR_GCHANDLE; return false; }
  return true;
}

// Lease + attached-thread gate between managed steps and once more before publication. Sets *status to a
// benign SKIP (expired / detached) or a CLOCK fault and returns false; true keeps the window open.
bool within_window(uint64_t deadline, int *status) {
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) { *status = PGR_MP_ERR_CLOCK; return false; }
  if (now >= deadline) { *status = PGR_MP_SKIP; return false; }
  if (!reinterpret_cast<ThreadCur>(g_cfg.thread_current)()) { *status = PGR_MP_SKIP; return false; }
  return true;
}

bool is_safe_ms(uint64_t v) { return static_cast<double>(v) <= kMaxSafeInteger; }

// A caught UTF16 id staged before commit, so dedup can compare against the already-emitted points.
struct Id {
  uint16_t chars[PGR_MP_MAX_ID];
  int32_t length = 0;
};

// True if `id` already appears among the first `count` emitted points (dedup across stop/gym, like legacy).
bool already_seen(const PgrMapPoiOutput *out, uint32_t count, const Id &id) {
  for (uint32_t i = 0; i < count; ++i) {
    const PgrMapPoiPoint &p = out->points[i];
    if (static_cast<int32_t>(p.id_length) != id.length) continue;
    if (std::memcmp(p.id, id.chars, static_cast<size_t>(id.length) * sizeof(uint16_t)) == 0) return true;
  }
  return false;
}

// Optional gym fields. Each read is isolated: a throw / bad class / out-of-range value only omits the field.
// `raid` is already rooted and proven; `obj` is the rooted candidate (team is invoked on it).
int read_optional_gym(const Root &obj, const Root &fort, PgrMapPoiPoint *pt, uint64_t wall, uint64_t deadline) {
  int err = PGR_MP_OK;
  if (!within_window(deadline, &err)) return err;
  // team: non-virtual instance invoke on the candidate -> boxed enum (exact class_teamBox, value size 4).
  if (g_has_team) {
    Root box;
    if (call(g_cfg.m_team, &obj, nullptr, box, /*allowNull=*/true) == CALL_OK &&
        is_exact(box.get(), g_cfg.class_teamBox)) {
      int32_t team = 0;
      if (unbox_copy(box, &team, sizeof(team))) { pt->team_id = team; pt->flags |= PGR_MP_FLAG_TEAM; }
    }
  }

  // slots: fort -> gym display (exact) -> int32 slots (0..6).
  if (!within_window(deadline, &err)) return err;
  if (g_has_slots) {
    void *gdRaw = nullptr;
    Root gd;
    if (field_ptr(fort, g_cfg.f_gymDisplay, &gdRaw) && gdRaw && gd.set(gdRaw) &&
        is_exact(gd.get(), g_cfg.class_gymDisplay)) {
      if (!within_window(deadline, &err)) return err;
      int32_t slots = 0;
      if (field_i32(gd, g_cfg.f_slots, &slots) && slots >= 0 && slots <= 6) {
        pt->slots = slots; pt->flags |= PGR_MP_FLAG_SLOTS;
      }
    }
  }

  // active raid: fort -> raid info (exact). Active == !hidden && spawn<=wall && end>wall, all timestamps safe.
  if (!within_window(deadline, &err)) return err;
  void *raidRaw = nullptr;
  if (!field_ptr(fort, g_cfg.f_raidInfo, &raidRaw) || !raidRaw) return PGR_MP_OK;
  Root raid;
  if (!raid.set(raidRaw) || !is_exact(raid.get(), g_cfg.class_raid)) return PGR_MP_OK;
  if (!within_window(deadline, &err)) return err;

  uint8_t hidden = 0;
  uint64_t spawn = 0, end = 0, battle = 0;
  if (!field_u8(raid, g_cfg.f_hidden, &hidden)) return PGR_MP_OK;
  if (!field_u64(raid, g_cfg.f_spawn, &spawn)) return PGR_MP_OK;
  if (!field_u64(raid, g_cfg.f_end, &end)) return PGR_MP_OK;
  if (!field_u64(raid, g_cfg.f_battle, &battle)) return PGR_MP_OK;
  if (hidden) return PGR_MP_OK;
  if (!is_safe_ms(spawn) || !is_safe_ms(end) || !is_safe_ms(battle)) return PGR_MP_OK;
  if (!(spawn <= wall && end > wall)) return PGR_MP_OK;

  const bool egg = battle > wall;
  pt->raid_end = static_cast<double>(end);
  pt->raid_start = static_cast<double>(battle);
  if (egg) pt->flags |= PGR_MP_FLAG_EGG;

  // level: raw signed enum, independent optional (mapped to a name in JS).
  if (g_has_level) {
    int32_t level = 0;
    if (field_i32(raid, g_cfg.f_level, &level)) { pt->level = level; pt->flags |= PGR_MP_FLAG_LEVEL; }
  }

  // boss: only once hatched. raid -> boss pokemon (exact) -> int32 species id (1..99999).
  if (!within_window(deadline, &err)) return err;
  if (!egg && g_has_boss) {
    void *pkmRaw = nullptr;
    Root pkm;
    if (field_ptr(raid, g_cfg.f_boss, &pkmRaw) && pkmRaw && pkm.set(pkmRaw) &&
        is_exact(pkm.get(), g_cfg.class_boss)) {
      if (!within_window(deadline, &err)) return err;
      int32_t boss = 0;
      if (field_i32(pkm, g_cfg.f_bossId, &boss) && boss >= 1 && boss <= 99999) {
        pt->boss = boss; pt->flags |= PGR_MP_FLAG_BOSS;
      }
    }
  }
  return within_window(deadline, &err) ? PGR_MP_OK : err;
}

// Walk one POI class, appending accepted points to out->points starting at *count. Returns a scan status:
// PGR_MP_OK keeps walking, PGR_MP_SKIP is a benign expiry (abort empty), negative is a fault (abort empty).
int scan_class(void *klass, uint32_t kind, const PgrMapPoiRequest *request, uint64_t deadline,
               PgrMapPoiOutput *out, uint32_t *count) {
  int err = PGR_MP_OK;
  if (!within_window(deadline, &err)) return err;

  // Root the System.Type BEFORE FindObjectsOfType (it is the method's reference argument), then invoke and
  // root the returned array. Both roots live for this class's inner loop only.
  Root klassType;
  if (!type_object(klass, klassType, &err)) return err;

  if (!within_window(deadline, &err)) return err;
  Root arrayObj;
  {
    void *type = klassType.get();
    if (!type) return PGR_MP_ERR_GCHANDLE;
    void *args[] = {type};
    switch (call(g_cfg.m_find, nullptr, args, arrayObj, /*allowNull=*/true)) {
      case CALL_THROW:    return PGR_MP_ERR_MANAGED;
      case CALL_GCHANDLE: return PGR_MP_ERR_GCHANDLE;
      case CALL_DEAD:     return PGR_MP_ERR_GCHANDLE;
      case CALL_OK:       break;
    }
  }
  if (!arrayObj.get()) return PGR_MP_OK;  // no array yet for this class: nothing to add
  if (!within_window(deadline, &err)) return err;

  const uintptr_t n = reinterpret_cast<ArrayLen>(g_cfg.array_length)(arrayObj.get());
  if (n > static_cast<uintptr_t>(PGR_MP_MAX_ARRAY)) return PGR_MP_ERR_BAD_COUNT;

  for (uintptr_t i = 0; i < n && *count < static_cast<uint32_t>(PGR_MP_MAX_POINTS); ++i) {
    if (!within_window(deadline, &err)) return err;  // lease re-checked every iteration

    // Small per-record RAII scope: live roots are bounded independent of the point count.
    void *base = arrayObj.get();  // re-resolve the (possibly relocated) array base on every indexing
    if (!base) return PGR_MP_ERR_GCHANDLE;
    void *element = nullptr;
    std::memcpy(&element,
                static_cast<char *>(base) + PGR_MP_ARRAY_DATA_OFF + static_cast<size_t>(i) * sizeof(void *),
                sizeof(void *));
    if (!element) continue;  // null element: skip

    Root obj;
    if (!obj.set(element)) return PGR_MP_ERR_GCHANDLE;  // root candidate BEFORE any other allocation
    if (!within_window(deadline, &err)) return err;
    if (!is_exact(obj.get(), klass)) continue;          // wrong class: skip

    // alive(obj): static, args[0] = the rooted candidate -> boxed Boolean. A throw is a main-path fault.
    {
      Root aliveBox;
      void *args[] = {obj.get()};
      switch (call(g_cfg.m_alive, nullptr, args, aliveBox, /*allowNull=*/false)) {
        case CALL_THROW:    return PGR_MP_ERR_MANAGED;
        case CALL_GCHANDLE: return PGR_MP_ERR_GCHANDLE;
        case CALL_DEAD:     return PGR_MP_ERR_GCHANDLE;
        case CALL_OK:       break;
      }
      if (!is_exact(aliveBox.get(), g_cfg.class_bool)) return PGR_MP_ERR_METADATA;
      uint8_t alive = 0;
      if (!unbox_copy(aliveBox, &alive, sizeof(alive))) return PGR_MP_ERR_GCHANDLE;
      if (alive > 1) return PGR_MP_ERR_METADATA;
      if (!alive) continue;  // dead object: skip
    }

    // fort = GetFort(): virtual, resolved on the fresh candidate, invoked as an instance method.
    if (!within_window(deadline, &err)) return err;
    Root fort;
    {
      void *self = obj.get();
      if (!self) return PGR_MP_ERR_GCHANDLE;
      void *vm = reinterpret_cast<VirtMethod>(g_cfg.object_get_virtual_method)(self, g_cfg.m_fort);
      if (!vm) return PGR_MP_ERR_METADATA;
      if (!within_window(deadline, &err)) return err;
      switch (call(vm, &obj, nullptr, fort, /*allowNull=*/true)) {
        case CALL_THROW:    return PGR_MP_ERR_MANAGED;
        case CALL_GCHANDLE: return PGR_MP_ERR_GCHANDLE;
        case CALL_DEAD:     return PGR_MP_ERR_GCHANDLE;
        case CALL_OK:       break;
      }
    }
    if (!within_window(deadline, &err)) return err;
    if (!is_exact(fort.get(), g_cfg.class_fort)) continue;  // null / non-fort: skip

    // fort id: System.String reference -> copy UTF16 immediately (no managed call between chars and memcpy).
    void *idRaw = nullptr;
    if (!field_ptr(fort, g_cfg.f_fortId, &idRaw) || !idRaw) continue;
    Root idStr;
    if (!idStr.set(idRaw)) return PGR_MP_ERR_GCHANDLE;
    if (!within_window(deadline, &err)) return err;
    if (!is_exact(idStr.get(), g_cfg.class_string)) continue;
    Id id;
    {
      void *s = idStr.get();
      if (!s) return PGR_MP_ERR_GCHANDLE;
      id.length = reinterpret_cast<StrLen>(g_cfg.string_length)(s);
      if (id.length < 1 || id.length > PGR_MP_MAX_ID) continue;
      uint16_t *chars = reinterpret_cast<StrChars>(g_cfg.string_chars)(s);
      if (!chars) continue;
      std::memcpy(id.chars, chars, static_cast<size_t>(id.length) * sizeof(uint16_t));
    }
    if (already_seen(out, *count, id)) continue;  // dedup across stop/gym

    // lat / lon: finite and in range.
    double lat = 0, lon = 0;
    if (!field_f64(fort, g_cfg.f_lat, &lat) || !field_f64(fort, g_cfg.f_lon, &lon)) return PGR_MP_ERR_GCHANDLE;
    if (!std::isfinite(lat) || !std::isfinite(lon) || std::fabs(lat) > 90.0 || std::fabs(lon) > 180.0) continue;

    // Commit the point into the caller's buffer (provisional: *count is stamped into out->count only on the
    // final success gate, so an expiry/fault after this still publishes nothing).
    PgrMapPoiPoint *pt = &out->points[*count];
    std::memset(pt, 0, sizeof(*pt));
    pt->kind = kind;
    pt->id_length = static_cast<uint32_t>(id.length);
    std::memcpy(pt->id, id.chars, static_cast<size_t>(id.length) * sizeof(uint16_t));
    pt->lat = lat;
    pt->lon = lon;
    if (kind == PGR_MP_KIND_GYM) {
      err = read_optional_gym(obj, fort, pt, request->wall_ms, deadline);
      if (err != PGR_MP_OK) return err;
    }

    ++(*count);
  }
  return PGR_MP_OK;
}

void init_output(PgrMapPoiOutput *out) {
  std::memset(out, 0, sizeof(*out));
  out->version = PGR_MP_OUTPUT_VERSION;
  out->size = sizeof(PgrMapPoiOutput);
}

// Supplied tid must be nonzero AND equal the current OS tid BEFORE the owner is latched; a later foreign tid
// refuses. Mirrors the sibling owner latch.
bool owner_ready(uint64_t tid) {
  if (tid == 0 || current_tid() != tid) return false;
  uint64_t owner = g_owner_tid.load(std::memory_order_acquire);
  if (!(owner == 0 || owner == tid)) return false;
  uint64_t expected = 0;
  g_owner_tid.compare_exchange_strong(expected, tid, std::memory_order_acq_rel);
  return g_owner_tid.load(std::memory_order_acquire) == tid;
}

bool check_request(const PgrMapPoiRequest *request, int *rc) {
  if (!g_installed.load(std::memory_order_acquire)) { *rc = PGR_MP_ERR_NOT_INSTALLED; return false; }
  if (request == nullptr) { *rc = PGR_MP_ERR_NULL_ARG; return false; }
  if (request->version != PGR_MP_REQUEST_VERSION) { *rc = PGR_MP_ERR_BAD_VERSION; return false; }
  if (request->size != sizeof(PgrMapPoiRequest)) { *rc = PGR_MP_ERR_BAD_SIZE; return false; }
  if (!is_safe_ms(request->wall_ms)) { *rc = PGR_MP_ERR_BAD_CONFIG; return false; }
  *rc = PGR_MP_OK;
  return true;
}

// --- config validation / install ---------------------------------------------------
// A group is valid when it is entirely null (disabled) or entirely populated (enabled); any mix is rejected.
bool group_ok(void *const *ptrs, const int *slots, int n, bool *enabled) {
  int present = 0;
  for (int i = 0; i < n; ++i) if (ptrs[slots[i]] != nullptr) ++present;
  if (present != 0 && present != n) return false;
  *enabled = present == n;
  return true;
}

bool validate_config(const PgrMapPoiConfig *c, int *rc, bool *team, bool *slots, bool *level, bool *boss) {
  if (c == nullptr) { *rc = PGR_MP_ERR_NULL_ARG; return false; }
  if (c->version != PGR_MP_CONFIG_VERSION) { *rc = PGR_MP_ERR_BAD_VERSION; return false; }
  if (c->size != sizeof(PgrMapPoiConfig)) { *rc = PGR_MP_ERR_BAD_SIZE; return false; }

  void *ptrs[kTotalPtrCount];
  std::memcpy(ptrs, reinterpret_cast<const unsigned char *>(c) + offsetof(PgrMapPoiConfig, runtime_invoke),
              sizeof(ptrs));
  for (int i = 0; i < kMandatoryPtrCount; ++i)
    if (ptrs[i] == nullptr) { *rc = PGR_MP_ERR_MISSING_PTR; return false; }

  const int team_slots[] = {31, 32};
  const int slot_slots[] = {33, 34, 35};
  const int boss_slots[] = {37, 38, 39};
  if (!group_ok(ptrs, team_slots, 2, team) || !group_ok(ptrs, slot_slots, 3, slots) ||
      !group_ok(ptrs, boss_slots, 3, boss)) {
    *rc = PGR_MP_ERR_BAD_CONFIG;
    return false;
  }
  *level = ptrs[36] != nullptr;  // independent optional
  *rc = PGR_MP_OK;
  return true;
}

int api_install(const PgrMapPoiConfig *config) {
  int rc = PGR_MP_OK;
  bool team = false, slots = false, level = false, boss = false;
  if (!validate_config(config, &rc, &team, &slots, &level, &boss)) return rc;
  uint64_t probe = 0;
  if (!read_boottime_ms(&probe)) return PGR_MP_ERR_CLOCK;

  std::lock_guard<std::mutex> install_lock(g_install_mutex);
  if (g_installed.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    return std::memcmp(&g_cfg, config, sizeof(*config)) == 0 ? 0 : PGR_MP_ERR_RECONFIG;
  }
  {
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    g_cfg = *config;
    g_has_team = team;
    g_has_slots = slots;
    g_has_level = level;
    g_has_boss = boss;
    g_busy = false;
  }
  g_installed.store(true, std::memory_order_release);
  return 0;
}

int api_scan(const PgrMapPoiRequest *request, PgrMapPoiOutput *out) {
  if (out == nullptr) return PGR_MP_ERR_NULL_ARG;  // cannot report
  init_output(out);
  int rc = PGR_MP_OK;
  if (!check_request(request, &rc)) { out->status = rc; return rc; }

  // Owner latch + attached-thread gate (benign SKIP on failure, no managed work).
  if (!owner_ready(request->tid) || !reinterpret_cast<ThreadCur>(g_cfg.thread_current)()) {
    out->status = PGR_MP_SKIP;
    return PGR_MP_SKIP;
  }
  const uint64_t remaining = request->remaining_ms > kMaxLeaseMs ? kMaxLeaseMs : request->remaining_ms;
  if (remaining == 0) { out->status = PGR_MP_SKIP; return PGR_MP_SKIP; }
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) { out->status = PGR_MP_ERR_CLOCK; return PGR_MP_ERR_CLOCK; }
  const uint64_t deadline = now + remaining;

  { std::lock_guard<std::mutex> lock(g_state_mutex); if (g_busy) { out->status = PGR_MP_ERR_BUSY; return PGR_MP_ERR_BUSY; } g_busy = true; }
  BusyExit busy_exit;
  // Reentrant managed calls may reuse the caller's output buffer. Keep provisional
  // rows private until every managed root has been released and the lease rechecked.
  std::unique_ptr<PgrMapPoiOutput> staged(new (std::nothrow) PgrMapPoiOutput{});
  if (!staged) { out->status = PGR_MP_ERR_ALLOCATION; return out->status; }

  // All managed work (and its roots) lives in this block; every Root is destroyed before the final gate.
  uint32_t count = 0;
  int status = PGR_MP_OK;
  {
    const void *classes[2] = {g_cfg.class_stop, g_cfg.class_gym};
    const uint32_t kinds[2] = {PGR_MP_KIND_STOP, PGR_MP_KIND_GYM};
    for (int k = 0; k < 2 && status == PGR_MP_OK; ++k)
      status = scan_class(const_cast<void *>(classes[k]), kinds[k], request, deadline, staged.get(), &count);
  }
  init_output(out);
  if (status != PGR_MP_OK) { out->status = status; return status; }  // fault / expiry: publish nothing

  // Fresh pre-publication gate, AFTER every root is released (the block above has closed).
  if (!within_window(deadline, &status)) { out->status = status; return status; }

  out->count = count;
  std::memcpy(out->points, staged->points, count * sizeof(PgrMapPoiPoint));
  out->status = PGR_MP_OK;
  return PGR_MP_OK;
}

const PgrMapPoiApi kApiTable = {
    PGR_MP_API_VERSION, static_cast<uint32_t>(sizeof(PgrMapPoiApi)),
    &api_install, &api_scan,
};

}  // namespace

extern "C" const void *pgrazor_map_poi_api(uint32_t version) {
  if (version != PGR_MP_API_VERSION) return nullptr;
  return &kApiTable;
}
