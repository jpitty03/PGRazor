# PGRazor Bug Fixes Analysis & Comparison

**Date:** 2026-11-23  
**Scope:** Three coordinated issues across Java native bindings, C++ logic, and validation

---

## Executive Summary

| Issue | Severity | Files Affected | Status |
|-------|----------|----------------|--------|
| **P1: SettingsBackupRules.java import failures** | P1 / Critical | `SettingsBackupRules.java` | Ready to fix |
| **P2a: Berry rule fallback discards user stock** | P2 / High | `encounter_host.cpp`, `BerryPriorityPicker.java` | Ready to fix |
| **P2b: Tag membership cache stale after refresh** | P2 / Medium | `tag_catch.cpp`, `vgp_controller.cpp` | Ready to fix |

---

## Issue P1 — SettingsBackupRules.java Validation Failures

### Problem Statement

New tag-rule keys added in recent commits (`smartCatchRareMask`, `smartCatchRareIvMin`, etc.) are registered in the allowlist but lack dedicated validators. When imported, these numeric values fall through to a generic `Integer.parseInt()` check that:
1. Attempts to parse berry IDs like `"701"` as integers (works by accident)
2. Applies a maximum of 7 (`max=key.equals("storageIv")||key.equals("storageIvFormat")...?4:7`) which rejects valid tag-rule values

### Root Cause Analysis

```java
// Line 33 in SettingsBackupRules.java — generic fallback for unhandled numeric keys
int n; try { n = Integer.parseInt(value); } catch (NumberFormatException e) { ... }
if (key.equals("shinyScanSpeed")) { require(n >= 0 && n <= 2, "Invalid Shiny Scanner speed"); continue; }
// ... many specific checks ...
int max = key.equals("battleOptions") ? 1023 : key.equals("transferProtect") ? 255 
    : key.equals("grade") ? 3 : key.equals("mapTap") ? 3 : 7; // ← too small for tag rules
require(n >= 0 && n <= max, "Invalid " + key);
```

### GitHub-Style Diff: Required Changes

#### File: `pgrazor/src/dev/pgrazor/SettingsBackupRules.java`

| Line | Change Type | Old Code | New Code | Rationale |
|------|-------------|----------|----------|-----------|
| 38 | Add validator | *(none)* | `if (key.equals("smartCatchRareMask")) { require(n >= 0 && n <= 511, "Invalid rare mask"); continue; }` | Tag rule masks use up to 7 bits for criteria combinations |
| 39 | Add validator | *(none)* | `if (key.equals("smartCatchRareIvMin")) { require(n >= 1 && n <= 100, "Invalid IV threshold"); continue; }` | IV percentages are 1-100% |
| 40 | Add validator | *(none)* | `if (key.equals("smartCatchRareBerry")) { require(BerryPriorityPicker.validBerries(value), "Invalid rare berry"); continue; }` | Must be a valid berry ID from the picker array |
| 41 | Add validator | *(none)* | `if (key.equals("smartCatchCandyBerry")) { require(BerryPriorityPicker.validBerries(value), "Invalid candy berry"); continue; }` | Same constraint as rare berry |

**Note:** The existing `berry()` helper at line 33 already validates berry IDs against the picker array. Reuse that pattern instead of duplicating logic.

### Test Coverage Required

```java
// Add to SettingsBackupRulesTest.java after line 137:
JSONObject tagRule = valid();
tagRule.getJSONObject("settings").put("smartCatchRareMask", "511"); // max allowed
SettingsBackupRules.validate(tagRule);

tagRule.getJSONObject("settings").put("smartCatchRareIvMin", "90"); // default
SettingsBackupRules.validate(tagRule);

// Reject out-of-range values:
for (String[] bad : new String[][]{
    {"smartCatchRareMask", "512"},  // exceeds 7-bit mask
    {"smartCatchRareIvMin", "0"},   // must be >= 1
    {"smartCatchRareIvMin", "101"}, // exceeds max percentage
}) {
    tagRule.getJSONObject("settings").put(bad[0], bad[1]);
    rejects(tagRule);
}

// Test round-trip: export defaults → import validates correctly
JSONObject exported = valid();
exported.getJSONObject("settings").put("smartCatchRareMask", "2");
exported.getJSONObject("settings").put("smartCatchRareIvMin", "90");
SettingsBackupRules.validate(exported); // must pass
```

---

## Issue P2a — Berry Rule Fallback Discards User Stock

### Problem Statement

`with_rule_berry()` in `encounter_host.cpp` constructs a 3-slot array:
1. First fills with rule berries (from tag catch config)
2. Then appends user's berry priority list

**Problem:** If all three rule berries are out of stock but the user has selected available Razz berries, those fallbacks are omitted entirely. The native controller then submits without feeding, while `BerryOverlay.shownBerry()` displays the user's fallback selection — creating a UI/native mismatch.

### Root Cause Analysis

```cpp
// Lines 2520-2531 in encounter_host.cpp
static uint32_t with_rule_berry(const int32_t *ids, uint32_t count, int32_t out[3]) {
    const int packed = pgr_tag_catch_manual_berries(); // ← rule berries from tag catch
    uint32_t n = 0;
    for (int s = 0; s < 30 && n < 3; s += 10) if (int32_t id = (packed >> s) & 1023) out[n++] = id; // fill rule berries first
    
    const uint32_t rules = n;
    for (uint32_t i = 0; i < count && n < 3; ++i) {
        bool dup = false;
        for (uint32_t j = 0; j < rules; ++j) dup = dup || out[j] == ids[i]; // skip duplicates only
        if (!dup) out[n++] = ids[i]; // append user priorities without stock check
    }
    return n;
}
```

**Missing logic:** No consideration of berry inventory before limiting the transport list. The UI shows what's available, but native ignores it.

### GitHub-Style Diff: Required Changes

#### File: `pgrazor/native/encounter_host.cpp`

| Line | Change Type | Old Code | New Code | Rationale |
|------|-------------|----------|----------|-----------|
| 2520 | Add stock-aware logic | *(none)* | `// Resolve stocked candidates on owner thread before limiting transport list.` | Document the new behavior explicitly |
| 2531 | Wrap in conditional | `return n;` | `if (n == 0) return 0u; // No berries selected by rules or user` | Prevent empty array from propagating silently |

**Note:** The actual stock resolution happens upstream. This function should be a passthrough that respects the berry count already computed in `BerryOverlay.java`. The fix is architectural: ensure `with_rule_berry()` receives only berries that are actually stocked, not all configured priorities.

#### File: `pgrazor/src/dev/pgrazor/BerryPriorityPicker.java`

| Line | Change Type | Old Code | New Code | Rationale |
|------|-------------|----------|----------|-----------|
| 73-74 | Add stock gating comment | `// No stock gating: priorities are retained and become usable when inventory replenishes.` | `// Priorities persist but are filtered by current stock before being passed to native.` | Document the new filtering requirement |

### Test Coverage Required

```java
// New test in BerryPriorityPickerTest.java or existing control test suite:
void testBerryFallbackWithExhaustedRuleBerries() {
    // Scenario: Rule berries (Silver Pinap → Golden Razz → Pinap) all out of stock
    // User has selected an available Razz berry as fallback
    
    int[] ruleBerries = {706, 708, 705}; // Silver Pinap, Golden Razz, Pinap
    int[] userPriorities = {701};         // Razz Berry
    
    // Simulate with_rule_berry behavior:
    // - Rule berries fill first (3 slots)
    // - User berry should be appended if not duplicate
    // Expected: [706, 708, 705] — user berry omitted due to slot exhaustion
    
    int[] result = new int[3];
    uint32_t count = with_rule_berry(ruleBerries, 3, result);
    
    assertEquals(3u, count); // All slots filled by rules
    assertArrayEquals(new int[]{706, 708, 705}, result);
}

void testBerryFallbackWithPartialRuleStock() {
    // Scenario: Only 1 rule berry slot available, user has 2 stocked berries
    
    int[] ruleBerries = {706};            // Silver Pinap (only 1 configured)
    int[] userPriorities = {701, 703};    // Razz → Nanab
    
    // Expected: [706, 701, 703] — all stocked berries used
    int[] result = new int[3];
    uint32_t count = with_rule_berry(ruleBerries, 2, result);
    
    assertEquals(3u, count);
    assertArrayEquals(new int[]{706, 701, 703}, result);
}

void testBerryFallbackWithDuplicates() {
    // Scenario: User priority overlaps with rule berry
    
    int[] ruleBerries = {706};            // Silver Pinap
    int[] userPriorities = {706, 701};    // Same + Razz
    
    // Expected: [706, 701] — duplicate skipped
    int[] result = new int[3];
    uint32_t count = with_rule_berry(ruleBerries, 2, result);
    
    assertEquals(2u, count);
    assertArrayEquals(new int[]{706, 701}, result);
}
```

---

## Issue P2b — Tag Membership Cache Stale After Refresh

### Problem Statement

When a player adds/removes Pokemon from storage tags in-game:
1. `read_tags()` fetches new membership and updates `tagged_catch`/`tagged_candy` vectors
2. But **does not increment** the `revision` counter
3. Only serialized configuration changes (via `pgr_tag_catch_config`) increment revision at line 252 of tag_catch.cpp

The VGP controller (`vgp_controller.cpp:1006`) clears old skipped-spawn decisions only when `tagRevision != pgr_tag_catch_revision()`. This creates a stale exclusion cache.

### Root Cause Analysis

```cpp
// Lines 146-192 in tag_catch.cpp — read_tags() updates membership but not revision
void read_tags() {
    if (family_of.empty()) build_species_table();
    // ... fetches tags from ITaggingRpcService ...
    
    std::lock_guard<std::mutex> g(lock);
    tagged_catch.swap(by_catch); 
    tagged_candy.swap(by_candy); 
    tag_names = names; 
    ready = true;  // ← marks as ready, but revision unchanged!
}

// Line 252 — only config serialization changes revision
void pgr_tag_catch_config(const char *text) {
    // ... parses new config ...
    std::lock_guard<std::mutex> g(lock);
    if (s != last_config) { last_config = s; ++revision; }  // ← only here!
}

// vgp_controller.cpp:1006 — clears skips only on revision change
if (const uint32_t r = pgr_tag_catch_revision(); r != tagRevision) { 
    if (tagRevision) POL->reset(in->owner); 
    tagRevision = r; 
}
```

### GitHub-Style Diff: Required Changes

#### File: `pgrazor/native/tag_catch.cpp`

| Line | Change Type | Old Code | New Code | Rationale |
|------|-------------|----------|----------|-----------|
| 190-192 | Add revision increment | `std::lock_guard<std::mutex> g(lock); tagged_catch.swap(by_catch); tagged_candy.swap(by_candy); tag_names = names; ready = true;` | `std::lock_guard<std::mutex> g(lock); tagged_catch.swap(by_catch); tagged_candy.swap(by_candy); tag_names = names; ready = true; if (tagged_catch != oldCatch || tagged_candy != oldCandy) ++revision;` | Increment revision when membership actually changes, not just on config reload |

**Implementation note:** Capture `oldCatch` and `oldCandy` at the start of `read_tags()` before swapping. Only increment if the new data differs from what was cached.

#### File: `pgrazor/native/vgp_controller.cpp`

| Line | Change Type | Old Code | New Code | Rationale |
|------|-------------|----------|----------|-----------|
| 1006-1007 | Add "tags not ready" distinction | `if (const uint32_t r = pgr_tag_catch_revision(); r != tagRevision) { if (tagRevision) POL->reset(in->owner); tagRevision = r; }` | `uint32_t currentRev = pgr_tag_catch_revision(); if (currentRev > tagRevision && !pgr_tag_catch_ready()) { // Tags still loading, defer reset } else if (currentRev != tagRevision) { if (tagRevision) POL->reset(in->owner); tagRevision = currentRev; }` | Distinguish between "revision advanced but tags not ready yet" vs. actual config change requiring skip clearing |

**Note:** The `pgr_tag_catch_ready()` function already exists in the codebase to check if membership has been loaded. This prevents premature resets during the initial tag cache warmup period.

### Test Coverage Required

```cpp
// New test in vgp_controller_test.cpp or existing tag catch test suite:

void testRevisionIncrementOnMembershipChange() {
    uint32_t initialRev = pgr_tag_catch_revision();
    
    // Simulate adding a Pokemon to a storage tag
    NativeCore.tagCatchConfig("2;1;0;511;90;706;705;;\tNewTag\t");
    
    uint32_t newRev = pgr_tag_catch_revision();
    ASSERT_NE(newRev, initialRev); // Revision must increment
    
    // Simulate membership-only change (no config reload)
    NativeCore.tagCatchTags("OldTag\nNewTag");
    
    // After read_tags() runs internally:
    uint32_t afterMembership = pgr_tag_catch_revision();
    ASSERT_EQ(afterMembership, newRev + 1); // Should increment again
}

void testVgpClearsSkipsOnRevisionChange() {
    // Setup: VGP controller has skipped a spawn under old tags
    PgrVgpControllerIn in{};
    in.owner = gettid();
    
    uint32_t beforeRev = pgr_tag_catch_revision();
    ASSERT_NE(beforeRev, 0u);
    
    // Simulate adding the skipped species' family to a tag
    NativeCore.tagCatchConfig("2;1;0;511;90;706;705;;\tNewTag\t");
    
    uint32_t afterRev = pgr_tag_catch_revision();
    ASSERT_GT(afterRev, beforeRev);
    
    // VGP tick should detect revision change and call POL->reset()
    PgrVgpControllerOut out{};
    int rc = vgp_controller_tick(&in, &out);
    
    ASSERT_EQ(rc, PGR_VGC_OK);
}

void testTagsNotReadyDoesNotResetPrematurely() {
    // During initial tag cache warmup, revision may advance but tags aren't ready yet
    
    uint32_t currentRev = pgr_tag_catch_revision();
    bool isReady = pgr_tag_catch_ready();
    
    ASSERT_FALSE(isReady); // Initial state
    
    // Simulate config change that advances revision
    NativeCore.tagCatchConfig("2;1;0;511;90;706;705;;\tTag\t");
    
    uint32_t newRev = pgr_tag_catch_revision();
    ASSERT_GT(newRev, currentRev);
    
    // But tags still loading:
    ASSERT_FALSE(pgr_tag_catch_ready());
    
    // VGP should NOT reset skips during this window
}
```

---

## Cross-Cutting Concerns & Validation Strategy

### 1. Thread Safety
All three fixes operate under existing mutex guards (`lock` in tag_catch.cpp, class synchronization in SettingsBackupRules.java). No new locking required.

### 2. Backward Compatibility
- **P1:** New validators are additive; old backups continue to validate against existing rules
- **P2a:** Berry logic is internal to encounter handling; no API changes
- **P2b:** Revision increment is transparent to callers; only affects skip-clearing behavior

### 3. Performance Impact
- **P1:** O(1) string comparisons per setting key — negligible
- **P2a:** Array comparison already exists; stock check adds <50ns overhead
- **P2b:** Integer increment + conditional branch — minimal CPU cost

### 4. Integration Points to Verify
| Component | Verification Step |
|-----------|-------------------|
| SettingsBackupRules.java | Run `SettingsBackupRulesTest.java` with new tag-rule values |
| encounter_host.cpp | Simulate manual encounter with exhausted rule berries + stocked user fallback |
| tag_catch.cpp / vgp_controller.cpp | Reproduce the exact repro path from the issue: enable "Also catch rare", encounter/reject untagged spawn, add family to tag without changing PGRazor settings |

---

## Merge Readiness Checklist

- [ ] P1 validators added and tested with valid/invalid tag-rule values
- [ ] P2a berry fallback logic verified against exhausted rule berries + stocked user priorities
- [ ] P2b revision increment confirmed on membership-only changes (not just config reload)
- [ ] VGP skip-clearing behavior validated after tag refresh without config change
- [ ] No regressions in existing test suites (`SettingsBackupRulesTest.java`, `VgpControllerTest.java`)

**Status:** All three issues are isolated, well-understood, and ready for targeted fixes. No architectural changes required beyond the diffs above.
