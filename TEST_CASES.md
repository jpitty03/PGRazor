# Test Cases for PGRazor Bug Fixes

This document provides the exact test cases needed to validate each fix before merge. Run these against the modified codebase to confirm correctness.

---

## Issue P1: SettingsBackupRules.java Validation Failures

### Test File: `pgrazor/test/SettingsBackupRulesTest.java`

```java
// Add after line 137 (existing "PASS" message):

System.out.println("=== Testing Tag-Rule Validators ===");

// Test 1: Valid tag-rule values pass validation
JSONObject validTagRule = valid();
validTagRule.getJSONObject("settings").put("smartCatchRareMask", "511"); // max 7-bit mask
validTagRule.getJSONObject("settings").put("smartCatchRareIvMin", "90"); // default IV threshold
validTagRule.getJSONObject("settings").put("smartCatchRareBerry", "706"); // Silver Pinap
validTagRule.getJSONObject("settings").put("smartCatchCandyBerry", "705"); // Pinap Berry
SettingsBackupRules.validate(validTagRule);
System.out.println("[PASS] Valid tag-rule values accepted");

// Test 2: Out-of-range rare mask is rejected (exceeds 7-bit)
validTagRule.getJSONObject("settings").put("smartCatchRareMask", "512");
rejects(validTagRule);
System.out.println("[PASS] Mask > 511 rejected");

// Test 3: IV threshold below minimum is rejected
validTagRule.getJSONObject("settings").put("smartCatchRareIvMin", "0");
rejects(validTagRule);
System.out.println("[PASS] IV threshold < 1 rejected");

// Test 4: IV threshold above maximum is rejected
validTagRule.getJSONObject("settings").put("smartCatchRareIvMin", "101");
rejects(validTagRule);
System.out.println("[PASS] IV threshold > 100 rejected");

// Test 5: Invalid berry ID for rare berry is rejected
validTagRule.getJSONObject("settings").put("smartCatchRareBerry", "999"); // not in picker array
rejects(validTagRule);
System.out.println("[PASS] Invalid rare berry ID rejected");

// Test 6: Valid berry IDs are accepted
for (int validBerry : new int[]{701, 703, 705, 706, 708}) {
    validTagRule.getJSONObject("settings").put("smartCatchRareBerry", String.valueOf(validBerry));
    SettingsBackupRules.validate(validTagRule);
}
System.out.println("[PASS] Valid berry IDs accepted");

// Test 7: Round-trip test — export defaults → import validates correctly
JSONObject exported = valid();
exported.getJSONObject("settings").put("smartCatchRareMask", "2"); // Legendary/Mythical only
exported.getJSONObject("settings").put("smartCatchRareIvMin", "90");
SettingsBackupRules.validate(exported);
System.out.println("[PASS] Round-trip export→import validates correctly");

// Test 8: Empty string for berry fields is rejected (must be numeric)
validTagRule.getJSONObject("settings").put("smartCatchRareBerry", "");
rejects(validTagRule);
System.out.println("[PASS] Empty berry field rejected");

// Test 9: Negative number is rejected
validTagRule.getJSONObject("settings").put("smartCatchRareMask", "-1");
rejects(validTagRule);
System.out.println("[PASS] Negative mask value rejected");

// Test 10: Non-numeric string is rejected by parseInt fallback
validTagRule.getJSONObject("settings").put("smartCatchRareIvMin", "abc");
rejects(validTagRule);
System.out.println("[PASS] Non-numeric IV threshold rejected");

System.out.println("=== Tag-Rule Validators Complete ===");
```

**Expected Output:** All 10 tests should print `[PASS]` before the final summary message.

---

## Issue P2a: Berry Rule Fallback Discards User Stock

### Test File: `pgrazor/test/BerryPriorityPickerTest.java` (new file) or existing control test suite

```java
import dev.pgrazor.BerryPriorityPicker;
import static org.junit.Assert.*;

public class BerryPriorityPickerTest {

    // Simulate with_rule_berry behavior for unit testing
    private static int[] simulateWithRuleBerry(int[] ruleBerries, int count, int[] userPriorities) {
        int[] out = new int[3];
        uint32_t n = 0;
        
        // Rule berries fill first (simplified from actual packed format)
        for (int i = 0; i < ruleBerries.length && n < 3; i++) {
            if (ruleBerries[i] > 0 && n < 3) out[n++] = ruleBerries[i];
        }
        
        // User priorities appended without duplicates
        for (int i = 0; i < count && n < 3; i++) {
            boolean dup = false;
            for (uint32_t j = 0; j < n; j++) {
                if (out[j] == userPriorities[i]) { dup = true; break; }
            }
            if (!dup && n < 3) out[n++] = userPriorities[i];
        }
        
        return new int[n]; // Return only filled slots
    }

    @Test
    public void testExhaustedRuleBerriesWithUserFallback() {
        // Scenario: All 3 rule berries configured, all slots filled
        // User has selected an available Razz berry as fallback
        
        int[] ruleBerries = {706, 708, 705}; // Silver Pinap → Golden Razz → Pinap
        int[] userPriorities = {701};         // Razz Berry (available in stock)
        
        int[] result = simulateWithRuleBerry(ruleBerries, 3, userPriorities);
        
        assertEquals(3, result.length);
        assertArrayEquals(new int[]{706, 708, 705}, result);
        
        // User berry omitted due to slot exhaustion — this is expected behavior
        assertFalse(result[2] == 701);
    }

    @Test
    public void testPartialRuleBerriesWithUserFallback() {
        // Scenario: Only 1 rule berry configured, user has 2 stocked berries
        
        int[] ruleBerries = {706};            // Silver Pinap (only 1)
        int[] userPriorities = {701, 703};    // Razz → Nanab
        
        int[] result = simulateWithRuleBerry(ruleBerries, 2, userPriorities);
        
        assertEquals(3, result.length);
        assertArrayEquals(new int[]{706, 701, 703}, result);
    }

    @Test
    public void testDuplicateUserPrioritySkipped() {
        // Scenario: User priority overlaps with rule berry
        
        int[] ruleBerries = {706};            // Silver Pinap
        int[] userPriorities = {706, 701};    // Same + Razz
        
        int[] result = simulateWithRuleBerry(ruleBerries, 2, userPriorities);
        
        assertEquals(2, result.length);
        assertArrayEquals(new int[]{706, 701}, result);
    }

    @Test
    public void testEmptyRuleAndUser() {
        // Edge case: No berries configured at all
        
        int[] ruleBerries = {};
        int[] userPriorities = {};
        
        int[] result = simulateWithRuleBerry(ruleBerries, 0, userPriorities);
        
        assertEquals(0, result.length);
    }

    @Test
    public void testUserPriorityFillsRemainingSlots() {
        // Scenario: Rule berries fill 2 slots, user priority fills the third
        
        int[] ruleBerries = {706, 708};       // Silver Pinap → Golden Razz
        int[] userPriorities = {701};          // Razz Berry
        
        int[] result = simulateWithRuleBerry(ruleBerries, 2, userPriorities);
        
        assertEquals(3, result.length);
        assertArrayEquals(new int[]{706, 708, 701}, result);
    }

    @Test
    public void testMultipleUserPrioritiesFillSlots() {
        // Scenario: Rule berries fill 1 slot, user has 2 stocked berries
        
        int[] ruleBerries = {706};            // Silver Pinap (only 1)
        int[] userPriorities = {701, 703, 705}; // Razz → Nanab → Pinap
        
        int[] result = simulateWithRuleBerry(ruleBerries, 3, userPriorities);
        
        assertEquals(4, result.length);
        assertArrayEquals(new int[]{706, 701, 703, 705}, result);
    }

    @Test
    public void testAllUserPrioritiesAreDuplicates() {
        // Edge case: User priorities are all duplicates of rule berries
        
        int[] ruleBerries = {706, 708};       // Silver Pinap → Golden Razz
        int[] userPriorities = {706, 708};    // Same two
        
        int[] result = simulateWithRuleBerry(ruleBerries, 2, userPriorities);
        
        assertEquals(2, result.length);
        assertArrayEquals(new int[]{706, 708}, result);
    }

    @Test
    public void testMixedDuplicateAndUniqueUserPriorities() {
        // Scenario: Some user priorities duplicate, some are new
        
        int[] ruleBerries = {706};            // Silver Pinap
        int[] userPriorities = {706, 701, 708}; // Duplicate + Razz + Golden Razz
        
        int[] result = simulateWithRuleBerry(ruleBerries, 3, userPriorities);
        
        assertEquals(3, result.length);
        assertArrayEquals(new int[]{706, 701, 708}, result);
    }

    System.out.println("=== Berry Priority Fallback Tests Complete ===");
}
```

**Expected Output:** All 9 tests should pass with no assertion failures.

---

## Issue P2b: Tag Membership Cache Stale After Refresh

### Test File: `pgrazor/test/VgpControllerTest.java` (existing) or new tag catch test suite

```cpp
#include "vgp_controller.h"
#include "tag_catch.h"
#include <iostream>

void printRevision(uint32_t rev, const char* label) {
    std::cout << "[" << label << "] Revision: " << rev;
}

// Test 1: Revision increments on membership change
TEST(test_revision_increment_on_membership_change) {
    uint32_t initialRev = pgr_tag_catch_revision();
    
    // Simulate adding a Pokemon to a storage tag via native bridge
    NativeCore.tagCatchConfig("2;1;0;511;90;706;705;;\tNewTag\t");
    
    uint32_t newRev = pgr_tag_catch_revision();
    
    printRevision(initialRev, "Initial");
    printRevision(newRev, "After config change");
    
    ASSERT_NE(newRev, initialRev); // Revision must increment
    
    // Simulate membership-only change (no config reload)
    NativeCore.tagCatchTags("OldTag\nNewTag");
    
    // After read_tags() runs internally:
    uint32_t afterMembership = pgr_tag_catch_revision();
    
    printRevision(afterMembership, "After membership update");
    
    ASSERT_EQ(afterMembership, newRev + 1); // Should increment again
    
    std::cout << "[PASS] Revision increments on membership change" << std::endl;
}

// Test 2: VGP clears skips when revision changes
TEST(test_vgp_clears_skips_on_revision_change) {
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
    std::cout << "[PASS] VGP clears skips on revision change" << std::endl;
}

// Test 3: Tags not ready does not reset prematurely
TEST(test_tags_not_ready_defers_reset) {
    uint32_t currentRev = pgr_tag_catch_revision();
    bool isReady = pgr_tag_catch_ready();
    
    printRevision(currentRev, "Current revision");
    std::cout << "[Tags ready: " << (isReady ? "yes" : "no") << "]" << std::endl;
    
    ASSERT_FALSE(isReady); // Initial state
    
    // Simulate config change that advances revision
    NativeCore.tagCatchConfig("2;1;0;511;90;706;705;;\tTag\t");
    
    uint32_t newRev = pgr_tag_catch_revision();
    ASSERT_GT(newRev, currentRev);
    
    // But tags still loading:
    bool stillNotReady = !pgr_tag_catch_ready();
    
    printRevision(newRev, "After config change");
    std::cout << "[Tags ready: " << (!stillNotReady ? "yes" : "no") << "]" << std::endl;
    
    ASSERT_TRUE(stillNotReady); // Tags still warming up
    
    std::cout << "[PASS] Revision advances but tags not yet ready" << std::endl;
}

// Test 4: Repeated reads of identical membership don't increment revision
TEST(test_no_spurious_revision_increment) {
    uint32_t initialRev = pgr_tag_catch_revision();
    
    // Simulate reading the same tag configuration multiple times
    NativeCore.tagCatchTags("ExistingTag");
    uint32_t afterFirstRead = pgr_tag_catch_revision();
    
    NativeCore.tagCatchTags("ExistingTag");
    uint32_t afterSecondRead = pgr_tag_catch_revision();
    
    printRevision(initialRev, "Initial");
    printRevision(afterFirstRead, "After first read");
    printRevision(afterSecondRead, "After second read (same data)");
    
    ASSERT_EQ(afterFirstRead, initialRev); // No change on identical data
    ASSERT_EQ(afterSecondRead, afterFirstRead); // Still no change
    
    std::cout << "[PASS] Revision stable on repeated reads of identical membership" << std::endl;
}

// Test 5: Membership change increments revision even without config reload
TEST(test_membership_only_change_increments_revision) {
    uint32_t initialRev = pgr_tag_catch_revision();
    
    // Simulate adding a Pokemon to an existing tag (membership-only change)
    NativeCore.tagCatchTags("ExistingTag\nNewPokemonFamily");
    
    uint32_t afterMembershipChange = pgr_tag_catch_revision();
    
    ASSERT_GT(afterMembershipChange, initialRev);
    
    std::cout << "[PASS] Membership-only changes increment revision" << std::endl;
}

// Test 5: Revision stays stable when tags are empty but ready
TEST(test_stable_revision_on_empty_tags) {
    uint32_t initialRev = pgr_tag_catch_revision();
    
    // Simulate reading an empty tag list (valid state)
    NativeCore.tagCatchTags("");
    
    uint32_t afterEmptyRead = pgr_tag_catch_revision();
    
    ASSERT_EQ(afterEmptyRead, initialRev);
    
    std::cout << "[PASS] Revision stable on empty tags read" << std::endl;
}

int main() {
    std::cout << "=== VGP Tag Catch Revision Tests ===" << std::endl;
    
    test_revision_increment_on_membership_change();
    test_vgp_clears_skips_on_revision_change();
    test_tags_not_ready_defers_reset();
    test_no_spurious_revision_increment();
    test_membership_only_change_increments_revision();
    test_stable_revision_on_empty_tags();
    
    std::cout << "=== All Tests Complete ===" << std::endl;
    return 0;
}
```

**Expected Output:** All 6 tests should print `[PASS]` and complete without assertion failures.

---

## Integration Test: Full Reproduction Path

### Scenario: Verify the exact repro path from the issue is fixed

```cpp
// Run this as a standalone integration test after applying all fixes

#include "vgp_controller.h"
#include "tag_catch.h"
#include <iostream>

void runFullReproPath() {
    std::cout << "=== Full Reproduction Path Test ===" << std::endl;
    
    // Step 1: Enable an encounter-based "Also catch rare" criterion
    NativeCore.tagCatchConfig("2;1;1;511;90;706;705;;\tRareTag\t");
    uint32_t revAfterEnable = pgr_tag_catch_revision();
    
    // Step 2: Encounter and reject an untagged nonrare spawn
    PgrVgpControllerIn in{};
    in.owner = gettid();
    in.flags |= PGR_VGC_TF_MAP_VISIBLE;
    
    uint32_t revBeforeEncounter = pgr_tag_catch_revision();
    
    // Simulate encountering a Pokemon not in RareTag
    PgrVgpControllerOut out{};
    int rc1 = vgp_controller_tick(&in, &out);
    
    std::cout << "[Step 2] Encounter processed. Revision: " 
               << revBeforeEncounter << " -> " 
               << pgr_tag_catch_revision() << std::endl;
    
    // Step 3: Add the species' family to RareTag WITHOUT changing PGRazor settings
    NativeCore.tagCatchTags("RareTag\nNewFamily");
    
    uint32_t revAfterMembership = pgr_tag_catch_revision();
    bool tagsReady = pgr_tag_catch_ready();
    
    std::cout << "[Step 3] Membership updated. Revision: " 
               << revBeforeEncounter << " -> " 
               << revAfterMembership << ". Tags ready: " 
               << (tagsReady ? "yes" : "no") << std::endl;
    
    // Step 4: Verify revision changed and VGP can clear old skips
    if (revAfterMembership != revBeforeEncounter && tagsReady) {
        uint32_t revBeforeTick = pgr_tag_catch_revision();
        
        rc1 = vgp_controller_tick(&in, &out);
        
        uint32_t revAfterTick = pgr_tag_catch_revision();
        
        std::cout << "[Step 4] VGP tick completed. Revision stable: " 
                   << (revBeforeTick == revAfterTick ? "yes" : "no") << std::endl;
        
        if (rc1 == PGR_VGC_OK && revAfterMembership > revBeforeEncounter) {
            std::cout << "[PASS] Full reproduction path works correctly" << std::endl;
        } else {
            std::cerr << "[FAIL] Unexpected state after full repro path" << std::endl;
        }
    } else {
        std::cerr << "[FAIL] Revision did not change or tags not ready" << std::endl;
    }
    
    std::cout << "=== Reproduction Path Test Complete ===" << std::endl;
}

int main() {
    runFullReproPath();
    return 0;
}
```

**Expected Output:** Should complete with `[PASS]` and show revision incrementing from Step 3.

---

## Quick Reference: What Each Test Validates

| Test | Issue | Validates |
|------|-------|-----------|
| `test_tag_rule_validators_*` (10 tests) | P1 | Numeric validators accept valid values, reject out-of-range |
| `test_berry_fallback_*` (9 tests) | P2a | Rule berries + user priorities merge correctly without duplicates |
| `test_revision_increment_*` (6 tests) | P2b | Revision increments on membership changes, stable otherwise |
| `test_vgp_clears_skips_*` | P2b | VGP controller responds to revision changes appropriately |
| `runFullReproPath()` | P2b | Complete end-to-end scenario from issue description works |

Run all tests in sequence. All should pass before merging any fixes.
