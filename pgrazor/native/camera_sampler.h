#pragma once
#include <stdint.h>

// Feature 41 (API 1). Native CONTINUOUS CAMERA SAMPLING: the C++ port of the continuous branch of
// diagnostics/camera-probe.js sample() (~303-448). From a RAW MainCameraRig receiver and the RAW
// UnityEngine.Camera the verified get_MainCamera leaf getter just returned (both borrowed for the call
// only), it re-reads the managed main camera, proves it on the world map, and publishes the forward-vector
// yaw plus the camera's instance id. It installs NO hook (the host keeps the Gum getter observer and the
// invocation-thread gate) and NEVER mutates the camera or the game: every managed call is a read-only
// getter. The legacy bounded (non-continuous) diagnostic path stays in JS.
//
// One synchronous entry:
//   sample(request,out) -> 1 SAMPLE (out holds a fresh world-map reading), 0 UNAVAILABLE (a benign,
//                          transient refusal: off/lease/owner-thread/non-map-rig/null-camera/
//                          camera-mismatch/null-transform/map-hidden), or a negative FAULT (a native or
//                          managed error the host latches, matching the legacy sampleError). Owner only.
//
// Rooting discipline (identical intent to the JS sampler): the RAW rig is only a class proof taken BEFORE
// any gchandle_new or runtime_invoke -- a non-MainCameraRig rig is the off-map case and yields 0, never a
// fault; the rig pointer is never retained or re-read. The RAW returned camera is rooted FIRST, before any
// other allocation, and its raw pointer is never used again. Every managed return is rooted immediately and
// re-resolved from its root after each invoke/gchandle_new (relocating collector). The owner is latched from
// a genuine on-thread call (tid nonzero AND == the current OS tid BEFORE the latch; a later foreign tid
// refuses). The residual lease is clamped to PGR_CS_MAX_LEASE_MS (CLOCK_BOOTTIME) and the deadline is
// re-sampled AFTER the managed reads and before publication (an expired lease is a clean 0). An atomic busy
// guard spans ALL temporary-root cleanup; reentry refuses. The WorldMap layer is verified == 13 (or reused
// from a prior verified sample). No GC root survives the call and no per-account state is kept. Managed
// exceptions surface through runtime_invoke's exception out-param as a negative fault, never a C++ throw.

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "CameraSampler native ABI requires LP64");

enum PgrCameraSamplerInfo {
  PGR_CS_FEATURE_ID       = 41,
  PGR_CS_API_VERSION      = 1,
  PGR_CS_CONFIG_VERSION   = 1,
  PGR_CS_REQUEST_VERSION  = 1,
  PGR_CS_OUTPUT_VERSION   = 1,
  PGR_CS_MAX_LEASE_MS     = 2500, // residual foreground lease ceiling (JS allowedUntil/deadline gate)
  PGR_CS_WORLD_MAP_LAYER  = 13    // the sole accepted LayerMask.NameToLayer("WorldMap") result
};

// sample() return == out.status: 1 SAMPLE (reading published) / 0 UNAVAILABLE (benign, output clean save
// for the reason + time_ms, plus mask/layer on map-hidden) / negative FAULT (output left clean: status +
// reason only). install() returns 0 on success; negatives are faults.
enum PgrCameraSamplerStatus {
  PGR_CS_SAMPLE            = 1,
  PGR_CS_UNAVAILABLE       = 0,
  PGR_CS_ERR_NULL_ARG      = -1,
  PGR_CS_ERR_BAD_VERSION   = -2,
  PGR_CS_ERR_BAD_SIZE      = -3,
  PGR_CS_ERR_MISSING_PTR   = -4,  // install(): a required config pointer was null
  PGR_CS_ERR_BAD_CONFIG    = -5,  // install(): expected_layer != 13 or reserved0 != 0
  PGR_CS_ERR_NOT_INSTALLED = -6,
  PGR_CS_ERR_RECONFIG      = -9,  // install(): a different config at an already-installed table
  PGR_CS_ERR_CLOCK         = -10,
  PGR_CS_ERR_BUSY          = -11, // reentrant managed call refused (defensive)
  PGR_CS_ERR_GCHANDLE      = -12, // a temporary root could not be allocated / an expected value was null
  PGR_CS_ERR_MANAGED       = -13, // a managed getter threw
  PGR_CS_ERR_METADATA      = -14, // a resolved object had the wrong class
  PGR_CS_ERR_LAYER         = -15, // LayerMask.NameToLayer("WorldMap") != 13
  PGR_CS_ERR_VECTOR        = -16  // forward vector non-finite or vertical (degenerate yaw)
};

// out.reason classifies both the 0 refusals and the negative faults. Mirrors the JS cameraUnavailable /
// cameraProbeReason strings; the adapter maps these back to the legacy event reason text.
enum PgrCameraSamplerReason {
  PGR_CS_REASON_NONE          = 0,
  PGR_CS_REASON_OK            = 1,  // status 1 (success)
  PGR_CS_REASON_OFF           = 2,  // residual lease 0 at entry (sampling window closed)
  PGR_CS_REASON_LEASE         = 3,  // lease expired at the fresh pre-publication gate
  PGR_CS_REASON_THREAD        = 4,  // owner-tid mismatch or Unity thread not attached
  PGR_CS_REASON_NON_MAP_RIG   = 5,  // the getter receiver is not the exact MainCameraRig (off the map)
  PGR_CS_REASON_NULL_CAMERA   = 6,  // the returned camera (or get_main) was null
  PGR_CS_REASON_CAMERA_MISMATCH = 7, // get_main did not match the returned camera
  PGR_CS_REASON_NULL_TRANSFORM = 8, // Component.get_transform returned null
  PGR_CS_REASON_MAP_HIDDEN    = 9,  // the WorldMap layer bit is clear in the culling mask
  // fault reasons (negative status)
  PGR_CS_REASON_CLOCK         = 10,
  PGR_CS_REASON_GCHANDLE      = 11,
  PGR_CS_REASON_MANAGED       = 12,
  PGR_CS_REASON_METADATA      = 13,
  PGR_CS_REASON_LAYER         = 14,
  PGR_CS_REASON_NONFINITE     = 15,
  PGR_CS_REASON_VERTICAL      = 16,
  PGR_CS_REASON_BUSY          = 17
};

// Fixed LP64 config (216 bytes): 8-byte header + 25 validated pointers in the EXACT order below + a trailing
// expected_layer(13)/reserved0(0) pair. Every pointer is an already-validated il2cpp export / Il2CppClass* /
// MethodInfo* / Il2CppFieldInfo* packed by the host; the native layer re-checks the header and rejects any
// null. No hook address. Reordering is forbidden.
struct PgrCameraSamplerConfig {
  uint32_t version;  // == PGR_CS_CONFIG_VERSION
  uint32_t size;     // == sizeof(PgrCameraSamplerConfig)

  // il2cpp runtime exports (slots 0-9).
  void *runtime_invoke;           // 0  void*  (MethodInfo*, void* obj, void** params, void** exc)
  void *object_get_class;         // 1  void*  (void* obj)
  void *class_is_assignable_from; // 2  bool   (Il2CppClass* expected, Il2CppClass* actual)
  void *field_get_value;          // 3  void   (void* obj, Il2CppFieldInfo*, void* out)
  void *object_unbox;             // 4  void*  (void* boxed) -> value pointer
  void *gchandle_new;             // 5  uintptr_t (void* obj, int pinned)
  void *gchandle_get_target;      // 6  void*  (uintptr_t handle)
  void *gchandle_free;            // 7  void   (uintptr_t handle)
  void *thread_current;           // 8  void*  (void) -> attached Unity thread or null
  void *string_new;               // 9  void*  (const char*) -> managed System.String

  // Exact Il2CppClass* identities (slots 10-15).
  void *class_camera;    // 10 classes[CAMERA]   (UnityEngine.Camera; exact)
  void *class_transform; // 11 classes[TRANSFORM](UnityEngine.Transform; assignable-from base)
  void *class_vector;    // 12 vector            (UnityEngine.Vector3; exact value type)
  void *class_rig;       // 13 rig               (Niantic.Holoholo.CameraRig.MainCameraRig; exact)
  void *class_int;       // 14 integer           (System.Int32; exact boxed value type)
  void *class_string;    // 15 string            (System.String; exact)

  // MethodInfo* (slots 16-21), invoked via runtime_invoke.
  void *m_main;        // 16 Camera.get_main           (static  UnityEngine.Camera())
  void *m_transform;   // 17 Component.get_transform   (instance UnityEngine.Transform())
  void *m_forward;     // 18 Transform.get_forward     (instance UnityEngine.Vector3())
  void *m_nametolayer; // 19 LayerMask.NameToLayer     (static  System.Int32(System.String))
  void *m_culling;     // 20 Camera.get_cullingMask    (instance System.Int32())
  void *m_instanceid;  // 21 Object.GetInstanceID      (instance System.Int32())

  // Validated Il2CppFieldInfo* for Vector3.x/y/z (slots 22-24), read via field_get_value.
  void *f_x; // 22
  void *f_y; // 23
  void *f_z; // 24

  int32_t expected_layer; // off 208 == PGR_CS_WORLD_MAP_LAYER (13)
  int32_t reserved0;      // off 212 == 0 in v1
};

static_assert(sizeof(PgrCameraSamplerConfig) == 216,
              "PgrCameraSamplerConfig must remain 216 bytes on LP64");

// Fixed LP64 request (40 bytes). tid is the proven owner tid (nonzero and == the current OS tid). remaining_ms
// is the residual lease (clamped to PGR_CS_MAX_LEASE_MS; 0 is the benign off refusal). rig and camera are RAW
// managed pointers borrowed from the getter observer for THIS CALL ONLY (never GC handles): the native layer
// class-proves rig before any allocation, roots camera first, and never retains either raw pointer.
struct PgrCameraSamplerRequest {
  uint32_t version;      // off 0  == PGR_CS_REQUEST_VERSION
  uint32_t size;         // off 4  == sizeof(PgrCameraSamplerRequest)
  uint64_t tid;          // off 8  proven owner tid
  uint64_t remaining_ms; // off 16 residual lease; clamped, 0 refuses (off)
  void *rig;             // off 24 raw MainCameraRig receiver (class proof only)
  void *camera;          // off 32 raw UnityEngine.Camera the getter returned (rooted first)
};

static_assert(sizeof(PgrCameraSamplerRequest) == 40,
              "PgrCameraSamplerRequest must remain 40 bytes on LP64");

// Fixed LP64 output (72 bytes). Always fully zeroed on entry (header re-stamped). status mirrors the return.
// On 1 every field is valid. On 0 only reason + time_ms are set, plus mask/layer when reason == map-hidden.
// On a negative fault only status + reason are set (no garbage reading).
struct PgrCameraSamplerOutput {
  uint32_t version;   // off 0  == PGR_CS_OUTPUT_VERSION
  uint32_t size;      // off 4  == sizeof(PgrCameraSamplerOutput)
  int32_t status;     // off 8  == the sample() return
  uint32_t reason;    // off 12 PgrCameraSamplerReason
  int32_t camera_id;  // off 16 Object.GetInstanceID (status 1)
  uint32_t mask;      // off 20 Camera.cullingMask bits (status 1 or map-hidden)
  int32_t layer;      // off 24 verified WorldMap layer index (status 1 or map-hidden)
  int32_t reserved0;  // off 28 0 in v1
  double x;           // off 32 forward.x (status 1)
  double y;           // off 40 forward.y (status 1)
  double z;           // off 48 forward.z (status 1)
  double yaw;         // off 56 atan2(x,z) degrees normalised to [0,360) (status 1)
  double time_ms;     // off 64 CLOCK_BOOTTIME ms at the sample (status 0 or 1)
};

static_assert(sizeof(PgrCameraSamplerOutput) == 72,
              "PgrCameraSamplerOutput must remain 72 bytes on LP64");

// Fixed LP64 table: 8-byte header + 2 function pointers = 24 bytes.
struct PgrCameraSamplerApi {
  uint32_t version;  // == PGR_CS_API_VERSION
  uint32_t size;     // == sizeof(PgrCameraSamplerApi)
  // Verify + publish the immutable config once for the process lifetime. Idempotent for an identical config;
  // a different config returns PGR_CS_ERR_RECONFIG. Any null required pointer, a bad header or a bad
  // expected_layer/reserved0 is rejected (fail closed). No hook is installed.
  int (*install)(const PgrCameraSamplerConfig *config);
  // Owner-thread continuous sample. See the file header for the full contract. 1 / 0 / negative.
  int (*sample)(const PgrCameraSamplerRequest *request, PgrCameraSamplerOutput *out);
};

static_assert(sizeof(PgrCameraSamplerApi) == 24,
              "PgrCameraSamplerApi must remain a 24-byte version-1 table");

// Hidden getter. NOT exported by the version script; the coordinator registers it in query_feature under
// feature id 41 and answers only on an exact version match.
const void *pgrazor_camera_sampler_api(uint32_t version);

#ifdef __cplusplus
}
#endif
