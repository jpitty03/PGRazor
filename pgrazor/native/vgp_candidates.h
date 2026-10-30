#pragma once
#include <stdint.h>

// Feature 29 (API 1). Native VGP CANDIDATE INSPECTION: a focused C++ port of the per-candidate
// predicate work inside runtime/features/vgp.js vgpTick (~lines 248-250), the map-POI fort read
// (~line 135) and the catch metadata reads. It evaluates ONE already-enumerated map object against
// the current catch/spin policy and reports whether that single candidate is eligible to act on,
// plus the dedup key the host seen-policy needs. It owns NOTHING else: the FindObjectsOfType
// enumeration, the service/inventory resolution, the seen-policy map, the pending record + its roots
// and every RPC / request issue stay in JS this increment. This module installs NO hook, adds NO
// automation behaviour, and NEVER calls back into JS.
//
// SCOPE: for each candidate the host hands over (a RAW managed pointer it keeps rooted via the rooted
// results array for the synchronous call) native roots it, checks it is the exact wild/stop class and
// alive, then:
//   catch (action 0): excludes the manual-encounter target (pointer identity), reads the virtual
//     VisibleOnMap getter (hidden -> reject), the non-virtual get_Location (16-byte Niantic.LatLng ->
//     40 m haversine range using the player coords in the request) and the non-virtual get_EncounterId
//     (UInt64) to build the 'p'<decimal> key. No fort handle is returned for a catch.
//   spin (action 1): reads the virtual IsActive / IsPlayerInRange / IsClosed / IsCoolingDown getters
//     (object_get_virtual_method), the virtual FortData getter (exact fort class), and the fort's
//     FortId System.String (length at +16, UTF-16 at +20, 1..256 units -> UTF-8 bounded 1024 bytes,
//     malformed/negative length rejected, never truncated) to build the 's'<utf8> key. On an eligible
//     stop ONLY, native roots the FortData object into a fresh GC handle and transfers ownership of it
//     to the caller (out.fort_handle); the caller adopts it into its owned-handle set, targets it when
//     it dispatches the spin, and frees it later. On every other outcome fort_handle is 0.
//
// Native read-only: every managed value gets a native temporary root for the duration of the call and
// is freed before return; the only handle that survives is the transferred fort handle on an eligible
// stop. No raw managed pointer is reused across a managed call (relocating collector); the FortId
// string is rooted BEFORE any further allocation and its address re-fetched before the length/char
// copy. inspect() runs managed calls only on the proven Unity owner thread and only inside a short
// residual lease (clamped to PGR_VC_MAX_LEASE_MS); the host re-checks expiry / stopping after the
// call and frees the fort handle itself if it then drops the candidate.
//
// Outcomes:
//   inspect() returns PGR_VC_ELIGIBLE (1)  -> out.eligible == 1, out.key / out.key_len set,
//                                             out.fort_handle set iff action == spin.
//                     PGR_VC_NOT (0)        -> out.eligible == 0, out.reason one of the soft reasons
//                                             below, out.fort_handle == 0 (candidate skipped, benign).
//                     a negative error      -> a conservative failure (bad thread, managed throw,
//                                             value class/size anomaly, root failure, bad coords).
//                                             The host PAUSES VGP; it does NOT fall back to legacy.

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "VgpCandidates native ABI requires LP64");

enum PgrVgpCandidateInfo {
  PGR_VC_FEATURE_ID     = 29,
  PGR_VC_API_VERSION    = 1,
  PGR_VC_CONFIG_VERSION = 1,
  PGR_VC_REQUEST_VERSION = 1,
  PGR_VC_RESULT_VERSION = 1,
  PGR_VC_MAX_LEASE_MS   = 2500,  // residual foreground lease ceiling (JS vgpLease gate)
  PGR_VC_ACTION_CATCH   = 0,     // target == VGP wild map pokemon
  PGR_VC_ACTION_SMART_CATCH = 2, // loaded visible wild; encounter determines usable range
  PGR_VC_ACTION_SPIN    = 1,     // target == VGP stop map object
  PGR_VC_MAX_FORT_UNITS = 256,   // accepted FortId length in UTF-16 code units (JS 1..256)
  PGR_VC_KEY_CAPACITY   = 1040   // out.key bytes: 1 prefix + <=1024 UTF-8 + NUL, rounded to 8
};

// inspect() status (return value).
enum PgrVgpCandidateStatus {
  PGR_VC_ELIGIBLE          = 1,
  PGR_VC_NOT               = 0,
  PGR_VC_ERR_NULL_ARG      = -1,
  PGR_VC_ERR_BAD_VERSION   = -2,
  PGR_VC_ERR_BAD_SIZE      = -3,
  PGR_VC_ERR_MISSING_PTR   = -4,   // install(): a required config pointer was null
  PGR_VC_ERR_NOT_INSTALLED = -6,
  PGR_VC_ERR_RECONFIG      = -9,   // install(): a different config at an already-installed table
  PGR_VC_ERR_CLOCK_FAILURE = -10,
  PGR_VC_ERR_THREAD        = -11,  // owner_tid 0 / not the calling OS thread
  PGR_VC_ERR_GCHANDLE      = -12,  // a temporary root or the fort handle could not be allocated
  PGR_VC_ERR_MANAGED       = -13,  // a managed getter threw
  PGR_VC_ERR_METADATA      = -14,  // a returned value had the wrong class (bool/latlng/uint64)
  PGR_VC_ERR_BAD_ACTION    = -15,  // action not catch/spin
  PGR_VC_ERR_COORDS        = -16   // player lat/lon non-finite or out of range
};

// out.reason when inspect() returns PGR_VC_NOT (benign skip). PGR_VC_REASON_NONE accompanies an
// eligible result. The host maps these to its scan counters / status.
enum PgrVgpCandidateReason {
  PGR_VC_REASON_NONE         = 0,
  PGR_VC_REASON_INVALID      = 1,  // not the exact wild/stop class, not alive, or lease expired mid-call
  PGR_VC_REASON_MANUAL       = 2,  // catch: identical to the manual-encounter target
  PGR_VC_REASON_HIDDEN       = 3,  // catch: VisibleOnMap == false
  PGR_VC_REASON_OUT_OF_RANGE = 4,  // catch: location outside the 40 m catch radius
  PGR_VC_REASON_INELIGIBLE   = 5   // spin: predicate false, wrong fort class, or null/malformed FortId
};

// Fixed LP64 config (200 bytes): 8-byte header + 24 validated il2cpp pointers. Every pointer is an
// already-validated export / MethodInfo* / FieldInfo* / Il2CppClass* packed by the host off p.vgp;
// the native layer re-checks the header and rejects any null. No hook address. The virtual getters are
// the BASE MethodInfo* (resolved per object with object_get_virtual_method); location / id / alive are
// invoked directly (non-virtual).
struct PgrVgpCandidateConfig {
  uint32_t version;  // == PGR_VC_CONFIG_VERSION
  uint32_t size;     // == sizeof(PgrVgpCandidateConfig)

  // il2cpp runtime exports.
  void *gchandle_new;            // uintptr_t (void* obj, int pinned)
  void *gchandle_get_target;     // void*     (uintptr_t handle)
  void *gchandle_free;           // void      (uintptr_t handle)
  void *object_get_class;        // void*     (void* obj)
  void *runtime_invoke;          // void*     (MethodInfo*, void* obj, void** params, void** exc)
  void *object_unbox;            // void*     (void* boxed) -> value storage
  void *object_get_virtual_method; // void*   (void* obj, MethodInfo* base) -> resolved slot
  void *field_get_value;         // void      (void* obj, FieldInfo*, void* out)

  // Non-virtual MethodInfo* (invoked directly via runtime_invoke).
  void *m_alive;     // static alive(obj) -> Boolean
  void *m_location;  // wild.get_Location() -> Niantic.LatLng (boxed, 16 bytes: lat, lon)
  void *m_id;        // wild.get_EncounterId() -> System.UInt64

  // Virtual base MethodInfo* (resolved per object, then invoked).
  void *m_visible;   // wild.get_VisibleOnMap() -> Boolean
  void *m_active;    // stop.get_IsActive() -> Boolean
  void *m_range;     // stop.get_IsPlayerInRange() -> Boolean
  void *m_closed;    // stop.get_IsClosed() -> Boolean
  void *m_cooling;   // stop.get_IsCoolingDown() -> Boolean
  void *m_fort;      // stop.get_FortData() -> FortData

  // FieldInfo*.
  void *f_fort_id;   // fort.FortId (System.String)

  // Exact Il2CppClass* identities.
  void *class_wild;    // VGP.wild (catch target)
  void *class_stop;    // VGP.stop (spin target), distinct from wild
  void *class_fort;    // VGP.fort (FortData)
  void *class_bool;    // System.Boolean (value size 1)
  void *class_latlng;  // Niantic.LatLng (value size 16)
  void *class_uint64;  // System.UInt64 (value size 8)
};

static_assert(sizeof(PgrVgpCandidateConfig) == 200,
              "PgrVgpCandidateConfig must remain 200 bytes on LP64");

// Fixed LP64 request (64 bytes). target / manual are raw managed pointers the caller keeps rooted for
// the synchronous call; native also roots them. manual may be null (no manual exclusion). lat/lon are
// the player coordinates (finite, |lat|<=90, |lon|<=180). owner_tid is the proven Unity owner tid
// (must equal the current OS tid). lease_ms is the residual foreground lease (clamped MAX_LEASE_MS).
struct PgrVgpCandidateRequest {
  uint32_t version;    // == PGR_VC_REQUEST_VERSION
  uint32_t size;       // == sizeof(PgrVgpCandidateRequest)
  uint32_t action;     // normal catch, smart catch, or spin (PGR_VC_ACTION_*)
  uint32_t reserved;   // 0
  uint64_t owner_tid;  // proven Unity owner tid
  uint64_t lease_ms;   // residual foreground lease (ms)
  double   lat;        // player latitude
  double   lon;        // player longitude
  void    *target;     // candidate map object (rooted)
  void    *manual;     // manual-encounter target, or null (catch only)
};

static_assert(sizeof(PgrVgpCandidateRequest) == 64,
              "PgrVgpCandidateRequest must remain 64 bytes on LP64");

// Fixed LP64 result (1072 bytes). Always fully initialised by inspect() before any early return.
struct PgrVgpCandidateResult {
  uint32_t version;      // == PGR_VC_RESULT_VERSION
  uint32_t size;         // == sizeof(PgrVgpCandidateResult)
  int32_t  eligible;     // 1 eligible, 0 otherwise
  int32_t  reason;       // PgrVgpCandidateReason
  uint32_t key_len;      // key bytes written (excluding the NUL); 0 unless eligible
  uint32_t reserved;     // 0
  uint64_t fort_handle;  // OWNED GC handle (uintptr) on an eligible spin; 0 otherwise
  char     key[PGR_VC_KEY_CAPACITY];  // 's'/'p' prefix + payload, NUL-terminated
};

static_assert(sizeof(PgrVgpCandidateResult) == 1072,
              "PgrVgpCandidateResult must remain 1072 bytes on LP64");

// Fixed LP64 table: 8-byte header + 2 function pointers = 24 bytes.
struct PgrVgpCandidateApi {
  uint32_t version;  // == PGR_VC_API_VERSION
  uint32_t size;     // == sizeof(PgrVgpCandidateApi)
  // install(config): bind the validated runtime once for the process lifetime. Idempotent for an
  // identical config; a different config returns PGR_VC_ERR_RECONFIG. Any null required pointer or a
  // bad header is rejected (fail closed). No hook is installed.
  int (*install)(const PgrVgpCandidateConfig *config);
  // inspect(request, out): evaluate ONE candidate. Returns PGR_VC_ELIGIBLE / PGR_VC_NOT / negative.
  // out is always fully initialised. Owner thread only.
  int (*inspect)(const PgrVgpCandidateRequest *request, PgrVgpCandidateResult *out);
};

static_assert(sizeof(PgrVgpCandidateApi) == 24,
              "PgrVgpCandidateApi must remain a 24-byte version-1 table");

// Hidden getter. NOT exported by the version script; the coordinator registers it in query_feature
// under feature id 29 and answers only on an exact version match.
const void *pgrazor_vgp_candidates_api(uint32_t version);

#ifdef __cplusplus
}
#endif
