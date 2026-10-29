#pragma once
#include <cstdint>

// PGRazor native migration: throw grade / curveball policy (feature id 1).
//
// Mirrors the throw-rewriting policy in diagnostics/quick-ball.js (throwWords /
// throwForRoute). The JS adapter resolves and validates every il2cpp pointer
// (method addresses, ball/owner classes, the il2cpp function pointers and the guard
// field metadata) BEFORE calling install(); this module owns the two trampolines,
// the CLOCK_BOOTTIME lease, the Unity-thread guard, the per-submit radius sample and
// its one-shot reuse by the paired hit hook.
//
// Verified ARM64 ABI of PokeballThrow: a 12-byte packed value {int item, float
// radius, bool bullseye, bool spinning, bool missed, pad}. It arrives split across
// x1 (item low32 + radius-bits high32) and the low word of x2 (three Boolean bytes +
// one pad byte). We model it as a by-value struct and declare the hooked functions as
// void(void* self, PgrThrowValue, void* methodInfo) so the C++ ABI marshals it exactly
// as the game does (two integer registers on ARM64; the host x86 test ABI may route the
// float eightbyte through SSE on its own). Never hand-split the value.

struct PgrThrowValue {        // 12 bytes, 4-byte aligned
    int32_t item;             // 0  Pokeball item id (1,2,3,5 are eligible)
    float   radius;           // 4  reticle radius, finite 0..1
    uint8_t bullseye;         // 8  0/1
    uint8_t spinning;         // 9  0/1
    uint8_t missed;           // 10 must be 0 to be eligible
    uint8_t pad;              // 11 preserved verbatim
};

struct PgrThrowConfig {       // 72 bytes: 8-byte header then eight pointers
    uint32_t version, size;   // 0  { version==1, size==sizeof }
    void *submit_code;        // 8  hook target: submit throw
    void *hit_code;           // 16 hook target: NotifyPokeballHit
    void *ball_class;         // 24 exact class the submit self must be
    void *owner_class;        // 32 encounter-owner class the hit self must derive from
    void *object_get_class;   // 40 void* (*)(void* obj)
    void *class_is_assignable; // 48 bool  (*)(void* klass, void* oklass)
    void *field_get_value;    // 56 void  (*)(void* obj, void* field, void* out)
    void *bread_field;        // 64 FieldInfo* for the submit guard byte
};

struct PgrThrowSample {       // 24 bytes: pull-only telemetry snapshot
    uint64_t count;           // 0  throws this layer changed
    uint32_t route;           // 8  of the last change: 0 submit, 1 hit
    uint32_t grade;           // 12 grade applied (0..3)
    uint32_t curve;           // 16 curve flag applied (0/1)
    float    radius;          // 20 reticle radius returned to the game
};

struct PgrThrowApi {          // 40 bytes: ordered, read-only table
    uint32_t version, size;   // 0  { version==1, size==sizeof }
    int (*install)(const PgrThrowConfig *config);
    int (*update)(int grade, int curve, int allowed, uint64_t lease_ms, uint64_t unity_tid);
    int (*sample)(PgrThrowSample *out);
    int (*rewrite_submit)(void *self, PgrThrowValue *value);
};

// Hidden getter. NOT exported by the version script; the coordinator registers it in the
// core getter registry under feature id 1, which answers only on an exact version match.
const void *pgrazor_throws_api(uint32_t version);
