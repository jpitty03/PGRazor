// Standalone Android (x86_64 host) test of the real feature49 catch observers: the btxu
// result observer (caller/thread/lease/rate gate, confirmed/escape -> speed delegation, return
// relocation, reentry suppression), the native outcome dedup/rate/classification, and the
// caught/escaped completion delegation. Modelled on always_hit_test.cpp: it dlopen()s the built
// core, pulls feature id 49 (v1) from the core getter and drives our own hooked btxu/btdd/btdf
// stubs through volatile pointers so the compiler cannot bypass the installed Dobby trampoline.
// The feature8 speed engine is replaced by a recording stub table so the DELEGATION is observable
// without the real engine; this is the actual-library Dobby harness required for this batch.
#include "catch_observers.h"       // pulls in catch_animation.h (PgrSpeedApi / PgrSpeedSample)
#include "pgrazor_native_api.h"
#include <dlfcn.h>
#include <unistd.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

// Enum literal values (distinct, mirror the pairwise-distinct requirement).
enum { S_SUCCESS = 7, S_ESCAPE = 3, S_FLEE = 5, S_MISSED = 9, S_ERROR = 1, S_ITEM = 11, S_UNKNOWN = 42 };

// Symbolic il2cpp identities compared by address only.
int ball_class, response_class, animator_class;
int fld_bread, fld_animator, fld_status;
int method_obj, capture_obj;

struct Animator { void *klass; };
struct Response { void *klass; int32_t status; };
struct Ball     { void *klass; uint8_t bread; void *animator; };

Animator g_animator{&animator_class};
Ball     g_ball{&ball_class, 0, &g_animator};
Response g_resp_success{&response_class, S_SUCCESS};
Response g_resp_success_moved{&response_class, S_SUCCESS}; // GC relocation target for g_resp_success
Response g_resp_escape{&response_class, S_ESCAPE};
Response g_resp_flee{&response_class, S_FLEE};
Response g_resp_missed{&response_class, S_MISSED};
Response g_resp_error{&response_class, S_ERROR};
Response g_resp_unknown{&response_class, S_UNKNOWN};
// Dedicated responses for the appended cases (distinct identities keep the dedup/rate windows clean).
Response g_resp_l1{&response_class, S_ESCAPE}, g_resp_l2{&response_class, S_ESCAPE}, g_resp_l3{&response_class, S_ESCAPE};
Response g_resp_n{&response_class, S_SUCCESS};
Response g_resp_reloc{&response_class, S_SUCCESS};       // relocates DURING a managed free (M1)
Response g_resp_reloc_moved{&response_class, S_SUCCESS}; // its post-relocation location
Response g_resp_p1{&response_class, S_ESCAPE}, g_resp_p2{&response_class, S_ESCAPE};
Response g_resp_q1{&response_class, S_ESCAPE}, g_resp_q2{&response_class, S_ESCAPE};

// il2cpp attachment probe (config.thread_current). Returns non-null while "attached".
bool  g_attached = true;
int   g_thread_token = 0;
void *fake_thread_current() { return g_attached ? static_cast<void *>(&g_thread_token) : nullptr; }

void *obj_class(void *o) { return *static_cast<void **>(o); }
void field_get(void *o, void *f, void *out) {
    if      (f == &fld_bread)    *static_cast<uint8_t *>(out) = static_cast<Ball *>(o)->bread;
    else if (f == &fld_animator) *static_cast<void **>(out)   = static_cast<Ball *>(o)->animator;
    else if (f == &fld_status)   *static_cast<int32_t *>(out) = static_cast<Response *>(o)->status;
}

// Relocating GC handles: gc_target returns the CURRENT location, which for g_resp_success is the
// "moved" copy -- so a pointer rooted before a managed read must be re-resolved before use/return.
int roots = 0;
struct H { void *cur; };
H pool[1024]; int pooln = 0;
void *fail_root_for=nullptr;
void *reloc(void *o) { return o == &g_resp_success ? static_cast<void *>(&g_resp_success_moved) : o; }
void *gc_new(void *o, int) { if(o==fail_root_for)return nullptr;++roots; H *h = &pool[pooln++]; h->cur = reloc(o); return h; }
void *gc_target(void *h) { return static_cast<H *>(h)->cur; }
// A managed free that can RELOCATE a live object as if a GC safepoint ran inside gchandle_free:
// when armed, the next free walks every outstanding handle and moves g_reloc_from -> g_reloc_to.
// This surfaces M1 -- the module must free OTHER handles first (response still pinned) and return
// the re-resolved pointer, never a stale pre-free one.
void *g_reloc_from = nullptr, *g_reloc_to = nullptr; int g_reloc_pending = 0;
void  gc_free(void *) {
    --roots;
    if (g_reloc_pending > 0 && --g_reloc_pending == 0)
        for (int i = 0; i < pooln; ++i) if (pool[i].cur == g_reloc_from) pool[i].cur = g_reloc_to;
}

// Recording feature8 speed engine stub.
int   stub_owned = 0, stub_applied = 0, stub_completed = 0;
int32_t stub_last_reason = -1;
void *stub_apply_animator = nullptr, *stub_apply_response = nullptr;
int sp_install(const PgrSpeedConfig *) { return 0; }
int sp_update(int, uint64_t, uint64_t) { return 0; }
int sp_apply(void *a, void *r) { stub_apply_animator = a; stub_apply_response = r; ++stub_applied; return 0; }
int sp_complete(void *r, int32_t reason) { (void)r; stub_last_reason = reason; ++stub_completed; return 0; }
int sp_tick(uint64_t) { return 0; }
int sp_sample(PgrSpeedSample *out) { if (out) { std::memset(out, 0, sizeof(*out)); out->owned = static_cast<uint32_t>(stub_owned); } return 0; }
PgrSpeedApi stub_speed{1, sizeof(PgrSpeedApi), &sp_install, &sp_update, &sp_apply, &sp_complete, &sp_tick, &sp_sample};

#define PAD asm volatile("nop; nop; nop; nop; nop; nop; nop; nop" ::: "memory")
typedef void *(*BtxuPtr)(void *, void *);
typedef void  (*CompPtr)(void *, void *, void *);
void *g_next_response = nullptr;
int orig_btxu_calls = 0, orig_caught_calls = 0, orig_escaped_calls = 0;
void *last_caught_resp = nullptr, *last_escaped_resp = nullptr;
int reenter_btxu = 0; // when >0, btxu_stub re-enters btxu once (nested-call test)

const PgrObsApi *g_api_for_stub = nullptr; // set in main, used to stop() from inside orig (M4)
int stop_in_orig = 0;                      // when >0, btxu_stub calls stop() mid-flight once

volatile BtxuPtr call_btxu;
volatile CompPtr nested_caught;
int reenter_caught=0;
bool g_throw_observed=false;void *last_btxu_self=nullptr,*last_caught_self=nullptr;
int berry_filter_hold=0,berry_filter_calls=0;
Ball berry_moved_ball=g_ball;
int berry_filter(void *ball,uint64_t tid){
    if(tid!=static_cast<uint64_t>(gettid())||roots<1)return -99;
    ++berry_filter_calls;
    for(int i=0;i<pooln;++i)if(pool[i].cur==ball)pool[i].cur=&berry_moved_ball;
    return berry_filter_hold;
}
int other_berry_filter(void*,uint64_t){return 0;}
__attribute__((noinline)) void *btxu_stub(void *self, void *method) {
    PAD; (void)self; (void)method; ++orig_btxu_calls;
    last_btxu_self=self;if(g_throw_observed)throw 749;
    if (reenter_btxu > 0) { --reenter_btxu; call_btxu(&g_ball, &method_obj); } // reentrant btxu
    // Revoke-and-REARM mid-flight (M4): stop() bumps the revocation generation, then update()
    // reopens the gate on a NEW generation. gate_open() is therefore true again at the leave
    // path, so only the generation re-check (gen != gen0) can catch that this in-flight btxu
    // armed under the stale generation and must not apply.
    if (stop_in_orig > 0 && g_api_for_stub) {
        --stop_in_orig;
        g_api_for_stub->stop(gettid());
        g_api_for_stub->update(1, 2500, gettid(), nullptr);
    }
    return g_next_response;
}

// A stable, non-inlined call site: the caller address the module observes via
// __builtin_return_address(0) is the return address into THIS function, identical on every call.
// Used to learn the real caller (through sample.last_caller) and arm a MATCHING caller gate (H3).
__attribute__((noinline)) void *wrapped_btxu(void *self, void *method) {
    void *result=call_btxu(self, method);
    asm volatile("" : "+r"(result) :: "memory"); // prevent tail-call: caller identity is this site
    return result;
}
__attribute__((noinline)) void caught_stub(void *self, void *resp, void *method) {
    PAD; (void)self; (void)method; ++orig_caught_calls; last_caught_resp = resp;
    last_caught_self=self;if(g_throw_observed)throw 749;
    if(reenter_caught){--reenter_caught;nested_caught(self,resp,method);}
}
__attribute__((noinline)) void escaped_stub(void *self, void *resp, void *method) {
    PAD; (void)self; (void)method; ++orig_escaped_calls; last_escaped_resp = resp;
    if(g_throw_observed)throw 749;
}

void sleep_ms(long ms) { timespec t{ms / 1000, (ms % 1000) * 1000000L}; nanosleep(&t, nullptr); }

int main(int argc, char **argv) {
    if (argc != 2) return 1;
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::puts(dlerror()); return 2; }
    auto getter = reinterpret_cast<const void *(*)(uint32_t)>(dlsym(lib, "pgrazor_native_api"));
    auto core = getter ? static_cast<const pgrazor_native_api_v2 *>(getter(2)) : nullptr;
    auto api  = core ? static_cast<const PgrObsApi *>(core->query_feature(49, 1)) : nullptr;
    if (!api || api->size != 48) return 3;
    g_api_for_stub = api;

    call_btxu = &btxu_stub;
    volatile CompPtr call_caught = &caught_stub;
    nested_caught = &caught_stub;
    volatile CompPtr call_escaped = &escaped_stub;

    PgrObsConfig cfg{};
    cfg.version = 1; cfg.size = sizeof(cfg);
    cfg.object_get_class = (void *)&obj_class; cfg.field_get_value = (void *)&field_get;
    cfg.gchandle_new = (void *)&gc_new; cfg.gchandle_get_target = (void *)&gc_target; cfg.gchandle_free = (void *)&gc_free;
    cfg.code_btxu = (void *)&btxu_stub; cfg.code_caught = (void *)&caught_stub; cfg.code_escaped = (void *)&escaped_stub;
    cfg.class_ball = &ball_class; cfg.class_response = &response_class; cfg.class_animator = &animator_class;
    cfg.field_bread = &fld_bread; cfg.field_animator = &fld_animator; cfg.field_status = &fld_status;
    cfg.speed_api = &stub_speed;
    cfg.status_success = S_SUCCESS; cfg.status_escape = S_ESCAPE; cfg.status_flee = S_FLEE;
    cfg.status_missed = S_MISSED; cfg.status_error = S_ERROR; cfg.status_item_replacement = S_ITEM;
    cfg.thread_current = (void *)&fake_thread_current; // appended attachment probe (168-byte config)
    if (api->install(&cfg) || api->install(&cfg)) return 4; // install + idempotent repeat

    PgrObsSample s{};

    // (A) No policy armed: passthrough returns the RAW original result, nothing rooted/applied.
    g_next_response = &g_resp_success;
    if (call_btxu(&g_ball, &method_obj) != &g_resp_success) return 5;
    if (orig_btxu_calls != 1 || stub_applied != 0 || roots != 0) return 6;

    // (B) Armed (caller gate off): confirmed success -> speed delegated with the RE-RESOLVED
    //     response (relocation), animator read from the ball, one seen handle held.
    api->update(1, 2500, gettid(), nullptr);
    g_next_response = &g_resp_success;
    if (call_btxu(&g_ball, &method_obj) != &g_resp_success_moved) return 7; // relocated return
    if (stub_applied != 1 || stub_apply_animator != &g_animator) return 8;
    if (stub_apply_response != &g_resp_success_moved) return 9;             // delegated re-resolved
    api->sample(&s);
    if (s.confirmed != 1 || s.applied != 1 || s.seen_count != 1 || roots != 1) return 10;

    // (C) Dedup + concurrent skip: same response while the speed engine reports owned -> no new
    //     confirmed, no second apply.
    stub_owned = 1; g_next_response = &g_resp_success;
    call_btxu(&g_ball, &method_obj);
    api->sample(&s);
    if (s.confirmed != 1 || s.applied != 1 || roots != 1) return 11;
    stub_owned = 0;

    // (D) Classification on distinct responses (fresh outcome-rate window). escape -> confirmed +
    //     apply; flee -> confirmed, NOT applied; missed -> suppressed ambiguous; error -> suppressed
    //     not-confirmed.
    sleep_ms(1050);
    g_next_response = &g_resp_escape;  call_btxu(&g_ball, &method_obj);
    g_next_response = &g_resp_flee;    call_btxu(&g_ball, &method_obj);
    g_next_response = &g_resp_missed;  call_btxu(&g_ball, &method_obj);
    g_next_response = &g_resp_error;   call_btxu(&g_ball, &method_obj);
    api->sample(&s);
    if (s.confirmed != 3) return 12;   // success + escape + flee
    if (s.suppressed != 2) return 13;  // missed + error
    if (s.applied != 2) return 14;     // success + escape (flee not applied)

    // (E) Unknown status -> suppressed unknown (fresh window).
    sleep_ms(1050);
    g_next_response = &g_resp_unknown; call_btxu(&g_ball, &method_obj);
    api->sample(&s);
    if (s.suppressed != 3) return 15;

    // (F) Completion delegation: caught -> feature8 reason 5, escaped -> 6; the game's own
    //     completion body always runs.
    api->update(1, 2500, gettid(), nullptr);
    call_caught(&capture_obj, &g_resp_success, &method_obj);
    if (stub_completed != 1 || stub_last_reason != 5 || orig_caught_calls != 1 || last_caught_resp != &g_resp_success_moved) return 16;
    call_escaped(&capture_obj, &g_resp_escape, &method_obj);
    if (stub_completed != 2 || stub_last_reason != 6 || orig_escaped_calls != 1 || last_escaped_resp != &g_resp_escape) return 17;

    // (G) Off-owner thread: btxu passes through raw, no apply/outcome.
    api->update(1, 2500, gettid() + 1, nullptr);
    { int ac = stub_applied, oc = orig_btxu_calls; g_next_response = &g_resp_success;
      if (call_btxu(&g_ball, &method_obj) != &g_resp_success) return 18;
      if (orig_btxu_calls != oc + 1 || stub_applied != ac) return 19; }

    // (H) Stale / expired lease: passthrough, original runs, no apply.
    api->update(1, 1, gettid(), nullptr); sleep_ms(20);
    { int ac = stub_applied, oc = orig_btxu_calls; g_next_response = &g_resp_success;
      call_btxu(&g_ball, &method_obj);
      if (stub_applied != ac || orig_btxu_calls != oc + 1) return 20; }

    // (I) Caller gate: a wrong expected caller forces passthrough; nullptr accepts again.
    api->update(1, 2500, gettid(), (void *)&main); // &main is never the real btxu caller
    { int ac = stub_applied; sleep_ms(1050); g_next_response = &g_resp_success;
      call_btxu(&g_ball, &method_obj);
      if (stub_applied != ac) return 21; } // caller mismatch -> no delegation
    api->update(1, 2500, gettid(), nullptr);
    { int ac = stub_applied; g_next_response = &g_resp_escape; call_btxu(&g_ball, &method_obj);
      if (stub_applied != ac + 1) return 22; } // caller gate off -> delegated

    // (J) Reentrant btxu while one is in flight: the busy guard suppresses the nested delegation.
    sleep_ms(1050);
    { int ac = stub_applied; reenter_btxu = 1; g_next_response = &g_resp_success;
      call_btxu(&g_ball, &method_obj);
      if (stub_applied != ac + 1) return 23; } // exactly one apply despite the nested call

    // (K) Seen prune: stop() revokes from any thread but frees nothing; an off-thread tick must not
    //     free; a proven-thread tick drains the whole ring and balances the roots.
    api->update(1, 2500, gettid(), nullptr);
    sleep_ms(1050); g_next_response = &g_resp_flee; call_btxu(&g_ball, &method_obj);
    api->sample(&s); if (s.seen_count == 0) return 24;
    api->stop(gettid());
    api->tick(gettid() + 1); api->sample(&s); if (s.seen_count == 0) return 25; // off-thread: no free
    api->tick(gettid());     api->sample(&s); if (s.seen_count != 0) return 26; // proven: drains all
    if (roots != 0) return 27;

    // (L) Caller gate MATCHING path (H3): learn the real caller through the stable wrapper, arm it,
    //     and assert delegation still happens; a different call site with that caller is rejected.
    api->update(1, 2500, gettid(), nullptr);
    sleep_ms(1050); g_next_response = &g_resp_l1; wrapped_btxu(&g_ball, &method_obj);
    api->sample(&s);
    void *real_caller = reinterpret_cast<void *>(s.last_caller);
    if (real_caller == nullptr) return 40; // the module must observe and publish a caller
    { int ac = stub_applied;
      api->update(1, 2500, gettid(), real_caller); // arm the CAPTURED matching caller
      sleep_ms(1050); g_next_response = &g_resp_l2; wrapped_btxu(&g_ball, &method_obj); // same site
      if (stub_applied != ac + 1) return 41; }     // matching caller -> delegated (gate validated)
    { int ac = stub_applied;
      sleep_ms(1050); g_next_response = &g_resp_l3; call_btxu(&g_ball, &method_obj); // different site
      if (stub_applied != ac) return 42; }         // same expected caller, other site -> rejected

    // (M) Completion AFTER lease expiry still delegates (H1): the completion is gated on the cleanup
    //     owner thread + attachment, NOT on a live lease, so complete() fires past the observer lease.
    { int cc = stub_completed, oc = orig_caught_calls;
      api->update(1, 1, gettid(), nullptr); sleep_ms(20); // lease armed then expired (cleanup_tid kept)
      call_caught(&capture_obj, &g_resp_success, &method_obj);
      if (stub_completed != cc + 1) return 43;     // delegated despite the dead lease
      if (orig_caught_calls != oc + 1) return 44; } // the game's own body still ran exactly once

    // (N) Attachment denial (M3): a detached thread fails closed -- btxu passes through with no apply
    //     and completion does not delegate, though the game's own bodies still run.
    g_attached = false;
    api->update(1, 2500, gettid(), nullptr); sleep_ms(1050);
    { int ac = stub_applied, oc = orig_btxu_calls, cc = stub_completed, cco = orig_caught_calls;
      g_next_response = &g_resp_n; if (call_btxu(&g_ball, &method_obj) != &g_resp_n) return 45;
      if (stub_applied != ac || orig_btxu_calls != oc + 1) return 46; // detached btxu: raw passthrough
      call_caught(&capture_obj, &g_resp_success, &method_obj);
      if (stub_completed != cc || orig_caught_calls != cco + 1) return 47; } // detached: no delegation
    g_attached = true;

    // (O) Relocation DURING a managed free (M1): arm the response to move when the FIRST handle of
    //     this btxu (self) is freed; the returned pointer must be the RE-RESOLVED (moved) one.
    api->update(1, 2500, gettid(), nullptr); sleep_ms(1050);
    g_reloc_from = &g_resp_reloc; g_reloc_to = &g_resp_reloc_moved; g_reloc_pending = 1;
    g_next_response = &g_resp_reloc;
    if (call_btxu(&g_ball, &method_obj) != &g_resp_reloc_moved) return 48; // re-resolved, not stale
    g_reloc_pending = 0; g_reloc_from = g_reloc_to = nullptr;

    // (P) Revoke / rearm (M4): a stop()+update() during orig bumps the revocation generation and
    //     reopens the gate, so gate_open() alone would NOT catch it -- only the generation re-check
    //     before the delegated mutation suppresses this stale-armed apply. A subsequent clean btxu
    //     (same, now-current generation) delegates normally.
    api->update(1, 2500, gettid(), nullptr); sleep_ms(1050);
    { int ac = stub_applied; stop_in_orig = 1; g_next_response = &g_resp_p1;
      call_btxu(&g_ball, &method_obj);
      if (stub_applied != ac) return 49; }          // stale generation -> no apply
    sleep_ms(1050);
    { int ac = stub_applied; g_next_response = &g_resp_p2; call_btxu(&g_ball, &method_obj);
      if (stub_applied != ac + 1) return 50; }       // current generation -> delegation restored

    // (Q) Multi-event ring (H2): several events between two polls are all retained (not collapsed to
    //     a single slot); event_head advances and the newest ring slot carries the matching seq.
    api->update(1, 2500, gettid(), nullptr); sleep_ms(1050);
    { PgrObsSample before{}; api->sample(&before);
      g_next_response = &g_resp_q1; call_btxu(&g_ball, &method_obj);   // confirmed + applied = 2 events
      g_next_response = &g_resp_q2; call_btxu(&g_ball, &method_obj);   // confirmed + applied = 2 events
      PgrObsSample after{}; api->sample(&after);
      if (after.event_head < before.event_head + 2) return 51;        // nothing lost to a single slot
      const PgrObsEvent &newest = after.events[after.event_head % PGR_OBS_EVENT_RING];
      if (newest.seq != after.event_head) return 52; }                // ring slot stamped with its seq

    // Drain the seen handles accumulated by L..Q and confirm the roots balance to zero.
    api->stop(gettid()); api->tick(gettid());
    api->sample(&s); if (s.seen_count != 0) return 53;
    if (roots != 0) return 54;

    // Busy ownership spans the original completion and root release, not just the
    // delegated feature8 call. A synchronous nested completion chains but never delegates twice.
    api->update(1,2500,gettid(),nullptr);
    {int completed=stub_completed,originals=orig_caught_calls;reenter_caught=1;
     call_caught(&capture_obj,&g_resp_escape,&method_obj);
     if(stub_completed!=completed+1||orig_caught_calls!=originals+2||roots!=0)return 55;}

    // Failed return-root allocation must not free the receiver across a borrowed
    // Original native unwinds must release receiver/response and the busy guard.
    g_throw_observed=true;
    try{call_btxu(&g_ball,&method_obj);return 58;}catch(int value){if(value!=749||roots!=0)return 59;}
    try{call_caught(&capture_obj,&g_resp_escape,&method_obj);return 60;}catch(int value){if(value!=749||roots!=0)return 61;}
    g_throw_observed=false;
    {int before=stub_completed;call_caught(&capture_obj,&g_resp_escape,&method_obj);if(stub_completed!=before+1||roots!=0)return 62;}

    // Failed return-root allocation must not free the receiver across a borrowed
    // raw return. Fault closes future arms; the next attached tick releases the
    // one deferred receiver root with no return value live.
    sleep_ms(1050);api->update(1,2500,gettid(),nullptr);
    fail_root_for=&g_resp_n;g_next_response=&g_resp_n;
    {int applied=stub_applied;
     if(call_btxu(&g_ball,&method_obj)!=&g_resp_n||stub_applied!=applied||roots!=1)return 56;}
    fail_root_for=nullptr;api->tick(gettid());
    if(roots!=0)return 57;

    // API2 sole-owner berry filter works independently of the (now faulted/off)
    // speed gate, with rooted receiver refresh and exact HOLD/pass originals.
    const auto *bridge=static_cast<const PgrObsApi2*>(core->query_feature(49,2));
    if(!bridge||bridge->size!=24||bridge->filter_ready())return 63;
    if(bridge->register_filter(berry_filter,gettid()+1)!=PGR_OBS_ERR_BAD_ARG)return 64;
    if(bridge->register_filter(berry_filter,gettid())||bridge->register_filter(berry_filter,gettid())||!bridge->filter_ready())return 65;
    if(bridge->register_filter(other_berry_filter,gettid())!=PGR_OBS_ERR_BAD_CONFIG)return 66;
    berry_filter_hold=1;
    {int original=orig_btxu_calls;if(call_btxu(&g_ball,&method_obj)||orig_btxu_calls!=original||berry_filter_calls!=1||roots)return 67;}
    berry_filter_hold=0;g_next_response=&g_resp_n;
    {int original=orig_btxu_calls;if(call_btxu(&g_ball,&method_obj)!=&g_resp_n||orig_btxu_calls!=original+1||last_btxu_self!=&berry_moved_ball||roots)return 68;}
    g_attached=false;
    {int calls=berry_filter_calls;if(call_btxu(&g_ball,&method_obj)!=&g_resp_n||berry_filter_calls!=calls||roots)return 69;}
    g_attached=true;

    std::puts("PASS native catch observers: btxu gate (caller/thread/lease/rate + MATCHING caller + "
              "attachment), return relocation (incl. during a managed free), reentry suppression, "
              "revoke/rearm, completion past lease expiry, multi-event ring drain, "
              "outcome dedup/rate/classification, speed apply/complete delegation, "
              "seen prune on stop + proven tick, root balance");
    return 0;
}
