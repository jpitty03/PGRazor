// Standalone Android (x86_64 host) test of the real Always Hit hook: argument ABI,
// correct-state single apply with duplicate suppression, launch/release rearm, the
// passthrough exclusions, makeThrow "missed" normalisation, the thread/lease/off/expiry
// gate, root balance, latched managed-exception fault, and the new game-thread tick()
// root clear. Modelled on ar_throw_test.cpp / adventure_sync_test.cpp: it dlopen()s the
// built core, pulls feature id 7 (v1) from the core getter and drives our own hooked
// functions through volatile pointers so the compiler cannot bypass the installed trampoline.
//
// API note (coordinator change): feature 7/v1 is now a 40-byte table
//   { version,size, install, update, sample, tick }.
// tick(proven_tid) is called by the verified Unity thread, never refreshes the lease, and
// frees the single handled GC root on stop/expiry. It must only free when the caller is
// truly that thread, so an off-thread tick (proven_tid != gettid()) must not free.
#include "always_hit.h"
#include "pgrazor_native_api.h"
#include <dlfcn.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <ctime>

// Symbolic il2cpp identities: the mocks compare FieldInfo*/class* by address only, and the
// memory mocks read field values through these symbolic FieldInfo addresses.
int ball_class, machine_class, action_class;
int fld_bread, fld_item, fld_state, fld_current, fld_flying, fld_falling, fld_hit, fld_pokemon;
int invoke_method, exc_obj, pokemon_obj, flying_state, falling_state, other_state;
int arg_obj, method_obj;

struct Machine { void *klass; void *current; };          // klass first: object_get_class reads it
struct Action  { void *klass; };
struct Ball {
    void *klass;                                         // 0  exact ball_class
    uint8_t bread;                                       //    HoldingBreadBall guard
    int32_t item;                                        //    BallType (1,2,3,5 eligible)
    void *state;                                         //    -> Machine
    void *flying;                                        //    flying coroutine ref
    void *falling;                                       //    falling coroutine ref
    void *hit;                                           //    -> Action (HitPokemon)
    void *pokemon;                                       //    IEncounterPokemon ref
};

int roots = 0, applied_hits = 0, orig_coll = 0, orig_trig = 0;
bool invoke_throws = false;
uint8_t mk_last_missed = 0; void *mk_last_self = nullptr, *mk_last_method = nullptr;
PgrAlwaysHitValue mk_ret{};
void *coll_self = nullptr, *coll_arg = nullptr, *coll_method = nullptr;

void *obj_class(void *o) { return *static_cast<void **>(o); } // klass is each struct's first field

void field_get(void *o, void *f, void *out) {
    if      (f == &fld_bread)   *static_cast<uint8_t *>(out) = static_cast<Ball *>(o)->bread;
    else if (f == &fld_item)    *static_cast<int32_t *>(out) = static_cast<Ball *>(o)->item;
    else if (f == &fld_state)   *static_cast<void **>(out)   = static_cast<Ball *>(o)->state;
    else if (f == &fld_current) *static_cast<void **>(out)   = static_cast<Machine *>(o)->current;
    else if (f == &fld_flying)  *static_cast<void **>(out)   = static_cast<Ball *>(o)->flying;
    else if (f == &fld_falling) *static_cast<void **>(out)   = static_cast<Ball *>(o)->falling;
    else if (f == &fld_hit)     *static_cast<void **>(out)   = static_cast<Ball *>(o)->hit;
    else if (f == &fld_pokemon) *static_cast<void **>(out)   = static_cast<Ball *>(o)->pokemon;
}

// Pointer-sized GC handles: handle == object, so gc_target(handle) == the rooted ball.
void *gc_new(void *o, int) { ++roots; return o; }
void *gc_target(void *h)   { return h; }
void  gc_free(void *)      { --roots; }

// The game's own HitPokemon via runtime_invoke. Sets *exc on a managed exception.
void *runtime_invoke(void *method, void *obj, void **params, void **exc) {
    asm volatile("nop; nop; nop; nop; nop; nop; nop; nop" ::: "memory");
    (void)method; (void)obj; (void)params;
    if (invoke_throws) { *exc = &exc_obj; return nullptr; }
    *exc = nullptr; ++applied_hits; return nullptr;
}

#define PAD asm volatile("nop; nop; nop; nop; nop; nop; nop; nop" ::: "memory")
// Our own hooked functions, padded with 8 nops so Dobby has room to patch and never bypassed.
__attribute__((noinline)) void collision(void *self, void *arg, void *method) {
    PAD; coll_self = self; coll_arg = arg; coll_method = method; ++orig_coll;
}
__attribute__((noinline)) void trigger(void *self, void *arg, void *method) {
    PAD; (void)self; (void)arg; (void)method; ++orig_trig;
}
__attribute__((noinline)) PgrAlwaysHitValue make_throw(void *self, uint8_t missed, void *method) {
    PAD; mk_last_self = self; mk_last_missed = missed; mk_last_method = method; return mk_ret;
}
__attribute__((noinline)) void launch(void *self)  { PAD; (void)self; }
__attribute__((noinline)) void release(void *self) { PAD; (void)self; }

int main(int argc, char **argv) {
    if (argc != 2) return 1;
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::puts(dlerror()); return 2; }
    auto getter = reinterpret_cast<const void *(*)(uint32_t)>(dlsym(lib, "pgrazor_native_api"));
    auto core = getter ? static_cast<const pgrazor_native_api_v2 *>(getter(2)) : nullptr;
    auto api  = core ? static_cast<const PgrAlwaysHitApi *>(core->query_feature(7, 1)) : nullptr;
    if (!api || api->size != 40) return 3; // 40-byte table incl. tick

    PgrAlwaysHitConfig cfg{
        1, sizeof(cfg),
        (void *)&launch, (void *)&release, (void *)&collision, (void *)&trigger, (void *)&make_throw,
        &ball_class, &machine_class, &action_class,
        (void *)&obj_class, (void *)&field_get, (void *)&runtime_invoke, &invoke_method,
        (void *)&gc_new, (void *)&gc_target, (void *)&gc_free,
        &fld_bread, &fld_item, &fld_state, &fld_current, &fld_flying, &fld_falling, &fld_hit, &fld_pokemon};
    if (api->install(&cfg) || api->install(&cfg)) return 4; // install + idempotent repeat

    Action  act{&action_class};
    Machine mac{&machine_class, &flying_state};
    Ball    ball{&ball_class, 0, 1, &mac, &flying_state, &falling_state, &act, &pokemon_obj};

    decltype(&collision)  volatile call_coll   = &collision;
    decltype(&trigger)    volatile call_trig   = &trigger;
    decltype(&make_throw) volatile call_mk     = &make_throw;
    decltype(&launch)     volatile call_launch = &launch;
    decltype(&release)    volatile call_rel    = &release;

    // (A) No policy armed: original ABI must be forwarded verbatim, nothing applied/normalised.
    call_coll(&ball, &arg_obj, &method_obj);
    if (orig_coll != 1 || coll_self != &ball || coll_arg != &arg_obj || coll_method != &method_obj ||
        applied_hits || roots) return 5;
    mk_ret = {2, 1.5f, 1, 0, 1, 0x7e};
    PgrAlwaysHitValue r = call_mk(&ball, 0xab, &invoke_method);
    if (mk_last_self != &ball || mk_last_missed != 0xab || mk_last_method != &invoke_method) return 6;
    if (r.item != 2 || r.radius != 1.5f || r.bullseye != 1 || r.spinning != 0 || r.missed != 1 || r.pad != 0x7e)
        return 7; // all struct bits incl. missed preserved while gate is closed

    // (B) Armed + correct state: one HitPokemon apply, original suppressed, handled rooted.
    api->update(1, 1, 2500, gettid());
    call_coll(&ball, &arg_obj, &method_obj);
    if (applied_hits != 1 || orig_coll != 1 || roots != 1) return 8;
    call_coll(&ball, &arg_obj, &method_obj);                // duplicate on same handled ball
    if (applied_hits != 1 || orig_coll != 1 || roots != 1) return 9;
    call_trig(&ball, &arg_obj, &method_obj);                // trigger shares the handled dedupe
    if (applied_hits != 1 || orig_trig != 0 || roots != 1) return 10;

    // (C) Launch / EndDrag entry instruments rearm: free the handled root -> next hit applies.
    call_launch(&ball);  if (roots != 0) return 11;
    call_coll(&ball, &arg_obj, &method_obj);
    if (applied_hits != 2 || roots != 1) return 12;
    call_rel(&ball);     if (roots != 0) return 13;
    call_trig(&ball, &arg_obj, &method_obj);
    if (applied_hits != 3 || roots != 1) return 14;

    // (D) Exclusions pass the original through with no apply and no root. Rearm (valid ball)
    //     before each so the handled dedupe never masks the exclusion under test.
    int oc;
    call_launch(&ball); ball.klass = &other_state; oc = orig_coll;
    call_coll(&ball, &arg_obj, &method_obj);
    if (orig_coll != oc + 1 || applied_hits != 3 || roots != 0) return 15; // wrong ball class
    ball.klass = &ball_class;

    call_launch(&ball); ball.bread = 1; oc = orig_coll;
    call_coll(&ball, &arg_obj, &method_obj);
    if (orig_coll != oc + 1 || roots != 0) return 16; // bread guard set
    ball.bread = 0;

    call_launch(&ball); ball.item = 4; oc = orig_coll;
    call_coll(&ball, &arg_obj, &method_obj);
    if (orig_coll != oc + 1 || roots != 0) return 17; // ineligible item
    ball.item = 1;

    call_launch(&ball); mac.current = &other_state; oc = orig_coll;
    call_coll(&ball, &arg_obj, &method_obj);
    if (orig_coll != oc + 1 || roots != 0) return 18; // current state neither flying nor falling
    mac.current = &flying_state;

    call_launch(&ball); ball.hit = nullptr; oc = orig_coll;
    call_coll(&ball, &arg_obj, &method_obj);
    if (orig_coll != oc + 1 || roots != 0) return 19; // no HitPokemon action
    ball.hit = &act;

    // (E) makeThrow normalisation: only missed 1 -> 0 when permitted; every other bit verbatim.
    mk_ret = {3, 2.25f, 1, 1, 1, 0x5a};
    PgrAlwaysHitValue rn = call_mk(&ball, 1, &invoke_method);
    if (mk_last_missed != 1 || mk_last_method != &invoke_method) return 20;
    if (rn.item != 3 || rn.radius != 2.25f || rn.bullseye != 1 || rn.spinning != 1 || rn.missed != 0 || rn.pad != 0x5a)
        return 21;
    mk_ret.missed = 0;
    PgrAlwaysHitValue rz = call_mk(&ball, 0, &invoke_method);
    if (rz.missed != 0) return 22; // already clear -> unchanged, not counted

    // (F) Gate: wrong thread, invalid lease, off, expiry each pass the original through.
    api->update(1, 1, 2500, gettid() + 1); oc = orig_coll;
    call_coll(&ball, &arg_obj, &method_obj);
    if (orig_coll != oc + 1 || applied_hits != 3) return 23; // not the proven Unity thread

    if (api->update(1, 1, 3000, gettid()) == 0) return 24;   // lease > cap must be refused
    oc = orig_coll; call_coll(&ball, &arg_obj, &method_obj);
    if (orig_coll != oc + 1) return 25;

    api->update(0, 0, 0, 0); oc = orig_coll;
    call_coll(&ball, &arg_obj, &method_obj);
    if (orig_coll != oc + 1) return 26; // disabled

    api->update(1, 1, 1, gettid());
    timespec pause{0, 10000000}; nanosleep(&pause, nullptr);
    oc = orig_coll; call_coll(&ball, &arg_obj, &method_obj);
    if (orig_coll != oc + 1 || applied_hits != 3) return 27; // lease expired

    // (G) Managed exception latches a permanent fault: the root taken before the invoke stays
    //     held, the original is suppressed, and every later event passes through.
    api->update(1, 1, 2500, gettid());
    call_launch(&ball); if (roots != 0) return 28; // clear any residual handled root
    invoke_throws = true; oc = orig_coll;
    call_coll(&ball, &arg_obj, &method_obj);
    if (orig_coll != oc || applied_hits != 3 || roots != 1) return 29; // faulted, rooted, suppressed
    // (H) tick() root clear: a stop requests the free, an off-thread tick must NOT free, and a
    //     tick proven to be on the game thread frees the single handled root.
    api->update(0, 0, 0, 0);
    api->tick(gettid() + 1); if (roots != 1) return 31; // off-thread: must not free
    api->tick(gettid());     if (roots != 0) return 32; // proven thread on stop: frees
    invoke_throws = false; oc = orig_coll;
    api->update(1,1,2500,gettid());
    call_coll(&ball, &arg_obj, &method_obj);
    if (orig_coll != oc + 1 || roots != 0) return 30; // fault stays latched; root already retired

    PgrAlwaysHitSample s{}; api->sample(&s);
    if (s.applied != 3 || s.normalized != 1 || s.errors != 1) return 33;

    std::puts("PASS native Always Hit: ABI/forwarding, single-apply dedupe, launch/release rearm, "
              "exclusions, missed normalisation, thread/lease/off/expiry gate, root balance, "
              "latched managed exception, off-thread-safe tick root clear");
    return 0;
}
