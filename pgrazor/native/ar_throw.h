#pragma once
#include <cstdint>
#include "throws.h"   // PgrThrowValue (the verified 12-byte PokeballThrow by-value)

// PGRazor native migration: AR+ catch metadata injection (feature id 5).
//
// This layer does NOT grant a catch bonus. It replaces the ARPlusEncounterValuesProto
// argument handed to the game's TryCapture with a freshly allocated instance whose
// Proximity/Awareness floats are 0 and PokemonFrightened is false, so the encounter is
// scored as a clean centered AR throw. Every other argument (the self instance, the
// by-value PokeballThrow, the MethodInfo*) is forwarded verbatim and the original's
// managed IPromise return is passed back unchanged.
//
// The JS adapter (runtime/native-ar-throw.js) resolves and validates every il2cpp
// pointer, the wild/values classes and the three field handles BEFORE calling install().
// This module owns the single trampoline, the CLOCK_BOOTTIME lease, the Unity-thread
// guard and the applied/errors counters. GC handles are treated as pointer-sized.
//
// Verified game ABI of TryCapture on ARM64:
//   managed-IPromise* TryCapture(void* self, PokeballThrow byValue,
//                                ARPlusEncounterValuesProto* values, MethodInfo*)
// PokeballThrow is the same packed 12-byte value modeled by PgrThrowValue in throws.h;
// we declare the hooked function as void*(void*, PgrThrowValue, void*, void*) so the C++
// ABI marshals it exactly as the game does. Never hand-split the value.

struct PgrArThrowConfig {        // 112 bytes: 8-byte header then 13 pointers
    uint32_t version, size;      // 0   { version==1, size==sizeof }
    void *capture_code;          // 8   hook target: TryCapture
    void *wild_class;            // 16  the wild-encounter class the self must derive from
    void *values_class;          // 24  class to allocate for the replacement values proto
    void *object_get_class;      // 32  void* (*)(void* obj)
    void *class_is_assignable;   // 40  int  (*)(void* klass, void* oklass)
    void *object_new;            // 48  void* (*)(void* klass)
    void *gchandle_new;          // 56  void* (*)(void* obj, int pinned)   -> pointer-sized handle
    void *gchandle_get_target;   // 64  void* (*)(void* handle)
    void *gchandle_free;         // 72  void  (*)(void* handle)
    void *field_set_value;       // 80  void  (*)(void* obj, void* field, void* value)
    void *field_proximity;       // 88  FieldInfo* Proximity (float)
    void *field_awareness;       // 96  FieldInfo* Awareness (float)
    void *field_frightened;      // 104 FieldInfo* PokemonFrightened (bool)
};

struct PgrArThrowSample {        // 16 bytes: pull-only telemetry snapshot
    uint64_t applied;            // 0  encounters this layer re-valued
    uint64_t errors;             // 8  alloc/root failures that forwarded the original args
};

struct PgrArThrowApi {           // 32 bytes: ordered, read-only table
    uint32_t version, size;      // 0  { version==1, size==sizeof }
    int (*install)(const PgrArThrowConfig *config);
    int (*update)(int enabled, int allowed, uint64_t lease_ms, uint64_t unity_tid);
    int (*sample)(PgrArThrowSample *out);
};

// Hidden getter. NOT exported by the version script; the coordinator registers it in the
// core getter registry under feature id 5, answering only on an exact version match.
const void *pgrazor_ar_throw_api(uint32_t version);
