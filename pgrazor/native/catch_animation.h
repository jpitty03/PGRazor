#pragma once
#include <cstdint>

// PGRazor native migration: catch animation speed ownership (feature id 8).
//
// Ports the per-instance Animator-speed block of diagnostics/quick-ball.js: the
// btxu leave path that, after a confirmed ordinary-ball catch, snapshots the
// Animator's original speed, writes the 20x BALL_SPEED and restores it on the
// verified Unity tick (completion / off / lease loss / 8 s timeout), proving the
// target survived and no external code changed the speed first.
//
// Division of labour with the JS adapter (runtime/native-catch-animation.js):
//   * JS keeps the Gum btxu/complete observers in this stage and performs the
//     ball/response/caller/rate/thread/bread/animator-class checks; on a confirmed
//     success it CALLS apply() here, and the two completion observers CALL
//     complete(). No Dobby hook is installed by this module in this stage.
//   * This module OWNS the pointer-sized GC roots for the animator + response, the
//     original-speed snapshot, the 20x write + readback, restoration (completion /
//     off / lease / timeout), the destroyed / external-speed-change behaviour, the
//     1 s retry after a restore failure and the latched fault. It never calls back
//     into JS; the adapter pulls telemetry through sample().
//
// --- speed ownership rules (mirrored from quick-ball.js) -------------------------
//   * Single attempt at a time; a confirmed success while one is live is ignored
//     (no re-apply). Ownership (the attempt + roots) is recorded BEFORE the setter,
//     because even a throwing set_speed may have mutated native state and must be
//     inspected/restored.
//   * The snapshot original speed must be finite and ~1.0, else the apply is skipped.
//   * Restore only overwrites when the current speed is still ~20x; an external
//     change releases the attempt WITHOUT overwriting it.
//   * A restore failure latches a fault: the policy window is cleared and stopping
//     is set, but the attempt is preserved and retried every 1 s until it succeeds.
//   * Root failures free any partial root and latch the fault; invalid config never
//     writes. All managed calls use runtime_invoke with the exception out-param and
//     exact boxed System.Single / System.Boolean unboxing; no mutex is held across a
//     managed invoke and a reentry guard blocks re-entry from within one.

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
    uint32_t pad;               // 20  reserved / alignment
    float    original;          // 24  snapshot original speed for the last event
    float    readback;          // 28  speed read back after the last write
};

struct PgrSpeedApi {             // 56 bytes: ordered, read-only table
    uint32_t version, size;      // 0   { version==1, size==sizeof }
    int (*install)(const PgrSpeedConfig *config);
    int (*update)(int allowed, uint64_t lease_ms, uint64_t unity_tid);
    int (*apply)(void *animator, void *response);
    int (*complete)(void *response, int32_t reason);
    int (*tick)(uint64_t proven_tid);
    int (*sample)(PgrSpeedSample *out);
};

// Hidden getter. NOT exported by the version script; the coordinator registers it in
// the core getter registry under feature id 8, answering only on an exact version match.
const void *pgrazor_catch_animation_api(uint32_t version);
