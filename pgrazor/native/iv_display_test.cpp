#include "iv_display.h"
#include "pgrazor_native_api.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>

// Real-API fake-IL2CPP / Dobby harness for feature 15 (encounter / inventory IV display).
// It resolves the table through the exported pgrazor_native_api v2 registry
// (query_feature(15,1)), installs against two real NOP-padded "methods" (so Dobby has
// room) with the exact SetPokemonUI / DoUpdate ABIs, and drives capture with a fake managed
// heap (classes, methods, fields, boxed value returns and System.String objects). No test
// claims are made here; the coordinator builds and runs this against libpgrazor.so after
// registering query_feature case 15.

namespace {

using GetHookApiFn = const void *(*)(uint32_t version);

// ----- class / method / field tokens (distinct addresses) -------------------------
int cls_plate, cls_map_pokemon, cls_wildmap, cls_pokemon, cls_text, cls_text_concrete,
    cls_encounter_out, cls_wild, cls_tappable, cls_panel, cls_string, cls_int, cls_bool,
    cls_single, cls_hwm, cls_ta, cls_outline, cls_color, cls_vector2, cls_gameobject, cls_other;
int m_get_level, m_get_multiplier, m_set_text, m_set_rich, m_set_font, m_set_fit, m_set_overflow,
    m_set_alignment, m_get_font, m_get_text, m_get_rich, m_get_fit, m_get_overflow, m_get_alignment,
    m_alive, m_ol_go, m_ol_get, m_ol_add, m_ol_geten, m_ol_seten, m_ol_colget, m_ol_colset,
    m_ol_distget, m_ol_distset;
int f_plate_text, f_plate_gm, f_map_wild, f_map_response, f_map_tappable, f_wild_lat, f_wild_lon,
    f_wild_pokemon, f_wild_id, f_response_wild, f_tappable_pokemon, f_cp, f_atk, f_def, f_sta,
    f_cpmul, f_addcpmul, f_panel_cptext, f_panel_displayed, f_panel_gm;
int g_gm_obj;  // GameMaster object token (non-null; passed as a reference arg, not class-checked)

// ----- fake managed objects --------------------------------------------------------
struct FakeOutline { void *klass; uint8_t enabled; float color[4]; float dist[2]; };
struct FakeGameObject { void *klass; FakeOutline *outline; };
struct FakeText {
  void *klass;
  char text[512];
  int font;
  int rich, fit, overflow, alignment;
  int destroyed;
  FakeGameObject go;
};
struct FakePokemon { void *klass; int32_t cp, atk, def, sta; float cpmul, addcpmul; };
struct FakeWild { void *klass; uint64_t id; double lat, lon; FakePokemon *pokemon; };
struct FakeEncounterOut { void *klass; FakeWild *wild; };
struct FakeTappable { void *klass; FakePokemon *pokemon; };
struct FakeMap { void *klass; FakeWild *map_wild; FakeEncounterOut *response; FakeTappable *tappable; };
struct FakePlate { void *klass; FakeText *cp_text; void *gm; };
struct FakePanel { void *klass; FakeText *cp_text; FakePokemon *displayed; void *gm; };

// Boxed value-return pool + System.String pool (reused; the native unboxes/reads before
// the next invoke, so a small pool is enough).
struct FakeBox { void *klass; union { int32_t i; float f; uint8_t b; float v4[4]; } data; };
struct FakeString { void *klass; char utf8[512]; uint16_t utf16[512]; int len; };
FakeBox g_box_pool[16];
int g_box_i = 0;
FakeString g_str_pool[16];
int g_str_i = 0;

FakeBox *box(void *klass) {
  FakeBox *b = &g_box_pool[g_box_i++ & 15];
  b->klass = klass;
  return b;
}
FakeBox *box_int(int32_t v) { FakeBox *b = box(&cls_int); b->data.i = v; return b; }
FakeBox *box_single(float v) { FakeBox *b = box(&cls_single); b->data.f = v; return b; }
FakeBox *box_bool(bool v) { FakeBox *b = box(&cls_bool); b->data.b = v ? 1 : 0; return b; }
FakeBox *box_enum(void *klass, int32_t v) { FakeBox *b = box(klass); b->data.i = v; return b; }
FakeBox *box_color(const float c[4]) { FakeBox *b = box(&cls_color); std::memcpy(b->data.v4, c, 16); return b; }
FakeBox *box_vector2(const float v[2]) { FakeBox *b = box(&cls_vector2); b->data.v4[0] = v[0]; b->data.v4[1] = v[1]; return b; }

FakeString *make_string(const char *utf8) {
  FakeString *s = &g_str_pool[g_str_i++ & 15];
  s->klass = &cls_string;
  std::snprintf(s->utf8, sizeof(s->utf8), "%s", utf8);
  int n = 0;
  for (const char *p = s->utf8; *p && n < 500; ++p) {
    unsigned char c = static_cast<unsigned char>(*p);
    if (c < 0x80) {
      s->utf16[n++] = c;
    } else if ((c & 0xE0) == 0xC0) {
      unsigned char c2 = static_cast<unsigned char>(*(p + 1));
      s->utf16[n++] = static_cast<uint16_t>(((c & 0x1F) << 6) | (c2 & 0x3F));
      ++p;
    }
  }
  s->len = n;
  return s;
}

// Runtime hooks the capture path can drive mid-call. Declared early so the string/GC shims
// below can re-enter the public API (reentrant disable during StringNew, nested stopping tick).
const PgrIvDisplayApi *g_api = nullptr;
uint64_t g_owner = 0;
bool g_stringnew_disables = false;  // on string_new: disable once (revokes before set_text runs)

// ----- GC handle bookkeeping -------------------------------------------------------
bool g_fail_root = false;
uint64_t g_reentrant_end_token = 0;
void *g_redirect_from = nullptr, *g_redirect_to = nullptr;
uintptr_t g_next_handle = 0x4000;
std::unordered_map<uintptr_t, void *> g_live;

uintptr_t fake_gc_new(void *obj, int) {
  if (obj == nullptr || g_fail_root) return 0;
  uintptr_t id = g_next_handle++;
  g_live[id] = obj;
  return id;
}
void *fake_gc_get(uintptr_t h) {
  auto it = g_live.find(h);
  return it != g_live.end() ? (it->second == g_redirect_from ? g_redirect_to : it->second) : nullptr;
}
void fake_gc_free(uintptr_t h) {
  if (g_reentrant_end_token) {
    const auto token = g_reentrant_end_token; g_reentrant_end_token = 0;
    assert(g_api->end_catch(token, g_owner) == PGR_IV_ERR_INVALID_ARG);
  }
  auto it = g_live.find(h);
  assert(it != g_live.end() && "double or invalid gchandle_free");
  g_live.erase(it);
}
void *fake_obj_class(void *obj) { return obj ? *static_cast<void **>(obj) : nullptr; }
void *fake_unbox(void *obj) { return &static_cast<FakeBox *>(obj)->data; }
uint8_t fake_assignable(void *klass, void *oklass) {
  if (klass == oklass) return 1;
  if (klass == &cls_text && oklass == &cls_text_concrete) return 1;
  if (klass == &cls_map_pokemon && oklass == &cls_wildmap) return 1;
  return 0;
}
void *fake_class_type(void *klass) { return klass; }  // 1:1 for the fake
void *fake_type_object(void *type) { return type; }
void *fake_string_new(const char *utf8) {
  if (g_stringnew_disables) { g_stringnew_disables = false; g_api->disable(); }
  return make_string(utf8);
}
int fake_string_len(void *s) { return static_cast<FakeString *>(s)->len; }
uint16_t *fake_string_chars(void *s) { return static_cast<FakeString *>(s)->utf16; }

// field_get_value dispatches on the field token and copies the member from the (rooted)
// fake object. Each token is globally unique, so the object type is implied.
void fake_field_get(void *obj, void *field, void *out) {
  if (field == &f_plate_text) { *static_cast<void **>(out) = static_cast<FakePlate *>(obj)->cp_text; return; }
  if (field == &f_plate_gm) { *static_cast<void **>(out) = static_cast<FakePlate *>(obj)->gm; return; }
  if (field == &f_map_wild) { *static_cast<void **>(out) = static_cast<FakeMap *>(obj)->map_wild; return; }
  if (field == &f_map_response) { *static_cast<void **>(out) = static_cast<FakeMap *>(obj)->response; return; }
  if (field == &f_map_tappable) { *static_cast<void **>(out) = static_cast<FakeMap *>(obj)->tappable; return; }
  if (field == &f_wild_lat) { *static_cast<double *>(out) = static_cast<FakeWild *>(obj)->lat; return; }
  if (field == &f_wild_lon) { *static_cast<double *>(out) = static_cast<FakeWild *>(obj)->lon; return; }
  if (field == &f_wild_pokemon) { *static_cast<void **>(out) = static_cast<FakeWild *>(obj)->pokemon; return; }
  if (field == &f_wild_id) { *static_cast<uint64_t *>(out) = static_cast<FakeWild *>(obj)->id; return; }
  if (field == &f_response_wild) { *static_cast<void **>(out) = static_cast<FakeEncounterOut *>(obj)->wild; return; }
  if (field == &f_tappable_pokemon) { *static_cast<void **>(out) = static_cast<FakeTappable *>(obj)->pokemon; return; }
  if (field == &f_cp) { *static_cast<int32_t *>(out) = static_cast<FakePokemon *>(obj)->cp; return; }
  if (field == &f_atk) { *static_cast<int32_t *>(out) = static_cast<FakePokemon *>(obj)->atk; return; }
  if (field == &f_def) { *static_cast<int32_t *>(out) = static_cast<FakePokemon *>(obj)->def; return; }
  if (field == &f_sta) { *static_cast<int32_t *>(out) = static_cast<FakePokemon *>(obj)->sta; return; }
  if (field == &f_cpmul) { *static_cast<float *>(out) = static_cast<FakePokemon *>(obj)->cpmul; return; }
  if (field == &f_addcpmul) { *static_cast<float *>(out) = static_cast<FakePokemon *>(obj)->addcpmul; return; }
  if (field == &f_panel_cptext) { *static_cast<void **>(out) = static_cast<FakePanel *>(obj)->cp_text; return; }
  if (field == &f_panel_displayed) { *static_cast<void **>(out) = static_cast<FakePanel *>(obj)->displayed; return; }
  if (field == &f_panel_gm) { *static_cast<void **>(out) = static_cast<FakePanel *>(obj)->gm; return; }
  assert(false && "unknown field token");
}

// More capture-path mid-call hooks (g_api / g_owner / g_stringnew_disables declared above).
bool g_getfont_disables = false;   // on get_font: disable once (revokes the in-flight apply)
bool g_getfont_toggles = false;    // on get_font: disable then re-enable (gen bump still revokes)
bool g_alive_nested_tick = false;  // on op_Implicit: re-enter a stopping tick once (guarded no-op)
void *g_throw_method = nullptr;    // make runtime_invoke of this method throw
bool g_outline_bad_class = false;  // AddComponent yields a non-outline object (local fail)

FakeOutline g_outline_pool[16];
int g_outline_i = 0;

void *fake_runtime_invoke(const void *method, void *obj, void **params, void **exc) {
  if (exc) *exc = nullptr;
  if (g_throw_method != nullptr && method == g_throw_method) {
    if (exc) *exc = &g_gm_obj;
    return nullptr;
  }
  if (method == &m_get_text) return make_string(static_cast<FakeText *>(obj)->text);
  if (method == &m_set_text) {
    std::snprintf(static_cast<FakeText *>(obj)->text, 512, "%s",
                  static_cast<FakeString *>(params[0])->utf8);
    return nullptr;
  }
  if (method == &m_get_font) {
    if (g_getfont_disables) { g_getfont_disables = false; g_api->disable(); }
    if (g_getfont_toggles) {
      g_getfont_toggles = false;
      g_api->disable();                        // bumps the revocation generation
      g_api->update(1, 1, 2000, g_owner);      // re-enable: a second bump still revokes the pinned gen
    }
    return box_int(static_cast<FakeText *>(obj)->font);
  }
  if (method == &m_set_font) { static_cast<FakeText *>(obj)->font = *static_cast<int32_t *>(params[0]); return nullptr; }
  if (method == &m_get_rich) return box_bool(static_cast<FakeText *>(obj)->rich != 0);
  if (method == &m_set_rich) { static_cast<FakeText *>(obj)->rich = *static_cast<int32_t *>(params[0]) & 0xff; return nullptr; }
  if (method == &m_get_fit) return box_bool(static_cast<FakeText *>(obj)->fit != 0);
  if (method == &m_set_fit) { static_cast<FakeText *>(obj)->fit = *static_cast<int32_t *>(params[0]) & 0xff; return nullptr; }
  if (method == &m_get_overflow) return box_enum(&cls_hwm, static_cast<FakeText *>(obj)->overflow);
  if (method == &m_set_overflow) { static_cast<FakeText *>(obj)->overflow = *static_cast<int32_t *>(params[0]); return nullptr; }
  if (method == &m_get_alignment) return box_enum(&cls_ta, static_cast<FakeText *>(obj)->alignment);
  if (method == &m_set_alignment) { static_cast<FakeText *>(obj)->alignment = *static_cast<int32_t *>(params[0]); return nullptr; }
  if (method == &m_alive) {
    if (g_alive_nested_tick) {
      g_alive_nested_tick = false;
      g_api->tick(g_owner, 1);  // reentrant stopping tick: guarded no-op while active_depth > 0
    }
    FakeText *t = static_cast<FakeText *>(params[0]);  // reference arg: object pointer itself
    return box_bool(t->destroyed == 0);
  }
  if (method == &m_get_level) {
    double mul = static_cast<double>(*static_cast<float *>(params[1]));
    return box_int(static_cast<int32_t>(mul * 20.0 + 1e-6));
  }
  if (method == &m_get_multiplier) {
    int32_t level = *static_cast<int32_t *>(params[1]);
    return box_single(level * 0.05f);
  }
  if (method == &m_ol_go) return &static_cast<FakeText *>(obj)->go;
  if (method == &m_ol_get) return static_cast<FakeGameObject *>(obj)->outline;
  if (method == &m_ol_add) {
    FakeOutline *o = &g_outline_pool[g_outline_i++ & 15];
    o->klass = g_outline_bad_class ? &cls_other : &cls_outline;
    o->enabled = 0;
    o->color[0] = o->color[1] = o->color[2] = 0.2f; o->color[3] = 0.5f;
    o->dist[0] = 2.0f; o->dist[1] = 2.0f;
    static_cast<FakeGameObject *>(obj)->outline = o;
    return o;
  }
  if (method == &m_ol_geten) return box_bool(static_cast<FakeOutline *>(obj)->enabled != 0);
  if (method == &m_ol_seten) { static_cast<FakeOutline *>(obj)->enabled = *static_cast<int32_t *>(params[0]) & 0xff; return nullptr; }
  if (method == &m_ol_colget) return box_color(static_cast<FakeOutline *>(obj)->color);
  if (method == &m_ol_colset) { std::memcpy(static_cast<FakeOutline *>(obj)->color, params[0], 16); return nullptr; }
  if (method == &m_ol_distget) return box_vector2(static_cast<FakeOutline *>(obj)->dist);
  if (method == &m_ol_distset) { std::memcpy(static_cast<FakeOutline *>(obj)->dist, params[0], 8); return nullptr; }
  assert(false && "unknown method token");
  return nullptr;
}

#define PGR_NOP8() asm volatile("nop; nop; nop; nop; nop; nop; nop; nop;")

// Game labels the originals write into the CP text before the onLeave snapshot.
const char *g_setup_label = "999";
const char *g_owned_label = "CP500";

void (*volatile call_setup)(void *, void *, int32_t, void *) = nullptr;
void (*volatile call_owned)(void *, void *, void *) = nullptr;

__attribute__((noinline)) void target_setup(void *self, void *map_pokemon, int32_t weather,
                                             void *method) {
  PGR_NOP8();
  (void)map_pokemon;
  (void)weather;
  (void)method;
  if (self) std::snprintf(static_cast<FakePlate *>(self)->cp_text->text, 512, "%s", g_setup_label);
}

__attribute__((noinline)) void target_owned(void *self, void *proto, void *method) {
  PGR_NOP8();
  (void)proto;
  (void)method;
  if (self) std::snprintf(static_cast<FakePanel *>(self)->cp_text->text, 512, "%s", g_owned_label);
}

uint64_t this_tid() { return static_cast<uint64_t>(static_cast<pid_t>(::gettid())); }

void init_text(FakeText *t) {
  t->klass = &cls_text_concrete;
  t->text[0] = '\0';
  t->font = 18;
  t->rich = 0;
  t->fit = 1;
  t->overflow = 7;
  t->alignment = 3;
  t->destroyed = 0;
  t->go.klass = &cls_gameobject;
  t->go.outline = nullptr;
}

PgrIvDisplayConfig make_config() {
  PgrIvDisplayConfig c{};
  c.version = 1;
  c.size = sizeof(PgrIvDisplayConfig);
  c.gchandle_new = reinterpret_cast<void *>(&fake_gc_new);
  c.gchandle_get_target = reinterpret_cast<void *>(&fake_gc_get);
  c.gchandle_free = reinterpret_cast<void *>(&fake_gc_free);
  c.object_get_class = reinterpret_cast<void *>(&fake_obj_class);
  c.object_unbox = reinterpret_cast<void *>(&fake_unbox);
  c.runtime_invoke = reinterpret_cast<void *>(&fake_runtime_invoke);
  c.field_get_value = reinterpret_cast<void *>(&fake_field_get);
  c.class_is_assignable_from = reinterpret_cast<void *>(&fake_assignable);
  c.class_get_type = reinterpret_cast<void *>(&fake_class_type);
  c.type_get_object = reinterpret_cast<void *>(&fake_type_object);
  c.string_new = reinterpret_cast<void *>(&fake_string_new);
  c.string_length = reinterpret_cast<void *>(&fake_string_len);
  c.string_chars = reinterpret_cast<void *>(&fake_string_chars);
  c.m_get_level = &m_get_level;
  c.m_get_multiplier = &m_get_multiplier;
  c.m_set_text = &m_set_text;
  c.m_set_rich = &m_set_rich;
  c.m_set_font = &m_set_font;
  c.m_set_fit = &m_set_fit;
  c.m_set_overflow = &m_set_overflow;
  c.m_set_alignment = &m_set_alignment;
  c.m_get_font = &m_get_font;
  c.m_get_text = &m_get_text;
  c.m_get_rich = &m_get_rich;
  c.m_get_fit = &m_get_fit;
  c.m_get_overflow = &m_get_overflow;
  c.m_get_alignment = &m_get_alignment;
  c.m_alive = &m_alive;
  c.m_outline_gameobject = &m_ol_go;
  c.m_outline_getcomponent = &m_ol_get;
  c.m_outline_addcomponent = &m_ol_add;
  c.m_outline_getenabled = &m_ol_geten;
  c.m_outline_setenabled = &m_ol_seten;
  c.m_outline_colorget = &m_ol_colget;
  c.m_outline_colorset = &m_ol_colset;
  c.m_outline_distanceget = &m_ol_distget;
  c.m_outline_distanceset = &m_ol_distset;
  c.setup_address = reinterpret_cast<void *>(&target_setup);
  c.owned_update_address = reinterpret_cast<void *>(&target_owned);
  c.class_plate = &cls_plate;
  c.class_map_pokemon = &cls_map_pokemon;
  c.class_wildmap = &cls_wildmap;
  c.class_pokemon = &cls_pokemon;
  c.class_text = &cls_text;
  c.class_encounter_out = &cls_encounter_out;
  c.class_wild_proto = &cls_wild;
  c.class_tappable = &cls_tappable;
  c.class_panel = &cls_panel;
  c.class_string = &cls_string;
  c.class_int = &cls_int;
  c.class_bool = &cls_bool;
  c.class_single = &cls_single;
  c.class_hwm = &cls_hwm;
  c.class_ta = &cls_ta;
  c.class_outline = &cls_outline;
  c.class_color = &cls_color;
  c.class_vector2 = &cls_vector2;
  c.f_plate_text = &f_plate_text;
  c.f_plate_gm = &f_plate_gm;
  c.f_map_wild = &f_map_wild;
  c.f_map_response = &f_map_response;
  c.f_map_tappable = &f_map_tappable;
  c.f_wild_lat = &f_wild_lat;
  c.f_wild_lon = &f_wild_lon;
  c.f_wild_pokemon = &f_wild_pokemon;
  c.f_wild_id = &f_wild_id;
  c.f_response_wild = &f_response_wild;
  c.f_tappable_pokemon = &f_tappable_pokemon;
  c.f_cp = &f_cp;
  c.f_atk = &f_atk;
  c.f_def = &f_def;
  c.f_sta = &f_sta;
  c.f_cpmul = &f_cpmul;
  c.f_addcpmul = &f_addcpmul;
  c.f_panel_cptext = &f_panel_cptext;
  c.f_panel_displayed = &f_panel_displayed;
  c.f_panel_gm = &f_panel_gm;
  c.overflow_value = 1;   // HorizontalWrapMode.Overflow
  c.alignment_value = 4;  // TextAnchor.MiddleCenter
  return c;
}

PgrIvDisplaySample take_sample() {
  PgrIvDisplaySample s{};
  assert(g_api->sample(&s) == PGR_IV_OK);
  assert(s.version == 1 && s.size == sizeof(PgrIvDisplaySample));
  return s;
}

// External-observer (shared) mode. Config is immutable per process once installed, so this runs
// in a forked child that installs with external_catch_observer == 1 BEFORE the parent installs
// its default-mode config. It verifies the Dobby setup hook is skipped (the original runs un-
// intercepted), that begin/end drive the catch overlay exactly like the standalone hook, and the
// identity/thread/nesting/cancellation/duplicate/mismatch guards, leaving every GC handle freed.
uint64_t begin_shared(const PgrIvDisplayApi *api, void *plate, void *map, uint64_t tid) {
  void *forwarded[2]{};
  const auto token = api->begin_catch(plate, map, tid, forwarded);
  if (token) {
    assert(forwarded[0] == (plate == g_redirect_from ? g_redirect_to : plate));
    assert(forwarded[1] == map);
  }
  return token;
}

int run_shared_mode(void *lib) {
  auto getter = reinterpret_cast<GetHookApiFn>(::dlsym(lib, "pgrazor_native_api"));
  assert(getter);
  const auto *core = static_cast<const pgrazor_native_api_v2 *>(getter(2));
  assert(core && core->size == 48);
  const auto *api = static_cast<const PgrIvDisplayApi *>(core->query_feature(15, 1));
  assert(api != nullptr && api->version == 1 && api->size == sizeof(PgrIvDisplayApi));
  g_api = api;

  const uint64_t tid = this_tid();
  g_owner = tid;
  int dummy_method = 0;
  void *gm = &g_gm_obj;

  PgrIvDisplayConfig cfg = make_config();
  cfg.external_catch_observer = 1;  // shared mode: native must NOT hook setup_address
  // begin/end are inert before install.
  assert(begin_shared(api, nullptr, nullptr, tid) == 0);
  assert(api->end_catch(1, tid) == PGR_IV_ERR_NOT_INSTALLED);
  assert(api->install(&cfg) == PGR_IV_OK);

  // Fixture: a perfect-IV (100%) wild encounter at level 10 (mirrors the default-mode block).
  FakeText plate_text; init_text(&plate_text);
  FakePlate plate{&cls_plate, &plate_text, gm};
  FakePokemon mon{&cls_pokemon, 123, 15, 15, 15, 0.5f, 0.0f};
  FakeWild wild_resp{&cls_wild, 1001, 37.5, -122.3, &mon};
  FakeWild wild_map{&cls_wild, 1001, 0.0, 0.0, nullptr};
  FakeEncounterOut enc{&cls_encounter_out, &wild_resp};
  FakeMap map{&cls_wildmap, &wild_map, &enc, nullptr};
  const char *kOverlay = "999 \xc2\xb7 Lv 10 \xc2\xb7 <color=\"#ff5252\">IV 100%</color>";

  // S1. The Dobby setup hook is SKIPPED: calling the original directly performs no capture.
  assert(api->update(1, 1, 2000, tid) == PGR_IV_OK);
  target_setup(&plate, &map, 0, &dummy_method);  // un-intercepted: writes only the game label
  assert(std::strcmp(plate_text.text, "999") == 0 && take_sample().catch_active == 0);

  // S2. begin/original/end overlays exactly like the standalone hook. begin returns a nonzero
  //     monotonic token; Gum runs the original; end captures from the refreshed roots.
  init_text(&plate_text);
  uint64_t t1 = begin_shared(api, &plate, &map, tid);
  assert(t1 != 0);
  assert(take_sample().encounter_serial >= 1);  // begin announced the encounter
  target_setup(&plate, &map, 0, &dummy_method);  // the original Gum would chain
  assert(api->end_catch(t1, tid) == PGR_IV_OK);
  assert(take_sample().catch_active == 1 && std::strcmp(plate_text.text, kOverlay) == 0);

  // S3. Duplicate end is rejected (frame already closed) -- no double free, no steal.
  assert(api->end_catch(t1, tid) == PGR_IV_ERR_INVALID_ARG);
  assert(api->tick(tid, 1) == PGR_IV_OK);  // clear the record

  // S4. Monotonic tokens + mismatched end: a wrong token must NOT steal/clear the live frame.
  init_text(&plate_text);
  uint64_t t2 = begin_shared(api, &plate, &map, tid);
  assert(t2 != 0 && t2 != t1);              // monotonic
  assert(api->end_catch(t2 + 9999, tid) == PGR_IV_ERR_INVALID_ARG);  // mismatch: frame intact
  target_setup(&plate, &map, 0, &dummy_method);
  assert(api->end_catch(t2, tid) == PGR_IV_OK);  // the matching end still completes
  assert(std::strcmp(plate_text.text, kOverlay) == 0);
  assert(api->tick(tid, 1) == PGR_IV_OK);

  // S5. Non-reentrant: a second begin while a frame is open returns 0 (no second claim).
  init_text(&plate_text);
  uint64_t t3 = begin_shared(api, &plate, &map, tid);
  assert(t3 != 0);
  assert(begin_shared(api, &plate, &map, tid) == 0);  // nested begin refused
  target_setup(&plate, &map, 0, &dummy_method);
  assert(api->end_catch(t3, tid) == PGR_IV_OK);
  assert(api->tick(tid, 1) == PGR_IV_OK);

  // S6. Thread identity: a foreign thread cannot claim (not the owner) nor complete a frame.
  init_text(&plate_text);
  uint64_t t4 = begin_shared(api, &plate, &map, tid);
  assert(t4 != 0);
  std::thread([&]() {
    assert(begin_shared(g_api, &plate, &map, this_tid()) == 0);        // foreign: not the owner
    assert(g_api->end_catch(t4, this_tid()) == PGR_IV_ERR_INVALID_ARG);  // foreign end rejected
  }).join();
  // A proven_tid that does not match the real caller is also refused.
  assert(begin_shared(api, &plate, &map, tid + 1) == 0);
  target_setup(&plate, &map, 0, &dummy_method);
  assert(api->end_catch(t4, tid) == PGR_IV_OK);  // the real owner still completes its frame
  assert(std::strcmp(plate_text.text, kOverlay) == 0);
  assert(api->tick(tid, 1) == PGR_IV_OK);

  // S7. Cancellation straddling the original: a disable after begin (before end) must never
  //     apply a stale overlay. The record is retained un-applied (the on-screen label stays the
  //     game's own) and the channel does not fault; a later enable+tick applies it.
  init_text(&plate_text);
  assert(api->update(1, 1, 2000, tid) == PGR_IV_OK);
  uint64_t t5 = begin_shared(api, &plate, &map, tid);
  assert(t5 != 0);
  target_setup(&plate, &map, 0, &dummy_method);
  assert(api->update(0, 0, 2000, tid) == PGR_IV_OK);  // preference OFF while in flight
  assert(api->end_catch(t5, tid) == PGR_IV_OK);
  PgrIvDisplaySample s = take_sample();
  assert(std::strcmp(plate_text.text, "999") == 0);                   // stale overlay NOT applied
  assert(s.catch_active == 1 && (s.fault & PGR_IV_FAULT_CATCH) == 0);  // retained, un-applied
  assert(api->update(1, 1, 2000, tid) == PGR_IV_OK);
  assert(api->tick(tid, 0) == PGR_IV_OK);
  assert(std::strcmp(plate_text.text, kOverlay) == 0);               // applied on re-enable

  // S7b. OFF then ON during the original still revokes the old generation.
  assert(api->tick(tid, 1) == PGR_IV_OK);
  assert(api->update(1, 1, 2000, tid) == PGR_IV_OK);
  uint64_t t6 = begin_shared(api, &plate, &map, tid);
  target_setup(&plate, &map, 0, &dummy_method);
  assert(api->update(0, 0, 2000, tid) == PGR_IV_OK);
  assert(api->update(1, 1, 2000, tid) == PGR_IV_OK);
  g_reentrant_end_token = t6; // cleanup re-entry must not consume this frame twice
  assert(api->end_catch(t6, tid) == PGR_IV_OK);
  assert(g_reentrant_end_token == 0);
  assert(std::strcmp(plate_text.text, "999") == 0);
  assert(api->tick(tid, 0) == PGR_IV_OK);
  assert(std::strcmp(plate_text.text, kOverlay) == 0);

  // S7c. Runtime stop while the original runs releases the frame without recapture.
  assert(api->tick(tid, 1) == PGR_IV_OK);
  assert(api->update(1, 1, 2000, tid) == PGR_IV_OK);
  uint64_t t7 = begin_shared(api, &plate, &map, tid);
  target_setup(&plate, &map, 0, &dummy_method);
  assert(api->disable() == PGR_IV_OK);
  assert(api->end_catch(t7, tid) == PGR_IV_OK);
  assert(take_sample().catch_active == 0 && take_sample().owned_root_count == 0);

  // S7d. A disabled entry keeps its zero generation even if enabled by the original.
  assert(api->update(0, 0, 2000, tid) == PGR_IV_OK);
  uint64_t t8 = begin_shared(api, &plate, &map, tid);
  target_setup(&plate, &map, 0, &dummy_method);
  assert(api->update(1, 1, 2000, tid) == PGR_IV_OK);
  assert(api->end_catch(t8, tid) == PGR_IV_OK);
  assert(std::strcmp(plate_text.text, "999") == 0);
  assert(api->tick(tid, 0) == PGR_IV_OK);
  assert(std::strcmp(plate_text.text, kOverlay) == 0);

  // S7e. Gum receives fresh arguments when the rooted plate target changes.
  FakeText moved_text; init_text(&moved_text);
  FakePlate moved_plate{&cls_plate, &moved_text, gm};
  g_redirect_from = &plate; g_redirect_to = &moved_plate;
  void *forwarded[2]{};
  uint64_t t9 = api->begin_catch(&plate, &map, tid, forwarded);
  assert(t9 && forwarded[0] == &moved_plate && forwarded[1] == &map);
  target_setup(forwarded[0], forwarded[1], 0, &dummy_method);
  assert(api->end_catch(t9, tid) == PGR_IV_OK);
  assert(std::strcmp(moved_text.text, kOverlay) == 0);
  g_redirect_from = g_redirect_to = nullptr;

  // S8. The owned panel hook is STILL a Dobby hook in shared mode.
  FakeText panel_text; init_text(&panel_text);
  FakePanel panel{&cls_panel, &panel_text, &mon, gm};
  assert(api->update(0, 1, 2000, tid) == PGR_IV_OK);
  call_owned(&panel, &mon, &dummy_method);
  assert(take_sample().owned_count == 1 && std::strstr(panel_text.text, "</b>") != nullptr);

  // S9. Stop restores+frees everything; every GC handle is balanced.
  assert(api->tick(tid, 1) == PGR_IV_OK);
  s = take_sample();
  assert(s.catch_active == 0 && s.owned_count == 0 && s.owned_root_count == 0);
  assert(g_live.empty());

  std::printf("iv_display_test[shared] passed: setup hook skipped, begin/end overlay parity, "
              "duplicate/mismatched end rejected, monotonic tokens, non-reentrant begin, thread "
              "identity, cancellation during original retains un-applied, owned Dobby hook intact, "
              "stop cleanup balanced\n");
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  const char *so_path = argc > 1 ? argv[1] : "./libpgrazor.so";
  void *lib = ::dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
  if (!lib) {
    std::fprintf(stderr, "dlopen(%s) failed: %s\n", so_path, ::dlerror());
    return 1;
  }
  call_setup = &target_setup;
  call_owned = &target_owned;

  // The native config is immutable once installed, so the shared-mode scenario must run in a
  // fresh process. Fork BEFORE the parent installs its default-mode config; the child installs
  // external_catch_observer == 1 against its own copy of the module's (still-fresh) state.
  pid_t shared_pid = ::fork();
  assert(shared_pid >= 0);
  if (shared_pid == 0) {
    int rc = run_shared_mode(lib);
    std::fflush(stdout);
    _exit(rc);
  }

  auto getter = reinterpret_cast<GetHookApiFn>(::dlsym(lib, "pgrazor_native_api"));
  assert(getter);
  const auto *core = static_cast<const pgrazor_native_api_v2 *>(getter(2));
  assert(core && core->size == 48);
  assert(core->query_feature(15, 99) == nullptr);  // exact version only
  const auto *api = static_cast<const PgrIvDisplayApi *>(core->query_feature(15, 1));
  assert(api != nullptr && api->version == 1 && api->size == sizeof(PgrIvDisplayApi));
  g_api = api;

  const uint64_t tid = this_tid();
  g_owner = tid;
  PgrIvDisplayConfig cfg = make_config();

  // Validation before install.
  assert(api->install(nullptr) == PGR_IV_ERR_INVALID_ARG);
  PgrIvDisplayConfig bad = cfg;
  bad.version = 2;
  assert(api->install(&bad) == PGR_IV_ERR_BAD_VERSION);
  bad = cfg; bad.size = sizeof(PgrIvDisplayConfig) - 1;
  assert(api->install(&bad) == PGR_IV_ERR_BAD_SIZE);
  bad = cfg; bad.class_text = nullptr;  // required pointer
  assert(api->install(&bad) == PGR_IV_ERR_MISSING_PTR);
  bad = cfg; bad.setup_address = bad.owned_update_address;
  assert(api->install(&bad) == PGR_IV_ERR_DUPLICATE_TARGET);

  // Not installed yet.
  PgrIvDisplaySample s{};
  assert(api->update(1, 1, 1000, tid) == PGR_IV_ERR_NOT_INSTALLED);
  assert(api->tick(tid, 0) == PGR_IV_ERR_NOT_INSTALLED);
  assert(api->disable() == PGR_IV_ERR_NOT_INSTALLED);

  // Install, idempotent re-install, refused rebind.
  assert(api->install(&cfg) == PGR_IV_OK);
  assert(api->install(&cfg) == PGR_IV_OK);
  bad = cfg; bad.class_color = &cls_other;
  assert(api->install(&bad) == PGR_IV_ERR_RECONFIG_MISMATCH);

  int dummy_method = 0;
  void *gm = &g_gm_obj;

  // Shared encounter fixture: a perfect-IV (100%) wild encounter at level 10.
  FakeText plate_text; init_text(&plate_text);
  FakePlate plate{&cls_plate, &plate_text, gm};
  FakePokemon mon{&cls_pokemon, 123, 15, 15, 15, 0.5f, 0.0f};
  FakeWild wild_resp{&cls_wild, 1001, 37.5, -122.3, &mon};
  FakeWild wild_map{&cls_wild, 1001, 0.0, 0.0, nullptr};
  FakeEncounterOut enc{&cls_encounter_out, &wild_resp};
  FakeMap map{&cls_wildmap, &wild_map, &enc, nullptr};

  const char *kCatchOverlay = "999 \xc2\xb7 Lv 10 \xc2\xb7 <color=\"#ff5252\">IV 100%</color>";
  const char *kCatchFull = "123 \xc2\xb7 Lv 10 \xc2\xb7 <color=\"#ff5252\">IV 100%</color>";

  // 1. Valid catch overlay. SetPokemonUI drives the original (writing "999"), then the
  //    onLeave capture overlays the computed label. The on-screen prefix is the game's own
  //    CP text ("999"); the telemetry label carries the computed CP ("123").
  assert(api->update(1, 1, 2000, tid) == PGR_IV_OK);
  call_setup(&plate, &map, 0, &dummy_method);
  s = take_sample();
  assert(s.catch_active == 1 && s.catch_serial >= 1 && s.catch_notice == PGR_IV_NOTICE_LABEL);
  assert(s.catch_cp == 123 && s.catch_attack == 15 && s.catch_level_half == 20);
  assert(std::strcmp(s.catch_label, kCatchFull) == 0);
  assert(std::strcmp(plate_text.text, kCatchOverlay) == 0);
  assert(plate_text.rich == 1 && plate_text.fit == 0 && plate_text.font == 18 &&
         plate_text.overflow == 1 && plate_text.alignment == 4);
  assert(s.position_serial >= 1);
  assert(std::fabs(s.latitude - 37.5) < 1e-9 && std::fabs(s.longitude + 122.3) < 1e-9 &&
         s.encounter_id == 1001);
  assert(plate_text.go.outline == nullptr);  // outline is OWNED-only; catch never touches it

  // 2. Disable restores the authentic label and style; the record is retained (re-enable
  //    re-applies). Only our own label is restored (ownership check).
  assert(api->disable() == PGR_IV_OK);
  assert(api->tick(tid, 0) == PGR_IV_OK);
  assert(std::strcmp(plate_text.text, "999") == 0 && plate_text.rich == 0 && plate_text.fit == 1);
  assert(take_sample().catch_active == 1);  // still present, just restored
  // Re-enable re-applies.
  assert(api->update(1, 1, 2000, tid) == PGR_IV_OK);
  assert(api->tick(tid, 0) == PGR_IV_OK);
  assert(std::strcmp(plate_text.text, kCatchOverlay) == 0);

  // 3. The game independently refreshed the label: a disable must NOT clobber it.
  std::snprintf(plate_text.text, 512, "%s", "player-edited");
  assert(api->disable() == PGR_IV_OK);
  assert(api->tick(tid, 0) == PGR_IV_OK);
  assert(std::strcmp(plate_text.text, "player-edited") == 0);  // left intact
  assert(api->tick(tid, 1) == PGR_IV_OK);  // stop clears the record
  assert(take_sample().catch_active == 0);

  // 4. Encounter-id mismatch -> pending, no overlay.
  wild_map.id = 2002;
  init_text(&plate_text);
  assert(api->update(1, 1, 2000, tid) == PGR_IV_OK);
  uint64_t before = take_sample().catch_serial;
  call_setup(&plate, &map, 0, &dummy_method);
  s = take_sample();
  assert(s.catch_serial == before + 1 && s.catch_notice == PGR_IV_NOTICE_PENDING &&
         s.catch_reason == PGR_IV_REASON_ENCOUNTER_ID_MISMATCH && s.catch_active == 0);
  assert(std::strcmp(plate_text.text, "999") == 0);
  wild_map.id = 1001;

  // 5. Tappable branch (no response): resolves the Pokemon through the tappable proto.
  FakeTappable tap{&cls_tappable, &mon};
  FakeMap tmap{&cls_wildmap, nullptr, nullptr, &tap};
  init_text(&plate_text);
  call_setup(&plate, &tmap, 0, &dummy_method);
  assert(take_sample().catch_active == 1 && std::strcmp(plate_text.text, kCatchOverlay) == 0);
  assert(api->tick(tid, 1) == PGR_IV_OK);

  // 6. Invalid CP (<=0) and invalid IV (>15): CP fails closed (pending); an out-of-range IV
  //    renders '--' not zero.
  init_text(&plate_text);
  FakePokemon bad_cp{&cls_pokemon, 0, 15, 15, 15, 0.5f, 0.0f};
  wild_resp.pokemon = &bad_cp;
  before = take_sample().catch_serial;
  call_setup(&plate, &map, 0, &dummy_method);
  s = take_sample();
  assert(s.catch_notice == PGR_IV_NOTICE_PENDING && s.catch_reason == PGR_IV_REASON_STATS_NOT_POPULATED);
  assert(std::strcmp(plate_text.text, "999") == 0);

  init_text(&plate_text);
  FakePokemon bad_iv{&cls_pokemon, 123, 99, 15, 15, 0.5f, 0.0f};  // attack out of 0..15
  wild_resp.pokemon = &bad_iv;
  call_setup(&plate, &map, 0, &dummy_method);
  assert(std::strstr(plate_text.text, "IV --") != nullptr);  // '--' not a number, not zero
  assert(api->tick(tid, 1) == PGR_IV_OK);
  wild_resp.pokemon = &mon;

  // 7. Gradient thresholds: >=80 band, <80 default (#dedede), and the perfect #ff5252.
  auto overlay_for = [&](int a, int d, int st) {
    init_text(&plate_text);
    FakePokemon p{&cls_pokemon, 123, a, d, st, 0.5f, 0.0f};
    wild_resp.pokemon = &p;
    call_setup(&plate, &map, 0, &dummy_method);
    static char buf[512];
    std::snprintf(buf, sizeof(buf), "%s", plate_text.text);
    api->tick(tid, 1);
    wild_resp.pokemon = &mon;
    return std::string(buf);
  };
  assert(overlay_for(15, 15, 15).find("#ff5252") != std::string::npos);     // 100%
  assert(overlay_for(10, 10, 10).find("#dedede") != std::string::npos);     // 66.7% -> default
  assert(overlay_for(12, 12, 13).find("<color=\"#") != std::string::npos);  // ~82% -> gradient

  // 8. Half-level math: a cp-multiplier at the quadratic mean of level 10 and 11 renders Lv 10.5.
  float lo = 10 * 0.05f, hi = 11 * 0.05f;
  float qm = static_cast<float>(std::sqrt((static_cast<double>(lo) * lo + static_cast<double>(hi) * hi) / 2.0));
  init_text(&plate_text);
  FakePokemon half{&cls_pokemon, 123, 15, 15, 15, qm, 0.0f};
  wild_resp.pokemon = &half;
  call_setup(&plate, &map, 0, &dummy_method);
  assert(std::strstr(plate_text.text, "Lv 10.5") != nullptr);
  assert(api->tick(tid, 1) == PGR_IV_OK);
  wild_resp.pokemon = &mon;

  // 9. Owned overlay is bold; catch and owned toggle independently.
  FakeText panel_text; init_text(&panel_text);
  FakePanel panel{&cls_panel, &panel_text, &mon, gm};
  assert(api->update(0, 1, 2000, tid) == PGR_IV_OK);  // catch OFF, owned ON
  call_owned(&panel, &mon, &dummy_method);
  s = take_sample();
  assert(s.owned_count == 1 && s.owned_notice == PGR_IV_NOTICE_LABEL && s.owned_cp == 123);
  assert(std::strncmp(panel_text.text, "<b>CP500 \xc2\xb7", 11) == 0 &&
         std::strstr(panel_text.text, "</b>") != nullptr);
  // Owned records DO snapshot/apply the optional outline (created then enabled here).
  assert(panel_text.go.outline != nullptr && panel_text.go.outline->enabled == 1);
  // Catch is off: a fresh encounter is CAPTURED but left un-applied -- the on-screen label
  // stays the game's own ("999"), yet the record is retained (catch_active == 1) so toggling
  // the channel ON mid-encounter re-applies it WITHOUT a new SetPokemonUI pass. This mirrors
  // the JS captureIv, which snapshots the record even while the display setting is off.
  init_text(&plate_text);
  call_setup(&plate, &map, 0, &dummy_method);
  s = take_sample();
  assert(std::strcmp(plate_text.text, "999") == 0);  // original label untouched (not applied)
  assert(s.catch_active == 1 && (s.fault & PGR_IV_FAULT_CATCH) == 0);  // retained, un-applied
  // Toggle catch ON (no new hook pass): the next owner-thread tick applies the retained record.
  assert(api->update(1, 1, 2000, tid) == PGR_IV_OK);
  assert(api->tick(tid, 0) == PGR_IV_OK);
  assert(std::strcmp(plate_text.text, kCatchOverlay) == 0 && take_sample().catch_active == 1);
  assert(api->tick(tid, 1) == PGR_IV_OK);  // clear catch+owned for the next block
  assert(panel_text.go.outline->enabled == 0);  // owned outline restored to its snapshot on stop

  // 10. max-8 eviction: nine distinct panels keep only eight overlays (oldest evicted).
  assert(api->update(0, 1, 2000, tid) == PGR_IV_OK);
  FakeText pt[9];
  FakePanel pp[9];
  for (int i = 0; i < 9; ++i) {
    init_text(&pt[i]);
    pp[i] = FakePanel{&cls_panel, &pt[i], &mon, gm};
    call_owned(&pp[i], &mon, &dummy_method);
  }
  s = take_sample();
  assert(s.owned_count == PGR_IV_MAX_OWNED);
  // The oldest panel was evicted and restored to its game label.
  assert(std::strcmp(pt[0].text, "CP500") == 0);
  assert(std::strstr(pt[8].text, "</b>") != nullptr);
  assert(api->tick(tid, 1) == PGR_IV_OK);
  assert(take_sample().owned_count == 0);

  // 10b. Runtime outline unavailable (AddComponent yields a wrong class) fails locally: the
  //      bold overlay still applies and the owned channel does NOT fault.
  FakeText ot; init_text(&ot);
  FakePanel op{&cls_panel, &ot, &mon, gm};
  assert(api->update(0, 1, 2000, tid) == PGR_IV_OK);
  g_outline_bad_class = true;
  call_owned(&op, &mon, &dummy_method);
  g_outline_bad_class = false;
  s = take_sample();
  assert(s.owned_count == 1 && (s.fault & PGR_IV_FAULT_OWNED) == 0);
  assert(std::strstr(ot.text, "</b>") != nullptr);
  assert(api->tick(tid, 1) == PGR_IV_OK);

  // 11. Lease expired: a short lease elapses, and the next tick restores the overlay.
  init_text(&plate_text);
  assert(api->update(1, 1, 10, tid) == PGR_IV_OK);
  call_setup(&plate, &map, 0, &dummy_method);
  assert(std::strcmp(plate_text.text, kCatchOverlay) == 0);
  usleep(20000);  // lease (10 ms) expires
  assert(api->tick(tid, 0) == PGR_IV_OK);
  assert(std::strcmp(plate_text.text, "999") == 0);  // restored, record kept
  assert(api->tick(tid, 1) == PGR_IV_OK);

  // 12. Reentrant disable DURING a getter revokes the apply (no overlay written) and never
  //     leaves the owner stolen or the record applied.
  init_text(&plate_text);
  assert(api->update(1, 1, 2000, tid) == PGR_IV_OK);
  g_getfont_disables = true;
  call_setup(&plate, &map, 0, &dummy_method);
  assert(std::strcmp(plate_text.text, "999") == 0);  // apply revoked by the reentrant disable
  assert(api->tick(tid, 1) == PGR_IV_OK);

  // 13. A foreign thread cannot steal the owner: its update is refused and a setup call on it
  //     performs no capture.
  init_text(&plate_text);
  std::thread([&]() {
    assert(g_api->update(1, 1, 2000, this_tid()) == PGR_IV_ERR_INVALID_ARG);
    call_setup(&plate, &map, 0, &dummy_method);  // not owner: chains the original only
  }).join();
  assert(std::strcmp(plate_text.text, "999") == 0 && take_sample().catch_active == 0);

  // 13a. Reentrant disable+RE-ENABLE during a capture getter still revokes the in-flight apply:
  //      the revocation generation bumped by the disable (and again by the re-enable) no longer
  //      matches the one pinned at the hook onLeave, so no overlay is written even though the
  //      channel ends enabled. The record is retained un-applied; a later tick (now on the
  //      current generation) applies it.
  init_text(&plate_text);
  assert(api->update(1, 1, 2000, tid) == PGR_IV_OK);
  g_getfont_toggles = true;
  call_setup(&plate, &map, 0, &dummy_method);
  s = take_sample();
  assert(std::strcmp(plate_text.text, "999") == 0);  // apply revoked by the disable+re-enable
  assert(s.catch_active == 1 && (s.fault & PGR_IV_FAULT_CATCH) == 0);  // retained, un-applied
  assert(api->tick(tid, 0) == PGR_IV_OK);            // current generation now applies
  assert(std::strcmp(plate_text.text, kCatchOverlay) == 0);
  assert(api->tick(tid, 1) == PGR_IV_OK);

  // 13b. Disable DURING string_new (the set_text allocation) revokes before set_text runs. The
  //      style setters that already ran are restored to the authentic snapshot (a partial apply
  //      must not leave a half-styled label) and the channel does NOT fault -- cancellation is
  //      not an error.
  init_text(&plate_text);
  assert(api->update(1, 1, 2000, tid) == PGR_IV_OK);
  g_stringnew_disables = true;
  call_setup(&plate, &map, 0, &dummy_method);
  g_stringnew_disables = false;
  s = take_sample();
  assert(std::strcmp(plate_text.text, "999") == 0);       // label never written
  assert(plate_text.rich == 0 && plate_text.fit == 1);    // style restored on cancellation
  assert(s.catch_active == 1 && (s.fault & PGR_IV_FAULT_CATCH) == 0);
  assert(api->tick(tid, 1) == PGR_IV_OK);

  // 13c. A destroyed target is dropped (never restored) on the next tick; its roots are freed
  //      and the channel does not fault.
  init_text(&plate_text);
  assert(api->update(1, 1, 2000, tid) == PGR_IV_OK);
  call_setup(&plate, &map, 0, &dummy_method);
  assert(std::strcmp(plate_text.text, kCatchOverlay) == 0 && take_sample().catch_active == 1);
  plate_text.destroyed = 1;  // UnityEngine.Object.op_Implicit now reports the Text dead
  assert(api->tick(tid, 0) == PGR_IV_OK);
  s = take_sample();
  assert(s.catch_active == 0 && (s.fault & PGR_IV_FAULT_CATCH) == 0);  // dropped, no restore call
  plate_text.destroyed = 0;

  // 13d. A reentrant STOPPING tick that fires from inside op_Implicit during a free is a guarded
  //      no-op (active_depth > 0 in the outer tick), so the outer drop neither double-frees (the
  //      fake gchandle_free asserts on a double free) nor leaks a record.
  FakeText nt; init_text(&nt);
  FakePanel np{&cls_panel, &nt, &mon, gm};
  assert(api->update(0, 1, 2000, tid) == PGR_IV_OK);
  call_owned(&np, &mon, &dummy_method);
  assert(take_sample().owned_count == 1);
  nt.destroyed = 1;            // forces a drop on the next tick
  g_alive_nested_tick = true;  // op_Implicit re-enters tick(stopping) exactly once
  assert(api->tick(tid, 0) == PGR_IV_OK);
  assert(take_sample().owned_count == 0);  // dropped exactly once; the nested stop was a no-op
  nt.destroyed = 0;

  // 13e. Gating AddComponent: capturing while the owned channel is DISABLED must NOT add an
  //      Outline (AddComponent is a mutation). The bold label is still captured un-applied and
  //      the channel does not fault; a later enable+tick applies the label (the outline stays
  //      absent until a fresh capture pass -- see the limitations note to the coordinator).
  FakeText gt; init_text(&gt);
  FakePanel gp{&cls_panel, &gt, &mon, gm};
  assert(api->update(0, 0, 2000, tid) == PGR_IV_OK);  // owner proven; both channels OFF
  call_owned(&gp, &mon, &dummy_method);
  s = take_sample();
  assert(s.owned_count == 1 && (s.fault & PGR_IV_FAULT_OWNED) == 0);
  assert(gt.go.outline == nullptr);            // no AddComponent while disabled
  assert(std::strcmp(gt.text, "CP500") == 0);  // captured but not applied
  assert(api->update(0, 1, 2000, tid) == PGR_IV_OK);
  assert(api->tick(tid, 0) == PGR_IV_OK);
  assert(std::strstr(gt.text, "</b>") != nullptr);  // label now applied on enable
  assert(api->tick(tid, 1) == PGR_IV_OK);

  // 14. Managed exception from a getter faults the channel, writes no overlay, frees roots.
  init_text(&plate_text);
  assert(api->update(1, 1, 2000, tid) == PGR_IV_OK);
  g_throw_method = &m_get_rich;
  before = take_sample().catch_serial;
  call_setup(&plate, &map, 0, &dummy_method);
  s = take_sample();
  assert((s.fault & PGR_IV_FAULT_CATCH) != 0 && s.catch_notice == PGR_IV_NOTICE_UNAVAILABLE);
  assert(std::strcmp(plate_text.text, "999") == 0);
  g_throw_method = nullptr;

  // 15. Root allocation failure: no overlay, no leak, owned channel unaffected (independent).
  init_text(&panel_text);
  assert(api->update(0, 1, 2000, tid) == PGR_IV_OK);
  g_fail_root = true;
  call_owned(&panel, &mon, &dummy_method);
  g_fail_root = false;
  s = take_sample();
  assert((s.fault & PGR_IV_FAULT_OWNED) != 0 && std::strcmp(panel_text.text, "CP500") == 0);

  // 16. Stop restores and frees everything; all GC handles are balanced.
  assert(api->tick(tid, 1) == PGR_IV_OK);
  s = take_sample();
  assert(s.catch_active == 0 && s.owned_count == 0 && s.owned_root_count == 0);
  assert(g_live.empty());

  // The forked shared-mode child must have passed its own scenario set.
  int shared_status = 0;
  assert(::waitpid(shared_pid, &shared_status, 0) == shared_pid);
  assert(WIFEXITED(shared_status) && WEXITSTATUS(shared_status) == 0);

  std::printf("iv_display_test passed: validation, catch overlay + telemetry, restore/ownership, "
              "encounter-id pending, tappable, invalid CP/IV, gradient thresholds, half-level, "
              "bold owned + independent toggles, un-applied capture + toggle-on, max8 eviction, "
              "lease expiry, reentrant disable, disable+re-enable during getter, disable during "
              "StringNew, destroyed target drop, nested stopping tick during free, gated "
              "AddComponent, wrong-thread no steal, managed exception, root-alloc failure, "
              "stop cleanup\n");
  return 0;
}
