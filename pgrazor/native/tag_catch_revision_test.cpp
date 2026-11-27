// Pure regression for VGP skip invalidation: membership-only revision changes,
// warmup deferral and unchanged refreshes. Include-only; no runtime needed.
#include "tag_catch_revision.h"
#include <cassert>
#include <cstdio>

int main() {
    const auto unchanged = pgr_tag_revision_decide(5u, 5u, true);
    assert(!unchanged.reset && unchanged.cached == 5u);

    const auto membership = pgr_tag_revision_decide(6u, 5u, true);
    assert(membership.reset && membership.cached == 6u);

    const auto loading = pgr_tag_revision_decide(6u, 5u, false);
    assert(!loading.reset && loading.cached == 5u);

    // Once tags become ready the deferred revision is applied exactly once.
    const auto deferred = pgr_tag_revision_decide(6u, loading.cached, true);
    assert(deferred.reset && deferred.cached == 6u);
    assert(!pgr_tag_revision_decide(deferred.cached, deferred.cached, true).reset);

    // First observation advances the cache without resetting anything (caller guards on cached != 0).
    const auto first = pgr_tag_revision_decide(9u, 0u, true);
    assert(first.reset && first.cached == 9u);

    std::puts("PASS tag catch revision: membership change resets, warmup defers, unchanged refresh does not");
    return 0;
}
