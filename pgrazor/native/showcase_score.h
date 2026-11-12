#pragma once
#include <cstdint>

namespace pgr_showcase {
// Bindings must be resolved with exact metadata signatures by the owner. This
// reader never chooses a reference species: the caller supplies either the
// actual contest or an explicitly labeled prediction reference. Prediction
// mode uses a locally constructed zero-reference ContestProto and the game's
// normal species/form scoring; it is not an exact claim about future contests.
struct ScoreBindings {
  uintptr_t (*root)(void*, int) = nullptr;
  void* (*target)(uintptr_t) = nullptr;
  void (*release)(uintptr_t) = nullptr;
  void* (*object_class)(void*) = nullptr;
  void* (*invoke)(const void*, void*, void**, void**) = nullptr;
  void* (*unbox)(void*) = nullptr;
  void (*field_get)(void*, void*, void*) = nullptr;
  // CLOCK_BOOTTIME milliseconds; zero means failure. owner_valid must use the
  // proven Unity scheduler owner, not merely compare an arbitrary supplied TID.
  // Both callbacks are native-only, nonallocating and make no managed calls.
  uint64_t (*boottime_ms)() = nullptr;
  bool (*owner_valid)(uint64_t, uint64_t) = nullptr;
  void* service_class = nullptr;
  void* pokemon_class = nullptr;
  void* contest_class = nullptr;
  void* single_class = nullptr;
  const void* score_method = nullptr;
  void* pokemon_id_field = nullptr;
};
struct ScoreRequest {
  uint64_t owner = 0, deadline_ms = 0, pokemon_id = 0;
  // Borrowed strong handles, held by caller throughout this synchronous call.
  // Raw pointers are deliberately not accepted across rooting safepoints.
  uintptr_t service = 0, pokemon = 0, contest = 0;
  uint64_t generation = 0; // scheduler generation that authorized this request
};
enum class ScoreStatus { valid, refused, bad_binding, bad_identity, managed_error, bad_score };
ScoreStatus read_score(const ScoreBindings&, const ScoreRequest&, double& score);
}
