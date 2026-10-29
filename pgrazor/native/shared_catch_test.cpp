#include "shared_catch.h"
#include "pgrazor_native_api.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

// Native-process unit harness for feature 51 (shared catch orchestration). This module installs NO hook
// and makes NO managed/il2cpp call -- it is pure bounded bookkeeping -- so the meaningful actual-native
// test is this process-level harness resolved through the exported pgrazor_native_api v2 registry
// (query_feature(51,1)), NOT a Dobby attach (there is no target to hook). The coordinator builds and runs
// it against libpgrazor.so after registering query_feature case 51 and linking shared_catch.cpp.

namespace {

using GetApiFn = const void *(*)(uint32_t);
const PgrSharedCatchApi *g_api = nullptr;

uint64_t this_tid() { return static_cast<uint64_t>(static_cast<pid_t>(::gettid())); }

PgrSharedCatchConfig make_config() {
  PgrSharedCatchConfig c{};
  c.version = 1;
  c.size = sizeof(PgrSharedCatchConfig);
  c.max_records = PGR_SH_MAX_RECORDS;
  c.timeout_ms = PGR_SH_TIMEOUT_MS;
  c.lease_ceiling_ms = PGR_SH_MAX_LEASE_MS;
  c.xfer_slots = PGR_SH_XFER_SLOTS;
  return c;
}

PgrSharedCatchSample take_sample() {
  PgrSharedCatchSample s{};
  assert(g_api->sample(&s) == PGR_SH_OK);
  assert(s.version == 1 && s.size == sizeof(PgrSharedCatchSample));
  return s;
}

uint64_t upsert(uint64_t gui_key, uint32_t kind, uint64_t tid) {
  uint64_t slot = 0;
  assert(g_api->upsert(gui_key, kind, tid, &slot) == PGR_SH_OK);
  return slot;
}

PgrSharedCatchDecision select(uint64_t ticks, uint64_t tid) {
  PgrSharedCatchDecision d{};
  assert(g_api->select(ticks, tid, &d) == PGR_SH_OK);
  assert(d.version == 1 && d.size == sizeof(PgrSharedCatchDecision) && d.tick == ticks);
  return d;
}

// Drain every pending event and return them (host behaviour: free the GC root per record).
std::vector<PgrSharedCatchEvent> drain() {
  std::vector<PgrSharedCatchEvent> out;
  PgrSharedCatchEvent e{};
  while (g_api->next_event(&e) == 1) out.push_back(e);
  return out;
}

int count_kind(const std::vector<PgrSharedCatchEvent> &v, uint32_t kind) {
  int n = 0;
  for (auto &e : v) if (e.kind == kind) ++n;
  return n;
}
bool has_reason(const std::vector<PgrSharedCatchEvent> &v, int32_t reason) {
  for (auto &e : v) if (e.kind == PGR_SH_EVENT_CANCELLED && e.reason == reason) return true;
  return false;
}

// Drive a summary candidate to ready.
void make_summary_ready(uint64_t slot, uint64_t tid) {
  assert(g_api->mark(slot, PGR_SH_STAGE_INIT, 0x3u /*success+captured*/, tid) == PGR_SH_OK);
  assert(g_api->mark(slot, PGR_SH_STAGE_SET_SCORES, 0, tid) == PGR_SH_OK);
  assert(g_api->mark(slot, PGR_SH_STAGE_OPEN_COMPLETE, 0, tid) == PGR_SH_OK);
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
  assert(core->query_feature(51, 99) == nullptr);
  const auto *api = static_cast<const PgrSharedCatchApi *>(core->query_feature(51, 1));
  assert(api != nullptr && api->version == 1 && api->size == sizeof(PgrSharedCatchApi));
  g_api = api;

  std::printf("ABI sizes: config=%zu decision=%zu event=%zu sample=%zu table=%zu\n",
              sizeof(PgrSharedCatchConfig), sizeof(PgrSharedCatchDecision),
              sizeof(PgrSharedCatchEvent), sizeof(PgrSharedCatchSample), sizeof(PgrSharedCatchApi));
  assert(sizeof(PgrSharedCatchConfig) == 32 && sizeof(PgrSharedCatchDecision) == 40 &&
         sizeof(PgrSharedCatchEvent) == 24 && sizeof(PgrSharedCatchSample) == 88 &&
         sizeof(PgrSharedCatchApi) == 112);

  const uint64_t tid = this_tid();
  PgrSharedCatchConfig cfg = make_config();
  // A transfer extends the UI record's initial discovery deadline. Exercise a
  // short initial deadline in a fresh child without sleeping the production11s.
  {
    const pid_t child=fork();assert(child>=0);
    if(child==0){
      auto short_config=cfg;short_config.timeout_ms=1;
      assert(api->install(&short_config)==PGR_SH_OK);
      const auto owner=this_tid();assert(api->update(1,2000,1,owner)==PGR_SH_OK);
      auto slot=upsert(0x1010,PGR_SH_KIND_SUMMARY,owner);make_summary_ready(slot,owner);
      assert(api->transfer_bind(slot,1234,owner)==PGR_SH_OK);
      usleep(30000);
      assert(select(1,owner).action==PGR_SH_ACT_ARM);
      assert(take_sample().records==1);
      _exit(0);
    }
    int status=0;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
  }

  // 1. Validation + not-installed + idempotence + reconfig + bad bounds.
  assert(api->install(nullptr) == PGR_SH_ERR_NULL_ARG);
  { PgrSharedCatchConfig b = cfg; b.version = 2; assert(api->install(&b) == PGR_SH_ERR_BAD_VERSION); }
  { PgrSharedCatchConfig b = cfg; b.size = sizeof(cfg) - 1; assert(api->install(&b) == PGR_SH_ERR_BAD_SIZE); }
  { PgrSharedCatchConfig b = cfg; b.max_records = 99; assert(api->install(&b) == PGR_SH_ERR_MISSING_PTR); }
  { PgrSharedCatchConfig b = cfg; b.lease_ceiling_ms = PGR_SH_MAX_LEASE_MS + 1; assert(api->install(&b) == PGR_SH_ERR_MISSING_PTR); }
  { uint64_t s = 0; assert(api->upsert(1, PGR_SH_KIND_SUMMARY, tid, &s) == PGR_SH_ERR_NOT_INSTALLED); }
  assert(api->install(&cfg) == PGR_SH_OK);
  assert(api->install(&cfg) == PGR_SH_OK);  // idempotent
  { PgrSharedCatchConfig b = cfg; b.timeout_ms = 1234; assert(api->install(&b) == PGR_SH_ERR_RECONFIG); }

  // 2. Summary success path: upsert -> mark -> ARM on first ready tick -> DISMISS on a later tick -> finish.
  {
    assert(api->update(1, 2000, 1, tid) == PGR_SH_OK);
    uint64_t slot = upsert(0x1001, PGR_SH_KIND_SUMMARY, tid);
    assert(slot != 0);
    // Not ready yet: NONE.
    assert(select(10, tid).action == PGR_SH_ACT_NONE);
    make_summary_ready(slot, tid);
    PgrSharedCatchDecision d1 = select(11, tid);
    assert(d1.action == PGR_SH_ACT_ARM && d1.slot == slot && d1.gui_key == 0x1001 && d1.kind == PGR_SH_KIND_SUMMARY);
    // Same tick -> still NONE (needs a STRICTLY later proven tick).
    assert(select(11, tid).action == PGR_SH_ACT_NONE);
    PgrSharedCatchDecision d2 = select(12, tid);
    assert(d2.action == PGR_SH_ACT_DISMISS && d2.slot == slot);
    assert(api->finish(slot, 1, tid) == PGR_SH_OK);
    auto ev = drain();
    assert(count_kind(ev, PGR_SH_EVENT_REAPED) == 1 && ev.back().slot == slot && ev.back().gui_key == 0x1001);
    assert(select(13, tid).action == PGR_SH_ACT_NONE);  // table empty after reap
    PgrSharedCatchSample sm = take_sample();
    assert(sm.records == 0 && sm.armed_serial >= 1 && sm.dismiss_serial >= 1 && sm.reap_serial >= 1);
  }

  // 3. mark() resets readyMatch: a late progress event re-arms (no stale dismiss).
  {
    assert(api->update(1, 2000, 2, tid) == PGR_SH_OK);
    uint64_t slot = upsert(0x2002, PGR_SH_KIND_SUMMARY, tid);
    make_summary_ready(slot, tid);
    assert(select(20, tid).action == PGR_SH_ACT_ARM);
    // A fresh SetScores arrives before dismiss: readyMatch must reset -> next select re-arms, not dismiss.
    assert(api->mark(slot, PGR_SH_STAGE_SET_SCORES, 0, tid) == PGR_SH_OK);
    assert(select(21, tid).action == PGR_SH_ACT_ARM);
    assert(select(22, tid).action == PGR_SH_ACT_DISMISS);
    assert(api->cancel(slot, PGR_SH_REASON_HOST, tid) == PGR_SH_OK);  // explicit teardown
    drain();
  }

  // 4. Details success path uses PRESENT, not SET_SCORES/success.
  {
    assert(api->update(1, 2000, 3, tid) == PGR_SH_OK);
    uint64_t slot = upsert(0x3003, PGR_SH_KIND_DETAILS, tid);
    assert(api->mark(slot, PGR_SH_STAGE_INIT, 0, tid) == PGR_SH_OK);         // details: success not required
    assert(api->mark(slot, PGR_SH_STAGE_OPEN_COMPLETE, 0, tid) == PGR_SH_OK);
    assert(select(30, tid).action == PGR_SH_ACT_NONE);                       // present_done missing
    assert(api->mark(slot, PGR_SH_STAGE_PRESENT, 0, tid) == PGR_SH_OK);
    PgrSharedCatchDecision d = select(31, tid);
    assert(d.action == PGR_SH_ACT_ARM && d.kind == PGR_SH_KIND_DETAILS);
    assert(select(32, tid).action == PGR_SH_ACT_DISMISS);
    assert(api->finish(slot, 0, tid) == PGR_SH_OK);  // failed dismiss still latches invoked -> reaped once
    assert(count_kind(drain(), PGR_SH_EVENT_REAPED) == 1);
  }

  // 5. Summary not confirmed (no success bit) never becomes ready.
  {
    assert(api->update(1, 2000, 4, tid) == PGR_SH_OK);
    uint64_t slot = upsert(0x4004, PGR_SH_KIND_SUMMARY, tid);
    assert(api->mark(slot, PGR_SH_STAGE_INIT, 0x2u /*captured but NOT success*/, tid) == PGR_SH_OK);
    assert(api->mark(slot, PGR_SH_STAGE_SET_SCORES, 0, tid) == PGR_SH_OK);
    assert(api->mark(slot, PGR_SH_STAGE_OPEN_COMPLETE, 0, tid) == PGR_SH_OK);
    assert(select(40, tid).action == PGR_SH_ACT_NONE);
    assert(api->cancel(slot, PGR_SH_REASON_HOST, tid) == PGR_SH_OK);
    drain();
  }

  // 6. cancel_all (new-initialization) drops every non-invoked record.
  {
    assert(api->update(1, 2000, 5, tid) == PGR_SH_OK);
    uint64_t a = upsert(0x5001, PGR_SH_KIND_SUMMARY, tid);
    uint64_t b = upsert(0x5002, PGR_SH_KIND_SUMMARY, tid);
    assert(a && b && a != b);
    assert(api->cancel_all(PGR_SH_REASON_NEW_INITIALIZATION, tid) == PGR_SH_OK);
    auto ev = drain();
    assert(count_kind(ev, PGR_SH_EVENT_CANCELLED) == 2 && has_reason(ev, PGR_SH_REASON_NEW_INITIALIZATION));
    assert(take_sample().records == 0);
  }

  // 7. Overflow eviction: a 5th gui key evicts the first non-invoked record.
  {
    assert(api->update(1, 2000, 6, tid) == PGR_SH_OK);
    uint64_t s1 = upsert(0x6001, PGR_SH_KIND_SUMMARY, tid);
    upsert(0x6002, PGR_SH_KIND_SUMMARY, tid);
    upsert(0x6003, PGR_SH_KIND_SUMMARY, tid);
    upsert(0x6004, PGR_SH_KIND_SUMMARY, tid);
    assert(take_sample().records == PGR_SH_MAX_RECORDS);
    uint64_t s5 = upsert(0x6005, PGR_SH_KIND_SUMMARY, tid);
    assert(s5 != 0);
    auto ev = drain();
    assert(count_kind(ev, PGR_SH_EVENT_CANCELLED) == 1 && has_reason(ev, PGR_SH_REASON_RECORD_OVERFLOW));
    assert(ev.front().slot == s1);
    assert(take_sample().records == PGR_SH_MAX_RECORDS);
    assert(api->cancel_all(PGR_SH_REASON_HOST, tid) == PGR_SH_OK);
    drain();
  }

  // 8. Prune reasons: epoch revoke, off, lease-expired, timeout (JS precedence).
  {
    assert(api->update(1, 2000, 7, tid) == PGR_SH_OK);
    uint64_t slot = upsert(0x7001, PGR_SH_KIND_SUMMARY, tid);
    assert(slot != 0);
    // Epoch move (owner records a new epoch) revokes the record on the next select.
    assert(api->update(1, 2000, 8, tid) == PGR_SH_OK);
    assert(select(70, tid).action == PGR_SH_ACT_NONE);
    assert(has_reason(drain(), PGR_SH_REASON_PERMISSION_REVOKED));

    uint64_t slot2 = upsert(0x7002, PGR_SH_KIND_SUMMARY, tid);
    assert(slot2 != 0);
    assert(api->update(0, 0, 8, tid) == PGR_SH_OK);  // OFF
    assert(select(71, tid).action == PGR_SH_ACT_NONE);
    assert(has_reason(drain(), PGR_SH_REASON_OFF));

    assert(api->update(1, 10, 9, tid) == PGR_SH_OK);
    uint64_t slot3 = upsert(0x7003, PGR_SH_KIND_SUMMARY, tid);
    assert(slot3 != 0);
    ::usleep(30000);  // lease (10 ms) expires
    assert(select(72, tid).action == PGR_SH_ACT_NONE);
    assert(has_reason(drain(), PGR_SH_REASON_LEASE_EXPIRED));
  }

  // 9. Policy revoke during the ready window: stop() between ARM and DISMISS yields NONE + drain.
  {
    assert(api->update(1, 2000, 10, tid) == PGR_SH_OK);
    uint64_t slot = upsert(0x8001, PGR_SH_KIND_SUMMARY, tid);
    make_summary_ready(slot, tid);
    assert(select(80, tid).action == PGR_SH_ACT_ARM);
    assert(api->stop(tid) == PGR_SH_OK);  // revoke mid-window
    assert(select(81, tid).action == PGR_SH_ACT_NONE);
    assert(has_reason(drain(), PGR_SH_REASON_STOP));
    // Re-arm clears stopping.
    assert(api->update(1, 2000, 10, tid) == PGR_SH_OK);
    uint64_t slot2 = upsert(0x8002, PGR_SH_KIND_SUMMARY, tid);
    make_summary_ready(slot2, tid);
    assert(select(82, tid).action == PGR_SH_ACT_ARM);
    assert(select(83, tid).action == PGR_SH_ACT_DISMISS);
    api->finish(slot2, 1, tid);
    drain();
  }

  // 10. Transfer bind/resolve one-shot + KeepAll no-replay (a 2nd resolve returns 0).
  {
    assert(api->update(1, 2000, 11, tid) == PGR_SH_OK);
    uint64_t slot = upsert(0x9001, PGR_SH_KIND_SUMMARY, tid);
    assert(api->transfer_bind(slot, 0xABCD, tid) == PGR_SH_OK);
    uint64_t got = 0;
    assert(api->transfer_resolve(0xABCD, tid, &got) == PGR_SH_OK && got == slot);
    got = 123;
    assert(api->transfer_resolve(0xABCD, tid, &got) == PGR_SH_OK && got == 0);  // consumed: no re-attribution
    got = 123;
    assert(api->transfer_resolve(0xDEAD, tid, &got) == PGR_SH_OK && got == 0);  // unknown token
    assert(api->cancel(slot, PGR_SH_REASON_HOST, tid) == PGR_SH_OK);
    drain();
  }

  // 11. Off-owner: a foreign thread cannot upsert/mark/select/cancel; it can only revoke via update/stop.
  std::thread([&]() {
    uint64_t s = 5;
    assert(g_api->upsert(0xF001, PGR_SH_KIND_SUMMARY, this_tid(), &s) == PGR_SH_OK && s == 0);
    PgrSharedCatchDecision d{};
    assert(g_api->select(1, this_tid(), &d) == PGR_SH_OK && d.action == PGR_SH_ACT_NONE);
    assert(g_api->mark(1, PGR_SH_STAGE_INIT, 0, this_tid()) == PGR_SH_OK);
    assert(g_api->stop(this_tid()) == PGR_SH_OK);  // revoke from any thread is allowed
  }).join();

  // 12. Mismatched proven_tid on the owner thread is a no-op.
  {
    assert(api->update(1, 2000, 12, tid) == PGR_SH_OK);
    uint64_t s = 5;
    assert(api->upsert(0xF002, PGR_SH_KIND_SUMMARY, tid + 1, &s) == PGR_SH_OK && s == 0);
  }

  // 13. Re-entry safety: draining events while selecting again does not corrupt the queue.
  {
    assert(api->update(1, 2000, 13, tid) == PGR_SH_OK);
    uint64_t slot = upsert(0xA001, PGR_SH_KIND_SUMMARY, tid);
    make_summary_ready(slot, tid);
    assert(select(130, tid).action == PGR_SH_ACT_ARM);
    assert(select(131, tid).action == PGR_SH_ACT_DISMISS);
    assert(api->finish(slot, 1, tid) == PGR_SH_OK);
    // A redundant finish on the already-reaped slot is a harmless no-op (no second REAPED).
    assert(api->finish(slot, 1, tid) == PGR_SH_OK);
    assert(count_kind(drain(), PGR_SH_EVENT_REAPED) == 1);
    assert(take_sample().pending_events == 0 && take_sample().fault == 0);
  }

  // Reusing an array hole must not reorder the oldest surviving candidate.
  {
    assert(api->update(1,2000,14,tid)==PGR_SH_OK);
    auto first=upsert(0xB001,PGR_SH_KIND_SUMMARY,tid);
    auto second=upsert(0xB002,PGR_SH_KIND_SUMMARY,tid);
    api->cancel(first,PGR_SH_REASON_HOST,tid);drain();
    auto third=upsert(0xB003,PGR_SH_KIND_SUMMARY,tid);
    make_summary_ready(second,tid);make_summary_ready(third,tid);
    assert(select(0,tid).action==PGR_SH_ACT_NONE); // cleanup cannot arm
    assert(select(140,tid).action==PGR_SH_ACT_ARM);
    assert(select(141,tid).slot==second);
    api->finish(second,1,tid);drain();
    assert(select(142,tid).slot==third);
    api->stop(tid);
    assert(select(0,tid).action==PGR_SH_ACT_NONE); // cleanup still prunes on stop
    assert(has_reason(drain(),PGR_SH_REASON_STOP));
    assert(take_sample().records==0);
  }
  // Undrained overflow faults permanently; the host sees it and releases mirrored roots.
  {
    assert(api->update(1,2000,15,tid)==PGR_SH_OK);
    for(uint64_t i=0;i<17;++i){auto s=upsert(0xC000+i,PGR_SH_KIND_SUMMARY,tid);api->cancel(s,PGR_SH_REASON_HOST,tid);}
    assert(take_sample().fault==1);
    uint64_t slot=123;assert(api->upsert(0xC099,PGR_SH_KIND_SUMMARY,tid,&slot)==PGR_SH_ERR_OVERFLOW&&slot==0);
    assert(select(150,tid).action==PGR_SH_ACT_NONE);
    assert(drain().size()==16);
  }

  std::printf("shared_catch_test passed: ABI/validation/idempotence/reconfig/bounds, summary success "
              "(upsert->mark->ARM->later-tick DISMISS->finish), readyMatch reset on progress, details "
              "PRESENT path, unconfirmed-never-ready, cancel_all new-initialization, overflow eviction, "
              "prune reasons (epoch/off/lease), stop mid-window, transfer bind/resolve one-shot no-replay, "
              "off-owner no-op + foreign revoke, mismatched tid no-op, re-entry/double-finish\n");
  return 0;
}
