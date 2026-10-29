#include "transfer_facts.h"
#include "pgrazor_native_api.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <map>
#include <thread>
#include <unistd.h>

// Real-API fake-IL2CPP harness for feature 35 (transfer facts reader). It resolves the ACTUAL production
// table through the exported pgrazor_native_api v2 registry (query_feature(35,1)) and drives inspect()
// against a fake managed heap: a Pokemon carrying id/IVs/species/favorite/egg + a display, an origin-event
// collection, an optional limited-identifier string, and boxed scalars returned from the size/rare/shiny/
// count invokes. gchandle_new/get/free back a handle table so root accounting is exact (this reader
// transfers NOTHING, so after every call the table must be EMPTY). The harness can flip flag/protect masks,
// every CharacterSize enum value, non-canonical booleans, wrong box/field classes, managed throws, a full
// 64-bit id mismatch (before and after the managed work), partial root-allocation failure, a relocating
// collector (PK + display), a foreign/zero/expired lease, and reentry. No long sleeps. The module installs
// NO hook and NEVER issues a release action.

namespace {

using GetApiFn = const void *(*)(uint32_t);

// ----- class / field / method tokens (distinct addresses) --------------------------
int cls_pk, cls_display, cls_bool, cls_int, cls_event, cls_charsize, cls_string, cls_gm, cls_other;
int f_id, f_species, f_display, f_events, f_limited, f_attack, f_defense, f_stamina, f_favorite, f_egg,
    f_costume, f_original_costume, f_background, f_art, f_art_asset;
int m_size, m_rare, m_shiny, m_count;

// ----- fake managed objects --------------------------------------------------------
struct FakeDisplay {
  void *klass;
  int32_t costume, original_costume, art, art_asset;
  void *background;
};
struct FakeEvents { void *klass; };
struct FakeString { void *klass; int32_t length; };
struct FakeGm { void *klass; };
struct FakeBox { void *klass; int32_t val; };  // boxed value type
struct FakePokemon {
  void *klass;
  uint64_t id;
  int32_t species, attack, defense, stamina;
  uint8_t favorite, egg;
  void *display, *events, *limited;
};

FakeGm g_gm{&cls_gm};
FakeEvents g_events{&cls_event};

// Per-method box objects (class + value set at invoke time).
FakeBox g_size_box{&cls_charsize, 0};
FakeBox g_rare_box{&cls_bool, 0};
FakeBox g_shiny_box{&cls_bool, 0};
FakeBox g_count_box{&cls_int, 0};

// ----- gc handle table -------------------------------------------------------------
std::map<uintptr_t, void *> g_handles;
uintptr_t g_handle_seq = 0x900;
uint64_t g_gcnew_calls = 0;
uint64_t g_gcnew_fail_index = 0;  // fail the Nth gc_new (1-based); 0 = never

uintptr_t fake_gc_new(void *obj, int /*pinned*/) {
  ++g_gcnew_calls;
  if (obj == nullptr) return 0;
  if (g_gcnew_fail_index != 0 && g_gcnew_calls == g_gcnew_fail_index) return 0;
  uintptr_t h = ++g_handle_seq;
  g_handles[h] = obj;
  return h;
}
void *fake_gc_get(uintptr_t h) { auto it = g_handles.find(h); return it == g_handles.end() ? nullptr : it->second; }
void fake_gc_free(uintptr_t h) { g_handles.erase(h); }

void *fake_obj_class(void *obj) { return obj ? *static_cast<void **>(obj) : nullptr; }
void *fake_unbox(void *box) { return &static_cast<FakeBox *>(box)->val; }

// ----- field get -------------------------------------------------------------------
void fake_field_get(void *obj, void *field, void *out) {
  if (field == &f_id) { *static_cast<uint64_t *>(out) = static_cast<FakePokemon *>(obj)->id; return; }
  if (field == &f_species) { *static_cast<int32_t *>(out) = static_cast<FakePokemon *>(obj)->species; return; }
  if (field == &f_display) { *static_cast<void **>(out) = static_cast<FakePokemon *>(obj)->display; return; }
  if (field == &f_events) { *static_cast<void **>(out) = static_cast<FakePokemon *>(obj)->events; return; }
  if (field == &f_limited) { *static_cast<void **>(out) = static_cast<FakePokemon *>(obj)->limited; return; }
  if (field == &f_attack) { *static_cast<int32_t *>(out) = static_cast<FakePokemon *>(obj)->attack; return; }
  if (field == &f_defense) { *static_cast<int32_t *>(out) = static_cast<FakePokemon *>(obj)->defense; return; }
  if (field == &f_stamina) { *static_cast<int32_t *>(out) = static_cast<FakePokemon *>(obj)->stamina; return; }
  if (field == &f_favorite) { *static_cast<uint8_t *>(out) = static_cast<FakePokemon *>(obj)->favorite; return; }
  if (field == &f_egg) { *static_cast<uint8_t *>(out) = static_cast<FakePokemon *>(obj)->egg; return; }
  if (field == &f_costume) { *static_cast<int32_t *>(out) = static_cast<FakeDisplay *>(obj)->costume; return; }
  if (field == &f_original_costume) { *static_cast<int32_t *>(out) = static_cast<FakeDisplay *>(obj)->original_costume; return; }
  if (field == &f_background) { *static_cast<void **>(out) = static_cast<FakeDisplay *>(obj)->background; return; }
  if (field == &f_art) { *static_cast<int32_t *>(out) = static_cast<FakeDisplay *>(obj)->art; return; }
  if (field == &f_art_asset) { *static_cast<int32_t *>(out) = static_cast<FakeDisplay *>(obj)->art_asset; return; }
  assert(false && "unknown field_get token");
}

int32_t fake_string_length(void *s) { return static_cast<FakeString *>(s)->length; }

// ----- runtime_invoke --------------------------------------------------------------
int g_size_val = 0, g_rare_val = 0, g_shiny_val = 0, g_count_val = 0;
bool g_throw_size = false, g_throw_rare = false, g_throw_shiny = false, g_throw_count = false;
bool g_null_size = false, g_null_rare = false, g_null_shiny = false, g_null_count = false;
int g_size_calls = 0, g_rare_calls = 0, g_shiny_calls = 0, g_count_calls = 0;
std::function<void()> g_size_oneshot, g_rare_oneshot, g_shiny_oneshot;
void *g_expected_gm = &g_gm;

void *fake_runtime_invoke(const void *method, void * /*self*/, void **params, void **exc) {
  if (exc) *exc = nullptr;
  if (method == &m_size || method == &m_rare) assert(params && params[0] == g_expected_gm);
  if (method == &m_size) {
    ++g_size_calls;
    if (g_size_oneshot) { auto fn = g_size_oneshot; g_size_oneshot = nullptr; fn(); }
    if (g_throw_size) { if (exc) *exc = reinterpret_cast<void *>(0x1); return nullptr; }
    if (g_null_size) return nullptr;
    g_size_box.val = g_size_val;
    return &g_size_box;
  }
  if (method == &m_rare) {
    ++g_rare_calls;
    if (g_rare_oneshot) { auto fn = g_rare_oneshot; g_rare_oneshot = nullptr; fn(); }
    if (g_throw_rare) { if (exc) *exc = reinterpret_cast<void *>(0x1); return nullptr; }
    if (g_null_rare) return nullptr;
    g_rare_box.val = g_rare_val;
    return &g_rare_box;
  }
  if (method == &m_shiny) {
    ++g_shiny_calls;
    if (g_shiny_oneshot) { auto fn = g_shiny_oneshot; g_shiny_oneshot = nullptr; fn(); }
    if (g_throw_shiny) { if (exc) *exc = reinterpret_cast<void *>(0x1); return nullptr; }
    if (g_null_shiny) return nullptr;
    g_shiny_box.val = g_shiny_val;
    return &g_shiny_box;
  }
  if (method == &m_count) {
    ++g_count_calls;
    if (g_throw_count) { if (exc) *exc = reinterpret_cast<void *>(0x1); return nullptr; }
    if (g_null_count) return nullptr;
    g_count_box.val = g_count_val;
    return &g_count_box;
  }
  assert(false && "unknown method token");
  return nullptr;
}

PgrTransferFactsConfig make_config() {
  PgrTransferFactsConfig c{};
  c.version = 1;
  c.size = sizeof(c);
  c.gc_new = reinterpret_cast<void *>(&fake_gc_new);
  c.gc_get = reinterpret_cast<void *>(&fake_gc_get);
  c.gc_free = reinterpret_cast<void *>(&fake_gc_free);
  c.object_get_class = reinterpret_cast<void *>(&fake_obj_class);
  c.runtime_invoke = reinterpret_cast<void *>(&fake_runtime_invoke);
  c.object_unbox = reinterpret_cast<void *>(&fake_unbox);
  c.field_get_value = reinterpret_cast<void *>(&fake_field_get);
  c.string_length = reinterpret_cast<void *>(&fake_string_length);
  c.class_pokemon = &cls_pk;
  c.class_display = &cls_display;
  c.class_bool = &cls_bool;
  c.class_int = &cls_int;
  c.class_event = &cls_event;
  c.class_character_size = &cls_charsize;
  c.class_string = &cls_string;
  c.m_size = &m_size;
  c.m_rare = &m_rare;
  c.m_shiny = &m_shiny;
  c.m_event_count = &m_count;
  c.f_id = &f_id;
  c.f_species = &f_species;
  c.f_display = &f_display;
  c.f_events = &f_events;
  c.f_limited = &f_limited;
  c.f_attack = &f_attack;
  c.f_defense = &f_defense;
  c.f_stamina = &f_stamina;
  c.f_favorite = &f_favorite;
  c.f_egg = &f_egg;
  c.f_costume = &f_costume;
  c.f_original_costume = &f_original_costume;
  c.f_background = &f_background;
  c.f_art = &f_art;
  c.f_art_asset = &f_art_asset;
  c.xxs = 10;
  c.xxl = 14;
  c.costume_none = 0;
  c.art_none = 0;
  c.asset_none = 0;
  c.sizes[0] = 10;  // Xxs
  c.sizes[1] = 11;  // Xs
  c.sizes[2] = 12;  // M
  c.sizes[3] = 13;  // Xl
  c.sizes[4] = 14;  // Xxl
  return c;
}

uint64_t this_tid() { return static_cast<uint64_t>(static_cast<pid_t>(::gettid())); }

const PgrTransferFactsApi *g_api = nullptr;

// A fresh baseline pokemon/display: mid-range IVs, valid species, no event/background markers.
FakeDisplay make_display() { return FakeDisplay{&cls_display, 0, 0, 0, 0, nullptr}; }
FakePokemon make_pokemon(FakeDisplay *disp) {
  FakePokemon pk{};
  pk.klass = &cls_pk;
  pk.id = 0xABCDEF0123456789ULL;  // exercises the full 64 bits
  pk.species = 25;
  pk.attack = 5; pk.defense = 10; pk.stamina = 15;
  pk.favorite = 0; pk.egg = 0;
  pk.display = disp; pk.events = &g_events; pk.limited = nullptr;
  return pk;
}
PgrTransferFactsRequest make_request(FakePokemon *pk, uint32_t protect) {
  PgrTransferFactsRequest r{};
  r.version = 1;
  r.size = sizeof(r);
  r.owner_tid = this_tid();
  r.lease_ms = 2000;
  r.caught_id = pk->id;
  r.pokemon = pk;
  r.gm = &g_gm;
  r.protect = protect;
  return r;
}

void reset_flags() {
  g_gcnew_fail_index = 0; g_gcnew_calls = 0;
  g_size_val = g_rare_val = g_shiny_val = g_count_val = 0;
  g_throw_size = g_throw_rare = g_throw_shiny = g_throw_count = false;
  g_null_size = g_null_rare = g_null_shiny = g_null_count = false;
  g_size_calls = g_rare_calls = g_shiny_calls = g_count_calls = 0;
  g_size_oneshot = g_rare_oneshot = g_shiny_oneshot = nullptr;
  g_size_box.klass = &cls_charsize; g_rare_box.klass = &cls_bool;
  g_shiny_box.klass = &cls_bool; g_count_box.klass = &cls_int;
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
  assert(core->query_feature(35, 99) == nullptr);
  const auto *api = static_cast<const PgrTransferFactsApi *>(core->query_feature(35, 1));
  assert(api != nullptr && api->version == 1 && api->size == sizeof(PgrTransferFactsApi));
  g_api = api;

  std::printf("ABI sizes: config=%zu request=%zu out=%zu table=%zu\n",
              sizeof(PgrTransferFactsConfig), sizeof(PgrTransferFactsRequest),
              sizeof(PgrTransferFactsOut), sizeof(PgrTransferFactsApi));
  assert(sizeof(PgrTransferFactsConfig) == 320 && sizeof(PgrTransferFactsRequest) == 56 &&
         sizeof(PgrTransferFactsOut) == 40 && sizeof(PgrTransferFactsApi) == 24);

  const uint64_t tid = this_tid();
  PgrTransferFactsConfig cfg = make_config();
  PgrTransferFactsOut out{};

  // 1. install validation + not-installed + idempotence + reconfig.
  {
    FakeDisplay d = make_display(); FakePokemon pk = make_pokemon(&d);
    PgrTransferFactsRequest r = make_request(&pk, 0);
    assert(api->install(nullptr) == PGR_TF_ERR_NULL_ARG);
    PgrTransferFactsConfig bad = cfg; bad.version = 2;
    assert(api->install(&bad) == PGR_TF_ERR_BAD_VERSION);
    bad = cfg; bad.size = sizeof(cfg) - 1;
    assert(api->install(&bad) == PGR_TF_ERR_BAD_SIZE);
    bad = cfg; bad.f_art_asset = nullptr;
    assert(api->install(&bad) == PGR_TF_ERR_MISSING_PTR);
    bad = cfg; bad.sizes[1] = bad.sizes[0];  // sizes not distinct
    assert(api->install(&bad) == PGR_TF_ERR_BAD_CONFIG);
    bad = cfg; bad.xxl = 99;  // xxl not a member of sizes[]
    assert(api->install(&bad) == PGR_TF_ERR_BAD_CONFIG);
    bad = cfg; bad.xxl = bad.xxs;  // xxs/xxl not distinct
    assert(api->install(&bad) == PGR_TF_ERR_BAD_CONFIG);
    assert(api->inspect(&r, &out) == PGR_TF_ERR_NOT_INSTALLED);

    assert(api->install(&cfg) == PGR_TF_OK);
    assert(api->install(&cfg) == PGR_TF_OK);  // idempotent
    bad = cfg; bad.class_pokemon = &cls_other;
    assert(api->install(&bad) == PGR_TF_ERR_RECONFIG);
  }

  // 2. VALID baseline: no protect, no markers -> flags 0, known 0xFF, full-64 id echoed, NO roots left.
  { reset_flags();
    FakeDisplay d = make_display(); FakePokemon pk = make_pokemon(&d);
    PgrTransferFactsRequest r = make_request(&pk, 0);
    std::memset(&out, 0xee, sizeof(out));
    assert(api->inspect(&r, &out) == PGR_TF_VALID);
    assert(out.version == 1 && out.size == sizeof(out));
    assert(out.attack == 5 && out.defense == 10 && out.stamina == 15 && out.species == 25);
    assert(out.known == 0xFF && out.flags == 0);
    assert(out.caught_id == 0xABCDEF0123456789ULL);
    assert(g_size_calls == 0 && g_rare_calls == 1 && g_shiny_calls == 1 && g_count_calls == 1);
    assert(g_handles.empty());  // reader transfers nothing
  }

  // 3. Non-size flags: rare, shiny, event (count), background (LocationCard), favorite, egg.
  { reset_flags();
    FakeDisplay d = make_display(); d.background = &g_gm;  // any non-null marks a background
    FakePokemon pk = make_pokemon(&d); pk.favorite = 1; pk.egg = 1;
    g_rare_val = 1; g_shiny_val = 1; g_count_val = 3;
    PgrTransferFactsRequest r = make_request(&pk, 0);
    assert(api->inspect(&r, &out) == PGR_TF_VALID);
    uint32_t expect = PGR_TF_FLAG_RARE | PGR_TF_FLAG_SHINY | PGR_TF_FLAG_EVENT |
                      PGR_TF_FLAG_BACKGROUND | PGR_TF_FLAG_FAVORITE | PGR_TF_FLAG_EGG;
    assert(out.flags == expect && out.known == 0xFF);
    assert((out.flags & (PGR_TF_FLAG_XXS | PGR_TF_FLAG_XXL)) == 0);  // no size requested
    assert(g_handles.empty());
  }

  // 4. Event via costume / original-costume / limited string length.
  { reset_flags();
    FakeDisplay d = make_display(); d.costume = 7;  // != costume_none
    FakePokemon pk = make_pokemon(&d);
    PgrTransferFactsRequest r = make_request(&pk, 0);
    assert(api->inspect(&r, &out) == PGR_TF_VALID && (out.flags & PGR_TF_FLAG_EVENT));
    assert(g_handles.empty());

    reset_flags();
    FakeDisplay d2 = make_display(); d2.original_costume = 4;
    FakePokemon pk2 = make_pokemon(&d2);
    PgrTransferFactsRequest r2 = make_request(&pk2, 0);
    assert(api->inspect(&r2, &out) == PGR_TF_VALID && (out.flags & PGR_TF_FLAG_EVENT));
    assert(g_handles.empty());

    reset_flags();
    FakeString limited{&cls_string, 5};  // non-empty limited identifier
    FakeDisplay d3 = make_display(); FakePokemon pk3 = make_pokemon(&d3); pk3.limited = &limited;
    PgrTransferFactsRequest r3 = make_request(&pk3, 0);
    assert(api->inspect(&r3, &out) == PGR_TF_VALID && (out.flags & PGR_TF_FLAG_EVENT));
    assert(g_handles.empty());

    // Empty limited string (length 0) is NOT an event.
    reset_flags();
    FakeString empty{&cls_string, 0};
    FakeDisplay d4 = make_display(); FakePokemon pk4 = make_pokemon(&d4); pk4.limited = &empty;
    PgrTransferFactsRequest r4 = make_request(&pk4, 0);
    assert(api->inspect(&r4, &out) == PGR_TF_VALID && !(out.flags & PGR_TF_FLAG_EVENT));
    assert(g_handles.empty());
  }

  // 5. Size path: every CharacterSize value, XXS/XXL flags only with a protect bit, unknown enum rejected.
  { reset_flags(); g_size_val = 10;  // Xxs
    FakeDisplay d = make_display(); FakePokemon pk = make_pokemon(&d);
    PgrTransferFactsRequest r = make_request(&pk, PGR_TF_PROTECT_SIZE);
    assert(api->inspect(&r, &out) == PGR_TF_VALID);
    assert((out.flags & PGR_TF_FLAG_XXS) && !(out.flags & PGR_TF_FLAG_XXL));
    assert(g_size_calls == 1 && g_handles.empty());

    for (int v = 11; v <= 13; ++v) {  // Xs/M/Xl: valid enum but neither xxs nor xxl flag
      reset_flags(); g_size_val = v;
      FakeDisplay dm = make_display(); FakePokemon pm = make_pokemon(&dm);
      PgrTransferFactsRequest rm = make_request(&pm, PGR_TF_PROTECT_SIZE);
      assert(api->inspect(&rm, &out) == PGR_TF_VALID);
      assert((out.flags & (PGR_TF_FLAG_XXS | PGR_TF_FLAG_XXL)) == 0);
      assert(g_handles.empty());
    }

    reset_flags(); g_size_val = 14;  // Xxl
    FakeDisplay dx = make_display(); FakePokemon px = make_pokemon(&dx);
    PgrTransferFactsRequest rx = make_request(&px, PGR_TF_PROTECT_SIZE);
    assert(api->inspect(&rx, &out) == PGR_TF_VALID && (out.flags & PGR_TF_FLAG_XXL));
    assert(g_handles.empty());

    // Same xxs value but NO protect bit -> size never invoked, xxs flag false.
    reset_flags(); g_size_val = 10;
    FakeDisplay dn = make_display(); FakePokemon pn = make_pokemon(&dn);
    PgrTransferFactsRequest rn = make_request(&pn, 0);
    assert(api->inspect(&rn, &out) == PGR_TF_VALID && g_size_calls == 0);
    assert((out.flags & PGR_TF_FLAG_XXS) == 0 && g_handles.empty());

    // Unknown size enum -> MALFORMED (keep), no facts, no leak.
    reset_flags(); g_size_val = 99;
    FakeDisplay du = make_display(); FakePokemon pu = make_pokemon(&du);
    PgrTransferFactsRequest ru = make_request(&pu, PGR_TF_PROTECT_SIZE);
    assert(api->inspect(&ru, &out) == PGR_TF_ERR_MALFORMED);
    assert(out.known == 0 && out.flags == 0 && g_handles.empty());
  }

  // 6. Non-canonical booleans: boxed bool value 2, and raw favorite/egg field value 2 -> MALFORMED.
  { reset_flags(); g_rare_val = 2;
    FakeDisplay d = make_display(); FakePokemon pk = make_pokemon(&d);
    PgrTransferFactsRequest r = make_request(&pk, 0);
    assert(api->inspect(&r, &out) == PGR_TF_ERR_MALFORMED && g_handles.empty());

    reset_flags();
    FakeDisplay d2 = make_display(); FakePokemon pk2 = make_pokemon(&d2); pk2.favorite = 2;
    PgrTransferFactsRequest r2 = make_request(&pk2, 0);
    assert(api->inspect(&r2, &out) == PGR_TF_ERR_MALFORMED && g_handles.empty());
  }

  // 7. Wrong classes along the inspection: pokemon / display / events / box / limited string.
  { reset_flags();
    FakeDisplay d = make_display(); FakePokemon pk = make_pokemon(&d); pk.klass = &cls_other;
    PgrTransferFactsRequest r = make_request(&pk, 0);
    assert(api->inspect(&r, &out) == PGR_TF_ERR_IDENTITY && g_handles.empty());

    reset_flags();
    FakeDisplay wd = make_display(); wd.klass = &cls_other; FakePokemon pk2 = make_pokemon(&wd);
    PgrTransferFactsRequest r2 = make_request(&pk2, 0);
    assert(api->inspect(&r2, &out) == PGR_TF_ERR_IDENTITY && g_handles.empty());

    reset_flags();
    FakeEvents we{&cls_other}; FakeDisplay d3 = make_display(); FakePokemon pk3 = make_pokemon(&d3);
    pk3.events = &we;
    PgrTransferFactsRequest r3 = make_request(&pk3, 0);
    assert(api->inspect(&r3, &out) == PGR_TF_ERR_IDENTITY && g_handles.empty());

    reset_flags(); g_rare_box.klass = &cls_other;  // rare box is not System.Boolean
    FakeDisplay d4 = make_display(); FakePokemon pk4 = make_pokemon(&d4);
    PgrTransferFactsRequest r4 = make_request(&pk4, 0);
    assert(api->inspect(&r4, &out) == PGR_TF_ERR_MALFORMED && g_handles.empty());

    reset_flags(); g_count_box.klass = &cls_other;  // count box is not System.Int32
    FakeDisplay d5 = make_display(); FakePokemon pk5 = make_pokemon(&d5);
    PgrTransferFactsRequest r5 = make_request(&pk5, 0);
    assert(api->inspect(&r5, &out) == PGR_TF_ERR_MALFORMED && g_handles.empty());

    reset_flags();
    FakeString ws{&cls_other, 3};  // limited value is not a System.String
    FakeDisplay d6 = make_display(); FakePokemon pk6 = make_pokemon(&d6); pk6.limited = &ws;
    PgrTransferFactsRequest r6 = make_request(&pk6, 0);
    assert(api->inspect(&r6, &out) == PGR_TF_ERR_MALFORMED && g_handles.empty());
  }

  // 8. Managed throws (size + rare) -> MANAGED, nothing leaks.
  { reset_flags(); g_throw_rare = true;
    FakeDisplay d = make_display(); FakePokemon pk = make_pokemon(&d);
    PgrTransferFactsRequest r = make_request(&pk, 0);
    assert(api->inspect(&r, &out) == PGR_TF_ERR_MANAGED && g_handles.empty());

    reset_flags(); g_throw_size = true;
    FakeDisplay d2 = make_display(); FakePokemon pk2 = make_pokemon(&d2);
    PgrTransferFactsRequest r2 = make_request(&pk2, PGR_TF_PROTECT_SIZE);
    assert(api->inspect(&r2, &out) == PGR_TF_ERR_MANAGED && g_handles.empty());
  }

  // 9. Full 64-bit id: zero caught id, high-bit mismatch, and a mid-inspection id shift -> IDENTITY.
  { reset_flags();
    FakeDisplay d = make_display(); FakePokemon pk = make_pokemon(&d);
    PgrTransferFactsRequest r = make_request(&pk, 0); r.caught_id = 0;
    assert(api->inspect(&r, &out) == PGR_TF_ERR_IDENTITY && g_handles.empty());

    reset_flags();
    FakeDisplay d2 = make_display(); FakePokemon pk2 = make_pokemon(&d2);
    PgrTransferFactsRequest r2 = make_request(&pk2, 0);
    r2.caught_id = pk2.id ^ 0x1000000000000000ULL;  // differs only in a high bit
    assert(api->inspect(&r2, &out) == PGR_TF_ERR_IDENTITY && g_handles.empty());

    reset_flags();  // id matches before the managed work but shifts during it -> caught at recheck
    FakeDisplay d3 = make_display(); FakePokemon pk3 = make_pokemon(&d3);
    g_shiny_oneshot = [&]() { pk3.id ^= 0x0000000100000000ULL; };
    PgrTransferFactsRequest r3 = make_request(&pk3, 0);
    assert(api->inspect(&r3, &out) == PGR_TF_ERR_IDENTITY && out.known == 0 && g_handles.empty());
  }

  // 10. Partial root-allocation failure: pk root (1st gc_new), gm (2nd), rare box (4th) -> GCHANDLE.
  { reset_flags(); g_gcnew_fail_index = 1;
    FakeDisplay d = make_display(); FakePokemon pk = make_pokemon(&d);
    PgrTransferFactsRequest r = make_request(&pk, 0);
    assert(api->inspect(&r, &out) == PGR_TF_ERR_GCHANDLE && g_rare_calls == 0 && g_handles.empty());

    reset_flags(); g_gcnew_fail_index = 2;
    FakeDisplay d2 = make_display(); FakePokemon pk2 = make_pokemon(&d2);
    PgrTransferFactsRequest r2 = make_request(&pk2, 0);
    assert(api->inspect(&r2, &out) == PGR_TF_ERR_GCHANDLE && g_handles.empty());

    reset_flags(); g_gcnew_fail_index = 4;  // the rare box root fails after pk/gm/display succeed
    FakeDisplay d3 = make_display(); FakePokemon pk3 = make_pokemon(&d3);
    PgrTransferFactsRequest r3 = make_request(&pk3, 0);
    assert(api->inspect(&r3, &out) == PGR_TF_ERR_GCHANDLE && g_handles.empty());
  }

  // 11. Relocation: during the rare invoke, the collector moves BOTH the pokemon and its display; the
  //     module re-resolves each from its handle, so the IVs come from the moved pokemon and the background
  //     flag from the moved display. The stale originals are never read again.
  { reset_flags();
    FakeDisplay d = make_display(); FakePokemon pk = make_pokemon(&d);
    FakeDisplay moved_disp = make_display(); moved_disp.art = 9;  // moved display marks a background
    FakePokemon moved_pk = pk; moved_pk.attack = 1; moved_pk.defense = 2; moved_pk.stamina = 3;
    moved_pk.species = 777; moved_pk.display = &moved_disp;
    g_rare_oneshot = [&]() {
      for (auto &kv : g_handles) {
        if (kv.second == &pk) kv.second = &moved_pk;
        else if (kv.second == &d) kv.second = &moved_disp;
      }
    };
    PgrTransferFactsRequest r = make_request(&pk, 0);
    assert(api->inspect(&r, &out) == PGR_TF_VALID);
    assert(out.attack == 1 && out.defense == 2 && out.stamina == 3 && out.species == 777);
    assert(out.flags & PGR_TF_FLAG_BACKGROUND);  // read from the moved display
    assert(g_handles.empty());

    // GM relocation during the size invoke must not disturb the result.
    reset_flags(); g_size_val = 12;
    FakeGm moved_gm{&cls_other};
    g_size_oneshot = [&]() { for (auto &kv : g_handles) if (kv.second == &g_gm) kv.second = &moved_gm; g_expected_gm = &moved_gm; };
    FakeDisplay d2 = make_display(); FakePokemon pk2 = make_pokemon(&d2);
    PgrTransferFactsRequest r2 = make_request(&pk2, PGR_TF_PROTECT_SIZE);
    assert(api->inspect(&r2, &out) == PGR_TF_VALID && g_handles.empty());
    g_expected_gm = &g_gm;
  }

  // 12. Out-of-range metadata is rejected (keep): IV > 15, IV < 0, species 0, species > 2000.
  { reset_flags();
    FakeDisplay d = make_display(); FakePokemon pk = make_pokemon(&d); pk.attack = 16;
    PgrTransferFactsRequest r = make_request(&pk, 0);
    assert(api->inspect(&r, &out) == PGR_TF_ERR_MALFORMED && g_handles.empty());

    reset_flags();
    FakeDisplay d2 = make_display(); FakePokemon pk2 = make_pokemon(&d2); pk2.defense = -1;
    PgrTransferFactsRequest r2 = make_request(&pk2, 0);
    assert(api->inspect(&r2, &out) == PGR_TF_ERR_MALFORMED && g_handles.empty());

    reset_flags();
    FakeDisplay d3 = make_display(); FakePokemon pk3 = make_pokemon(&d3); pk3.species = 0;
    PgrTransferFactsRequest r3 = make_request(&pk3, 0);
    assert(api->inspect(&r3, &out) == PGR_TF_ERR_MALFORMED && g_handles.empty());

    reset_flags();
    FakeDisplay d4 = make_display(); FakePokemon pk4 = make_pokemon(&d4); pk4.species = 2001;
    PgrTransferFactsRequest r4 = make_request(&pk4, 0);
    assert(api->inspect(&r4, &out) == PGR_TF_ERR_MALFORMED && g_handles.empty());
  }

  // 13. Negative event count -> MALFORMED.
  { reset_flags(); g_count_val = -1;
    FakeDisplay d = make_display(); FakePokemon pk = make_pokemon(&d);
    PgrTransferFactsRequest r = make_request(&pk, 0);
    assert(api->inspect(&r, &out) == PGR_TF_ERR_MALFORMED && g_handles.empty());
  }

  // 14. Gate: zero lease / zero owner / wrong tid refuse (0) without any managed work or roots.
  { reset_flags();
    FakeDisplay d = make_display(); FakePokemon pk = make_pokemon(&d);
    PgrTransferFactsRequest r = make_request(&pk, 0);
    r.lease_ms = 0;
    assert(api->inspect(&r, &out) == PGR_TF_OK && g_rare_calls == 0 && out.known == 0);
    r.lease_ms = 2000; r.owner_tid = 0;
    assert(api->inspect(&r, &out) == PGR_TF_OK && g_rare_calls == 0);
    r.owner_tid = tid + 1;
    assert(api->inspect(&r, &out) == PGR_TF_OK && g_rare_calls == 0);
    std::thread([&]() {
      FakeDisplay dt = make_display(); FakePokemon pt = make_pokemon(&dt);
      PgrTransferFactsRequest rt = make_request(&pt, 0);
      rt.owner_tid = this_tid() + 1;  // owner tid never matches this worker thread
      PgrTransferFactsOut ot{};
      assert(g_api->inspect(&rt, &ot) == PGR_TF_OK && ot.known == 0);
    }).join();
    assert(g_handles.empty());
  }

  // 15. Expiry: the lease is spent DURING the managed work -> refused (0), out zeroed, nothing leaked.
  { reset_flags();
    FakeDisplay d = make_display(); FakePokemon pk = make_pokemon(&d);
    g_rare_oneshot = [&]() { ::usleep(3000); };  // outlast the 1 ms lease
    PgrTransferFactsRequest r = make_request(&pk, 0); r.lease_ms = 1;
    assert(api->inspect(&r, &out) == PGR_TF_OK && out.known == 0 && g_handles.empty());
  }

  // 16. Reentry: a nested inspect fired from inside a managed invoke is refused (busy); outer completes.
  { reset_flags();
    FakeDisplay d = make_display(); FakePokemon pk = make_pokemon(&d);
    int nested_rc = -99;
    g_rare_oneshot = [&]() {
      FakeDisplay dn = make_display(); FakePokemon pn = make_pokemon(&dn);
      PgrTransferFactsRequest rn = make_request(&pn, 0);
      PgrTransferFactsOut on{};
      nested_rc = g_api->inspect(&rn, &on);  // busy -> refused
      assert(on.known == 0);
    };
    PgrTransferFactsRequest r = make_request(&pk, 0);
    assert(api->inspect(&r, &out) == PGR_TF_VALID);
    assert(nested_rc == PGR_TF_OK && g_handles.empty());
  }

  std::printf("transfer_facts_test passed: ABI/validation/idempotence/reconfig/bad-config, VALID baseline "
              "(flags 0, known 0xFF, full-64 id, no roots), rare/shiny/event/background/favorite/egg flags, "
              "costume/limited event, every CharacterSize value + protect gating + unknown-size reject, "
              "non-canonical bool reject, wrong pokemon/display/events/box/string classes, managed throws, "
              "zero/high-bit/mid-inspection id mismatch, partial root failures, PK+display+GM relocation "
              "re-resolve, IV/species range reject, negative count, gate (lease/owner/thread), mid-work "
              "expiry, reentry busy guard\n");
  return 0;
}
