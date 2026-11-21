#pragma once
#include <atomic>
#include <stdint.h>
#include <time.h>

// A catch in a Max Battle (Dynamax / Gigantamax), raid or Rocket encounter ends through the game's own
// exit for that battle, which also restores the map camera. Dismissing that catch's summary early skipped
// the exit and left the map blank (Pixel, 2026-11-20: after a Dynamax catch, and on some raids).
// quickball_host notes such an encounter when its catch result arrives; shared_host then leaves the next
// summary to the game. Include at global scope only.
inline std::atomic<uint64_t> g_pgr_max_catch_ms{0};
inline uint64_t pgr_max_catch_clock_ms() {
  timespec t{};
  clock_gettime(CLOCK_BOOTTIME, &t);
  return uint64_t(t.tv_sec) * 1000 + uint64_t(t.tv_nsec) / 1000000;
}
inline void pgr_note_max_catch() { g_pgr_max_catch_ms.store(pgr_max_catch_clock_ms(), std::memory_order_relaxed); }
// True once for the summary that follows a Max encounter throw within the last 60 s.
inline bool pgr_take_max_catch() {
  const uint64_t at = g_pgr_max_catch_ms.exchange(0, std::memory_order_relaxed);
  return at && pgr_max_catch_clock_ms() - at < 60000;
}
