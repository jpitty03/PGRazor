#pragma once
#include <stdint.h>

// Feature 13. Native in-game MAP TAP coordinate capture: mirrors the mapTap block
// of diagnostics/quick-ball.js (lines ~258 metadata, ~713-737 runtime). It observes
// the game's own tap handling and emits a {mode, epoch, latitude, longitude} event
// for a verified ground tap. It performs NO gameplay location mutation: Java keeps
// sole responsibility for walk / teleport / route-confirmation / double-tap behaviour.
// This feature only hands Java the coordinate Unity resolved for the tap the user made.
//
// Two Dobby hooks replace the two legacy Gum observers (never both engines at once):
//   * Ground.OnTap              void(self, System.Object sender, System.EventArgs, MethodInfo*)
//   * MapView.GetGroundPosition bool(self, Vector2 byValue, Vector3* out, MethodInfo*)
// Vector2 (float2, 8 bytes) and Vector3 (float3, 12 bytes) are declared as by-value
// homogeneous float aggregates so the C++ ABI marshals them exactly as the game does
// on BOTH the ARM64 target and the x86_64 host test ABI. Never hand-split the value.
// Each original is chained exactly once with the same arguments and its real outcome.
// A coordinate is only captured inside a matching, verified Unity Ground.OnTap scope.
//
// MapView.GetWorldLocation(Vector3) -> LatLng is invoked (never hooked) via runtime
// _invoke; its boxed LatLng (16 bytes, exact class) is unboxed and its Latitude /
// Longitude doubles are read and range-validated. All il2cpp runtime pointers, the
// method and the Ground/MapView/LatLng classes are supplied by the validated adapter.
//
// Loader contract (matches the other native features):
//   root.PGRazorNativeFeature(13, 1, 48) -> query_feature(13,1) -> pgrazor_map_tap_api(1).
// A null return means the pilot/optional metadata is absent (the feature stays dormant,
// never a second Gum-hook fallback). Any install/update error after selection fails
// closed and is surfaced to the caller; it is not retried against Gum.
//
// Ownership / concurrency (enforced in map_tap.cpp):
//   * LP64 only; GC handles are pointer-sized.
//   * The permanent cleanup owner is latched once from a genuine on-thread observation
//     whose tid matches the policy's proven_tid, and is never reassigned by a policy
//     update from another thread (a wrong-thread update can only disable).
//   * A captured coordinate is fully materialised (plain doubles) before it is queued,
//     so drain()/sample() never touch managed memory and never free a GC handle. The
//     map GC root lives only for the single owner-thread hook frame that creates it.
//   * The queue is a bounded POD ring; drain() pops ONE event at a time. Two quick taps
//     are both retained (Java's double-tap teleport needs both) -- they are NEVER
//     collapsed into one latest sample. On overflow the layer fails closed (latches a
//     fault, flushes the queue) rather than quietly misrouting a tap.

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "MapTap native ABI requires LP64");

enum PgrMapTapFeatureInfo {
  PGR_MAP_TAP_FEATURE_ID = 13,
  PGR_MAP_TAP_API_VERSION = 1,
  PGR_MAP_TAP_CONFIG_VERSION = 1,
  PGR_MAP_TAP_SAMPLE_VERSION = 1,
  PGR_MAP_TAP_EVENT_VERSION = 1,
  PGR_MAP_TAP_QUEUE_CAPACITY = 16  // bounded; overflow fails closed, never overwrites
};

// install()/update()/drain()/sample()/disable() status codes.
enum PgrMapTapStatus {
  PGR_MAP_TAP_OK = 0,
  PGR_MAP_TAP_EMPTY = 1,  // drain(): no event was available (not an error)
  PGR_MAP_TAP_ERR_INVALID_ARG = -1,
  PGR_MAP_TAP_ERR_BAD_VERSION = -2,
  PGR_MAP_TAP_ERR_BAD_SIZE = -3,
  PGR_MAP_TAP_ERR_MISSING_PTR = -4,
  PGR_MAP_TAP_ERR_DUPLICATE_TARGET = -5,
  PGR_MAP_TAP_ERR_NOT_INSTALLED = -6,
  PGR_MAP_TAP_ERR_HOOK_TAP = -7,
  PGR_MAP_TAP_ERR_HOOK_POSITION = -8,
  PGR_MAP_TAP_ERR_RECONFIG_ADDRESS_MISMATCH = -9,
  PGR_MAP_TAP_ERR_CLOCK_FAILURE = -10,
  PGR_MAP_TAP_ERR_BAD_MODE = -11
};

// Pull-only runtime error codes surfaced through PgrMapTapSample.last_error_code.
enum PgrMapTapRuntimeError {
  PGR_MAP_TAP_RUNTIME_NONE = 0,
  PGR_MAP_TAP_RUNTIME_POSITION_NONFINITE = 1,
  PGR_MAP_TAP_RUNTIME_LOCATION_NONFINITE = 2,
  PGR_MAP_TAP_RUNTIME_LOCATION_BOUNDS = 3,
  PGR_MAP_TAP_RUNTIME_BOXED_CLASS_MISMATCH = 4,
  PGR_MAP_TAP_RUNTIME_BOXED_NULL = 5,
  PGR_MAP_TAP_RUNTIME_INVOKE_EXCEPTION = 6,
  PGR_MAP_TAP_RUNTIME_GCHANDLE_MAP_FAILED = 7,
  PGR_MAP_TAP_RUNTIME_GCHANDLE_BOXED_FAILED = 8,
  PGR_MAP_TAP_RUNTIME_CLOCK_BOOTTIME_FAILED = 9,
  PGR_MAP_TAP_RUNTIME_REENTRANT_OVERFLOW = 10,
  PGR_MAP_TAP_RUNTIME_QUEUE_OVERFLOW = 11
};

// Fixed LP64 layout: 8-byte header + 12 pointers = 104 bytes.
struct PgrMapTapConfig {
  uint32_t version;
  uint32_t size;

  // IL2CPP runtime exports (pointer-sized GC handles on LP64).
  void *gchandle_new;        // void* (*)(void* obj, int pinned)
  void *gchandle_get_target; // void* (*)(void* handle)
  void *gchandle_free;       // void  (*)(void* handle)
  void *object_get_class;    // void* (*)(void* obj)
  void *runtime_invoke;      // void* (*)(const void* method, void* obj, void** params, void** exc)
  void *object_unbox;        // void* (*)(void* obj)

  // Dobby hook targets (ONLY these two are hooked; location is invoked).
  void *tap_address;         // Ground.OnTap            void(self, object, EventArgs, MethodInfo*)
  void *position_address;    // MapView.GetGroundPosition bool(self, Vector2, Vector3* out, MethodInfo*)

  // MethodInfo* for runtime_invoke (never hooked).
  void *location_method;     // MapView.GetWorldLocation(Vector3) -> LatLng

  // Exact Il2CppClass* identities.
  void *ground_class;        // Ground   (OnTap self must be exactly this class)
  void *map_class;           // MapView  (GetGroundPosition self must be exactly this class)
  void *latlng_class;        // LatLng   (boxed GetWorldLocation return must be exactly this class)
};

static_assert(sizeof(PgrMapTapConfig) == 104,
              "PgrMapTapConfig must remain 104 bytes on LP64");

// Fixed LP64 layout: 48 bytes. One captured tap coordinate. Plain POD: a queued event
// carries no managed reference, so drain() copies it out without any GC interaction.
struct PgrMapTapEvent {
  int32_t mode;       // 1..3, the policy mode active when the tap was captured
  uint32_t reserved;  // zero
  uint64_t epoch;     // full policy epoch; never truncate JS safe integers
  double latitude;    // finite, |latitude|  <= 90
  double longitude;   // finite, |longitude| <= 180
  uint64_t tid;       // owner tid that captured the tap
  uint64_t boottime_ms; // capture time, preserving double-tap timing across queued delivery
};

static_assert(sizeof(PgrMapTapEvent) == 48,
              "PgrMapTapEvent must remain 48 bytes on LP64");

// Fixed LP64 layout: 64 bytes. Pull-only telemetry; the native layer never calls JS.
struct PgrMapTapSample {
  uint32_t version;
  uint32_t size;
  uint32_t owned_root_count;  // GC roots in flight right now (finish guard)
  uint32_t queued_count;      // events currently waiting in the queue
  uint32_t captured_serial;   // total events enqueued
  uint32_t dropped_serial;    // captures abandoned (stale policy / invalid data)
  uint32_t error_serial;      // runtime errors observed
  uint32_t overflow_serial;   // fail-closed queue overflows
  uint32_t last_error_code;   // PgrMapTapRuntimeError
  uint32_t fault;             // 1 once latched (feature reports map-tap-unavailable)
  uint64_t last_tid;          // owner tid of the last event / error
  uint64_t active_generation; // non-zero while a capture scope is live
  uint64_t reserved;
};

static_assert(sizeof(PgrMapTapSample) == 64,
              "PgrMapTapSample must remain 64 bytes on LP64");

// Fixed LP64 layout: 8-byte header + 5 function pointers = 48 bytes.
struct PgrMapTapApi {
  uint32_t version;
  uint32_t size;
  int (*install)(const PgrMapTapConfig *config);
  int (*update)(int enabled,
                uint64_t remaining_lease_ms,
                uint64_t proven_tid,
                uint64_t epoch,
                int mode);
  int (*drain)(PgrMapTapEvent *out_event);  // pops ONE event; PGR_MAP_TAP_EMPTY when none
  int (*sample)(PgrMapTapSample *out_sample);
  int (*disable)(void);
};

static_assert(sizeof(PgrMapTapApi) == 48,
              "PgrMapTapApi must remain a 48-byte version-1 table");

// Standard feature getter. Returns a pointer to the static const 48-byte table when
// `version` == 1, otherwise NULL. Registered by the dispatcher as case 13.
const void *pgrazor_map_tap_api(uint32_t version);

#ifdef __cplusplus
}
#endif
