#pragma once
#include <stdint.h>

// Feature 40 (API 1). Native POST-CATCH CANDIDATE DISCOVERY: the C++ port of the two scene lookups inside
// diagnostics/quick-close.js discoverCatchDetails (~1507-1524) -- the registry TopGui(layer) probe and the
// bounded FindObjectsOfType(typeof DetailsGuiController) scene walk. From ONLY a borrowed GuiLayerService
// handle (top) or nothing (scene) it resolves the candidate DetailsGuiController(s), roots each one, and
// hands the native-owned GC handles back to the host. This reader NEVER hides, dismisses or transfers and
// installs NO hook: the existing host recovery (feature 38) still re-verifies the caught identity on each
// returned candidate; this module only enumerates and roots.
//
// Two entry points so the host can stop at the first accepted candidate:
//   top(request,out)   -> invoke IGuiController TopGui() on the borrowed layer, root a single result and,
//                         when it is an exact DetailsGuiController, publish ONE native-owned handle. A null
//                         TopGui (some opening screens are excluded) or a non-Details result is a benign
//                         refuse (0) so the host falls through to scene(). Requires request->layer.
//   scene(request,out) -> FindObjectsOfType(typeof DetailsGuiController), validate the element count
//                         (<= PGR_CD_MAX_CANDIDATES; a larger array is REJECTED, never truncated), and
//                         publish one native-owned handle per NONNULL exact-Details element (at most 16).
//                         Ignores request->layer. A null array is a benign refuse (0).
// The host queries scene() only when top() did not yield an accepted candidate.
//
// Read-only: every managed value is temporarily rooted on the proven owner thread and freed before return;
// no raw managed pointer survives a runtime_invoke or a gchandle_new (the array base is re-resolved from
// its root on every indexing, element[i] at LP64 data offset 32). All managed work runs only on the
// latched Unity owner thread (the supplied tid must be nonzero AND equal the current OS tid BEFORE the
// owner is latched; a later foreign tid refuses) inside a residual lease clamped to PGR_CD_MAX_LEASE_MS
// (CLOCK_BOOTTIME). The deadline is re-sampled AFTER the lookups/root allocation and before publication;
// an expired lease returns clean with no retained root. An atomic busy guard spans root cleanup to refuse
// managed reentry. Managed exceptions surface through runtime_invoke's exception out-param as a negative
// fault, never a C++ throw. Ownership of *out passes to the caller ONLY on a positive (1) return; a 0
// refuse and a negative fault both leave the output fully zeroed. No root is retained outside the output.

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "CatchDiscovery native ABI requires LP64");

enum PgrCatchDiscoveryInfo {
  PGR_CD_FEATURE_ID      = 40,
  PGR_CD_API_VERSION     = 1,
  PGR_CD_CONFIG_VERSION  = 1,
  PGR_CD_REQUEST_VERSION = 1,
  PGR_CD_OUTPUT_VERSION  = 1,
  PGR_CD_MAX_LEASE_MS    = 2500, // residual foreground lease ceiling (JS allowedUntil/deadline gate)
  PGR_CD_MAX_CANDIDATES  = 16,   // strict element ceiling (JS 'catch-scene-count' guard); never truncate
  PGR_CD_ARRAY_DATA_OFF  = 32    // raw LP64 managed array data start (element[i] @ base + 32 + i*8)
};

// top()/scene() return 1 FOUND (output owned by the caller) / 0 REFUSE (benign gate/null/identity skip;
// output clean) / negative FAULT (output clean). install() returns 0; release() returns PGR_CD_OK; faults are negative.
enum PgrCatchDiscoveryStatus {
  PGR_CD_OK                = 1,   // top/scene: candidate(s) found and published
  PGR_CD_SKIP              = 0,   // benign refuse: closed/expired lease, owner mismatch, null result
  PGR_CD_ERR_NULL_ARG      = -1,
  PGR_CD_ERR_BAD_VERSION   = -2,
  PGR_CD_ERR_BAD_SIZE      = -3,
  PGR_CD_ERR_MISSING_PTR   = -4,  // install(): a required config pointer was null
  PGR_CD_ERR_NOT_INSTALLED = -6,
  PGR_CD_ERR_RECONFIG      = -9,  // install(): a different config at an already-installed table
  PGR_CD_ERR_CLOCK_FAILURE = -10,
  PGR_CD_ERR_BUSY          = -11, // reentrant managed call refused (defensive)
  PGR_CD_ERR_GCHANDLE      = -12, // a temporary root or a returned handle could not be allocated
  PGR_CD_ERR_MANAGED       = -13, // a managed call threw
  PGR_CD_ERR_METADATA      = -14, // a resolved object had the wrong class
  PGR_CD_ERR_BAD_COUNT     = -16  // scene(): array length exceeded PGR_CD_MAX_CANDIDATES
};

// Fixed LP64 config (104 bytes): 8-byte header + 12 validated pointers in the EXACT order below. Every
// pointer is an already-validated il2cpp export / Il2CppClass* / MethodInfo* packed by the host; the
// native layer re-checks the header and rejects any null. No hook address. Reordering is forbidden.
struct PgrCatchDiscoveryConfig {
  uint32_t version;  // == PGR_CD_CONFIG_VERSION
  uint32_t size;     // == sizeof(PgrCatchDiscoveryConfig)

  // il2cpp runtime exports (slots 0-7).
  void *object_get_class;    // 0  void*     (void* obj)
  void *runtime_invoke;      // 1  void*     (MethodInfo*, void* obj, void** params, void** exc)
  void *gchandle_new;        // 2  uintptr_t (void* obj, int pinned)
  void *gchandle_get_target; // 3  void*     (uintptr_t handle)
  void *gchandle_free;       // 4  void      (uintptr_t handle)
  void *class_get_type;      // 5  void*     (void* klass) -> Il2CppType*
  void *type_get_object;     // 6  void*     (Il2CppType*) -> managed System.Type
  void *array_length;        // 7  uintptr_t (void* array)

  // Exact Il2CppClass* identities (slots 8-9).
  void *class_layer;         // 8  p.catchDiscovery.klass (GuiLayerService; TopGui receiver class)
  void *class_details;       // 9  classes[DGUI] (DetailsGuiController; the only accepted candidate class)

  // MethodInfo* (slots 10-11), invoked via runtime_invoke.
  void *m_top;               // 10 p.catchDiscovery.top.method (instance IGuiController TopGui())
  void *m_find;              // 11 p.catchScene.find (static Object[] FindObjectsOfType(Type))
};

static_assert(sizeof(PgrCatchDiscoveryConfig) == 104,
              "PgrCatchDiscoveryConfig must remain 104 bytes on LP64");

// Fixed LP64 request (32 bytes). tid is the proven owner tid (must be nonzero and equal the current OS
// tid). remaining_ms is the residual lease (clamped to PGR_CD_MAX_LEASE_MS; 0 refuses). layer is a
// borrowed GC HANDLE ID (not a managed address) for the GuiLayerService; top() requires it, scene() ignores
// it. The caller retains the borrowed handle for the duration of the call.
struct PgrCatchDiscoveryRequest {
  uint32_t version;      // off 0  == PGR_CD_REQUEST_VERSION
  uint32_t size;         // off 4  == sizeof(PgrCatchDiscoveryRequest)
  uint64_t tid;          // off 8  proven owner tid
  uint64_t remaining_ms; // off 16 residual lease; clamped, 0 refuses
  void *layer;           // off 24 borrowed GC handle -> GuiLayerService (top only)
};

static_assert(sizeof(PgrCatchDiscoveryRequest) == 32,
              "PgrCatchDiscoveryRequest must remain 32 bytes on LP64");

// Fixed LP64 output (144 bytes). Always fully zeroed on entry (header/count/reserved re-stamped). On a
// positive return the first `count` handles are NATIVE-OWNED GC handle IDs (ownership transfers to the
// caller, which MUST call release()); the remaining slots stay zero. top() publishes at most 1; scene()
// at most PGR_CD_MAX_CANDIDATES. On a 0 refuse / negative fault every handle stays zero.
struct PgrCatchDiscoveryOutput {
  uint32_t version;  // off 0  == PGR_CD_OUTPUT_VERSION
  uint32_t size;     // off 4  == sizeof(PgrCatchDiscoveryOutput)
  uint32_t count;    // off 8  number of published handles (<= PGR_CD_MAX_CANDIDATES)
  uint32_t reserved; // off 12 0 in v1
  void *handles[PGR_CD_MAX_CANDIDATES]; // off 16 native-owned candidate handles (DetailsGuiController)
};

static_assert(sizeof(PgrCatchDiscoveryOutput) == 144,
              "PgrCatchDiscoveryOutput must remain 144 bytes on LP64");

// Fixed LP64 table: 8-byte header + 4 function pointers = 40 bytes (no reserved slot).
struct PgrCatchDiscoveryApi {
  uint32_t version;  // == PGR_CD_API_VERSION
  uint32_t size;     // == sizeof(PgrCatchDiscoveryApi)
  // Verify + publish the immutable config once for the process lifetime. Idempotent for an identical
  // config; a different config returns PGR_CD_ERR_RECONFIG. Any null required pointer or a bad header is
  // rejected (fail closed). No hook is installed.
  int (*install)(const PgrCatchDiscoveryConfig *config);
  // Owner-thread registry probe. Roots request->layer, invokes TopGui(), and publishes ONE native-owned
  // handle when the result is an exact DetailsGuiController. Null/non-Details -> 0 refuse. Owner only.
  int (*top)(const PgrCatchDiscoveryRequest *request, PgrCatchDiscoveryOutput *out);
  // Owner-thread bounded scene walk. FindObjectsOfType(typeof DetailsGuiController), rejects an oversize
  // array, and publishes one native-owned handle per nonnull exact-Details element. Owner only.
  int (*scene)(const PgrCatchDiscoveryRequest *request, PgrCatchDiscoveryOutput *out);
  // Free every nonzero native-owned handle in *out and zero it (idempotent, repeat-safe). Never frees a
  // borrowed input. Any thread.
  int (*release)(PgrCatchDiscoveryOutput *out);
};

static_assert(sizeof(PgrCatchDiscoveryApi) == 40,
              "PgrCatchDiscoveryApi must remain a 40-byte version-1 table");

// Hidden getter. NOT exported by the version script; the coordinator registers it in query_feature under
// feature id 40 and answers only on an exact version match.
const void *pgrazor_catch_discovery_api(uint32_t version);

#ifdef __cplusplus
}
#endif
