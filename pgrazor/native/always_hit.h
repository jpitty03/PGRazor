#pragma once
#include <cstdint>

// PGRazor native migration: Always Hit throw normalisation (feature id 7).
//
// Mirrors the Always Hit block in diagnostics/quick-ball.js (the launch/release
// observers, the OnCollisionEnter / OnTriggerEnter replacements and the btyb
// makeThrow normaliser). It does NOT grant a catch; it forces the manually released
// ordinary ball to resolve its hit by invoking the pokeball's own already-wired
// HitPokemon System.Action exactly once per throw, and it clears a spuriously-set
// "missed" byte on the value btyb would otherwise hand to grading. Server catch
// outcomes are never touched.
//
// Division of labour with the JS adapter (runtime/native-always-hit.js):
//   * JS resolves and validates every il2cpp pointer (method addresses + instruction
//     prefixes, the ball/machine/action class identities, the field handles and the
//     il2cpp primitive exports) BEFORE calling install().
//   * This module owns the collision/trigger/makeThrow trampolines, the launch/release
//     entry instruments, the CLOCK_BOOTTIME lease, the Unity-thread guard, the single
//     GC-rooted "handled ball" (one applied hit per launch/release) and the telemetry
//     counters. It never calls back into JS.
//
// --- ABI notes --------------------------------------------------------------------
// OnCollisionEnter(UnityEngine.Collision) and OnTriggerEnter(UnityEngine.Collider)
// have a determinable il2cpp instance ABI: void(void* self, void* arg, void* method).
// The argument is an object reference (pointer) we forward verbatim; we hook them with a
// full chained replacement.
//
// Launch(UnityEngine.Vector2) and EndDrag() carry a by-value Vector2 / no managed
// argument. Rather than guess the argument marshalling we attach signature-agnostic
// entry instruments that read ONLY the `this` ball (x0 on arm64, rdi on x86_64), exactly
// as the JS onEnter did with args[0]; they never alter the call.
//
// btyb (makeThrow) returns the game's 12-byte PokeballThrow BY VALUE. Its il2cpp
// metadata declares ONE managed parameter, System.Boolean, so the verified native ABI is
//   PgrThrowValue btyb(void* self, uint8_t missedArg, void* methodInfo)
// i.e. THREE native parameters, not the (self, MethodInfo) pair named in the original
// task brief. See always_hit.cpp for the coordinator-facing blocker note. Modelling it
// as a by-value struct return lets the C++ ABI place {item,radius} in x0 and the three
// Boolean bytes + pad in x1 exactly as the game does, so clearing `missed` here is the
// same bit operation as the old `x1 & 0xff00ffff` register edit. Never hand-split it.

// 12-byte packed PokeballThrow; identical layout to PgrThrowValue in throws.h. Redefined
// here under a feature-local name so this header is self-contained for the coordinator.
struct PgrAlwaysHitValue {   // 12 bytes, 4-byte aligned
    int32_t item;            // 0  Pokeball item id (1,2,3,5 are eligible)
    float   radius;          // 4  reticle radius (untouched by this layer)
    uint8_t bullseye;        // 8  0/1 (untouched)
    uint8_t spinning;        // 9  0/1 (untouched)
    uint8_t missed;          // 10 normalised 1 -> 0 only when permitted
    uint8_t pad;             // 11 preserved verbatim
};

struct PgrAlwaysHitConfig {  // 192 bytes: 8-byte header then 23 pointers in table order
    uint32_t version, size;  // 0   { version==1, size==sizeof }
    void *code_launch;       // 8   instrument target: Launch (reads x0/rdi only)
    void *code_release;      // 16  instrument target: EndDrag (reads x0/rdi only)
    void *code_collision;    // 24  hook target: OnCollisionEnter
    void *code_trigger;      // 32  hook target: OnTriggerEnter
    void *code_make_throw;   // 40  hook target: btyb (makeThrow)
    void *ball_class;        // 48  exact class the ball self must be
    void *machine_class;     // 56  exact class of the SchedulerStateMachine
    void *action_class;      // 64  exact class of the HitPokemon System.Action
    void *object_get_class;  // 72  void* (*)(void* obj)
    void *field_get_value;   // 80  void  (*)(void* obj, void* field, void* out)
    void *runtime_invoke;    // 88  void* (*)(void* method, void* obj, void** params, void** exc)
    void *invoke_method;     // 96  MethodInfo* for System.Action.Invoke
    void *gchandle_new;      // 104 void* (*)(void* obj, int pinned) -> pointer-sized handle
    void *gchandle_get_target; // 112 void* (*)(void* handle)
    void *gchandle_free;     // 120 void  (*)(void* handle)
    void *field_bread;       // 128 FieldInfo* <HoldingBreadBall> guard byte (bool)
    void *field_item;        // 136 FieldInfo* <BallType> backing field (int enum)
    void *field_state;       // 144 FieldInfo* the ball's state-machine reference
    void *field_current;     // 152 FieldInfo* the machine's current-state reference
    void *field_flying;      // 160 FieldInfo* the ball's flying coroutine reference
    void *field_falling;     // 168 FieldInfo* the ball's falling coroutine reference
    void *field_hit;         // 176 FieldInfo* HitPokemon action reference
    void *field_pokemon;     // 184 FieldInfo* IEncounterPokemon reference
};

struct PgrAlwaysHitSample {  // 24 bytes: pull-only telemetry snapshot
    uint64_t applied;        // 0  HitPokemon actions invoked
    uint64_t normalized;     // 8  makeThrow "missed" bytes cleared
    uint64_t errors;         // 16 managed-exception / root failures that latched the fault
};

struct PgrAlwaysHitApi {     // 40 bytes: ordered, read-only table
    uint32_t version, size;  // 0  { version==1, size==sizeof }
    int (*install)(const PgrAlwaysHitConfig *config);
    int (*update)(int enabled, int allowed, uint64_t lease_ms, uint64_t unity_tid);
    int (*sample)(PgrAlwaysHitSample *out);
    int (*tick)(uint64_t proven_tid);
};

// Hidden getter. NOT exported by the version script; the coordinator registers it in the
// core getter registry under feature id 7, answering only on an exact version match.
const void *pgrazor_always_hit_api(uint32_t version);
