#pragma once
#include <cstdint>
// Read-only hook-engine diagnostic, free of the Dobby header so the runtime
// host can export it. It carries code addresses and counters only: no account
// id, token or coordinate. The failure fields describe the FIRST failed
// pgrazor_hook() of the process (the scheduler hook when startup fails).
enum PgrHookReason : int32_t {
    PGR_HOOK_REASON_NONE = 0, PGR_HOOK_REASON_BAD_ARGS = 1,
    PGR_HOOK_REASON_NEAR_ISLAND = 2, PGR_HOOK_REASON_DOBBY = 3
};
// Where the most recent island page came from.
enum PgrHookIslandSource : uint32_t {
    PGR_HOOK_ISLAND_NONE = 0, PGR_HOOK_ISLAND_MMAP = 1,
    PGR_HOOK_ISLAND_ELF_PADDING = 2,  // range aligned to the ELF segment alignment
    PGR_HOOK_ISLAND_PAGE_PADDING = 3  // narrower range aligned to a 4K page
};
// Bitmask describing how the ELF padding search went for the failed hook.
enum PgrHookPadding : uint32_t {
    PGR_HOOK_PAD_SEARCHED = 1,       // the mmap gap scan found nothing usable
    PGR_HOOK_PAD_OWNER = 2,          // found the module that owns the target
    PGR_HOOK_PAD_CANDIDATE = 4,      // a range was big enough and in branch reach
    PGR_HOOK_PAD_OVERLAP = 8,        // a range was rejected: overlaps a segment/island
    PGR_HOOK_PAD_NOT_READABLE = 16,  // a range was rejected: not private read-only
    PGR_HOOK_PAD_PAGE_RETRY = 32,    // the 4K-aligned range was tried as well
    PGR_HOOK_PAD_WRITE_FAILED = 64,  // an island was chosen but writing it failed
    PGR_HOOK_PAD_MAPS_FAILED = 128   // /proc/self/maps could not be opened
};
struct PgrHookDiag {
    uint32_t version, size;   // version==1, size==sizeof(PgrHookDiag)
    uint32_t failures;        // failed pgrazor_hook() calls so far
    int32_t reason;           // PgrHookReason of the first failure
    int32_t rc;               // Dobby rc of the first failure (0 unless reason is DOBBY)
    int32_t error;            // errno of the last refused mmap (near-island) or after Dobby
    uint32_t gaps;            // reachable address gaps offered to mmap for that hook
    uint32_t padding;         // PgrHookPadding bits for that hook
    uint64_t target;          // address that could not be hooked
    uint32_t island_source;   // PgrHookIslandSource of the most recent island page
    uint32_t island_pages;    // island pages claimed so far
};
static_assert(sizeof(PgrHookDiag) == 48, "Hook diag ABI1");
// Returns 0 and fills *out, or -1 for a null argument. Installs nothing.
int pgrazor_hook_diag(PgrHookDiag *out);
