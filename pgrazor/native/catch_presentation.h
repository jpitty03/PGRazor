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
