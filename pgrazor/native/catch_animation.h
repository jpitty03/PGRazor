#pragma once
#include <cstdint>

// PGRazor native migration: catch animation speed ownership (feature id 8).
//
// Historically ported the per-instance Animator-speed block of the legacy
// diagnostics/quick-ball.js adapter: the btxu leave path that, after a confirmed
// catch, snapshots the Animator's original speed, writes the 20x BALL_SPEED and
// restores it on the verified Unity tick (completion / off / lease loss / 8 s
// timeout), proving the target survived and no external code changed the speed
// first. That ordinary-ball ("no-Max") behaviour is the V1 table and is preserved
// byte-for-byte.
//
// V2 adds the Dynamax / Max-ball extension: apply_max(). The Max catch coroutine
// writes its own game speed into the Animator mid-flight and explicitly resets the
// Animator back to 1.0 at the end of its sequence. The V1 "snapshot once, restore
// once" model cannot follow that, so a Max attempt additionally polls the live
// speed on the proven tick and performs exactly one later 20x reset when the game
// drops the Animator to its Max game speed, while never fighting the game's own
// end-of-coroutine reset or any other unrecognised speed.
//
// Division of labour with the host adapter (the coordinator owns the Gum observers
// and the Max field-read; this module owns the native speed state machine):
//   * The host keeps the btxu/complete observers, performs the
//     ball/response/caller/rate/thread/bread/animator-class checks, reads the Max
//     game-speed baseline off the response, and on a confirmed success CALLS apply()
//     (ordinary) or apply_max() (Max) here; the completion observers CALL complete().
//     No Dobby hook is installed by this module.
//   * This module OWNS the pointer-sized GC roots for the animator + response, the
//     original-speed snapshot, the 20x write + readback, restoration (completion /
//     off / lease / timeout), the destroyed / external-speed-change behaviour, the
//     Max active-tick re-apply, the 1 s retry after a restore failure and the latched
//     fault. It never calls back into the host; the adapter pulls telemetry through
//     sample().
//
// --- speed ownership rules -------------------------------------------------------
//   * Single attempt at a time; a confirmed success while one is live is ignored
//     (no re-apply). Ownership (the attempt + roots) is recorded BEFORE the setter,
//     because even a throwing set_speed may have mutated native state and must be
//     inspected/restored.
//   * The snapshot original speed must be finite and ~1.0 for an ordinary ball. A
//     Max ball additionally accepts a snapshot ~ the caller's expected Max game
//     speed; any other baseline is skipped.
//   * Restore only overwrites when the current speed is still ~20x; an external
//     change releases the attempt WITHOUT overwriting it.
//   * A restore failure latches a fault: the policy window is cleared and stopping
//     is set, but the attempt is preserved and retried every 1 s until it succeeds.
//   * Root failures free any partial root and latch the fault; invalid config never
//     writes. All managed calls use runtime_invoke with the exception out-param and
//     exact boxed System.Single / System.Boolean unboxing; no mutex is held across a
//     managed invoke and a reentry guard blocks re-entry from within one.
//
// --- Max-ball (V2) extra rules ---------------------------------------------------
//   * expected_max_speed must be finite, > 0, <= 2 and never ~20x, else apply_max
//     returns ERR_BAD_ARG and touches nothing.
//   * Only while the policy window is fully open (not stopping / off / past the lease
//     or the 8 s timeout -- normal cleanup always takes priority) the proven tick
//     polls a Max attempt's live speed: a destroyed target releases immediately; a
//     speed still ~20x holds; a speed ~ expected_max_speed (with expected not ~1.0)
//     triggers exactly ONE later reset -- snapshot the current game speed as the new
//     original, write 20x and read it back once (the re-apply flag is set BEFORE the
//     setter) -- surfaced as a speed-applied event with the max-game-speed-reset
//     reason; any other speed (including the game's end-of-coroutine 1.0 reset) is
//     treated as an external change and releases the attempt WITHOUT overwriting.
//   * Any read/write failure in that poll retains ownership and latches the fault
//     exactly like the restore path.

// Config v1: 8-byte header then 12 pointers in table order (104 bytes). The il2cpp
// primitives are real module exports; the three MethodInfo* and three il2cpp classes
// were resolved and validated by the caller. No ball metadata is needed here.
struct PgrSpeedConfig {          // 104 bytes
    uint32_t version, size;      // 0   { version==1, size==sizeof }
    void *runtime_invoke;        // 8   void* (*)(void* method, void* obj, void** params, void** exc)
    void *gchandle_new;          // 16  void* (*)(void* obj, int pinned)
    void *gchandle_get_target;   // 24  void* (*)(void* handle)
    void *gchandle_free;         // 32  void  (*)(void* handle)
    void *object_get_class;      // 40  void* (*)(void* obj)
    void *object_unbox;          // 48  void* (*)(void* boxed)
    void *method_get_speed;      // 56  MethodInfo* Animator.get_speed  -> System.Single
    void *method_set_speed;      // 64  MethodInfo* Animator.set_speed(System.Single)
    void *method_alive;          // 72  MethodInfo* Object.op_Implicit(Object) -> System.Boolean (static)
    void *class_single;          // 80  exact class of a boxed System.Single
    void *class_bool;            // 88  exact class of a boxed System.Boolean
    void *class_animator;        // 96  exact class a speed target must be
};

// Pull-only telemetry. The adapter turns serial transitions into the existing
// speed-* event names; no native->JS callback is ever made.
struct PgrSpeedSample {          // 32 bytes
    uint32_t owned;              // 0   1 while an attempt holds the animator speed
    uint32_t fault;             // 4   latched restore/apply/root fault
    uint32_t action;            // 8   last event code (see catch_animation.cpp)
    uint32_t serial;            // 12  increments once per emitted event
    uint32_t reason;            // 16  last reason code
    uint32_t pad;               // 20  bit0 == last event was for a Max ball; rest reserved
    float    original;          // 24  snapshot original speed for the last event
    float    readback;          // 28  speed read back after the last write
};

struct PgrSpeedApi {             // 56 bytes: ordered, read-only table (V1, ordinary ball)
    uint32_t version, size;      // 0   { version==1, size==sizeof }
    int (*install)(const PgrSpeedConfig *config);
    int (*update)(int allowed, uint64_t lease_ms, uint64_t unity_tid);
    int (*apply)(void *animator, void *response);
    int (*complete)(void *response, int32_t reason);
    int (*tick)(uint64_t proven_tid);
    int (*sample)(PgrSpeedSample *out);
};

// V2 appends the Dynamax / Max-ball entry point. The V1 table above is unchanged;
// install/update/complete/tick/sample stay on V1 and are shared by both ball kinds.
// apply_max() validates expected_max_speed (finite, > 0, <= 2, never ~20x) then runs
// the shared apply engine with the Max baseline + active-tick reset enabled. Like
// apply(), it returns OK even when it skips (gate closed / concurrent / destroyed /
// non-standard baseline); the caller reads sample().owned/action to learn the result.
struct PgrSpeedApi2 {            // 16 bytes: ordered, read-only table (V2, Max ball)
    uint32_t version, size;      // 0   { version==2, size==sizeof }
    int (*apply_max)(void *animator, void *response, float expected_max_speed);
};

// Hidden getter. NOT exported by the version script; the coordinator registers it in
// the core getter registry under feature id 8. Answers version 1 with PgrSpeedApi and
// version 2 with PgrSpeedApi2 on an exact match; any other version returns nullptr.
const void *pgrazor_catch_animation_api(uint32_t version);
