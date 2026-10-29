#pragma once
#include <stdint.h>

// Feature 42 (API 1). Native MAP POI SNAPSHOT: the C++ port of the read-only snapshot branch of
// runtime/features/vgp.js mapPoiTick (~147-194). From NO borrowed handle it enumerates the two on-map POI
// classes -- PokeStop (class_stop) and Gym (class_gym) -- via FindObjectsOfType, proves each candidate, reads
// its Fort (virtually resolved), copies the fort id (UTF16), latitude/longitude, and for gyms the optional
// team / gym-display slots / active-raid fields, and publishes a FIXED bounded array of scalar points. It is a
// pure reader: it installs NO hook, issues NO gameplay request, NEVER mutates the game and NEVER calls back
// into JS. The legacy 'map-pois' points event is preserved by the host adapter, which maps the raw team/level
// enum values to names and shapes the legacy point objects. All name mapping stays in JS.
//
// One synchronous entry:
//   scan(request,out) -> 1 SNAPSHOT (out holds `count` points; count 0 is a valid empty snapshot),
//                        0 REFUSED/EXPIRED (owner mismatch / unattached thread / closed or expired lease;
//                        output left empty, count 0), or a negative FAULT (a native or managed error; output
//                        left empty, count 0). Owner only. A partial snapshot is NEVER published: on an
//                        expiry or fault discovered mid-walk the count stays 0.
//
// Rooting discipline (identical intent to the JS roots[]/root() helper and the sibling readers): every managed
// return is rooted immediately on the proven owner thread and re-resolved from its root after any
// runtime_invoke / gchandle_new / gchandle_free (relocating collector). The array base is re-read from its
// root on every indexing (element[i] at LP64 data offset 32); a raw array/candidate/fort pointer is NEVER
// reused across an allocation. Per-record roots live in a small RAII scope that is torn down before the next
// candidate, so the number of live roots is bounded independent of the point count -- the 294928-byte output
// is the caller's buffer (never a stack object) and holds only copied scalars + copied UTF16, never a managed
// pointer or a GC handle. The owner is latched from a genuine on-thread call (tid nonzero AND == the current
// OS tid BEFORE the latch; a later foreign tid refuses). The residual lease is clamped to PGR_MP_MAX_LEASE_MS
// (CLOCK_BOOTTIME), re-checked between managed calls and once more AFTER every root is released and before the
// output is published. An atomic busy guard spans ALL temporary-root cleanup; reentry refuses. Managed
// exceptions surface through runtime_invoke's exception out-param: a throw on the main path (find / alive /
// fort) is a negative fault, while a throw or a bad class/out-of-range value on an OPTIONAL field (team /
// slots / level / boss) only omits that field and keeps the snapshot.

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "MapPoi native ABI requires LP64");

enum PgrMapPoiInfo {
  PGR_MP_FEATURE_ID      = 42,
  PGR_MP_API_VERSION     = 1,
  PGR_MP_CONFIG_VERSION  = 1,
  PGR_MP_REQUEST_VERSION = 1,
  PGR_MP_OUTPUT_VERSION  = 1,
  PGR_MP_MAX_LEASE_MS    = 2500,          // residual foreground lease ceiling (JS allowedUntil/deadline gate)
  PGR_MP_MAX_POINTS      = 512,           // total snapshot ceiling across both classes (JS points.length<512)
  PGR_MP_MAX_ARRAY       = 2048,          // per-class FindObjectsOfType element ceiling (JS 'map-count' guard)
  PGR_MP_MAX_ID          = 256,           // UTF16 id length ceiling (JS id length 1..256)
  PGR_MP_ARRAY_DATA_OFF  = 32             // raw LP64 managed array data start (element[i] @ base + 32 + i*8)
};

// scan() return == out.status: 1 SNAPSHOT (count points published) / 0 REFUSED (benign gate/lease; count 0) /
// negative FAULT (count 0). install() returns 0 on success; negatives are faults.
enum PgrMapPoiStatus {
  PGR_MP_OK                = 1,   // snapshot published (count may be 0)
  PGR_MP_SKIP              = 0,   // benign refuse: owner mismatch / unattached thread / closed/expired lease
  PGR_MP_ERR_NULL_ARG      = -1,
  PGR_MP_ERR_BAD_VERSION   = -2,
  PGR_MP_ERR_BAD_SIZE      = -3,
  PGR_MP_ERR_MISSING_PTR   = -4,  // install(): a mandatory config pointer was null
  PGR_MP_ERR_BAD_CONFIG    = -5,  // install(): a partially-populated optional pointer group
  PGR_MP_ERR_NOT_INSTALLED = -6,
  PGR_MP_ERR_RECONFIG      = -9,  // install(): a different config at an already-installed table
  PGR_MP_ERR_CLOCK         = -10,
  PGR_MP_ERR_BUSY          = -11, // reentrant managed call refused (defensive)
  PGR_MP_ERR_GCHANDLE      = -12, // a temporary root could not be allocated / an expected value was null
  PGR_MP_ERR_MANAGED       = -13, // a main-path managed call threw
  PGR_MP_ERR_METADATA      = -14, // a resolved object had the wrong class
  PGR_MP_ERR_BAD_COUNT     = -16, // a per-class FindObjectsOfType array exceeded PGR_MP_MAX_ARRAY
  PGR_MP_ERR_ALLOCATION    = -17  // scalar snapshot staging allocation failed
};

// Point.kind: the enumerated POI class.
enum PgrMapPoiKind {
  PGR_MP_KIND_STOP = 0,
  PGR_MP_KIND_GYM  = 1
};

// Point.flags: which optional fields carry a value. Missing optional field -> bit clear + 0 value.
enum PgrMapPoiFlags {
  PGR_MP_FLAG_TEAM  = 1,   // team_id valid
  PGR_MP_FLAG_SLOTS = 2,   // slots valid (0..6)
  PGR_MP_FLAG_LEVEL = 4,   // level valid (raw signed enum)
  PGR_MP_FLAG_BOSS  = 8,   // boss valid (1..99999)
  PGR_MP_FLAG_EGG   = 16   // active raid is an unhatched egg (battle_ms > wall)
};

// Fixed LP64 config (328 bytes): 8-byte header + 40 pointers in the EXACT slot order below. Slots 0..30 are
// MANDATORY (non-null). The OPTIONAL trailer forms three all-or-none groups plus one independent slot:
//   team  = {m_team(31), class_teamBox(32)}
//   slots = {f_gymDisplay(33), class_gymDisplay(34), f_slots(35)}
//   boss  = {f_boss(37), class_boss(38), f_bossId(39)}
//   level = f_level(36)   (independent)
// A partially-populated group is rejected (PGR_MP_ERR_BAD_CONFIG); an all-null group simply disables that
// optional field. Every pointer is an already-validated il2cpp export / Il2CppClass* / MethodInfo* /
// Il2CppFieldInfo* packed by the host. No hook address. Reordering is forbidden.
struct PgrMapPoiConfig {
  uint32_t version;  // == PGR_MP_CONFIG_VERSION
  uint32_t size;     // == sizeof(PgrMapPoiConfig)

  // il2cpp runtime exports (slots 0-13).
  void *runtime_invoke;            // 0  void*     (MethodInfo*, void* obj, void** params, void** exc)
  void *object_get_class;          // 1  void*     (void* obj)
  void *field_get_value;           // 2  void      (void* obj, FieldInfo*, void* out)  [no error return]
  void *object_unbox;              // 3  void*     (void* boxed) -> value pointer
  void *gchandle_new;              // 4  uintptr_t (void* obj, int pinned)
  void *gchandle_get_target;       // 5  void*     (uintptr_t handle)
  void *gchandle_free;             // 6  void      (uintptr_t handle)
  void *thread_current;            // 7  void*     (void) -> attached Unity thread or null
  void *class_get_type;            // 8  void*     (void* klass) -> Il2CppType*
  void *type_get_object;           // 9  void*     (Il2CppType*) -> managed System.Type
  void *array_length;              // 10 uintptr_t (void* array)
  void *object_get_virtual_method; // 11 void*     (void* obj, MethodInfo*) -> resolved MethodInfo*
  void *string_length;             // 12 int32_t   (void* string)
  void *string_chars;              // 13 uint16_t* (void* string) -> interior chars (not NUL terminated)

  // Exact Il2CppClass* identities (slots 14-19).
  void *class_stop;    // 14 classes[VGP.stop] (PokeStop; kind 0 candidate)
  void *class_gym;     // 15 p.mapPoi.gym      (Gym; kind 1 candidate)
  void *class_fort;    // 16 classes[VGP.fort] (the fort resolved from each candidate)
  void *class_raid;    // 17 p.mapPoi.raid     (the raid info object; exact)
  void *class_bool;    // 18 System.Boolean    (alive() boxed result; exact)
  void *class_string;  // 19 System.String     (fort id; exact)

  // MethodInfo* (slots 20-22), invoked via runtime_invoke.
  void *m_find;  // 20 static  Object[] FindObjectsOfType(Type)
  void *m_alive; // 21 static  Boolean IsAlive(obj)  (args[0] = the rooted candidate)
  void *m_fort;  // 22 instance Fort GetFort()       (virtually resolved on a fresh candidate)

  // Validated Il2CppFieldInfo* for the mandatory fort/raid fields (slots 23-30), read via field_get_value.
  void *f_fortId;   // 23 fort -> System.String id
  void *f_lat;      // 24 fort -> double latitude
  void *f_lon;      // 25 fort -> double longitude
  void *f_raidInfo; // 26 fort -> raid info object (reference)
  void *f_spawn;    // 27 raid -> uint64 spawn ms
  void *f_end;      // 28 raid -> uint64 end ms
  void *f_battle;   // 29 raid -> uint64 battle ms
  void *f_hidden;   // 30 raid -> bool hidden

  // OPTIONAL (slots 31-39). See the group rules above. Null (per whole group) disables the field.
  void *m_team;           // 31 instance boxed-enum Team()   (non-virtual invoke on the candidate)
  void *class_teamBox;    // 32 boxed team enum class (exact; value size 4)
  void *f_gymDisplay;     // 33 fort -> gym display object (reference)
  void *class_gymDisplay; // 34 gym display class (exact)
  void *f_slots;          // 35 gym display -> int32 occupied slots
  void *f_level;          // 36 raid -> int32 level (independent optional)
  void *f_boss;           // 37 raid -> boss pokemon object (reference)
  void *class_boss;       // 38 boss pokemon class (exact)
  void *f_bossId;         // 39 boss pokemon -> int32 species id
};

static_assert(sizeof(PgrMapPoiConfig) == 328, "PgrMapPoiConfig must remain 328 bytes on LP64");

// Fixed LP64 request (32 bytes). tid is the proven owner tid (nonzero and == the current OS tid). remaining_ms
// is the residual lease (clamped to PGR_MP_MAX_LEASE_MS; 0 refuses). wall_ms is the host wall clock in ms
// (Date.now()) used only for the active-raid window comparison -- never for the lease.
struct PgrMapPoiRequest {
  uint32_t version;      // off 0  == PGR_MP_REQUEST_VERSION
  uint32_t size;         // off 4  == sizeof(PgrMapPoiRequest)
  uint64_t tid;          // off 8  proven owner tid
  uint64_t remaining_ms; // off 16 residual lease; clamped, 0 refuses
  uint64_t wall_ms;      // off 24 host wall clock ms for the raid window (Date.now())
};

static_assert(sizeof(PgrMapPoiRequest) == 32, "PgrMapPoiRequest must remain 32 bytes on LP64");

// Fixed LP64 point (576 bytes). Scalar only: the id is copied UTF16 (NOT NUL terminated; use id_length). A
// stop carries only kind/id/lat/lon; a gym may additionally carry the optional fields flagged in `flags`.
struct PgrMapPoiPoint {
  uint32_t kind;       // off 0  PgrMapPoiKind
  uint32_t flags;      // off 4  PgrMapPoiFlags bitset
  uint32_t id_length;  // off 8  UTF16 units in id (1..256)
  uint32_t reserved0;  // off 12 0 in v1
  double lat;          // off 16 latitude (|lat| <= 90)
  double lon;          // off 24 longitude (|lon| <= 180)
  double raid_end;     // off 32 active-raid end ms (0 when no active raid)
  double raid_start;   // off 40 active-raid battle ms (0 when no active raid)
  int32_t team_id;     // off 48 raw team enum (FLAG_TEAM)
  int32_t slots;       // off 52 occupied gym slots 0..6 (FLAG_SLOTS)
  int32_t level;       // off 56 raw raid level enum (FLAG_LEVEL)
  int32_t boss;        // off 60 raid boss species id 1..99999 (FLAG_BOSS)
  uint16_t id[PGR_MP_MAX_ID]; // off 64 copied UTF16 fort id
};

static_assert(sizeof(PgrMapPoiPoint) == 576, "PgrMapPoiPoint must remain 576 bytes on LP64");

// Fixed LP64 output (294928 bytes). Always fully zeroed on entry (header re-stamped). status mirrors the
// return. On a positive return the first `count` points are valid; the rest stay zero. On a 0 refuse / a
// negative fault count stays 0.
struct PgrMapPoiOutput {
  uint32_t version;  // off 0  == PGR_MP_OUTPUT_VERSION
  uint32_t size;     // off 4  == sizeof(PgrMapPoiOutput)
  int32_t status;    // off 8  == the scan() return
  uint32_t count;    // off 12 number of valid points (<= PGR_MP_MAX_POINTS)
  PgrMapPoiPoint points[PGR_MP_MAX_POINTS]; // off 16
};

static_assert(sizeof(PgrMapPoiOutput) == 294928, "PgrMapPoiOutput must remain 294928 bytes on LP64");

// Fixed LP64 table: 8-byte header + 2 function pointers = 24 bytes.
struct PgrMapPoiApi {
  uint32_t version;  // == PGR_MP_API_VERSION
  uint32_t size;     // == sizeof(PgrMapPoiApi)
  // Verify + publish the immutable config once for the process lifetime. Idempotent for an identical config;
  // a different config returns PGR_MP_ERR_RECONFIG. A null mandatory pointer, a bad header or a partial
  // optional group is rejected (fail closed). No hook is installed.
  int (*install)(const PgrMapPoiConfig *config);
  // Owner-thread read-only snapshot. See the file header for the full contract. 1 / 0 / negative.
  int (*scan)(const PgrMapPoiRequest *request, PgrMapPoiOutput *out);
};

static_assert(sizeof(PgrMapPoiApi) == 24, "PgrMapPoiApi must remain a 24-byte version-1 table");

// Hidden getter. NOT exported by the version script; the coordinator registers it in query_feature under
// feature id 42 and answers only on an exact version match.
const void *pgrazor_map_poi_api(uint32_t version);

#ifdef __cplusplus
}
#endif
