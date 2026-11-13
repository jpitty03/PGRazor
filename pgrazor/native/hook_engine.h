#pragma once
#include "dobby.h"
#include "hook_engine_diag.h"
#include <cstdint>
// All native modules share Dobby's process-global trampoline registry. Serialize
// mutations even when independent feature adapters initialize on different threads.
int pgrazor_hook(void *address, void *replacement, void **original);
int pgrazor_unhook(void *address);
int pgrazor_instrument(void *address, dobby_instrument_callback_t callback);

// Alignment padding around one executable LOAD segment [begin, end): the bytes
// between the aligned-down start and the segment, and between the segment and
// the aligned-up end. Pure arithmetic; the caller still has to prove each range
// is mapped, private, read-only and outside every LOAD segment.
struct PgrPaddingRanges { uintptr_t before_lo, before_hi, after_lo, after_hi; };
constexpr PgrPaddingRanges pgr_padding_ranges(uintptr_t begin, uintptr_t end, uintptr_t alignment) {
    return {begin & ~(alignment - 1), begin & ~uintptr_t(3),
            (end + 3) & ~uintptr_t(3), (end + alignment - 1) & ~(alignment - 1)};
}
