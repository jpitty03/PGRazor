// Host test for the pure C++ Showcase keeper policy (showcase_policy.h).
// Build and run via: python pgrazor/test/test_showcase_native.py
// Or directly with any C++17 host compiler; prints "PASS ..." on success.
#include "showcase_policy.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

using namespace pgr_showcase;

namespace {

int g_checks = 0;

void check(bool condition, const char* label) {
  ++g_checks;
  if (!condition) {
    std::cerr << "FAILED: " << label << '\n';
    std::exit(1);
  }
}

// Canonical type bits used in the tests.
constexpr uint32_t kWater = uint32_t(1) << 0;
constexpr uint32_t kFire = uint32_t(1) << 1;
constexpr uint32_t kGrass = uint32_t(1) << 2;
constexpr uint32_t kLast = uint32_t(1) << 17;  // 18th type (index 17)

Row row(uint64_t id, uint32_t types, double score, uint64_t created, int species = 1) {
  Row r;
  r.id = id;
  r.types = types;
  for (unsigned t=0;t<kTypes;++t) if(types & (uint32_t(1)<<t)) r.scores[t]=score;
  r.created = created;
  r.species = species;
  return r;
}

bool same_rows(const std::vector<Row>& a, const std::vector<Row>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    const Row& x = a[i];
    const Row& y = b[i];
    if (x.id != y.id || x.created != y.created || x.species != y.species ||
        x.types != y.types || x.scores != y.scores)
      return false;
  }
  return true;
}

std::vector<uint64_t> sorted(std::vector<uint64_t> v) {
  std::sort(v.begin(), v.end());
  return v;
}

// Run select on a throwaway output; used where only the accept/reject verdict matters.
bool runs(std::vector<Row> rows, unsigned slots, uint64_t exclude = 0) {
  Selection sink;
  return select(rows, slots, sink, exclude);
}

void test_slots_and_competition() {
  // Water (3 species compete), Fire (1), Grass (2). Scores are species-agnostic.
  std::vector<Row> rows = {
      row(10, kWater, 10.0, 100, 5),
      row(11, kWater, 30.0, 100, 7),
      row(12, kWater, 20.0, 100, 9),
      row(20, kFire, 5.0, 100, 11),
      row(30, kGrass, 8.0, 100, 13),
      row(31, kGrass, 9.0, 100, 14),
  };

  Selection one, two, three;
  check(select(rows, 1, one), "slots=1 selects");
  check(one.types[0] == std::vector<uint64_t>{11}, "slots=1 picks highest water across species");
  check(one.types[1] == std::vector<uint64_t>{20}, "slots=1 lone fire");
  check(one.types[2] == std::vector<uint64_t>{31}, "slots=1 best grass");

  check(select(rows, 2, two), "slots=2 selects");
  check((two.types[0] == std::vector<uint64_t>{11, 12}), "slots=2 top two water, best first");
  check((two.types[2] == std::vector<uint64_t>{31, 30}), "slots=2 grass ordered by score");
  check(two.types[1] == std::vector<uint64_t>{20}, "slots=2 but only one fire exists");

  check(select(rows, 3, three), "slots=3 selects");
  check((three.types[0] == std::vector<uint64_t>{11, 12, 10}), "slots=3 full water ranking");
  check(three.contains(11) && three.contains(10) && !three.contains(99), "contains reflects selection");

  check(!select(rows, 0, one) && one.types[0].empty(), "slots=0 rejected, out emptied");
  check(!select(rows, 4, two) && two.types[0].empty(), "slots=4 rejected, out emptied");
}

void test_dual_type_membership() {
  std::vector<Row> rows = {
      row(1, kWater | kGrass, 50.0, 100, 7),  // dual-type leads both
      row(2, kWater, 40.0, 100, 8),
      row(3, kGrass, 10.0, 100, 9),
  };
  Selection sel;
  check(select(rows, 1, sel), "dual-type dataset selects");
  check(sel.types[0] == std::vector<uint64_t>{1}, "dual-type wins water");
  check(sel.types[2] == std::vector<uint64_t>{1}, "same dual-type wins grass");
  check(sel.contains(1), "dual-type kept");

  // A dual-type spanning the last canonical index is valid and placed in both buckets.
  std::vector<Row> edge = {row(5, kWater | kLast, 1.0, 1, 1)};
  Selection e;
  check(select(edge, 1, e), "dual-type with index-17 valid");
  check(e.types[0] == std::vector<uint64_t>{5} && e.types[17] == std::vector<uint64_t>{5},
        "index-17 membership");
}

void test_type_specific_scores() {
  auto a=row(1,kWater|kFire,10,1);
  auto b=row(2,kWater|kFire,20,2);
  a.scores[1]=30;
  Selection picked;
  check(select({a,b},1,picked),"type-specific scores accepted");
  check(picked.types[0]==std::vector<uint64_t>{2},"water uses water score");
  check(picked.types[1]==std::vector<uint64_t>{1},"fire uses fire score");
  a.scores[1]=std::numeric_limits<double>::quiet_NaN();
  check(!select({a,b},1,picked),"invalid second eligible score fails closed");
  check(!picked.contains(1)&&!picked.contains(2),"invalid second score clears all buckets");
}

void test_tie_breaking_and_extreme_ids() {
  // Equal score: older (smaller created) retained for slots=1.
  std::vector<Row> aged = {
      row(100, kFire, 25.0, 500, 2),
      row(101, kFire, 25.0, 200, 3),  // older
      row(102, kFire, 25.0, 900, 4),
  };
  Selection sel;
  check(select(aged, 1, sel), "tie dataset selects");
  check(sel.types[1] == std::vector<uint64_t>{101}, "oldest retained on score tie");
  check(select(aged, 3, sel) && (sel.types[1] == std::vector<uint64_t>{101, 100, 102}),
        "full tie order by created ascending");

  // Equal score and created: smaller unsigned id wins, even at the top of the range.
  const uint64_t big = std::numeric_limits<uint64_t>::max();
  std::vector<Row> extreme = {
      row(big, kWater, 7.0, 10, 1),
      row(big - 1, kWater, 7.0, 10, 1),
  };
  Selection x;
  check(select(extreme, 1, x), "extreme-id dataset selects");
  check(x.types[0] == std::vector<uint64_t>{big - 1}, "smaller unsigned id wins full tie");
  check(select(extreme, 2, x) && (x.types[0] == std::vector<uint64_t>{big - 1, big}),
        "extreme ids ordered ascending on tie");
}

void test_replacement_and_displaced() {
  std::vector<Row> before_rows = {
      row(1, kWater, 10.0, 100, 1),
      row(2, kWater, 5.0, 100, 2),
      row(3, kFire, 8.0, 100, 3),
  };
  Selection before;
  check(select(before_rows, 1, before), "before selection");
  check(before.types[0] == std::vector<uint64_t>{1}, "before water keeper");

  // A new higher-scoring water row takes the slot.
  std::vector<Row> after_rows = before_rows;
  after_rows.push_back(row(9, kWater, 20.0, 100, 4));
  Selection after;
  check(select(after_rows, 1, after), "after selection");
  check(after.types[0] == std::vector<uint64_t>{9}, "replacement water keeper");
  check(displaced(before, after) == std::vector<uint64_t>{1}, "old keeper displaced");
  check(displaced(after, after).empty(), "identical selection displaces nothing");

  // Losing one type while still winning another is NOT displaced.
  Selection multi_before, multi_after;
  multi_before.types[0] = {7, 8};  // id 7 kept for water and fire
  multi_before.types[1] = {7};
  multi_after.types[0] = {8};  // id 7 lost water...
  multi_after.types[1] = {7};  // ...but still kept for fire
  check(displaced(multi_before, multi_after).empty(), "type loss with other win not displaced");

  // Full drop and uniqueness: an id kept under two types is reported once.
  Selection full_before, empty_after;
  full_before.types[0] = {42};
  full_before.types[1] = {42};
  full_before.types[2] = {43};
  check(sorted(displaced(full_before, empty_after)) == (std::vector<uint64_t>{42, 43}),
        "all dropped ids unique");
}

void test_exclude() {
  std::vector<Row> rows = {
      row(1, kWater, 30.0, 100, 1),  // would win water
      row(2, kWater, 20.0, 100, 2),
      row(3, kFire, 5.0, 100, 3),
  };
  Selection sel;
  check(select(rows, 1, sel, /*exclude=*/1), "exclude dataset selects");
  check(sel.types[0] == std::vector<uint64_t>{2}, "excluded top keeper skipped");
  check(!sel.contains(1), "excluded id absent from selection");

  // The excluded row is still validated: a malformed excluded row fails the pass.
  std::vector<Row> bad = rows;
  bad[0].scores[0] = std::numeric_limits<double>::quiet_NaN();
  Selection fail;
  check(!select(bad, 1, fail, /*exclude=*/1) && fail.types[0].empty(),
        "excluded row still validated");

  // Excluding an id not present in the dataset changes nothing.
  Selection none;
  check(select(rows, 1, none, /*exclude=*/999) && none.types[0] == std::vector<uint64_t>{1},
        "absent exclude is a no-op");
}

void test_validation_and_atomicity() {
  const Row good = row(1, kWater, 1.0, 1, 1);

  // Duplicate id.
  check(!runs({good, good}, 1), "duplicate id rejected");

  // Species bounds.
  check(!runs({row(1, kWater, 1.0, 1, 0)}, 1), "species 0 rejected");
  check(!runs({row(1, kWater, 1.0, 1, 2001)}, 1), "species 2001 rejected");
  check(runs({row(1, kWater, 1.0, 1, 1)}, 1) && runs({row(1, kWater, 1.0, 1, 2000)}, 1),
        "species 1 and 2000 accepted");

  // Score: zero, negative, NaN, +Inf, -Inf.
  check(!runs({row(1, kWater, 0.0, 1, 1)}, 1), "score 0 rejected");
  check(!runs({row(1, kWater, -1.0, 1, 1)}, 1), "negative score rejected");
  check(!runs({row(1, kWater, std::numeric_limits<double>::quiet_NaN(), 1, 1)}, 1),
        "NaN score rejected");
  check(!runs({row(1, kWater, std::numeric_limits<double>::infinity(), 1, 1)}, 1),
        "+Inf score rejected");
  check(!runs({row(1, kWater, -std::numeric_limits<double>::infinity(), 1, 1)}, 1),
        "-Inf score rejected");

  // Type mask: zero, out-of-range bit, three bits.
  check(!runs({row(1, 0u, 1.0, 1, 1)}, 1), "zero type mask rejected");
  check(!runs({row(1, uint32_t(1) << 18, 1.0, 1, 1)}, 1), "type bit 18 rejected");
  check(!runs({row(1, kWater | kFire | kGrass, 1.0, 1, 1)}, 1), "three-type mask rejected");
  check(runs({row(1, kWater | kFire, 1.0, 1, 1)}, 1), "two-type mask accepted");

  // Created timestamp.
  check(!runs({row(1, kWater, 1.0, 0, 1)}, 1), "created 0 rejected");

  // Nonzero id.
  check(!runs({row(0, kWater, 1.0, 1, 1)}, 1), "zero id rejected");

  // Atomic fail-closed: a prior valid selection is emptied when the next dataset is bad.
  Selection sel;
  check(select({good}, 1, sel) && sel.contains(1), "seed a valid selection");
  std::vector<Row> poisoned = {good, row(2, 0u, 1.0, 1, 1)};  // second row invalid
  check(!select(poisoned, 1, sel), "poisoned dataset rejected");
  for (const auto& bucket : sel.types) check(bucket.empty(), "output emptied on failure");
}

void test_cap_and_unchanged_input() {
  // Exactly the cap is accepted; one over is rejected.
  std::vector<Row> full;
  full.reserve(kMaxPokemon + 1);
  for (size_t i = 0; i < kMaxPokemon; ++i)
    full.push_back(row(static_cast<uint64_t>(i + 1), kWater, 1.0, i + 1, 1));
  Selection sel;
  check(select(full, 1, sel), "dataset at cap accepted");
  check(sel.types[0] == std::vector<uint64_t>{1}, "cap dataset oldest wins the tie");

  full.push_back(row(static_cast<uint64_t>(kMaxPokemon + 1), kWater, 1.0, kMaxPokemon + 1, 1));
  check(full.size() == kMaxPokemon + 1, "dataset is one over the cap");
  Selection over;
  check(!select(full, 1, over) && over.types[0].empty(), "dataset over cap rejected");

  // Input is not mutated, even on a rejected pass.
  std::vector<Row> rows = {
      row(1, kWater | kFire, 10.0, 100, 7),
      row(2, kGrass, 5.0, 50, 8),
  };
  const std::vector<Row> snapshot = rows;
  Selection ok;
  check(select(rows, 2, ok), "valid select leaves input unchanged");
  check(same_rows(rows, snapshot), "rows unchanged after success");
  std::vector<Row> bad = rows;
  bad.push_back(row(0, kWater, 1.0, 1, 1));
  const std::vector<Row> bad_snapshot = bad;
  Selection fail;
  check(!select(bad, 1, fail), "invalid select returns false");
  check(same_rows(bad, bad_snapshot), "rows unchanged after failure");
}

void test_config_parse() {
  unsigned slots = 7;
  check(parse("S1|0", 4, slots) && slots == 0, "S1|0 parses as disabled");
  check(parse("S1|1", 4, slots) && slots == 1, "S1|1 parses as one slot");
  check(parse("S1|2", 4, slots) && slots == 2, "S1|2 parses as two slots");
  check(parse("S1|3", 4, slots) && slots == 3, "S1|3 parses as three slots");

  slots = 7;
  check(!parse("S1|4", 4, slots) && slots == 0, "S1|4 out of range");
  check(!parse("S1|9", 4, slots) && slots == 0, "S1|9 out of range");
  check(!parse("S1|-", 4, slots) && slots == 0, "non-digit rejected");
  check(!parse("S2|1", 4, slots) && slots == 0, "wrong version rejected");
  check(!parse("s1|1", 4, slots) && slots == 0, "lowercase prefix rejected");
  check(!parse("S1-1", 4, slots) && slots == 0, "wrong delimiter rejected");
  check(!parse("S1|2x", 5, slots) && slots == 0, "trailing byte rejected");
  check(!parse("S1|", 3, slots) && slots == 0, "short string rejected");
  check(!parse("", 0, slots) && slots == 0, "empty string rejected");
  check(!parse(nullptr, 4, slots) && slots == 0, "null pointer rejected");

  // A disabled config feeds slots=0, which select then refuses.
  check(parse("S1|0", 4, slots) && slots == 0, "disabled config");
  Selection sel;
  check(!select({row(1, kWater, 1.0, 1, 1)}, slots, sel), "disabled slots refused by select");
}

}  // namespace

void test_showcase_score();
void test_showcase_tags();
void test_showcase_bindings();
void test_showcase_retention();
int main() {
  test_showcase_retention();
  test_showcase_score();
  test_showcase_tags();
  test_showcase_bindings();
  test_slots_and_competition();
  test_dual_type_membership();
  test_type_specific_scores();
  test_tie_breaking_and_extreme_ids();
  test_replacement_and_displaced();
  test_exclude();
  test_validation_and_atomicity();
  test_cap_and_unchanged_input();
  test_config_parse();
  std::cout << "PASS native showcase policy: slots 1-3, cross-species per-type competition, "
               "dual-type membership, replacement/displaced, tie-break (older then id), extreme "
               "ids, duplicate/malformed rejection, cap, atomic fail-closed, unchanged input, "
               "config parse (" << g_checks << " checks)\n";
  return 0;
}
