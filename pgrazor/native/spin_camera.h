#pragma once
#include <cstdint>

// Feature 10. Quick-spin camera spline acceleration (Approach / Resolve states).
//
// Mirrors the spinCamera portion of quick-close.js without migrating spinTick or
// spin request methods. Hooks Approach/Resolve EnterState & ExitState and the
// compiler-generated spline camera coroutine iterator MoveNext.
//
// Loader contract:
// Resolved via root.PGRazorNativeFeature(10, 1, 40) -> pgrazor_spin_camera_api(1).
// Null return means pilot absent; any install/update error after selection fails
// closed without falling back to Gum hooks.

struct PgrSpinCameraConfig {
    uint32_t version;
    uint32_t size;
    // Raw IL2CPP runtime exports (LP64 pointer-sized GC handle ABI).
    void *(*gchandle_new)(void *obj, int pinned);
    void *(*gchandle_get_target)(void *handle);
    void (*gchandle_free)(void *handle);
    void *(*object_get_class)(void *obj);
    void (*field_get_value)(void *obj, void *field, void *out);
    // Hook target method addresses.
    void *in_enter_addr;      // Approach.EnterState  void(self, MethodInfo*)
    void *in_exit_addr;       // Approach.ExitState   void(self, MethodInfo*)
    void *out_enter_addr;     // Resolve.EnterState   void(self, MethodInfo*)
    void *out_exit_addr;      // Resolve.ExitState    void(self, MethodInfo*)
    void *move_next_addr;     // Spline.<>cso.MoveNext bool(self, MethodInfo*)
    // Exact Il2CppClass* identities.
    void *in_class;           // classes[APPROACH]
    void *out_class;          // classes[RESOLVE]
    void *spline_class;       // classes[SPLINE]
    void *iterator_class;     // c.iterator
    // Validated Il2CppFieldInfo* pointers (read via field_get_value).
    void *in_camera_field;    // Approach.splineCam (0x78, SPLINE)
    void *out_camera_field;   // Resolve.splineCam  (0x58, SPLINE)
    void *owner_field;        // iterator.<>4__this (0x28, SPLINE)
    void *state_field;        // iterator.djjp      (0x10, System.Int32)
};
static_assert(sizeof(PgrSpinCameraConfig) == 152,
              "PgrSpinCameraConfig layout must match the JS adapter writer");

struct PgrSpinCameraSample {
    uint32_t version;
    uint32_t size;
    uint32_t fast_serial;
    uint32_t error_serial;
    uint32_t last_phase;        // 0 = none, 1 = 'in', 2 = 'out'
    uint32_t last_error;        // PgrSpinCameraError code
    uint32_t active_roots_mask; // bit 0 = 'in' rooted, bit 1 = 'out' rooted
    uint32_t fault;             // 1 if hook/runtime fault latched
    float last_original_speed;
    float last_applied_speed;
    uint64_t fast_count;
    uint64_t enter_count;
    uint64_t exit_count;
    uint64_t prune_count;
};
static_assert(sizeof(PgrSpinCameraSample) == 72,
              "PgrSpinCameraSample layout must match the JS adapter reader");

enum PgrSpinCameraStatus {
    PGR_SCAM_OK = 0,
    PGR_SCAM_ERR_NULL_ARG = 1,
    PGR_SCAM_ERR_BAD_CONFIG = 2,
    PGR_SCAM_ERR_ALREADY_BOUND = 3,
    PGR_SCAM_ERR_HOOK_FAILED = 4,
    PGR_SCAM_ERR_CLOCK_FAILED = 5
};

enum PgrSpinCameraRuntimeError {
    PGR_SCAM_RTERR_NONE = 0,
    PGR_SCAM_RTERR_GCHANDLE_ENTER = 1,
    PGR_SCAM_RTERR_GCHANDLE_ITER = 2,
    PGR_SCAM_RTERR_CLOCK = 3,
    PGR_SCAM_RTERR_REENTRANT_OVERFLOW = 4
};

struct PgrSpinCameraApi {
    uint32_t version;
    uint32_t size;
    int (*install)(const PgrSpinCameraConfig *config);
    int (*update)(int allowed, uint64_t remaining_lease_ms, uint64_t proven_tid, uint64_t epoch);
    int (*tick)(uint64_t proven_tid);
    int (*sample)(PgrSpinCameraSample *out);
};
static_assert(sizeof(PgrSpinCameraApi) == 40,
              "PgrSpinCameraApi must stay a 40-byte version-1 table");

#ifdef __cplusplus
extern "C" {
#endif
const void *pgrazor_spin_camera_api(uint32_t version);
#ifdef __cplusplus
}
#endif
