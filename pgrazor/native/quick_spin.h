#pragma once
#include <stdint.h>

// Feature 11. Native quick-spin: mirrors the spinTick request/dismiss flow of
// quick-close.js (the Pokestop auto-spin + success auto-close), hooking the spin
// state machine's EnterState/ExitState, the result method, and Spinner.Spin.
//
// This is a distinct feature from the feature-10 spin CAMERA spline accelerator;
// the two never share a hook site.
//
// Loader contract (matches the other native features):
//   root.PGRazorNativeFeature(11, 1, 48) -> query_feature(11,1)
//                                        -> pgrazor_quick_spin_api(1).
// Null return means the pilot/optional metadata is absent (feature stays dormant,
// never a second Gum-hook fallback). Any install/update error after selection
// fails closed and is surfaced to the caller; it is not retried against Gum.
//
// Ownership rules baked into the ABI:
//   * LP64 only; GC handles are pointer-sized.
//   * Only the single bound Unity owner thread ever creates OR frees GC handles.
//     update()/clear()/sample() may run on an arbitrary thread; they only mark
//     cancellation and never touch managed state. The owner thread drains the
//     retired-handle list when no managed call is in flight.
//   * The cleanup owner is latched once from a genuine on-thread observation
//     (hook or tick) and is never reassigned by a policy update.

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "QuickSpin native ABI requires LP64");

enum PgrQuickSpinFeatureInfo {
  PGR_QUICK_SPIN_FEATURE_ID = 11,
  PGR_QUICK_SPIN_API_VERSION = 1,
  PGR_QUICK_SPIN_CONFIG_VERSION = 1,
  PGR_QUICK_SPIN_SAMPLE_VERSION = 1
};

// install()/update()/tick()/clear() status codes. 0 == success.
enum PgrQuickSpinStatus {
  PGR_QUICK_SPIN_OK = 0,
  PGR_QUICK_SPIN_ERR_INVALID_ARG = -1,
  PGR_QUICK_SPIN_ERR_BAD_VERSION = -2,
  PGR_QUICK_SPIN_ERR_BAD_SIZE = -3,
  PGR_QUICK_SPIN_ERR_MISSING_PTR = -4,
  PGR_QUICK_SPIN_ERR_DUPLICATE_TARGET = -5,
  PGR_QUICK_SPIN_ERR_NOT_INSTALLED = -6,
  PGR_QUICK_SPIN_ERR_HOOK_ENTER = -7,
  PGR_QUICK_SPIN_ERR_HOOK_EXIT = -8,
  PGR_QUICK_SPIN_ERR_HOOK_RESULT = -9,
  PGR_QUICK_SPIN_ERR_HOOK_SPIN = -10,
  PGR_QUICK_SPIN_ERR_RECONFIG_ADDRESS_MISMATCH = -11,
  PGR_QUICK_SPIN_ERR_CLOCK_FAILURE = -12
};

// Pull-only runtime error codes surfaced through PgrQuickSpinSample.last_error_code.
enum PgrQuickSpinRuntimeError {
  PGR_QUICK_SPIN_RUNTIME_NONE = 0,
  PGR_QUICK_SPIN_RUNTIME_RESULT_NON_SUCCESS = 1,
  PGR_QUICK_SPIN_RUNTIME_GCHANDLE_STATE_FAILED = 2,
  PGR_QUICK_SPIN_RUNTIME_GCHANDLE_GUI_FAILED = 3,
  PGR_QUICK_SPIN_RUNTIME_GCHANDLE_TEMP_FAILED = 4,
  PGR_QUICK_SPIN_RUNTIME_BOOL_TYPE_MISMATCH = 5,
  PGR_QUICK_SPIN_RUNTIME_ALIVE_EXCEPTION = 6,
  PGR_QUICK_SPIN_RUNTIME_ACTIVE_EXCEPTION = 7,
  PGR_QUICK_SPIN_RUNTIME_CAN_EXCEPTION = 8,
  PGR_QUICK_SPIN_RUNTIME_SPIN_EXCEPTION = 9,
  PGR_QUICK_SPIN_RUNTIME_REQUEST_EXCEPTION = 10,
  PGR_QUICK_SPIN_RUNTIME_CLOSE_EXCEPTION = 11,
  PGR_QUICK_SPIN_RUNTIME_CLOCK_BOOTTIME_FAILED = 12,
  PGR_QUICK_SPIN_RUNTIME_REENTRANT_OVERFLOW = 13
};

// Fixed LP64 layout: 16-byte header + 28 pointers = 240 bytes.
struct PgrQuickSpinConfig {
  uint32_t version;
  uint32_t size;
  int32_t success_value;     // SpinResult.Result literal value for "Success"
  int32_t state_open_value;  // STATE_OPEN gui-state value

  // IL2CPP runtime exports (pointer-sized GC handles on LP64).
  void *gchandle_new;        // void* (*)(void* obj, bool pinned)
  void *gchandle_get_target; // void* (*)(void* handle)
  void *gchandle_free;       // void  (*)(void* handle)
  void *object_get_class;    // void* (*)(void* obj)
  void *field_get_value;     // void  (*)(void* obj, void* field, void* out)
  void *runtime_invoke;      // void* (*)(const void* method, void* obj, void** params, void** exc)
  void *object_unbox;        // void* (*)(void* obj)

  // Dobby hook targets (ONLY enter/exit/result/spin are hooked; the rest are invoked).
  void *enter_address;       // SpinState.EnterState  void(self, MethodInfo*)
  void *exit_address;        // SpinState.ExitState   void(self, MethodInfo*)
  void *result_address;      // SpinState.<result>    void(self, SpinResult, MethodInfo*)
  void *spin_address;        // Spinner.Spin          void(self, float, MethodInfo*)

  // MethodInfo* targets for runtime_invoke (never hooked).
  void *spin_method;         // Spinner.Spin(float)
  void *request_method;      // Spinner.<request>()
  void *can_method;          // Spinner.get_CanSpin() -> bool
  void *active_method;       // Gesture.get_IsActive() -> bool
  void *close_method;        // SpinGui.ClickExit()
  void *alive_method;        // UnityEngine.Object.op_Implicit(Object) -> bool (static)

  // Exact Il2CppClass* identities.
  void *spin_state_class;
  void *spin_gui_class;
  void *gesture_class;
  void *spinner_class;
  void *spin_result_class;
  void *bool_class;

  // Il2CppFieldInfo* pointers read via field_get_value.
  void *gui_field;           // SpinState -> SpinGui
  void *gesture_field;       // SpinState -> Gesture
  void *spinner_field;       // Gesture   -> Spinner
  void *result_field;        // SpinResult -> Result (enum i32)
  void *current_state_field; // SpinGui.currentState (i32)
};

static_assert(sizeof(PgrQuickSpinConfig) == 240,
              "PgrQuickSpinConfig must remain 240 bytes on LP64");

// Fixed LP64 layout: 64 bytes. Pull-only telemetry; the native layer never calls JS.
struct PgrQuickSpinSample {
  uint32_t version;
  uint32_t size;
  uint32_t owned_root_count;  // GC roots this layer currently owns (finish guard)
  uint32_t request_serial;    // bumped on each spin-request issued
  uint32_t closed_serial;     // bumped on each successful auto-close
  uint32_t error_serial;      // bumped on each runtime error
  uint32_t cooldown_serial;   // bumped for EVERY valid spin result (even when disabled)
  int32_t last_result;        // last SpinResult.Result value seen / error result
  uint32_t last_error_code;   // PgrQuickSpinRuntimeError
  int32_t last_gui_state;     // gui state at last request
  uint64_t last_opened_ms;    // ms the gui was open when it was acted on
  uint64_t last_tid;          // owner tid for the last event
  uint64_t active_generation; // non-zero while a record is live
};

static_assert(sizeof(PgrQuickSpinSample) == 64,
              "PgrQuickSpinSample must remain 64 bytes on LP64");

// Fixed LP64 layout: 8-byte header + 5 function pointers = 48 bytes.
struct PgrQuickSpinApi {
  uint32_t version;
  uint32_t size;
  int (*install)(const PgrQuickSpinConfig *config);
  int (*update)(int enabled,
                uint64_t remaining_lease_ms,
                uint64_t proven_tid,
                uint64_t epoch);
  int (*tick)(uint64_t proven_tid,
              uint64_t tick_number,
              uint64_t opener_depth);
  int (*sample)(PgrQuickSpinSample *out_sample);
  int (*clear)(void);
};

static_assert(sizeof(PgrQuickSpinApi) == 48,
              "PgrQuickSpinApi must remain a 48-byte version-1 table");

// Standard feature getter. Returns a pointer to the static const 48-byte table when
// `version` == 1, otherwise NULL. Registered by the dispatcher as case 11.
const void *pgrazor_quick_spin_api(uint32_t version);

#ifdef __cplusplus
}
#endif
