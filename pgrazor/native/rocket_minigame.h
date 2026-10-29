#pragma once
#include <cstdint>

// Feature 6. Rocket charged-attack particle minigame collector.
//
// Mirrors the legacy Gum path in runtime/features/rocketMinigame.js. The native module
// owns the four lifecycle/update method hooks for the minigame challenge, the GC-root
// ownership of the challenge/transform/gameObject it touches, and the collect/restore of
// the original transform localScale + gameObject activeSelf flag. Pointer-sized GC
// handles and raw il2cpp export addresses match the validated Unity runtime ABI; all
// IL2CPP method invocations go through runtime_invoke with a MethodInfo to avoid Unity
// value-type ABI mistakes.
//
// Loader contract (identical to the other feature modules). Nothing here is exported by
// the version script. The JS adapter resolves the table through the coordinator-owned
// loader root.PGRazorNativeFeature(6, 1, 40); the hidden getter
// pgrazor_rocket_minigame_api() is registered by the coordinator in the core getter
// registry, which answers only on an exact version match. A null table means the pilot
// is absent (JS keeps the Gum fallback); an ABI mismatch is a hard error (JS goes inert,
// never Gum).
//
// Threading / Unity-thread proof. The Unity thread identity is learned ONLY from
// tick(proven_unity_tick != 0): tick is invoked by the host's verified Unity loop, so
// the thread it runs on IS the Unity thread, and only then is its tid latched. The
// method hooks never learn or overwrite that tid; they merely gate rooted work on it.
// All GC rooting, collection and restoration happen only on the latched Unity thread.
// install()/update()/tick()/restore() touch only atomics and the pending flag across
// threads, never the rooted handles; a busy guard serialises the Unity-thread state
// mutation against the SetActive-driven reentrant lifecycle hooks, and the install mutex
// is never held across a runtime_invoke.
//
// The config is defined explicitly here (and written byte-for-byte by the adapter) so
// the JS pilot and C++ feature agree on layout. All fields past the version/size header
// are pointer-sized (LP64). collider_field/running_field are il2cpp FieldInfo* and are
// read with field_get_value exactly as the JS readPtr()/present() helpers do -- they are
// NOT raw byte offsets.
struct PgrRocketMiniConfig {
    uint32_t version, size;
    // Patched method entry points (direct chained intercepts of the types below).
    void *update_addr;    // UpdateMinigame        void(self, int64, MethodInfo*)
    void *exit_addr;      // ExitGame              void(self, MethodInfo*)
    void *hide_addr;      // Hide                  void(self, MethodInfo*)
    void *destroy_addr;   // OnDestroy             void(self, MethodInfo*)
    // MethodInfo pointers invoked through runtime_invoke (never the raw address).
    void *transform_method;   // Component.get_transform    no args -> Transform
    void *gameObject_method;  // Component.get_gameObject   no args -> GameObject
    void *scale_method;       // Transform.get_localScale   no args -> boxed Vector3
    void *active_method;      // GameObject.get_activeSelf  no args -> boxed bool
    void *setScale_method;    // Transform.set_localScale   Vector3 (value-type, size 12)
    void *setActive_method;   // GameObject.SetActive       bool
    void *alive_method;       // UnityEngine.Object.op_Implicit  static bool(object)
    // Validated il2cpp FieldInfo* pointers (NOT offsets); read via field_get_value.
    void *collider_field;     // challenge.tapCollider  (reference, matches readPtr)
    void *running_field;      // base.combatIsRunning   (bool, matches present(...,1))
    // Exact class identities for object_get_class comparisons.
    void *mini_class;         // RMINI challenge class
    void *bool_class;         // System.Boolean boxed class
    // Raw il2cpp runtime exports (pointer-sized GC handle ABI).
    void *(*root)(void *, int);          // il2cpp_gchandle_new(obj, pinned)
    void (*free_root)(void *);           // il2cpp_gchandle_free(handle)
    void *(*target)(void *);             // il2cpp_gchandle_get_target(handle)
    void *(*object_class)(void *);       // il2cpp_object_get_class(obj)
    void *(*object_unbox)(void *);       // il2cpp_object_unbox(boxed)
    void *(*runtime_invoke)(void *method, void *obj, void **params, void **exc);
    void (*field_get_value)(void *obj, void *field, void *out);
};
static_assert(sizeof(PgrRocketMiniConfig) == 184,
              "PgrRocketMiniConfig layout must match the JS adapter writer");

// Status codes (0 == success). Shared spirit with the other feature headers.
enum {
    PGR_RMINI_OK = 0,
    PGR_RMINI_ERR_NULL_ARG = 1,       // config pointer / a required member pointer was null
    PGR_RMINI_ERR_BAD_CONFIG = 2,     // header, size or a pointer member invalid
    PGR_RMINI_ERR_ALREADY_BOUND = 3,  // install() called again with a different config
    PGR_RMINI_ERR_HOOK_FAILED = 4     // a target could not be hooked (install latched failed)
};

// Version-1 table: 8-byte header + install/update/tick/restore (40 bytes total).
//   install : validates the config, refuses a differing repeat config, refuses any
//             install after a partial failure, and wires the four hooks. A reported
//             success with a null trampoline undoes that one site; any partial failure
//             latches a permanent error, keeps the cleanly-patched sites as pass-through
//             originals, and never re-patches on a later call.
//   update  : policy only. allowed + remaining lease (ms, already capped by the
//             adapter); native re-derives a CLOCK_BOOTTIME deadline so a cached allowed
//             bool cannot keep collecting indefinitely. Mutates atomics only.
//   tick    : proven-Unity-tick heartbeat. When proven_unity_tick != 0 it latches the
//             running thread as the Unity thread and processes a pending restore. It is
//             the ONLY authority for the Unity tid. Must be called on the Unity thread.
//   restore : request an immediate stop of collection; restores now if called on the
//             Unity thread, otherwise latches a pending restore for the next on-thread
//             hook/tick. Restoration is preserved even after a latched failure.
struct PgrRocketMiniApi {
    uint32_t version, size;
    int (*install)(const PgrRocketMiniConfig *);
    int (*update)(int allowed, int64_t lease_remaining_ms);
    int (*tick)(int proven_unity_tick);
    int (*restore)();
};
static_assert(sizeof(PgrRocketMiniApi) == 40,
              "PgrRocketMiniApi must stay a 40-byte version-1 table");

#ifdef __cplusplus
extern "C" {
#endif
// Hidden getter (NOT exported by the version script); the coordinator registers it in
// the core getter registry. Returns the process-lifetime table on an exact version
// match, else nullptr.
const void *pgrazor_rocket_minigame_api(uint32_t version);
#ifdef __cplusplus
}
#endif
