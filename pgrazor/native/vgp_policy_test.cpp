#include "vgp_policy.h"
#include "pgrazor_native_api.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <thread>
#include <unistd.h>

// Real-API harness for feature 25 (native VGP scheduling / retry policy). It resolves the table
// through the exported pgrazor_native_api v2 registry (query_feature(25,1)) and drives the bounded
// quarantine state machine directly: scan cadence (arm/due), catch/spin alternation (order), the
// seen-key quarantine with monotonic expiry and capacity ceiling (seen/touch/sweep), the catch-delay
// table, and the uncertain-response latch (block/clear_block) which preserves the quarantine across a
// mask toggle and is dropped only by reset(). The module installs NO hook and NEVER calls back into
// JS. The coordinator builds and runs this against libpgrazor.so after registering query_feature
// case 25 and linking vgp_policy.cpp.

namespace {

using GetApiFn = const void *(*)(uint32_t);

constexpr int32_t kSuccess = 11, kFlee = 13;             // -> terminal_delay_ms
constexpr int32_t kEscape = 12, kMissed = 14, kReplace = 16;  // -> retry_delay_ms

PgrVgpPolicyConfig make_config() {
  PgrVgpPolicyConfig c{};
  c.version = PGR_VP_CONFIG_VERSION;
  c.size = sizeof(PgrVgpPolicyConfig);
  c.scan_interval_ms = 2500;
  c.catch_seen_ms = 30000;
  c.spin_seen_ms = 300000;
  c.reject_delay_ms = 30000;
  c.terminal_delay_ms = 3600000;
  c.retry_delay_ms = 5000;
  c.default_delay_ms = 30000;
  c.max_keys = 4;  // a tiny ceiling so the capacity path is exercised
  c.status_terminal[0] = kSuccess; c.status_terminal[1] = kFlee;
  c.status_terminal[2] = -1; c.status_terminal[3] = -1;
  c.status_retry[0] = kEscape; c.status_retry[1] = kMissed; c.status_retry[2] = kReplace;
  c.status_retry[3] = -1;
  return c;
}

uint64_t this_tid() { return static_cast<uint64_t>(static_cast<pid_t>(::gettid())); }

const PgrVgpPolicyApi *g_api = nullptr;

PgrVgpPolicySample take_sample() {
  PgrVgpPolicySample s{};
  assert(g_api->sample(&s) == PGR_VP_OK);
  assert(s.version == 1 && s.size == sizeof(PgrVgpPolicySample));
  return s;
}

int seen(uint64_t tid, const std::string &k) {
  return g_api->seen(tid, k.c_str(), static_cast<uint32_t>(k.size()));
}
int touch(uint64_t tid, const std::string &k, uint64_t ms) {
  return g_api->touch(tid, k.c_str(), static_cast<uint32_t>(k.size()), ms);
}

}  // namespace

int main(int argc, char **argv) {
  const char *so_path = argc > 1 ? argv[1] : "./libpgrazor.so";
  void *lib = ::dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
  if (!lib) { std::fprintf(stderr, "dlopen(%s) failed: %s\n", so_path, ::dlerror()); return 1; }
  auto getter = reinterpret_cast<GetApiFn>(::dlsym(lib, "pgrazor_native_api"));
  assert(getter);
  const auto *core = static_cast<const pgrazor_native_api_v2 *>(getter(2));
  assert(core && core->size == 48);
  assert(core->query_feature(25, 99) == nullptr);
  const auto *api = static_cast<const PgrVgpPolicyApi *>(core->query_feature(25, 1));
  assert(api != nullptr && api->version == 1 && api->size == sizeof(PgrVgpPolicyApi));
  g_api = api;

  std::printf("ABI sizes: config=%zu sample=%zu table=%zu\n",
              sizeof(PgrVgpPolicyConfig), sizeof(PgrVgpPolicySample), sizeof(PgrVgpPolicyApi));
  assert(sizeof(PgrVgpPolicyConfig) == 128 && sizeof(PgrVgpPolicySample) == 64 &&
         sizeof(PgrVgpPolicyApi) == 112);

  const uint64_t tid = this_tid();
  PgrVgpPolicyConfig cfg = make_config();

  // 1. Validation + not-installed + idempotence + reconfig.
  assert(api->install(nullptr) == PGR_VP_ERR_NULL_ARG);
  PgrVgpPolicyConfig bad = cfg; bad.version = 2;
  assert(api->install(&bad) == PGR_VP_ERR_BAD_VERSION);
  bad = cfg; bad.size = sizeof(PgrVgpPolicyConfig) - 1;
  assert(api->install(&bad) == PGR_VP_ERR_BAD_SIZE);
  bad = cfg; bad.max_keys = PGR_VP_MAX_KEYS + 1;
  assert(api->install(&bad) == PGR_VP_ERR_BAD_CONFIG);
  bad = cfg; bad.max_keys = 0;
  assert(api->install(&bad) == PGR_VP_ERR_BAD_CONFIG);
  assert(api->due(tid) == PGR_VP_ERR_NOT_INSTALLED);
  assert(touch(tid, "p1", 1000) == PGR_VP_ERR_NOT_INSTALLED);

  assert(api->install(&cfg) == PGR_VP_OK);
  assert(api->install(&cfg) == PGR_VP_OK);  // idempotent
  bad = cfg; bad.retry_delay_ms = 1;
  assert(api->install(&bad) == PGR_VP_ERR_RECONFIG);
  assert(api->update(1, tid) == PGR_VP_OK);

  // 2. Off-owner refusal (owner already latched here).
  std::thread([&]() {
    const uint64_t other = this_tid();
    assert(g_api->due(other) == PGR_VP_ERR_OFF_OWNER);
    assert(g_api->order(other, nullptr) == PGR_VP_ERR_NULL_ARG);  // null out checked first
    uint32_t o = 9;
    assert(g_api->order(other, &o) == PGR_VP_ERR_OFF_OWNER);
    assert(touch(other, "p9", 1000) == PGR_VP_ERR_OFF_OWNER);
  }).join();
  assert(api->due(tid + 1) == PGR_VP_ERR_OFF_OWNER);  // mismatched proven tid

  // 3. Scan cadence: arm sets now+delay; due is OK until it elapses, then YES.
  assert(api->arm(tid, 10000) == PGR_VP_OK);
  assert(api->due(tid) == PGR_VP_OK);          // not elapsed -> not due
  assert(api->arm(tid, 1) == PGR_VP_OK);
  ::usleep(3000);
  assert(api->due(tid) == PGR_VP_YES);         // cadence elapsed -> due

  // 4. Alternation: order flips catch-first / spin-first on successive calls.
  api->reset(tid);
  uint32_t o0 = 9, o1 = 9, o2 = 9;
  assert(api->order(tid, &o0) == PGR_VP_OK && o0 == 0);  // first scan: catch-first
  assert(api->order(tid, &o1) == PGR_VP_OK && o1 == 1);  // then spin-first
  assert(api->order(tid, &o2) == PGR_VP_OK && o2 == 0);

  // 5. Quarantine: touch suppresses; seen reports YES; expiry clears it; sweep compacts + counts.
  api->reset(tid);
  assert(seen(tid, "p100") == PGR_VP_OK);               // absent
  assert(touch(tid, "p100", 10000) == PGR_VP_OK);
  assert(seen(tid, "p100") == PGR_VP_YES);              // suppressed
  assert(touch(tid, "s-fortA", 10000) == PGR_VP_OK);
  assert(touch(tid, "p200", 1) == PGR_VP_OK);           // expires almost immediately
  ::usleep(3000);
  assert(seen(tid, "p200") == PGR_VP_OK);               // expired -> not suppressed (pre-sweep)
  uint32_t size = 99;
  assert(api->sweep(tid, &size) == PGR_VP_OK && size == 2);  // p200 swept, p100 + s-fortA remain
  assert(seen(tid, "p100") == PGR_VP_YES && seen(tid, "s-fortA") == PGR_VP_YES);

  // 6. Capacity ceiling (max_keys == 4): a new key past the ceiling is FULL; a refresh still works.
  api->reset(tid);
  for (int i = 0; i < 4; ++i) { char k[16]; std::snprintf(k, sizeof k, "p%d", i); assert(touch(tid, k, 60000) == PGR_VP_OK); }
  assert(touch(tid, "p-overflow", 60000) == PGR_VP_FULL);   // at capacity, new key refused
  assert(touch(tid, "p0", 90000) == PGR_VP_OK);             // refresh an existing key still allowed
  uint32_t full_size = 0;
  assert(api->sweep(tid, &full_size) == PGR_VP_YES && full_size == 4);  // sweep reports FULL at ceiling

  // 7. Catch-delay table maps terminal / retry / default exactly.
  uint64_t ms = 0;
  assert(api->catch_delay(kSuccess, &ms) == PGR_VP_OK && ms == 3600000);
  assert(api->catch_delay(kFlee, &ms) == PGR_VP_OK && ms == 3600000);
  assert(api->catch_delay(kEscape, &ms) == PGR_VP_OK && ms == 5000);
  assert(api->catch_delay(kMissed, &ms) == PGR_VP_OK && ms == 5000);
  assert(api->catch_delay(kReplace, &ms) == PGR_VP_OK && ms == 5000);
  assert(api->catch_delay(999, &ms) == PGR_VP_OK && ms == 30000);
  assert(api->catch_delay(kSuccess, nullptr) == PGR_VP_ERR_NULL_ARG);

  // 8. Uncertain latch: block() stops due(); clear_block() resumes and PRESERVES the quarantine;
  //    only reset() drops it.
  api->reset(tid);
  assert(touch(tid, "p500", 60000) == PGR_VP_OK);
  assert(api->arm(tid, 1) == PGR_VP_OK); ::usleep(3000);
  assert(api->due(tid) == PGR_VP_YES);
  assert(api->block(tid) == PGR_VP_OK);
  assert(api->due(tid) == PGR_VP_OK);                   // blocked -> never due
  assert(seen(tid, "p500") == PGR_VP_YES);              // quarantine intact while blocked
  assert(api->clear_block(tid) == PGR_VP_OK);
  assert(seen(tid, "p500") == PGR_VP_YES);              // mask toggle preserved the quarantine
  assert(api->arm(tid, 1) == PGR_VP_OK); ::usleep(3000);
  assert(api->due(tid) == PGR_VP_YES);                  // resumes after clear_block
  assert(api->reset(tid) == PGR_VP_OK);
  assert(seen(tid, "p500") == PGR_VP_OK);               // reset dropped it

  // 9. Bad-key rejection.
  assert(g_api->seen(tid, nullptr, 3) == PGR_VP_ERR_BAD_KEY);
  assert(g_api->touch(tid, "x", 0, 1000) == PGR_VP_ERR_BAD_KEY);
  assert(g_api->touch(tid, "x", PGR_VP_MAX_KEY_LEN + 1, 1000) == PGR_VP_ERR_BAD_KEY);

  // 10. Telemetry reflects the drive above (non-zero counters, sane flags).
  PgrVgpPolicySample sm = take_sample();
  assert(sm.scans >= 1 && sm.marks >= 1 && sm.evictions >= 1 && sm.blocks >= 1 && sm.drops >= 1);
  assert(sm.full_rejects >= 1 && sm.blocked == 0 && sm.seen_size == 0);

  std::printf("vgp_policy_test passed: ABI/validation/idempotence/reconfig, off-owner refusal, scan "
              "cadence (arm/due), catch/spin alternation, quarantine touch/seen/expiry/sweep, capacity "
              "ceiling (FULL + refresh), catch-delay table, uncertain latch (block/clear preserves "
              "quarantine, reset drops), bad-key rejection, telemetry\n");
  return 0;
}
