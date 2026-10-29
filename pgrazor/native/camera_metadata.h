#pragma once
#include "runtime_bootstrap.h"
#include "runtime_metadata.h"
#include <cstdint>

// Feature 59 / API 1: complete compiled port of diagnostics/camera-probe.js
// discover(module, api, domain, sampleCamera, continuous) and its validation helpers
// (text, typeName, pointerHandleAbi, executable, DEBUG_INFO_WORDS, TARGETS, LIMITS).
// Built on feature 47's reusable metadata primitives (pgrazor_runtime_metadata_api(1))
// and cross-checked against feature 54's bootstrap snapshot (pgrazor_runtime_bootstrap_api(1))
// where active, using the canonical raw PT_LOAD module range consistent with quickball (55)
// and routes (53). No hooks, no Gum/Dobby, no GC handle creation, no runtime_invoke, no JS
// evaluator: pure metadata discovery + read-only code fingerprints.

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "CameraMetadata native ABI requires LP64");

enum PgrCameraMetaInfo {
  PGR_CAM_FEATURE_ID      = 59,
  PGR_CAM_API_VERSION     = 1,
  PGR_CAM_CFG_VERSION     = 1,
  PGR_CAM_EXTRA_VERSION   = 1,
  PGR_CAM_OUT_VERSION     = 1,
  PGR_CAM_REASON_MAX      = 120,
  PGR_CAM_ASM_LIMIT       = 128,        // camera-probe LIMITS.assemblies
  PGR_CAM_SCAN_MS         = 3000,       // camera-probe LIMITS.scanMs
  PGR_CAM_NAME_MAX        = 256,        // camera-probe LIMITS.nameBytes
  PGR_CAM_MAIN_CAMERA_RVA = 0xae09554u  // CameraStack.get_MainCamera pinned RVA
};

// resolve() return codes.
//  0 (PGR_CAM_OK)          : discovery succeeded; output fully populated.
//  1 (PGR_CAM_RETRY)       : retriable readiness status ({retry: out->reason}).
//  2 (PGR_CAM_UNAVAILABLE) : terminal unavailable status ({unavailable: out->reason}).
// <0                       : fatal caller/metadata/environment fault (throws in adapter).
enum PgrCameraMetaStatus {
  PGR_CAM_OK                     = 0,
  PGR_CAM_RETRY                  = 1,
  PGR_CAM_UNAVAILABLE            = 2,
  PGR_CAM_ERR_ATTACH             = -30,  // thread not attached or owner_tid != gettid()
  PGR_CAM_ERR_METADATA           = -31,  // feature 47 runtime_metadata API unavailable
  PGR_CAM_ERR_INTERNAL           = -32,  // internal schema fault
  PGR_CAM_ERR_RVA_OVERFLOW       = -33,  // module_base + rva overflow
  PGR_CAM_ERR_EXTRA_MISSING      = -34,  // extra null/bad-version/bad-size/reserved!=0 or image_get_name null
  PGR_CAM_ERR_NULL_ASSEMBLIES    = -35,  // domain_get_assemblies returned null with non-zero count
  PGR_CAM_ERR_NULL_STRING        = -36,  // null metadata string from image_get_name or method_get_name
  PGR_CAM_ERR_NULL_TYPE          = -37,  // null metadata type from return/field/param type accessor
  PGR_CAM_ERR_BOOTSTRAP          = -38   // active feature 54 bootstrap snapshot mismatched domain/ExecuteTasks
};

// Vector3 preparatory field validation status (camera-probe discover() vectorFields).
enum PgrCameraVectorFields {
  PGR_CAM_VF_NONE                = 0,
  PGR_CAM_VF_EXPORTS_UNAVAILABLE = 1,  // "exports-unavailable"
  PGR_CAM_VF_CLASS_UNAVAILABLE   = 2,  // "class-unavailable"
  PGR_CAM_VF_SIGNATURE_MISMATCH  = 3,  // "signature-mismatch"
  PGR_CAM_VF_VERIFIED            = 4   // "verified"
};

// Optional export presence bitmask (PgrCameraMetaExtra.present).
enum PgrCameraExport {
  PGR_CAM_EXP_CLASS_GET_FIELD_FROM_NAME = 1u << 0,
  PGR_CAM_EXP_FIELD_GET_TYPE            = 1u << 1,
  PGR_CAM_EXP_FIELD_GET_PARENT          = 1u << 2,
  PGR_CAM_EXP_FIELD_GET_FLAGS           = 1u << 3,
  PGR_CAM_EXP_RUNTIME_INVOKE            = 1u << 4,
  PGR_CAM_EXP_OBJECT_GET_CLASS          = 1u << 5,
  PGR_CAM_EXP_OBJECT_UNBOX              = 1u << 6,
  PGR_CAM_EXP_GCHANDLE_NEW              = 1u << 7,
  PGR_CAM_EXP_GCHANDLE_GET_TARGET       = 1u << 8,
  PGR_CAM_EXP_GCHANDLE_FREE             = 1u << 9,
  PGR_CAM_EXP_FIELD_GET_VALUE           = 1u << 10,
  PGR_CAM_EXP_GET_CORLIB                = 1u << 11,
  PGR_CAM_EXP_CLASS_IS_ASSIGNABLE_FROM  = 1u << 12,
  PGR_CAM_EXP_CLASS_IS_VALUETYPE        = 1u << 13,
  PGR_CAM_EXP_CLASS_VALUE_SIZE          = 1u << 14,
  PGR_CAM_EXP_DEBUG_GET_METHOD_INFO     = 1u << 15,
  PGR_CAM_EXP_METHOD_IS_GENERIC         = 1u << 16,
  PGR_CAM_EXP_METHOD_IS_INFLATED        = 1u << 17,
  PGR_CAM_EXP_METHOD_GET_PARAM          = 1u << 18,
  PGR_CAM_EXP_STRING_NEW                = 1u << 19
};

#ifdef __cplusplus
}  // extern "C"

namespace pgr_camera_schema {

enum ClassIndex {
  C_CAMERA = 0,     // UnityEngine.Camera (CoreModule, mandatory)
  C_COMPONENT,      // UnityEngine.Component (CoreModule, mandatory)
  C_TRANSFORM,      // UnityEngine.Transform (CoreModule, mandatory)
  C_SYNCCTX,        // UnityEngine.UnitySynchronizationContext (CoreModule, mandatory)
  C_OBJECT,         // UnityEngine.Object (CoreModule, sampleCamera)
  C_VECTOR3,        // UnityEngine.Vector3 (CoreModule, vector field check)
  C_INT32,          // System.Int32 (corlib, sampleCamera)
  C_RIG,            // Niantic.Holoholo.CameraRig.MainCameraRig (holo-game, continuous)
  C_STACK,          // Niantic.Holoholo.Cameras.CameraStack (holo-game, continuous)
  C_STRING,         // System.String (corlib, continuous)
  C_LAYERMASK,      // UnityEngine.LayerMask (CoreModule, continuous)
  C_CLASS_END
};
inline constexpr uint32_t kClassCount = C_CLASS_END;
static_assert(kClassCount == 11, "camera_metadata resolves 11 class slots");

enum FieldIndex {
  F_X = 0,          // UnityEngine.Vector3.x (non-static System.Single)
  F_Y,              // UnityEngine.Vector3.y (non-static System.Single)
  F_Z,              // UnityEngine.Vector3.z (non-static System.Single)
  F_FIELD_END
};
inline constexpr uint32_t kFieldCount = F_FIELD_END;
static_assert(kFieldCount == 3, "camera_metadata resolves 3 Vector3 fields");

enum MethodIndex {
  M_GET_MAIN = 0,        // UnityEngine.Camera.get_main() -> UnityEngine.Camera (static)
  M_GET_TRANSFORM,       // UnityEngine.Component.get_transform() -> UnityEngine.Transform (instance)
  M_GET_FORWARD,         // UnityEngine.Transform.get_forward() -> UnityEngine.Vector3 (instance)
  M_EXECUTE_TASKS,       // UnityEngine.UnitySynchronizationContext.ExecuteTasks() -> System.Void (static)
  M_GET_INSTANCE_ID,     // UnityEngine.Object.GetInstanceID() -> System.Int32 (instance, sampleCamera)
  M_GET_MAIN_CAMERA,     // Niantic.Holoholo.Cameras.CameraStack.get_MainCamera() -> UnityEngine.Camera (continuous)
  M_GET_CULLING_MASK,    // UnityEngine.Camera.get_cullingMask() -> System.Int32 (continuous)
  M_NAME_TO_LAYER,       // UnityEngine.LayerMask.NameToLayer(System.String) -> System.Int32 (static, continuous)
  M_METHOD_END
};
inline constexpr uint32_t kMethodCount = M_METHOD_END;
static_assert(kMethodCount == 8, "camera_metadata resolves 8 method slots");

struct TargetSpec {
  uint32_t class_idx;
  uint32_t method_idx;
  const char *class_name;
  const char *method_name;
  const char *return_type;
  uint32_t is_static;
  uint32_t sample_only;
};

inline constexpr TargetSpec kTargets[] = {
  {C_CAMERA,    M_GET_MAIN,        "Camera",                      "get_main",       "UnityEngine.Camera",    1, 0},
  {C_COMPONENT, M_GET_TRANSFORM,   "Component",                   "get_transform",  "UnityEngine.Transform", 0, 0},
  {C_TRANSFORM, M_GET_FORWARD,     "Transform",                   "get_forward",    "UnityEngine.Vector3",   0, 0},
  {C_SYNCCTX,   M_EXECUTE_TASKS,   "UnitySynchronizationContext", "ExecuteTasks",   "System.Void",           1, 0},
  {C_OBJECT,    M_GET_INSTANCE_ID, "Object",                      "GetInstanceID",  "System.Int32",          0, 1},
};
inline constexpr uint32_t kTargetCount = sizeof(kTargets) / sizeof(kTargets[0]);
static_assert(kTargetCount == 5, "camera_metadata has 4 base targets + 1 sampleCamera target");

inline constexpr const char *kVectorFieldNames[kFieldCount] = {"x", "y", "z"};

// pointerHandleAbi (camera-probe.js lines 80..111): read-only AArch64 code fingerprint of
// il2cpp_gchandle_new / il2cpp_gchandle_get_target / il2cpp_gchandle_free.
inline constexpr uint32_t kGcCreateWords[] = {0xaa0003e8u, 0x52800049u, 0x7200003fu, 0x1a890520u, 0xaa0803e1u};
inline constexpr uint32_t kGcCreateWordCount = 5;
inline constexpr uint64_t kGcCreateBranchOff = 20;
inline constexpr uint64_t kGcCreateBodyOff   = 0x384;
inline constexpr uint32_t kGcCreateBodyWord  = 0x9a953120u;
inline constexpr uint32_t kGcGetHead         = 0xf81b0ffeu;
inline constexpr uint64_t kGcGetTailOff      = 20;
inline constexpr uint32_t kGcGetTail[]       = {0x9273c817u, 0x394082e8u};
inline constexpr uint32_t kGcFreeHead        = 0xf81b0ffeu;
inline constexpr uint64_t kGcFreeTailOff     = 24;
inline constexpr uint32_t kGcFreeTail[]      = {0x9273c816u, 0x394082c8u};

// 80-byte il2cpp_debug_get_method_info prologue (camera-probe.js DEBUG_INFO_WORDS, lines 26..29).
inline constexpr unsigned char kDebugPrologue[] = {
  0x08,0x10,0x05,0x90, 0x08,0x45,0x47,0xf9, 0x48,0x01,0x00,0xb4, 0x09,0x10,0x05,0x90,
  0x29,0x49,0x47,0xf9, 0xe9,0x00,0x00,0xb4, 0x0a,0x04,0x40,0xf9, 0x1f,0x01,0x0a,0xeb,
  0xe8,0x03,0x1f,0x2a, 0x28,0x01,0x00,0x54, 0x3f,0x01,0x0a,0xeb, 0xe3,0x00,0x00,0x54,
  0xa1,0x00,0x00,0xb4, 0x08,0x04,0x40,0xf9, 0x3f,0x08,0x00,0xb9, 0x3f,0x08,0x00,0xf9,
  0x28,0x00,0x00,0xf9, 0x28,0x00,0x80,0x52, 0xe0,0x03,0x08,0x2a, 0xc0,0x03,0x5f,0xd6};
inline constexpr uint32_t kDebugPrologueLen = sizeof(kDebugPrologue);
static_assert(kDebugPrologueLen == 80, "DEBUG_INFO_WORDS is 20 AArch64 words (80 bytes)");

// CameraStack.get_MainCamera 2-word leaf getter prologue at RVA 0xae09554 (ldr x0,[x0,#24]; ret).
inline constexpr uint32_t kGetMainCameraWords[] = {0xf9400c00u, 0xd65f03c0u};
inline constexpr uint32_t kGetMainCameraByteLen = sizeof(kGetMainCameraWords);
static_assert(kGetMainCameraByteLen == 8, "get_MainCamera leaf getter prologue is 8 bytes");

}  // namespace pgr_camera_schema

extern "C" {
#endif  // __cplusplus

// Extra typed il2cpp primitives beyond feature 47's 31 metadata exports, plus an optional monotonic
// clock test seam for deterministic scan-budget verification.
struct PgrCameraMetaExtra {
  uint32_t version, size;               // 0..7    (1, 112)
  void *image_get_name;                 // 8..15   mandatory: const char* (void* image)
  void *get_corlib;                     // 16..23  optional (sampleCamera): void* (void)
  void *runtime_invoke;                 // 24..31  optional (sampleCamera)
  void *object_get_class;               // 32..39  optional (sampleCamera)
  void *object_unbox;                   // 40..47  optional (sampleCamera)
  const void *gchandle_new;             // 48..55  optional (pointerHandleAbi + sampleCamera)
  const void *gchandle_get_target;      // 56..63  optional (pointerHandleAbi + sampleCamera)
  const void *gchandle_free;            // 64..71  optional (pointerHandleAbi + sampleCamera)
  void *field_get_value;                // 72..79  optional (sampleCamera)
  void *string_new;                     // 80..87  optional (continuous)
  double (*clock_ms)(void *ctx);        // 88..95  optional monotonic ms seam (null -> CLOCK_MONOTONIC)
  void *clock_ctx;                      // 96..103 ctx for clock_ms
  uint32_t present;                     // 104..107 PgrCameraExport bitmask
  uint32_t reserved;                    // 108..111 must be 0
};

struct PgrCameraMetaConfig {
  uint32_t version, size;               // 0..7    (1, 112)
  PgrMetaBindConfig binding;            // 8..71   (64 bytes, feature 47 bind config)
  void *thread_current;                 // 72..79  mandatory (il2cpp_thread_current)
  uint64_t owner_tid;                   // 80..87  gettid() of attached caller thread
  const void *domain;                   // 88..95  optional explicit domain (null -> domain_get())
  const PgrCameraMetaExtra *extra;      // 96..103 mandatory
  uint32_t sample_camera;               // 104..107 0 or 1
  uint32_t continuous;                  // 108..111 0 or 1
};

#ifdef __cplusplus
struct PgrCameraMetaOutput {
  uint32_t version, size;                                // 0..7    (1, 368)
  int32_t status;                                        // 8..11   0=OK, 1=RETRY, 2=UNAVAILABLE
  uint32_t vector_fields;                                // 12..15  PgrCameraVectorFields (1..4 on OK)
  uint32_t target_count;                                 // 16..19  4 or 5 on OK, else 0
  uint32_t reserved;                                     // 20..23  0
  const void *core_image;                                // 24..31  UnityEngine.CoreModule.dll image
  const void *game_image;                                // 32..39  holo-game.dll image (continuous)
  const void *corlib_image;                              // 40..47  get_corlib() image (sampleCamera)
  const void *execute;                                   // 48..55  ExecuteTasks MethodInfo*
  const void *debug_address;                             // 56..63  debug_get_method_info (continuous)
  const void *get_main_camera_address;                   // 64..71  get_MainCamera code (continuous)
  const void *classes[pgr_camera_schema::kClassCount];   // 72..159 (11 * 8 = 88)
  const void *fields[pgr_camera_schema::kFieldCount];    // 160..183 (3 * 8 = 24)
  const void *methods[pgr_camera_schema::kMethodCount];  // 184..247 (8 * 8 = 64)
  char reason[PGR_CAM_REASON_MAX];                       // 248..367 (120 bytes)
};

struct PgrCameraMetaApi {
  uint32_t version, size, config_size, output_size;
  int (*resolve)(const PgrCameraMetaConfig *, PgrCameraMetaOutput *);
};

static_assert(sizeof(PgrCameraMetaExtra) == 112, "LP64 extra block");
static_assert(sizeof(PgrCameraMetaConfig) == 112, "LP64 config");
static_assert(sizeof(PgrCameraMetaOutput) == 368, "LP64 output");
static_assert(sizeof(PgrCameraMetaApi) == 24, "LP64 API");
#endif

const void *pgrazor_camera_metadata_api(uint32_t version);

#ifdef __cplusplus
}
#endif
