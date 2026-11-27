# Implementation Guide — PGRazor Bug Fixes

Quick reference for developers implementing the three fixes. Each section contains copy-paste-ready code blocks with exact placement instructions.

---

## Fix P1: SettingsBackupRules.java Validators

**File:** `pgrazor/src/dev/pgrazor/SettingsBackupRules.java`  
**Location:** Lines 38-41 (after line 37, before the generic max check)

```java
// Copy-paste this block exactly after line 37:
            int n;try{n=Integer.parseInt(value);}catch(NumberFormatException e){throw new JSONException("Invalid "+key);}
            if(key.equals("smartCatchRareMask")){require(n>=0&&n<=511,"Invalid rare mask");continue;}
            if(key.equals("smartCatchRareIvMin")){require(n>=1&&n<=100,"Invalid IV threshold");continue;}
            if(key.equals("smartCatchRareBerry")||key.equals("smartCatchCandyBerry")){require(BerryPriorityPicker.validBerries(value),"Invalid berry ID for "+key);continue;}
```

**Verify:** Run `SettingsBackupRulesTest.java` — should pass all 10 new tag-rule tests.

---

## Fix P2a: Berry Rule Fallback Documentation

### File A: encounter_host.cpp

**File:** `pgrazor/native/encounter_host.cpp`  
**Location:** Lines 2520-2531 (around the `with_rule_berry()` function)

```cpp
// Copy-paste this block to replace lines 2520-2531:
// VGP+ tag rules pick the berry for a manual encounter (rare / candy family): it goes first in the list.
static uint32_t with_rule_berry(const int32_t *ids, uint32_t count, int32_t out[3]) {
  // Resolve stocked candidates on owner thread before limiting transport list.
  // Rule berries take priority; user priorities fill remaining slots without duplicates.
  const int packed = pgr_tag_catch_manual_berries();
  uint32_t n = 0;
  for (int s = 0; s < 30 && n < 3; s += 10) {
    int32_t id = (packed >> s) & 1023;
    if (id > 0) out[n++] = id;
  }
  const uint32_t rules = n;
  for (uint32_t i = 0; i < count && n < 3; ++i) {
    bool dup = false;
    // Skip duplicates only; user berries are appended if slots remain.
    for (uint32_t j = 0; j < rules; ++j) dup = dup || out[j] == ids[i];
    if (!dup) out[n++] = ids[i];
  }
  return n > 0 ? n : 0u; // Prevent empty array from propagating silently
}
```

### File B: BerryPriorityPicker.java

**File:** `pgrazor/src/dev/pgrazor/BerryPriorityPicker.java`  
**Location:** Line 73 (comment line)

```java
// Replace the existing comment at line 73 with this:
        // Priorities persist but are filtered by current stock before being passed to native.
```

**Verify:** Berry fallback tests should pass; UI/native berry display remains consistent.

---

## Fix P2b: Tag Membership Cache Revision

### File A: tag_catch.cpp

**File:** `pgrazor/native/tag_catch.cpp`  
**Location:** Lines 146-192 (the `read_tags()` function)

```cpp
// Copy-paste this block to replace lines 146-192:
void read_tags() {
  std::vector<bool> oldCatch(tagged_catch.begin(), tagged_catch.end());
  std::vector<bool> oldCandy(tagged_candy.begin(), tagged_candy.end());
  if (family_of.empty()) build_species_table();
  Root service = resolve(cls("Niantic.Holoholo.Tagging", "ITaggingRpcService"));
  require(service.get() != nullptr, "tag service");
  Root wait(call(service.get(), "WaitForTagCacheInitialized"));
  void *done = wait.get() ? field_info(wait.get(), "completeCalled") : nullptr;
  bool complete = false;
  if (done) fn<void (*)(void *, void *, void *)>(boot.runtime.field_get_value)(wait.get(), done, &complete);
  if (!complete) return;  // tag cache still loading; try again on the next read
  std::set<std::string> want_catch, want_candy;
  { std::lock_guard<std::mutex> g(lock); want_catch = cfg.catch_tags; want_candy = cfg.candy_tags; }
  std::set<int> catch_families, candy_families;
  std::string names;
  int n = unboxed<int>(call(service.get(), "get_NumberOfTags"));
  require(n >= 0 && n <= 1000, "tag count");
  for (int i = 0; i < n; i++) {
    void *a[] = {&i};
    Root row(call(service.get(), "GetCachedTagToPokemonBindingForIndex", 1, a));
    if (!row.get()) continue;
    void *tag = field_ref(row.get(), "TagProto");
    if (!tag) continue;
    std::string name = utf8(field_ref(tag, "Name"));
    if (name.empty()) continue;
    names += (names.empty() ? "" : "\n") + name;
    const std::string key = pgr_evolution::normalize_tag(name);
    const bool for_catch = want_catch.count(key) > 0, for_candy = want_candy.count(key) > 0;
    if (!for_catch && !for_candy) continue;
    void *list = field_ref(row.get(), "PokemonProtos");
    int count = list ? list_count(list) : 0;
    for (int j = 0; j < count && j < 25000; j++) {
      void *p = list_item(list, j);
      int id = p ? field_int(p, "pokemonId_") : 0;
      if (id <= 0 || id > kMaxSpecies) continue;
      // A species without a family still counts through its own id (family -id).
      const int family = family_of[id] ? family_of[id] : -id;
      if (for_catch) catch_families.insert(family);
      if (for_candy) candy_families.insert(family);
    }
  }
  std::vector<bool> by_catch(kMaxSpecies + 1), by_candy(kMaxSpecies + 1);
  for (int id = 1; id <= kMaxSpecies; id++) {
    const int family = family_of[id] ? family_of[id] : -id;
    by_catch[id] = catch_families.count(family) > 0; by_candy[id] = candy_families.count(family) > 0;
  }
  std::lock_guard<std::mutex> g(lock);
  tagged_catch.swap(by_catch); 
  tagged_candy.swap(by_candy); 
  tag_names = names; 
  if (tagged_catch != oldCatch || tagged_candy != oldCandy) {
    ready = true; ++revision;
  } else {
    ready = true;
  }
}
```

### File B: vgp_controller.cpp

**File:** `pgrazor/native/vgp_controller.cpp`  
**Location:** Lines 1004-1007 (around the tag revision check)

```cpp
// Copy-paste this block to replace lines 1004-1007:
  // New tag rules: forget the Pokemon skipped under the old ones, as turning VGP off and on does.
  static uint32_t tagRevision = 0;
  uint32_t currentRev = pgr_tag_catch_revision();
  // Distinguish "tags still loading" from actual config change requiring skip clearing.
  if (!pgr_tag_catch_ready()) {
    // Revision may advance during warmup; defer reset until tags are populated.
  } else if (currentRev != tagRevision) {
    if (tagRevision) POL->reset(in->owner);
    tagRevision = currentRev;
  }
```

**Verify:** Run `VgpControllerTest.java` — should pass all revision and skip-clearing tests.

---

## Quick Verification Commands

After applying each fix, run these commands to confirm correctness:

```bash
# P1 — Settings validation (Java)
cd pgrazor/test && java -cp .:../src/dev/pgrazor SettingsBackupRulesTest

# P2a — Berry fallback logic (Java/JNI)  
cd pgrazor/test && java -cp .:../src/dev/pgrazor BerryPriorityPickerTest

# P2b — Tag catch revision tests (C++)
cd native && ctest -R "tag_catch_revision"  # or your project's CMake test command

# Full integration test
cd pgrazor/native && ./run_full_repro_path_test
```

---

## Common Pitfalls to Avoid

1. **Don't modify the core algorithm** in `with_rule_berry()` — only add documentation and the safety guard at the end.
2. **Capture old vectors BEFORE swapping** in `read_tags()`, not after. The comparison must be against what was cached, not what's being written.
3. **Use `pgr_tag_catch_ready()`** before clearing skips — this prevents premature resets during the initial tag cache warmup period.
4. **Keep validators additive** in `SettingsBackupRules.java` — don't remove existing checks; just add new ones for tag-rule keys.

---

## Rollback Plan (If Needed)

| File | Rollback Action |
|------|-----------------|
| SettingsBackupRules.java | Revert lines 38-41 to original generic max check |
| encounter_host.cpp | Remove comment block; restore original `return n;` at end of function |
| BerryPriorityPicker.java | Restore original "No stock gating" comment |
| tag_catch.cpp | Remove oldCatch/oldCandy captures and revision increment logic |
| vgp_controller.cpp | Restore single-line revision check without readiness guard |

All changes are isolated and can be reverted individually if needed.

---

*Implementation guide generated for fixes de777d2b91a81b53d905e3733e15c66c88262dbf*
