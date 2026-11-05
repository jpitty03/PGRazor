#pragma once
#include <cstdint>

// Feature 3. Pointer-sized GC handles match the validated Unity runtime ABI.
struct PgrCatchConfig {
    uint32_t version, size;
    void *address;
    void *(*root)(void *, int);
    void (*free_root)(void *);
    void *(*target)(void *);
    void *(*object_class)(void *);
    void *view_class, *action_class, *pokemon_class;
};
struct PgrCatchCapture {
    void *service, *pokemon, *callback;
    uint32_t confirm, persist;
    int32_t open_hyper_training;
    uint32_t hide_ar, post, suppress;
};
struct PgrCatchApi {
    uint32_t version, size;
    int (*install)(const PgrCatchConfig *);
    int (*begin)(void *response, int32_t thread);
    int (*end)(PgrCatchCapture *);
    int (*dispose)();
};
const void *pgrazor_catch_presentation_api(uint32_t version);

// Shared use of the one ShowPokemonInfo hook (a second hook on the same code is not supported).
// A bypass, when set, is asked about every call this bridge does not capture; returning true means
// the call was handled and the game's ShowPokemonInfo is not run. Same-library callers only.
// dismissed is the 6th argument (System.Action); params the 8th (Action<DismissParameters>).
using PgrCatchBypass = bool (*)(void *instance, void *pokemon, void *dismissed, void *params);
// Sets (or clears, with null) the bypass. address is the validated ShowPokemonInfo code; if the
// bridge has not hooked it yet, a pass-through hook is attached there now and install() later
// adopts it for that same address. Returns 0 when the hook is in place, negative otherwise.
int pgr_catch_presentation_set_bypass(PgrCatchBypass bypass, void *address);
// True when the bridge's hook is attached at address (its prologue then no longer matches).
bool pgr_catch_presentation_attached(const void *address);
