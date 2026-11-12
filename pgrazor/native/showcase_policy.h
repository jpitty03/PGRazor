#ifndef PGR_SHOWCASE_POLICY_H
#define PGR_SHOWCASE_POLICY_H

// Pure C++ ranking policy for proposed Showcase keepers, grouped by type.
// Callers MUST supply scores from one consistent scoring mode within each bucket.
// Normal species-normalized predicted points can rank future candidates. They
// must not be presented as exact points for a contest using a different scalar.
// This policy alone neither protects inventory nor authorizes any transfer.
//
// No managed calls, hooks or cache. Temporary sets/vectors are bounded by the input
// cap. The
// caller hands over a validated snapshot of the box and receives, per canonical type,
// the IDs it should hold. The dataset is validated as a whole before any selection so
// a single malformed row fails the whole pass closed (empty output) rather than
// silently keeping the wrong Pokemon.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pgr_showcase {

constexpr size_t kMaxPokemon = 25000;  // refuse datasets larger than a full box inventory
constexpr unsigned kTypes = 18;        // the canonical type indices (bits 0..17)

struct Row {
  uint64_t id = 0;        // inventory id; nonzero and unique within the dataset
  uint64_t created = 0;   // capture timestamp; older (smaller) wins a score tie
  int species = 0;        // 1..2000
  uint32_t types = 0;     // bitmask over the 18 canonical types; 1 or 2 bits set
  std::array<double, kTypes> scores{}; // comparable score for each eligible type
};

struct Selection {
  // IDs to keep for each type, best first. A dual-type Pokemon appears under both types.
  std::array<std::vector<uint64_t>, kTypes> types;

  bool contains(uint64_t id) const {
    if (id == 0) return false;
    for (const auto& bucket : types)
      for (uint64_t value : bucket)
        if (value == id) return true;
    return false;
  }
};

namespace detail {

// Nonzero, no bits beyond the 18 canonical types, and at most two set (single or dual).
inline bool valid_types(uint32_t mask) {
  if (mask == 0) return false;
  if (mask >> kTypes) return false;
  unsigned bits = 0;
  for (uint32_t remaining = mask; remaining; remaining &= remaining - 1) ++bits;
  return bits <= 2;
}

inline bool valid_row(const Row& row) {
  if (row.id == 0) return false;
  if (row.species < 1 || row.species > 2000) return false;
  for (unsigned t = 0; t < kTypes; ++t)
    if ((row.types & (uint32_t(1) << t)) &&
        (!(row.scores[t] > 0.0) || !std::isfinite(row.scores[t]))) return false;
  if (!valid_types(row.types)) return false;
  if (row.created == 0) return false;
  return true;
}

// Highest score first; ties prefer the older (smaller) created timestamp, then the
// smaller id. Ids are unique, so this is a strict total order over distinct rows.
inline bool better(const Row& a, const Row& b, unsigned type) {
  if (a.scores[type] != b.scores[type]) return a.scores[type] > b.scores[type];
  if (a.created != b.created) return a.created < b.created;
  return a.id < b.id;
}

}  // namespace detail

// Fill out with up to `slots` keepers per type. Returns false (and empties out) when
// slots is outside 1..3 or any row in the whole dataset is invalid or shares an id.
// The excluded row (exclude != 0) is still validated but never selected. rows is never
// modified. On success out is replaced atomically; on failure it is left empty.
inline bool select(const std::vector<Row>& rows, unsigned slots, Selection& out, uint64_t exclude = 0) {
  out = Selection{};
  if (slots < 1 || slots > 3) return false;
  if (rows.size() > kMaxPokemon) return false;

  std::unordered_set<uint64_t> seen;
  seen.reserve(rows.size() * 2);
  for (const Row& row : rows) {
    if (!detail::valid_row(row)) return false;
    if (!seen.insert(row.id).second) return false;  // duplicate id
  }

  std::array<std::vector<const Row*>, kTypes> buckets;
  for (const Row& row : rows) {
    if (exclude != 0 && row.id == exclude) continue;
    for (unsigned t = 0; t < kTypes; ++t)
      if (row.types & (uint32_t(1) << t)) buckets[t].push_back(&row);
  }

  Selection built;
  for (unsigned t = 0; t < kTypes; ++t) {
    auto& bucket = buckets[t];
    std::sort(bucket.begin(), bucket.end(),
              [t](const Row* a, const Row* b) { return detail::better(*a, *b, t); });
    const size_t take = std::min(static_cast<size_t>(slots), bucket.size());
    built.types[t].reserve(take);
    for (size_t i = 0; i < take; ++i) built.types[t].push_back(bucket[i]->id);
  }
  out = std::move(built);
  return true;
}

// Parse the per-user config wire "S1|<n>" where n is 0..3 (0 disables showcasing).
// Only these exact four-byte strings are accepted; anything else sets slots to 0 and
// returns false.
inline bool parse(const char* text, size_t length, unsigned& slots) {
  slots = 0;
  if (!text || length != 4) return false;
  if (text[0] != 'S' || text[1] != '1' || text[2] != '|') return false;
  const char digit = text[3];
  if (digit < '0' || digit > '3') return false;
  slots = static_cast<unsigned>(digit - '0');
  return true;
}

// IDs that were kept in `before` but are kept by no type in `after`. An id that merely
// lost one type while still winning another is NOT displaced, because membership is
// judged over the union of all types. Each displaced id appears once.
inline std::vector<uint64_t> displaced(const Selection& before, const Selection& after) {
  std::unordered_set<uint64_t> after_ids;
  for (const auto& bucket : after.types)
    for (uint64_t id : bucket) after_ids.insert(id);

  std::vector<uint64_t> result;
  std::unordered_set<uint64_t> emitted;
  for (const auto& bucket : before.types)
    for (uint64_t id : bucket)
      if (!after_ids.count(id) && emitted.insert(id).second) result.push_back(id);
  return result;
}

}  // namespace pgr_showcase

#endif
