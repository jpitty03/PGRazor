#ifndef PGR_TAG_CATCH_REVISION_H
#define PGR_TAG_CATCH_REVISION_H

#include <cstdint>

// Pure decision used by the VGP controller: an effective-rule revision change
// clears skipped-spawn decisions only once the refreshed tag membership is ready.
// An unchanged refresh must never clear the cache again.
struct PgrTagRevisionDecision {
    bool reset;
    uint32_t cached;
};

inline PgrTagRevisionDecision pgr_tag_revision_decide(uint32_t current, uint32_t cached, bool ready) {
    if (!ready || current == cached) return {false, cached};
    return {true, current};
}

#endif
