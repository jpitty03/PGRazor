// Production dlopen/query60 fixture for the compiled Routes orchestration host
// (pgrazor/native/routes_host.{h,cpp}).
//
// Exercises Feature 60 (API 1) end-to-end against the real compiled library:
//   * Feature 53 (routes_metadata) + Feature 47 (runtime_metadata) resolution
//   * Feature 27 (routes_gate) + C++ JSON command parser parity
//   * Feature 43 (routes_actions) + Feature 44 (routes_discovery) + boundSnapshot clamping
//   * Feature 45 (routes_services) + Feature 46 (routes_observers) + Feature 50 (routes_tracking)
//     installed with real Dobby hooks on NOP-padded fixture functions while validating exact
//     ARM64 prologues at the production RVAs in the synthetic module mapping
//   * Feature 52 (runtime_transport) direct event/snapshot/action emission (opcodes 7, 8, 9, 19)
//   * Feature 48 (unity_scheduler) on_unity_pulse subscriber callback + stop(tid) root drain
#include "routes_host.h"
#include "pgrazor_native_api.h"
#include "routes_metadata.h"
#include "runtime_transport.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <memory>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace {

using namespace pgr_routes_schema;

// --- RVA & prologue constants (must match routes.js / routes_host.cpp) -------------
constexpr uint64_t kRvaDismissGui       = 0xa375344ULL;
constexpr uint64_t kRvaConfirmComplete  = 174801868ULL;
constexpr uint64_t kRvaRequestNearby    = 175049024ULL;
constexpr uint64_t kRvaInRangeOfEnd     = 174741504ULL;
constexpr uint64_t kRvaRewardInit       = 175272412ULL;
constexpr uint64_t kRvaRatingInit       = 175286128ULL;
constexpr uint64_t kRvaCompletionResp   = 174816912ULL;
constexpr uint64_t kRvaTrackingStatus   = 174921352ULL;
constexpr uint64_t kRvaTrackingWaypoint = 174925564ULL;

constexpr unsigned char kPrefixConfirmComplete[] = {
    0xfe, 0x5f, 0xbd, 0xa9, 0xf6, 0x57, 0x01, 0xa9, 0xf4, 0x4f, 0x02, 0xa9,
    0xd4, 0x50, 0x02, 0xb0, 0xf3, 0x03, 0x00, 0xaa, 0x88, 0x3a, 0x4e, 0x39};
constexpr unsigned char kPrefixRequestNearby[] = {
    0xfe, 0x67, 0xbc, 0xa9, 0xf8, 0x5f, 0x01, 0xa9, 0xf6, 0x57, 0x02, 0xa9,
    0xf4, 0x4f, 0x03, 0xa9, 0xf3, 0x4e, 0x02, 0xb0, 0xf4, 0x03, 0x00, 0xaa};
constexpr unsigned char kPrefixInRangeOfEnd[] = {
    0xeb, 0x2b, 0xbc, 0x6d, 0xe9, 0x23, 0x01, 0x6d, 0xfe, 0x57, 0x02, 0xa9,
    0xf4, 0x4f, 0x03, 0xa9, 0x54, 0x51, 0x02, 0x90, 0xf3, 0x03, 0x00, 0xaa};
constexpr unsigned char kPrefixRewardInit[] = {
    0xff, 0x43, 0x03, 0xd1, 0xea, 0x2b, 0x00, 0xfd, 0xe9, 0x23, 0x06, 0x6d,
    0xfd, 0x7b, 0x07, 0xa9, 0xfc, 0x6f, 0x08, 0xa9, 0xfa, 0x67, 0x09, 0xa9};
constexpr unsigned char kPrefixRatingInit[] = {
    0xff, 0x83, 0x01, 0xd1, 0xfe, 0x6f, 0x01, 0xa9, 0xfa, 0x67, 0x02, 0xa9,
    0xf8, 0x5f, 0x03, 0xa9, 0xf6, 0x57, 0x04, 0xa9, 0xf4, 0x4f, 0x05, 0xa9};
constexpr unsigned char kPrefixCompletionResp[] = {
    0xfe, 0x67, 0xbc, 0xa9, 0xf8, 0x5f, 0x01, 0xa9, 0xf6, 0x57, 0x02, 0xa9,
    0xf4, 0x4f, 0x03, 0xa9, 0xb5, 0x50, 0x02, 0xd0, 0xf4, 0x03, 0x01, 0xaa};
constexpr unsigned char kPrefixTrackingStatus[] = {
    0xff, 0x83, 0x01, 0xd1, 0xfe, 0x6f, 0x01, 0xa9, 0xfa, 0x67, 0x02, 0xa9,
    0xf8, 0x5f, 0x03, 0xa9, 0xf6, 0x57, 0x04, 0xa9, 0xf4, 0x4f, 0x05, 0xa9};
constexpr unsigned char kPrefixTrackingWaypoint[] = {
    0xfe, 0x0f, 0x1d, 0xf8, 0xf6, 0x57, 0x01, 0xa9, 0xf4, 0x4f, 0x02, 0xa9,
    0xd5, 0x4f, 0x02, 0xf0, 0xf4, 0x03, 0x01, 0xaa, 0xf3, 0x03, 0x00, 0xaa};

uint64_t self_tid() { return static_cast<uint64_t>(static_cast<pid_t>(gettid())); }

// --- Synthetic metadata & managed object model ------------------------------------
struct Class;
struct Type {
  std::string name;
  Class *klass = nullptr;
  int kind = 18; // IL2CPP_TYPE_CLASS by default
  bool byref = false;
};
struct Class {
  Type *type = nullptr;
  bool value = false;
  bool enumeration = false;
  Type *base = nullptr;
  int size = 0;
  int align = 0;
};
struct Field {
  Class *lookup = nullptr;
  Class *owner = nullptr;
  std::string name;
  Type *type = nullptr;
  uint32_t flags = 0;
  uint64_t offset = 0;
};
struct Method {
  void *code = nullptr;
  Class *lookup = nullptr;
  Class *owner = nullptr;
  std::string name;
  Type *result = nullptr;
  Type *param = nullptr;
  uint32_t count = 0;
  uint32_t flags = 0;
  bool generic = false;
  bool inflated = false;
};
static_assert(offsetof(Method, code) == 0, "MethodInfo code pointer at offset 0");

std::vector<std::unique_ptr<Type>> g_types;
std::vector<std::unique_ptr<Class>> g_extra_classes;
std::vector<std::unique_ptr<Field>> g_fields;
std::vector<std::unique_ptr<Method>> g_methods;

Class g_classes[kClassCount];
Class *g_string_class = nullptr;
Class *g_int32_class = nullptr;
Class *g_list_class = nullptr;
Class *g_wp_list_class = nullptr;
Class *g_enum_class = nullptr;
Class *g_gui_init_arg_class = nullptr;
Type *g_void_type = nullptr;
Type *g_int32_type = nullptr;
Type *g_promise_type = nullptr;
Type *g_generic_ref_type = nullptr;

Method *g_extra_methods[5]{};
Method *g_reward_init_method = nullptr;
Method *g_rating_init_method = nullptr;

int g_domain_token = 1, g_image_a = 1, g_thread_token = 1;
void *g_assemblies[] = {&g_image_a};
bool g_domain_ready = true;
bool g_thread_attached = true;
unsigned char *g_module_base = nullptr;
uint64_t g_module_size = 0;

Type *new_type(const std::string &name, Class *klass, int kind = 18) {
  g_types.emplace_back(new Type{name, klass, kind, false});
  return g_types.back().get();
}
Class *new_class(const std::string &name, bool value = false, int size = 0, int align = 0, int kind = 18) {
  g_extra_classes.emplace_back(new Class);
  Class *k = g_extra_classes.back().get();
  k->type = new_type(name, k, kind);
  k->value = value;
  k->size = size;
  k->align = align;
  return k;
}
Type *find_or_create_type(const char *name) {
  assert(name);
  for (const auto &t : g_types)
    if (t->name == name) return t.get();
  int kind = 18;
  bool val = false;
  int sz = 0, al = 0;
  if (std::strcmp(name, "System.Void") == 0) {
    kind = 1; val = true; sz = 0; al = 1;
  } else if (std::strcmp(name, "System.Boolean") == 0) {
    kind = 2; val = true; sz = 1; al = 1;
  } else if (std::strcmp(name, "System.Int32") == 0) {
    kind = 8; val = true; sz = 4; al = 4;
  } else if (std::strcmp(name, "System.Int64") == 0) {
    kind = 10; val = true; sz = 8; al = 8;
  } else if (std::strcmp(name, "System.Double") == 0) {
    kind = 13; val = true; sz = 8; al = 8;
  } else if (std::strcmp(name, "Niantic.Promises.IPromise") == 0) {
    kind = 21;
  }
  return new_class(name, val, sz, al, kind)->type;
}
Method *new_method(Class *owner, const char *name, Type *result, Type *param = nullptr, bool inflated = false) {
  g_methods.emplace_back(
      new Method{nullptr, owner, owner, name, result, param, uint32_t(param ? 1 : 0), 0, false, inflated});
  return g_methods.back().get();
}

// --- Managed object simulation -----------------------------------------------------
struct alignas(8) FakeObj {
  unsigned char b[640]{};
};

void set_obj_ptr(FakeObj *o, size_t off, const void *p) {
  assert(off + sizeof(p) <= sizeof(o->b));
  std::memcpy(o->b + off, &p, sizeof(p));
}
void *get_obj_ptr(const void *o, size_t off) {
  void *p = nullptr;
  std::memcpy(&p, static_cast<const unsigned char *>(o) + off, sizeof(p));
  return p;
}
void set_obj_klass(FakeObj *o, Class *k) { set_obj_ptr(o, 0, k); }
void set_obj_u8(FakeObj *o, size_t off, uint8_t v) { o->b[off] = v; }
uint8_t get_obj_u8(const void *o, size_t off) { return static_cast<const unsigned char *>(o)[off]; }
void set_obj_i32(FakeObj *o, size_t off, int32_t v) { std::memcpy(o->b + off, &v, sizeof(v)); }
int32_t get_obj_i32(const void *o, size_t off) {
  int32_t v = 0;
  std::memcpy(&v, static_cast<const unsigned char *>(o) + off, sizeof(v));
  return v;
}
void set_obj_i64(FakeObj *o, size_t off, int64_t v) { std::memcpy(o->b + off, &v, sizeof(v)); }
void set_obj_f64(FakeObj *o, size_t off, double v) { std::memcpy(o->b + off, &v, sizeof(v)); }

FakeObj make_utf16_string(const char *ascii) {
  FakeObj s{};
  set_obj_klass(&s, g_string_class);
  int32_t len = static_cast<int32_t>(std::strlen(ascii));
  set_obj_i32(&s, 0x10, len);
  uint16_t *chars = reinterpret_cast<uint16_t *>(s.b + 0x18);
  for (int32_t i = 0; i < len; ++i) chars[i] = static_cast<uint8_t>(ascii[i]);
  return s;
}

// Global managed instances used across tests
FakeObj g_routes_svc{};
FakeObj g_consumption_svc{};
FakeObj g_tracking_svc{};
FakeObj g_nearby_info{};
FakeObj g_route_list{};
FakeObj g_shared_1{}, g_shared_2{};
FakeObj g_str_id_1{}, g_str_name_1{}, g_str_id_2{}, g_str_name_2{};
FakeObj g_wp_list_1{}, g_wp_list_2{};
FakeObj g_wp_1a{}, g_wp_1b{}, g_wp_1c{};
FakeObj g_wp_start{}, g_wp_end{};
FakeObj g_anchor_start{}, g_anchor_end{};
FakeObj g_stats_1{};
FakeObj g_play_obj{};
FakeObj g_progress_ok{};
FakeObj g_reward_gui{}, g_gui_arg{};
FakeObj g_alloc_start_data{};
FakeObj g_boxed_bool_true{}, g_boxed_bool_false{};
FakeObj g_boxed_int[8]{};
FakeObj g_promise_dummy{};

std::vector<FakeObj *> g_routes_items;
std::vector<FakeObj *> g_wp1_items;
std::vector<FakeObj *> g_wp2_items;

// Consumption state flags
bool g_flag_initialized = true;
bool g_enabled_for_player = true;
bool g_is_consuming = false;
bool g_has_started = false;
bool g_in_range_end = false;
bool g_in_range_resume = false;
bool g_is_completing = false;
bool g_is_paused = false;
FakeObj *g_current_play_ptr = nullptr;

// Action side-effect counters
int g_request_nearby_invokes = 0;
int g_start_route_invokes = 0;
int g_confirm_complete_invokes = 0;
int g_pause_route_invokes = 0;
int g_ctor_invokes = 0;
int g_dismiss_gui_invokes = 0;
void *g_last_dismissed_gui = nullptr;
bool g_trigger_bjan_on_complete = false;

// GC handle table
std::vector<void *> g_handles;
int g_live_handles = 0;

uintptr_t fake_gchandle_new(void *obj, int) {
  if (!obj) return 0;
  g_handles.push_back(obj);
  ++g_live_handles;
  return static_cast<uintptr_t>(g_handles.size());
}
void *fake_gchandle_get_target(uintptr_t h) {
  if (h == 0 || h > g_handles.size()) return nullptr;
  return g_handles[h - 1];
}
void fake_gchandle_free(uintptr_t h) {
  if (h == 0 || h > g_handles.size()) return;
  if (g_handles[h - 1]) {
    g_handles[h - 1] = nullptr;
    --g_live_handles;
  }
}

// --- NOP-padded Dobby hook target functions ----------------------------------------
extern "C" __attribute__((noinline, aligned(16))) uint8_t fixture_location(void);

std::atomic<int> g_hook_req_calls{0};
std::atomic<int> g_hook_ir_calls{0};
std::atomic<int> g_hook_reward_calls{0};
std::atomic<int> g_hook_rating_calls{0};
std::atomic<int> g_hook_bjan_calls{0};
std::atomic<int> g_hook_tracking_calls{0};
std::atomic<int> g_hook_waypoint_calls{0};
std::atomic<int> g_hook_location_calls{0};
std::atomic<int32_t> g_tracking_ret_code{5};
std::atomic<uint8_t> g_location_seen_in_tracking{0};

using LocationFn = uint8_t (*)(void);
LocationFn volatile g_call_location = &fixture_location;

extern "C" __attribute__((noinline, aligned(16))) void *fixture_req_nearby(void *self, const void *) {
  asm volatile(".rept 24; nop; .endr" ::: "memory");
  g_hook_req_calls.fetch_add(1);
  (void)self;
  return &g_promise_dummy;
}
extern "C" __attribute__((noinline, aligned(16))) bool fixture_in_range_end(void *self, const void *) {
  asm volatile(".rept 24; nop; .endr" ::: "memory");
  g_hook_ir_calls.fetch_add(1);
  (void)self;
  return true;
}
extern "C" __attribute__((noinline, aligned(16))) void fixture_reward_init(void *self, void *arg, const void *) {
  asm volatile(".rept 24; nop; .endr" ::: "memory");
  g_hook_reward_calls.fetch_add(1);
  (void)self;
  (void)arg;
}
extern "C" __attribute__((noinline, aligned(16))) void fixture_rating_init(void *self, void *arg, const void *) {
  asm volatile(".rept 24; nop; .endr" ::: "memory");
  g_hook_rating_calls.fetch_add(1);
  (void)self;
  (void)arg;
}
extern "C" __attribute__((noinline, aligned(16))) void fixture_bjan(void *self, void *resp, const void *) {
  asm volatile(".rept 24; nop; .endr" ::: "memory");
  g_hook_bjan_calls.fetch_add(1);
  (void)self;
  (void)resp;
}
extern "C" __attribute__((noinline, aligned(16))) int32_t fixture_tracking(void *self, const void *) {
  asm volatile(".rept 24; nop; .endr" ::: "memory");
  g_hook_tracking_calls.fetch_add(1);
  (void)self;
  g_location_seen_in_tracking.store(g_call_location());
  return g_tracking_ret_code.load();
}
extern "C" __attribute__((noinline, aligned(16))) void fixture_waypoint(void *self, void *wp, const void *) {
  asm volatile(".rept 24; nop; .endr" ::: "memory");
  g_hook_waypoint_calls.fetch_add(1);
  (void)self;
  (void)wp;
}
extern "C" __attribute__((noinline, aligned(16))) uint8_t fixture_location(void) {
  asm volatile(".rept 24; nop; .endr" ::: "memory");
  g_hook_location_calls.fetch_add(1);
  return 0;
}

// Volatile calls retain arguments that the unhooked fixture bodies do not use.
decltype(&fixture_req_nearby) volatile call_req_nearby = &fixture_req_nearby;
decltype(&fixture_reward_init) volatile call_reward_init = &fixture_reward_init;
decltype(&fixture_bjan) volatile call_bjan = &fixture_bjan;
decltype(&fixture_tracking) volatile call_tracking = &fixture_tracking;
decltype(&fixture_waypoint) volatile call_waypoint = &fixture_waypoint;

// --- Fake IL2CPP metadata & runtime exports ----------------------------------------
void *fake_domain_get() { return g_domain_ready ? &g_domain_token : nullptr; }
void *fake_domain_assemblies(void *, size_t *count) {
  *count = 1;
  return g_assemblies;
}
void *fake_assembly_image(void *a) { return a; }
void *fake_class_from_name(void *, const char *ns, const char *name) {
  for (size_t i = 0; i < kClassCount; ++i) {
    if (std::strcmp(ns, kClasses[i].name_space) == 0 && std::strcmp(name, kClasses[i].name) == 0)
      return &g_classes[i];
  }
  return nullptr;
}
void *fake_class_get_type(void *k) { return static_cast<Class *>(k)->type; }
char *fake_type_get_name(void *t) {
  const auto &s = static_cast<Type *>(t)->name;
  char *c = static_cast<char *>(std::malloc(s.size() + 1));
  std::memcpy(c, s.c_str(), s.size() + 1);
  return c;
}
void fake_free(void *p) { std::free(p); }
void *fake_field_from_name(void *owner, const char *name) {
  for (auto &f : g_fields)
    if (f->lookup == owner && f->name == name) return f.get();
  return nullptr;
}
void *fake_field_parent(void *f) { return static_cast<Field *>(f)->owner; }
int fake_field_flags(void *f) { return static_cast<int>(static_cast<Field *>(f)->flags); }
size_t fake_field_offset(void *f) { return static_cast<Field *>(f)->offset; }
void *fake_field_type(void *f) { return static_cast<Field *>(f)->type; }
void *fake_class_from_type(void *t) { return t ? static_cast<Type *>(t)->klass : nullptr; }
bool fake_class_is_valuetype(void *k) { return static_cast<Class *>(k)->value; }
bool fake_class_is_enum(void *k) { return static_cast<Class *>(k)->enumeration; }
void *fake_class_enum_basetype(void *k) { return static_cast<Class *>(k)->base; }
int fake_class_value_size(void *k, int *align) {
  auto *c = static_cast<Class *>(k);
  if (align) *align = c->align;
  return c->size;
}
void *fake_method_from_name(void *owner, const char *name, int count) {
  for (auto &m : g_methods)
    if (m->lookup == owner && m->name == name && static_cast<int>(m->count) == count) return m.get();
  return nullptr;
}
void *fake_method_get_class(void *m) { return static_cast<Method *>(m)->owner; }
char *fake_method_get_name(void *m) { return const_cast<char *>(static_cast<Method *>(m)->name.c_str()); }
uint32_t fake_method_get_flags(void *m, uint32_t *impl) {
  if (impl) *impl = 0;
  return static_cast<Method *>(m)->flags;
}
uint32_t fake_method_get_param_count(void *m) { return static_cast<Method *>(m)->count; }
void *fake_method_get_param(void *m, uint32_t idx) {
  assert(idx == 0);
  return static_cast<Method *>(m)->param;
}
void *fake_method_get_return_type(void *m) { return static_cast<Method *>(m)->result; }
bool fake_method_is_generic(void *m) { return static_cast<Method *>(m)->generic; }
bool fake_method_is_inflated(void *m) { return static_cast<Method *>(m)->inflated; }
bool fake_debug_get_method_info(void *m, void *out) {
  static_cast<void **>(out)[0] = static_cast<Method *>(m)->code;
  return true;
}
bool fake_assignable(void *, void *) { return false; }
void *fake_enumerate(void *, void **) { return nullptr; }
void *fake_declaring(void *) { return nullptr; }

void *fake_thread_current() { return g_thread_attached ? &g_thread_token : nullptr; }
void *fake_object_get_class(void *obj) { return obj ? get_obj_ptr(obj, 0) : nullptr; }
void fake_field_get_value(void *obj, void *field_raw, void *out) {
  auto *f = static_cast<Field *>(field_raw);
  assert(obj && f && out);
  size_t sz = 8;
  if (f->type == g_classes[14].type) sz = 1; // System.Boolean
  else if (f->type == g_enum_class->type || f->type == g_int32_type) sz = 4;
  std::memcpy(out, static_cast<const unsigned char *>(obj) + f->offset, sz);
}
void fake_field_set_value(void *obj, void *field_raw, void *val) {
  auto *f = static_cast<Field *>(field_raw);
  assert(obj && f && val);
  size_t sz = (f->type == g_classes[14].type) ? 1 : 8;
  std::memcpy(static_cast<unsigned char *>(obj) + f->offset, val, sz);
}
void fake_field_set_value_object(void *obj, void *field_raw, void *ref) {
  auto *f = static_cast<Field *>(field_raw);
  assert(obj && f);
  set_obj_ptr(static_cast<FakeObj *>(obj), f->offset, ref);
}
void *fake_object_unbox(void *obj) {
  return obj ? static_cast<unsigned char *>(obj) + 0x10 : nullptr;
}
void *fake_object_new(void *klass) {
  std::memset(&g_alloc_start_data, 0, sizeof(g_alloc_start_data));
  set_obj_klass(&g_alloc_start_data, static_cast<Class *>(klass));
  return &g_alloc_start_data;
}
int32_t fake_string_length(void *str) {
  return str ? get_obj_i32(str, 0x10) : 0;
}
const uint16_t *fake_string_chars(void *str) {
  return str ? reinterpret_cast<const uint16_t *>(static_cast<const unsigned char *>(str) + 0x18) : nullptr;
}
int fake_type_get_type(const void *t) {
  return t ? static_cast<const Type *>(t)->kind : 0;
}
uint8_t fake_type_is_byref(const void *t) {
  return (t && static_cast<const Type *>(t)->byref) ? 1u : 0u;
}
void *fake_resolve_icall(const char *name) {
  if (name && std::strcmp(name, "UnityEngine.LocationService::IsServiceEnabledByUser()") == 0)
    return reinterpret_cast<void *>(&fixture_location);
  return nullptr;
}

FakeObj *box_bool(bool v) { return v ? &g_boxed_bool_true : &g_boxed_bool_false; }
FakeObj *box_int(int32_t v) {
  assert(v >= 0 && v < 8);
  set_obj_klass(&g_boxed_int[v], g_int32_class);
  set_obj_i32(&g_boxed_int[v], 0x10, v);
  return &g_boxed_int[v];
}

void *fake_runtime_invoke(const void *method_raw, void *obj, void **args, void **exc) {
  if (exc) *exc = nullptr;
  const auto *m = static_cast<const Method *>(method_raw);
  assert(m);

  // Derived list / waypoint accessors & startData..ctor
  if (m == g_extra_methods[0]) { // listCount
    assert(obj == &g_route_list);
    return box_int(static_cast<int32_t>(g_routes_items.size()));
  }
  if (m == g_extra_methods[1]) { // listItem
    assert(obj == &g_route_list && args && args[0]);
    int32_t idx = *static_cast<const int32_t *>(args[0]);
    return (idx >= 0 && static_cast<size_t>(idx) < g_routes_items.size()) ? g_routes_items[idx] : nullptr;
  }
  if (m == g_extra_methods[2]) { // wpCount
    if (obj == &g_wp_list_1) return box_int(static_cast<int32_t>(g_wp1_items.size()));
    if (obj == &g_wp_list_2) return box_int(static_cast<int32_t>(g_wp2_items.size()));
    return box_int(0);
  }
  if (m == g_extra_methods[3]) { // wpItem
    assert(args && args[0]);
    int32_t idx = *static_cast<const int32_t *>(args[0]);
    if (obj == &g_wp_list_1 && idx >= 0 && static_cast<size_t>(idx) < g_wp1_items.size()) return g_wp1_items[idx];
    if (obj == &g_wp_list_2 && idx >= 0 && static_cast<size_t>(idx) < g_wp2_items.size()) return g_wp2_items[idx];
    return nullptr;
  }
  if (m == g_extra_methods[4]) { // .ctor
    ++g_ctor_invokes;
    return nullptr;
  }

  // Named methods
  if (m->name == "get_CurrentRoute") return g_current_play_ptr;
  if (m->name == "get_RouteHasStarted") return box_bool(g_has_started);
  if (m->name == "get_InRangeOfEnd") return box_bool(g_in_range_end);
  if (m->name == "get_InRangeToResume") return box_bool(g_in_range_resume);
  if (m->name == "get_IsCompletingRoute") return box_bool(g_is_completing);
  if (m->name == "get_FeatureFlagInitialized") return box_bool(g_flag_initialized);
  if (m->name == "get_RouteConsumptionEnabledForPlayer") return box_bool(g_enabled_for_player);
  if (m->name == "get_IsConsumingRoute") return box_bool(g_is_consuming);
  if (m->name == "get_IsRoutePaused") return box_bool(g_is_paused);
  if (m->name == "RequestNearbyRoutes") {
    ++g_request_nearby_invokes;
    return &g_promise_dummy;
  }
  if (m->name == "StartRoute") {
    ++g_start_route_invokes;
    return nullptr;
  }
  if (m->name == "bizv") {
    ++g_confirm_complete_invokes;
    if (g_trigger_bjan_on_complete) {
      call_bjan(obj, &g_progress_ok, nullptr);
      call_reward_init(&g_reward_gui, &g_gui_arg, nullptr);
    }
    return nullptr;
  }
  if (m->name == "PauseRoute") {
    ++g_pause_route_invokes;
    return &g_promise_dummy;
  }
  if (m->name == "Dismiss") {
    ++g_dismiss_gui_invokes;
    g_last_dismissed_gui = obj;
    set_obj_i32(static_cast<FakeObj *>(obj), 0x70, 0); // transition off open state (2)
    return nullptr;
  }
  return nullptr;
}

int fixture_seg_query(void *, const void *address, uint64_t length, PgrMetaSegment *out) {
  *out = {};
  uintptr_t a = reinterpret_cast<uintptr_t>(address);
  uintptr_t base = reinterpret_cast<uintptr_t>(g_module_base);
  if (a < base || a - base > g_module_size || !length || length > g_module_size - (a - base)) return 0;
  out->found = 1;
  out->readable = 1;
  out->host_exec = 1;
  out->writable = 1;
  out->seg_base = g_module_base;
  out->seg_size = g_module_size;
  return 0;
}

PgrMetaExports make_meta_exports() {
  PgrMetaExports e{};
  e.domain_get = reinterpret_cast<void *>(&fake_domain_get);
  e.domain_get_assemblies = reinterpret_cast<void *>(&fake_domain_assemblies);
  e.assembly_get_image = reinterpret_cast<void *>(&fake_assembly_image);
  e.class_from_name = reinterpret_cast<void *>(&fake_class_from_name);
  e.class_get_type = reinterpret_cast<void *>(&fake_class_get_type);
  e.type_get_name = reinterpret_cast<void *>(&fake_type_get_name);
  e.mem_free = reinterpret_cast<void *>(&fake_free);
  e.class_get_field_from_name = reinterpret_cast<void *>(&fake_field_from_name);
  e.field_get_parent = reinterpret_cast<void *>(&fake_field_parent);
  e.field_get_flags = reinterpret_cast<void *>(&fake_field_flags);
  e.field_get_offset = reinterpret_cast<void *>(&fake_field_offset);
  e.field_get_type = reinterpret_cast<void *>(&fake_field_type);
  e.class_from_type = reinterpret_cast<void *>(&fake_class_from_type);
  e.class_is_valuetype = reinterpret_cast<void *>(&fake_class_is_valuetype);
  e.class_is_enum = reinterpret_cast<void *>(&fake_class_is_enum);
  e.class_enum_basetype = reinterpret_cast<void *>(&fake_class_enum_basetype);
  e.class_value_size = reinterpret_cast<void *>(&fake_class_value_size);
  e.class_get_method_from_name = reinterpret_cast<void *>(&fake_method_from_name);
  e.method_get_class = reinterpret_cast<void *>(&fake_method_get_class);
  e.method_get_name = reinterpret_cast<void *>(&fake_method_get_name);
  e.method_get_flags = reinterpret_cast<void *>(&fake_method_get_flags);
  e.method_get_param_count = reinterpret_cast<void *>(&fake_method_get_param_count);
  e.method_get_param = reinterpret_cast<void *>(&fake_method_get_param);
  e.method_get_return_type = reinterpret_cast<void *>(&fake_method_get_return_type);
  e.method_is_generic = reinterpret_cast<void *>(&fake_method_is_generic);
  e.method_is_inflated = reinterpret_cast<void *>(&fake_method_is_inflated);
  e.debug_get_method_info = reinterpret_cast<void *>(&fake_debug_get_method_info);
  e.class_is_assignable_from = reinterpret_cast<void *>(&fake_assignable);
  e.class_get_methods = reinterpret_cast<void *>(&fake_enumerate);
  e.class_get_nested_types = reinterpret_cast<void *>(&fake_enumerate);
  e.class_get_declaring_type = reinterpret_cast<void *>(&fake_declaring);
  return e;
}

void setup_fixture_world() {
  long page = sysconf(_SC_PAGESIZE);
  assert(page > 0);
  const uint64_t max_rva = kRvaRatingInit + 128ULL;
  g_module_size = ((max_rva + static_cast<uint64_t>(page) - 1ULL) / static_cast<uint64_t>(page)) * static_cast<uint64_t>(page);
  void *mapped = mmap(nullptr, g_module_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(mapped != MAP_FAILED);
  g_module_base = static_cast<unsigned char *>(mapped);

  // Populate all disasm-pinned ARM64 prologues at their exact RVAs in g_module_base.
  std::memcpy(g_module_base + kRvaConfirmComplete, kPrefixConfirmComplete, sizeof(kPrefixConfirmComplete));
  std::memcpy(g_module_base + kRvaRequestNearby, kPrefixRequestNearby, sizeof(kPrefixRequestNearby));
  std::memcpy(g_module_base + kRvaInRangeOfEnd, kPrefixInRangeOfEnd, sizeof(kPrefixInRangeOfEnd));
  std::memcpy(g_module_base + kRvaRewardInit, kPrefixRewardInit, sizeof(kPrefixRewardInit));
  std::memcpy(g_module_base + kRvaRatingInit, kPrefixRatingInit, sizeof(kPrefixRatingInit));
  std::memcpy(g_module_base + kRvaCompletionResp, kPrefixCompletionResp, sizeof(kPrefixCompletionResp));
  std::memcpy(g_module_base + kRvaTrackingStatus, kPrefixTrackingStatus, sizeof(kPrefixTrackingStatus));
  std::memcpy(g_module_base + kRvaTrackingWaypoint, kPrefixTrackingWaypoint, sizeof(kPrefixTrackingWaypoint));

  for (size_t i = 0; i < kClassCount; ++i) {
    int kind = (i == 14) ? 2 : 18;
    g_classes[i].type = new_type(kClasses[i].canonical, &g_classes[i], kind);
  }
  g_classes[14].value = true;
  g_classes[14].size = 1;
  g_classes[14].align = 1;

  g_int32_class = new_class("System.Int32", true, 4, 4, 8);
  g_int32_type = g_int32_class->type;
  g_void_type = find_or_create_type("System.Void");
  find_or_create_type("System.Int64");
  find_or_create_type("System.Double");
  g_promise_type = find_or_create_type("Niantic.Promises.IPromise");
  g_string_class = new_class("System.String", false, 0, 0, 14);
  g_enum_class = new_class("Fixture.Status", true, 4, 4, 17);
  g_enum_class->enumeration = true;
  g_enum_class->base = g_int32_type;
  g_generic_ref_type = new_class("Fixture.Reference", false, 0, 0, 18)->type;
  g_list_class = new_class("Fixture.RouteList", false, 0, 0, 21);
  g_wp_list_class = new_class("Fixture.WaypointList", false, 0, 0, 21);
  g_gui_init_arg_class = new_class("Fixture.GuiInitArg", false, 0, 0, 18);

  for (size_t i = 0; i < kFieldCount; ++i) {
    const auto &s = kFields[i];
    Type *t = s.type_name ? find_or_create_type(s.type_name)
                          : (s.kind == PGR_MD_FIELD_ENUM_I32 ? g_enum_class->type : g_generic_ref_type);
    if (i == 4) t = g_list_class->type;
    if (i == 11) t = g_wp_list_class->type;
    g_fields.emplace_back(new Field{&g_classes[s.owner], &g_classes[s.owner], s.name, t, 0, s.offset});
  }

  for (size_t i = 0; i < kMethodCount; ++i) {
    const auto &s = kMethods[i];
    Type *ret = s.return_type ? find_or_create_type(s.return_type) : nullptr;
    if (std::strcmp(s.key, "trackingStatus") == 0) ret = g_enum_class->type;
    if (std::strcmp(s.key, "requestNearby") == 0) ret = g_promise_type;
    Type *p0 = s.param_count ? find_or_create_type(s.param0) : nullptr;
    Method *m = new_method(&g_classes[s.owner], s.name, ret, p0, false);
    if (std::strcmp(s.key, "trackingStatus") == 0) m->code = g_module_base + kRvaTrackingStatus;
    else if (std::strcmp(s.key, "trackingWaypoint") == 0) m->code = g_module_base + kRvaTrackingWaypoint;
    else if (std::strcmp(s.key, "dismissGui") == 0) m->code = g_module_base + kRvaDismissGui;
    else if (std::strcmp(s.key, "inRangeOfEnd") == 0) m->code = g_module_base + kRvaInRangeOfEnd;
    else if (std::strcmp(s.key, "confirmComplete") == 0) m->code = g_module_base + kRvaConfirmComplete;
    else if (std::strcmp(s.key, "completionResponse") == 0) m->code = g_module_base + kRvaCompletionResp;
    else if (std::strcmp(s.key, "requestNearby") == 0) m->code = g_module_base + kRvaRequestNearby;
  }

  g_extra_methods[0] = new_method(g_list_class, "get_Count", g_int32_type, nullptr, true);
  g_extra_methods[1] = new_method(g_list_class, "get_Item", g_classes[5].type, g_int32_type, true);
  g_extra_methods[2] = new_method(g_wp_list_class, "get_Count", g_int32_type, nullptr, true);
  g_extra_methods[3] = new_method(g_wp_list_class, "get_Item", g_classes[7].type, g_int32_type, true);
  g_extra_methods[4] = new_method(&g_classes[4], ".ctor", g_void_type, nullptr, false);

  // Observer Initialize(1) methods on rewardGui (11) and ratingGui (12)
  g_reward_init_method = new_method(&g_classes[11], "Initialize", g_void_type, g_gui_init_arg_class->type, false);
  g_reward_init_method->code = g_module_base + kRvaRewardInit;
  g_rating_init_method = new_method(&g_classes[12], "Initialize", g_void_type, g_gui_init_arg_class->type, false);
  g_rating_init_method->code = g_module_base + kRvaRatingInit;

  // Boxed boolean singletons
  set_obj_klass(&g_boxed_bool_true, &g_classes[14]);
  set_obj_u8(&g_boxed_bool_true, 0x10, 1);
  set_obj_klass(&g_boxed_bool_false, &g_classes[14]);
  set_obj_u8(&g_boxed_bool_false, 0x10, 0);

  // Service + route object graph
  set_obj_klass(&g_routes_svc, &g_classes[2]);
  set_obj_klass(&g_consumption_svc, &g_classes[1]);
  set_obj_klass(&g_tracking_svc, &g_classes[0]);
  set_obj_ptr(&g_routes_svc, 0x80, &g_consumption_svc); // dica -> RouteConsumptionService
  set_obj_ptr(&g_routes_svc, 0x210, &g_nearby_info);    // didn -> NearbyRoutesInfo

  set_obj_klass(&g_nearby_info, &g_classes[3]);
  set_obj_ptr(&g_nearby_info, 0x10, &g_route_list);

  set_obj_klass(&g_route_list, g_list_class);
  set_obj_klass(&g_wp_list_1, g_wp_list_class);
  set_obj_klass(&g_wp_list_2, g_wp_list_class);

  g_str_id_1 = make_utf16_string("route-alpha");
  g_str_name_1 = make_utf16_string("Alpha Park Trail");
  g_str_id_2 = make_utf16_string("route-one-pt");
  g_str_name_2 = make_utf16_string("Too Short");

  set_obj_klass(&g_wp_1a, &g_classes[7]);
  set_obj_f64(&g_wp_1a, 0x18, 37.789);
  set_obj_f64(&g_wp_1a, 0x20, -122.401);
  set_obj_klass(&g_wp_1b, &g_classes[7]);
  set_obj_f64(&g_wp_1b, 0x18, 37.790);
  set_obj_f64(&g_wp_1b, 0x20, -122.402);
  set_obj_klass(&g_wp_1c, &g_classes[7]);
  set_obj_f64(&g_wp_1c, 0x18, 37.791);
  set_obj_f64(&g_wp_1c, 0x20, -122.403);
  g_wp1_items = {&g_wp_1a, &g_wp_1b, &g_wp_1c};
  g_wp2_items = {&g_wp_1a}; // only 1 point -> must be filtered out by boundSnapshot

  set_obj_klass(&g_wp_start, &g_classes[7]);
  set_obj_f64(&g_wp_start, 0x18, 37.789);
  set_obj_f64(&g_wp_start, 0x20, -122.401);
  set_obj_klass(&g_wp_end, &g_classes[7]);
  set_obj_f64(&g_wp_end, 0x18, 37.791);
  set_obj_f64(&g_wp_end, 0x20, -122.403);

  set_obj_klass(&g_anchor_start, &g_classes[6]);
  set_obj_ptr(&g_anchor_start, 0x10, &g_wp_start);
  set_obj_klass(&g_anchor_end, &g_classes[6]);
  set_obj_ptr(&g_anchor_end, 0x10, &g_wp_end);

  set_obj_klass(&g_stats_1, &g_classes[13]);
  set_obj_i64(&g_stats_1, 0x18, 60000);

  // Route 1: valid 3-waypoint reversible route
  set_obj_klass(&g_shared_1, &g_classes[5]);
  set_obj_ptr(&g_shared_1, 0x10, &g_str_id_1);
  set_obj_ptr(&g_shared_1, 0x18, &g_wp_list_1);
  set_obj_ptr(&g_shared_1, 0x28, &g_str_name_1);
  set_obj_u8(&g_shared_1, 0x48, 1); // Reversible = true
  set_obj_i64(&g_shared_1, 0x58, 1250);
  set_obj_ptr(&g_shared_1, 0x90, &g_stats_1);
  set_obj_ptr(&g_shared_1, 0xa8, &g_anchor_start);
  set_obj_ptr(&g_shared_1, 0xb0, &g_anchor_end);

  // Route 2: invalid 1-waypoint route (rejected by boundSnapshot)
  set_obj_klass(&g_shared_2, &g_classes[5]);
  set_obj_ptr(&g_shared_2, 0x10, &g_str_id_2);
  set_obj_ptr(&g_shared_2, 0x18, &g_wp_list_2);
  set_obj_ptr(&g_shared_2, 0x28, &g_str_name_2);
  set_obj_u8(&g_shared_2, 0x48, 0);
  set_obj_i64(&g_shared_2, 0x58, 500);

  g_routes_items = {&g_shared_1, &g_shared_2};

  // Current RoutePlayProto
  set_obj_klass(&g_play_obj, &g_classes[8]);
  set_obj_ptr(&g_play_obj, 0x10, &g_shared_1);
  set_obj_u8(&g_play_obj, 0x3c, 1); // CompletedWalk
  set_obj_u8(&g_play_obj, 0x3d, 0); // Paused
  set_obj_u8(&g_play_obj, 0x3e, 0); // AcquiredReward
  set_obj_u8(&g_play_obj, 0x70, 0); // TravelInReverse
  g_current_play_ptr = &g_play_obj;

  // ProgressRouteOutProto (Status = 1 -> confirmed)
  set_obj_klass(&g_progress_ok, &g_classes[9]);
  set_obj_i32(&g_progress_ok, 0x14, 1);

  // Reward GUI (currentState = 2 -> open)
  set_obj_klass(&g_reward_gui, &g_classes[11]);
  set_obj_i32(&g_reward_gui, 0x70, 2);
}

struct Popped {
  PgrRoutesHostEvent ev{};
  std::string json;
};

std::vector<Popped> drain_all(const PgrRoutesHostApi *host) {
  std::vector<Popped> out;
  char buf[16384];
  while (true) {
    PgrRoutesHostEvent ev{};
    int rc = host->next_event(&ev, buf, sizeof(buf));
    if (rc == 0) break;
    assert(rc == PGR_RH_EVENT_POPPED);
    assert(ev.version == PGR_RH_EVENT_VERSION && ev.size == sizeof(PgrRoutesHostEvent));
    out.push_back(Popped{ev, std::string(buf, ev.json_len)});
  }
  return out;
}

const Popped *find_ev(const std::vector<Popped> &evs, uint32_t kind) {
  for (const auto &e : evs)
    if (e.ev.kind == kind) return &e;
  return nullptr;
}

} // namespace

int main(int argc, char **argv) {
  assert(argc == 2);
  void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  assert(library && "dlopen core.so failed");

  auto get_core = reinterpret_cast<const void *(*)(uint32_t)>(dlsym(library, "pgrazor_native_api"));
  assert(get_core);
  const auto *core = static_cast<const pgrazor_native_api_v2 *>(get_core(2));
  assert(core && core->query_feature);
  assert(core->query_feature(60, 0) == nullptr);
  assert(core->query_feature(60, 2) == nullptr);

  const auto *host = static_cast<const PgrRoutesHostApi *>(core->query_feature(60, 1));
  assert(host && host->version == 1 && host->size == sizeof(PgrRoutesHostApi));
  assert(host->configure && host->set_options && host->tick && host->next_event &&
         host->stop && host->sample && host->on_unity_pulse);

  const auto *tr = static_cast<const PgrRuntimeTransportApi *>(core->query_feature(52, 1));
  assert(tr && tr->version == 1 && tr->size == sizeof(PgrRuntimeTransportApi));
  // Java normally installs52 before compiled gameplay starts. No snapshot is
  // needed for testing its producer queue; drain on a separate host thread.
  PgrRuntimeTransportConfig transport_config{1,sizeof(PgrRuntimeTransportConfig),500,
      PGR_TRANSPORT_MAX_EVENTS,PGR_TRANSPORT_MAX_STRING,PGR_TRANSPORT_MAX_FRAME,0,0};
  assert(tr->install(&transport_config)==PGR_TRANSPORT_OK);


  setup_fixture_world();
  PgrMetaExports meta_exports = make_meta_exports();

  // Compute bounds around fixture_location so libunity.so containment check succeeds.
  const uintptr_t loc_addr = reinterpret_cast<uintptr_t>(&fixture_location);
  const void *unity_base = reinterpret_cast<const void *>(loc_addr & ~uintptr_t(0xFFF));
  const uint64_t unity_size = 65536ULL;

  PgrRoutesHostConfig cfg{};
  cfg.version = PGR_RH_CONFIG_VERSION;
  cfg.size = sizeof(cfg);
  cfg.flags = PGR_RH_CFG_FIXTURE_HOOKS | PGR_RH_CFG_EMIT_TRANSPORT52;
  cfg.module_base = g_module_base;
  cfg.module_size = g_module_size;
  cfg.meta_exports = &meta_exports;
  cfg.seg_query = &fixture_seg_query;
  cfg.runtime_invoke = reinterpret_cast<void *>(&fake_runtime_invoke);
  cfg.object_get_class = reinterpret_cast<void *>(&fake_object_get_class);
  cfg.field_get_value = reinterpret_cast<void *>(&fake_field_get_value);
  cfg.field_set_value = reinterpret_cast<void *>(&fake_field_set_value);
  cfg.field_set_value_object = reinterpret_cast<void *>(&fake_field_set_value_object);
  cfg.object_unbox = reinterpret_cast<void *>(&fake_object_unbox);
  cfg.object_new = reinterpret_cast<void *>(&fake_object_new);
  cfg.gchandle_new = reinterpret_cast<void *>(&fake_gchandle_new);
  cfg.gchandle_get_target = reinterpret_cast<void *>(&fake_gchandle_get_target);
  cfg.gchandle_free = reinterpret_cast<void *>(&fake_gchandle_free);
  cfg.thread_current = reinterpret_cast<void *>(&fake_thread_current);
  cfg.string_length = reinterpret_cast<void *>(&fake_string_length);
  cfg.string_chars = reinterpret_cast<void *>(&fake_string_chars);
  cfg.type_get_type = reinterpret_cast<void *>(&fake_type_get_type);
  cfg.type_is_byref = reinterpret_cast<void *>(&fake_type_is_byref);
  cfg.resolve_icall = reinterpret_cast<void *>(&fake_resolve_icall);
  cfg.fixture_request_nearby = reinterpret_cast<void *>(&fixture_req_nearby);
  cfg.fixture_in_range_of_end = reinterpret_cast<void *>(&fixture_in_range_end);
  cfg.fixture_reward_init = reinterpret_cast<void *>(&fixture_reward_init);
  cfg.fixture_rating_init = reinterpret_cast<void *>(&fixture_rating_init);
  cfg.fixture_completion_resp = reinterpret_cast<void *>(&fixture_bjan);
  cfg.fixture_tracking_status = reinterpret_cast<void *>(&fixture_tracking);
  cfg.fixture_tracking_waypoint = reinterpret_cast<void *>(&fixture_waypoint);
  cfg.fixture_location_icall = reinterpret_cast<void *>(&fixture_location);
  cfg.unity_module_base = unity_base;
  cfg.unity_module_size = unity_size;

  // --- Phase 1: pre-configure guards + configure validation + idempotency ------------
  assert(host->tick(self_tid()) == PGR_RH_ERR_NOT_CONFIGURED);
  {
    auto bad = cfg;
    bad.version = 2;
    assert(host->configure(&bad) == PGR_RH_ERR_BAD_VERSION);
    bad = cfg;
    bad.size = sizeof(cfg) - 8;
    assert(host->configure(&bad) == PGR_RH_ERR_BAD_SIZE);
    bad = cfg;
    bad.seg_query = nullptr; // FIXTURE_HOOKS without seg_query must fail closed
    assert(host->configure(&bad) == PGR_RH_ERR_MISSING_PTR);
  }
  assert(host->configure(&cfg) == PGR_RH_OK);
  assert(host->configure(&cfg) == PGR_RH_OK); // idempotent
  {
    auto diff = cfg;
    diff.module_size += 4096;
    assert(host->configure(&diff) == PGR_RH_ERR_RECONFIG);
  }

  // --- Phase 2: command JSON parser + Feature 27 gate parity -------------------------
  auto check_opt = [&](int allowed, const char *json, int32_t exp_status, const char *exp_reason,
                       uint64_t exp_seq = 0, uint32_t exp_act = 0) {
    PgrRoutesHostOptionsOut out{};
    uint32_t len = json ? static_cast<uint32_t>(std::strlen(json)) : 0;
    assert(host->set_options(allowed, json, len, &out) == PGR_RH_OK);
    assert(out.version == PGR_RH_OPTIONS_OUT_VERSION && out.size == sizeof(out));
    assert(out.status_code == exp_status);
    if (exp_status == PGR_RH_OPT_REJECTED) {
      if(std::strcmp(out.reason,exp_reason)!=0)std::fprintf(stderr,"command=%s reason=%s expected=%s\n",json?json:"<null>",out.reason,exp_reason);
      assert(std::strcmp(out.reason, exp_reason) == 0);
    } else if (exp_status == PGR_RH_OPT_ACCEPTED) {
      assert(out.seq == exp_seq && out.action_code == exp_act);
    }
  };

  check_opt(0, "{\"seq\":1,\"action\":\"discover\"}", PGR_RH_OPT_OFF, "not-allowed");
  check_opt(1, "", PGR_RH_OPT_REJECTED, "no-command");
  check_opt(1, "not json", PGR_RH_OPT_REJECTED, "bad-json");
  check_opt(1, "{\"seq\":1,\"action\":\"discover\"} trailing", PGR_RH_OPT_REJECTED, "bad-json");
  check_opt(1, "[1,2,3]", PGR_RH_OPT_REJECTED, "bad-seq"); // Arrays are objects in the legacy JSON contract.
  check_opt(1, "{\"seq\":0,\"action\":\"discover\"}", PGR_RH_OPT_REJECTED, "bad-seq");
  check_opt(1, "{\"seq\":1.5,\"action\":\"discover\"}", PGR_RH_OPT_REJECTED, "bad-seq");
  check_opt(1, "{\"seq\":1,\"action\":\"noop\"}", PGR_RH_OPT_REJECTED, "bad-action");
  check_opt(1, "{\"seq\":1,\"action\":\"start\"}", PGR_RH_OPT_REJECTED, "id-required");
  check_opt(1, "{\"seq\":1,\"action\":\"complete\"}", PGR_RH_OPT_REJECTED, "id-required");
  check_opt(1, "{\"seq\":1,\"action\":\"start\",\"id\":\"\"}", PGR_RH_OPT_REJECTED, "bad-id");
  check_opt(1, "{\"seq\":1,\"action\":\"start\",\"id\":\"r1\",\"reversed\":\"yes\"}", PGR_RH_OPT_REJECTED, "bad-reversed");

  check_opt(1, "{\"seq\":1,\"action\":\"discover\"}", PGR_RH_OPT_ACCEPTED, "", 1, 1);
  check_opt(1, "{\"seq\":1,\"action\":\"discover\"}", PGR_RH_OPT_REJECTED, "stale-seq");
  // Revoking lease dooms pending seq 1
  check_opt(0, nullptr, PGR_RH_OPT_OFF, "not-allowed");

  // --- Phase 3: cold-start metadata failure + 2000ms throttle + off-owner rejection --
  g_domain_ready = false;
  assert(host->tick(self_tid() + 999) == PGR_RH_ERR_OFF_OWNER);
  assert(host->tick(self_tid()) == PGR_RH_OK);
  {
    auto evs = drain_all(host);
    assert(evs.size() == 1);
    assert(evs[0].ev.kind == PGR_RH_EV_READY && evs[0].ev.scalar0 == 0);
    assert(evs[0].json.find("\"available\":false") != std::string::npos);
  }
  // Immediate retry within 2000ms is throttled and deduplicated
  assert(host->tick(self_tid()) == PGR_RH_OK);
  assert(drain_all(host).empty());

  // --- Phase 4: metadata 53 resolution + leaf 43/44/45/46/50 install -----------------
  // Wait for the 2000ms cold-start retry window to elapse, then enable domain.
  usleep(2050 * 1000);
  g_domain_ready = true;
  assert(host->tick(self_tid()) == PGR_RH_OK);
  {
    auto evs = drain_all(host);
    const Popped *loc_ev = find_ev(evs, PGR_RH_EV_LOCATION_GATE_READY);
    const Popped *meta_ev = find_ev(evs, PGR_RH_EV_METADATA_READY);
    const Popped *rdy_ev = find_ev(evs, PGR_RH_EV_READY);
    const Popped *act_ev = find_ev(evs, PGR_RH_EV_ACTION);
    assert(loc_ev && loc_ev->json == "{\"backend\":\"native\"}");
    assert(meta_ev && meta_ev->json.find("\"actionsBackend\":\"native\"") != std::string::npos);
    assert(rdy_ev && rdy_ev->json == "{\"available\":false,\"reason\":\"awaiting-service\"}");
    // Doomed seq 1 from Phase 2 drained as lease-expired
    assert(act_ev && act_ev->ev.seq == 1 && act_ev->json.find("\"reason\":\"lease-expired\"") != std::string::npos);

    PgrRoutesHostSample s{};
    assert(host->sample(&s) == PGR_RH_OK);
    assert(s.configured == 1 && s.meta_ready == 1 && s.tracking_ready == 1 && s.services_ready == 0);
    assert(s.owner_tid == self_tid() && s.last_seq == 1);
  }

  // --- Phase 5: real Dobby service capture (Feature 45) + dica derivation ------------
  assert(call_req_nearby(&g_routes_svc, nullptr) == &g_promise_dummy);
  assert(g_hook_req_calls.load() == 1);
  assert(host->tick(self_tid()) == PGR_RH_OK);
  {
    auto evs = drain_all(host);
    int captured_cnt = 0;
    bool saw_dica = false;
    for (const auto &e : evs) {
      if (e.ev.kind == PGR_RH_EV_SERVICE_CAPTURED) {
        ++captured_cnt;
        if (e.json.find("\"routes-dependency\"") != std::string::npos) saw_dica = true;
      }
    }
    if(captured_cnt!=2||!saw_dica)for(const auto &e:evs)std::fprintf(stderr,"capture event kind=%u json=%s\n",e.ev.kind,e.json.c_str());
    assert(captured_cnt == 2 && saw_dica);
    const Popped *rdy_ev = find_ev(evs, PGR_RH_EV_READY);
    assert(rdy_ev && rdy_ev->ev.scalar0 == 1 && rdy_ev->json == "{\"available\":true,\"reason\":null}");
  }

  // --- Phase 6: current read + discovery + boundSnapshot + small-buffer -8 check -----
  g_has_started = true;
  g_in_range_end = true;
  check_opt(1, "", PGR_RH_OPT_REJECTED, "no-command"); // grants fresh 2500ms lease
  assert(host->tick(self_tid()) == PGR_RH_OK);
  {
    // Small buffer check: must return PGR_RH_ERR_BUFFER_TOO_SMALL (-8) without popping
    char tiny[4]{};
    PgrRoutesHostEvent peek{};
    assert(host->next_event(&peek, tiny, sizeof(tiny)) == PGR_RH_ERR_BUFFER_TOO_SMALL);
    assert(peek.json_len > sizeof(tiny));

    auto evs = drain_all(host);
    const Popped *cur_ev = find_ev(evs, PGR_RH_EV_CURRENT);
    const Popped *snap_ev = find_ev(evs, PGR_RH_EV_SNAPSHOT);
    assert(cur_ev && cur_ev->json.find("\"id\":\"route-alpha\"") != std::string::npos);
    assert(cur_ev->json.find("\"started\":true") != std::string::npos);
    assert(snap_ev && snap_ev->json.find("\"id\":\"route-alpha\"") != std::string::npos);
    assert(snap_ev->json.find("\"Alpha Park Trail\"") != std::string::npos);
    // 1-waypoint route must be filtered out by boundSnapshot
    assert(snap_ev->json.find("route-one-pt") == std::string::npos);

    PgrRoutesHostSample s{};
    assert(host->sample(&s) == PGR_RH_OK);
    assert(s.services_ready == 1 && s.cached_route_count == 1);
  }

  // --- Phase 7: discover / start / pause / complete + bjan + modal dismissal ---------
  // 7a. discover (seq 2)
  check_opt(1, "{\"seq\":2,\"action\":\"discover\"}", PGR_RH_OPT_ACCEPTED, "", 2, 1);
  assert(host->tick(self_tid()) == PGR_RH_OK);
  {
    auto evs = drain_all(host);
    const Popped *act_ev = find_ev(evs, PGR_RH_EV_ACTION);
    assert(act_ev && act_ev->ev.seq == 2 && act_ev->json == "{\"seq\":2,\"action\":\"discover\",\"status\":\"submitted\"}");
    assert(g_request_nearby_invokes == 1);
  }

  // 7b. start unknown route (seq 3) -> route-not-discovered
  check_opt(1, "{\"seq\":3,\"action\":\"start\",\"id\":\"missing-route\"}", PGR_RH_OPT_ACCEPTED, "", 3, 2);
  assert(host->tick(self_tid()) == PGR_RH_OK);
  {
    auto evs = drain_all(host);
    const Popped *act_ev = find_ev(evs, PGR_RH_EV_ACTION);
    assert(act_ev && act_ev->ev.seq == 3 && act_ev->json.find("\"reason\":\"route-not-discovered\"") != std::string::npos);
  }

  // 7c. start discovered route-alpha reversed=true (seq 4) -> submitted
  g_is_consuming = false;
  check_opt(1, "{\"seq\":4,\"action\":\"start\",\"id\":\"route-alpha\",\"reversed\":true}", PGR_RH_OPT_ACCEPTED, "", 4, 2);
  assert(host->tick(self_tid()) == PGR_RH_OK);
  {
    auto evs = drain_all(host);
    const Popped *act_ev = find_ev(evs, PGR_RH_EV_ACTION);
    assert(act_ev && act_ev->ev.seq == 4 && act_ev->json == "{\"seq\":4,\"action\":\"start\",\"status\":\"submitted\"}");
    assert(g_ctor_invokes == 1 && g_start_route_invokes == 1);
    assert(get_obj_ptr(&g_alloc_start_data, 0x10) == &g_shared_1);
    assert(get_obj_u8(&g_alloc_start_data, 0x18) == 1);
  }

  // 7d. pause active route (seq 5) -> submitted
  g_is_consuming = true;
  g_is_paused = false;
  check_opt(1, "{\"seq\":5,\"action\":\"pause\"}", PGR_RH_OPT_ACCEPTED, "", 5, 4);
  assert(host->tick(self_tid()) == PGR_RH_OK);
  {
    auto evs = drain_all(host);
    const Popped *act_ev = find_ev(evs, PGR_RH_EV_ACTION);
    assert(act_ev && act_ev->ev.seq == 5 && act_ev->json == "{\"seq\":5,\"action\":\"pause\",\"status\":\"submitted\"}");
    assert(g_pause_route_invokes == 1);
  }

  // 7e. complete route-alpha (seq 6) with synchronous bjan + reward GUI Initialize + 100ms settle dismiss
  g_is_consuming = true;
  g_has_started = true;
  g_in_range_end = true;
  g_is_completing = false;
  g_trigger_bjan_on_complete = true;
  set_obj_i32(&g_reward_gui, 0x70, 2);
  check_opt(1, "{\"seq\":6,\"action\":\"complete\",\"id\":\"route-alpha\"}", PGR_RH_OPT_ACCEPTED, "", 6, 3);
  assert(host->tick(self_tid()) == PGR_RH_OK);
  {
    auto evs = drain_all(host);
    const Popped *act_ev = find_ev(evs, PGR_RH_EV_ACTION);
    assert(act_ev && act_ev->ev.seq == 6 && act_ev->json == "{\"seq\":6,\"action\":\"complete\",\"status\":\"submitted\"}");
    assert(g_confirm_complete_invokes == 1 && g_hook_bjan_calls.load() == 1 && g_hook_reward_calls.load() == 1);
  }
  // Wait 120ms for modal settle guard (>=100ms), then tick to drain reward-confirmed + result-dismissed
  usleep(120 * 1000);
  check_opt(1, "", PGR_RH_OPT_REJECTED, "no-command");
  assert(host->tick(self_tid()) == PGR_RH_OK);
  {
    auto evs = drain_all(host);
    const Popped *rew_ev = find_ev(evs, PGR_RH_EV_REWARD_CONFIRMED);
    const Popped *dis_ev = find_ev(evs, PGR_RH_EV_RESULT_DISMISSED);
    assert(rew_ev && rew_ev->ev.seq == 6 && rew_ev->json == "{\"id\":\"route-alpha\",\"seq\":6}");
    assert(dis_ev && dis_ev->json == "{\"screen\":\"Niantic.Holoholo.Routes.RouteCompletionGuiController\"}");
    assert(g_dismiss_gui_invokes == 1 && g_last_dismissed_gui == &g_reward_gui);
  }

  // --- Phase 8: real Dobby tracking hooks (Feature 50) -------------------------------
  assert(fixture_location() == 0); // outside CheckTrackingErrorStatus -> base value 0
  assert(call_tracking(&g_tracking_svc, nullptr) == 5);
  assert(g_location_seen_in_tracking.load() == 1); // nested call inside CheckTrackingErrorStatus -> overridden to 1!
  call_waypoint(&g_tracking_svc, &g_wp_1a, nullptr);
  assert(host->tick(self_tid()) == PGR_RH_OK);
  {
    auto evs = drain_all(host);
    const Popped *trk_ev = find_ev(evs, PGR_RH_EV_TRACKING_STATUS);
    const Popped *wp_ev = find_ev(evs, PGR_RH_EV_WAYPOINT);
    assert(trk_ev && trk_ev->ev.scalar0 == 5 && trk_ev->json == "{\"code\":5}");
    assert(wp_ev && wp_ev->json.find("37.789") != std::string::npos);
  }

  // --- Phase 9: Transport 52 emission verification + Feature 48 pulse + stop ---------
  {
    bool saw_op7 = false, saw_op8 = false, saw_op9 = false, saw_op19 = false;
    std::thread host_drain([&]{
      std::vector<uint8_t> frame(PGR_TRANSPORT_MAX_FRAME);
      auto u32=[&](uint32_t offset)->uint32_t{
        assert(offset<=frame.size()-4);return uint32_t(frame[offset])|
          (uint32_t(frame[offset+1])<<8)|(uint32_t(frame[offset+2])<<16)|(uint32_t(frame[offset+3])<<24);
      };
      for(;;){
        uint32_t length=0;
        assert(tr->exchange(nullptr,0,frame.data(),uint32_t(frame.size()),&length)==PGR_TRANSPORT_OK);
        assert(length>=PGR_TRANSPORT_EVENT_HEADER&&length<=frame.size());
        assert(u32(0)==PGR_TRANSPORT_EVENTS_MAGIC&&u32(4)==PGR_TRANSPORT_FRAME_VERSION&&u32(8)==length);
        const uint32_t count=u32(12);assert(count<=PGR_TRANSPORT_DRAIN_CAP);
        uint32_t offset=PGR_TRANSPORT_EVENT_HEADER;
        for(uint32_t i=0;i<count;++i){
          assert(offset<=length&&length-offset>=12);
          const uint32_t opcode=u32(offset),scalars=u32(offset+4),strings=u32(offset+8);offset+=12;
          assert(opcode<PGR_TRANSPORT_ACTIONS&&scalars<=PGR_TRANSPORT_MAX_SCALARS&&strings<=PGR_TRANSPORT_MAX_STR_ARGS);
          assert(offset<=length&&uint64_t(scalars)*8<=length-offset);
          std::vector<int64_t> scalar_values;
          for(uint32_t n=0;n<scalars;++n){
            uint64_t bits=uint64_t(u32(offset))|(uint64_t(u32(offset+4))<<32);
            int64_t value;std::memcpy(&value,&bits,sizeof(value));scalar_values.push_back(value);offset+=8;
          }
          std::vector<std::string> text;
          for(uint32_t n=0;n<strings;++n){
            assert(offset<=length&&length-offset>=4);uint32_t bytes=u32(offset);offset+=4;
            assert(bytes<=length-offset);text.emplace_back(reinterpret_cast<const char *>(frame.data()+offset),bytes);offset+=bytes;
          }
          if(opcode==7){assert(scalars==1&&strings==1&&(scalar_values[0]==0||scalar_values[0]==1));saw_op7=true;}
          if(opcode==8){assert(scalars==0&&strings==1&&text[0].find("routes-snapshot")!=std::string::npos);saw_op8=true;}
          if(opcode==9){assert(scalars==0&&strings==1&&text[0].find("routes-action")!=std::string::npos);saw_op9=true;}
          if(opcode==19){assert(scalars==0&&strings==2&&text[0]=="quick-close"&&text[1].find("routes-")!=std::string::npos);saw_op19=true;}
        }
        assert(offset==length);if(!count)break;
      }
    });
    host_drain.join();
    assert(saw_op7 && saw_op8 && saw_op9 && saw_op19);
  }
  {
    PgrRoutesHostSample before{}, after{};
    assert(host->sample(&before) == PGR_RH_OK);
    PgrUnityPulse pulse{};
    pulse.generation = 1;
    pulse.sequence = 1;
    pulse.tid = static_cast<int32_t>(self_tid());
    host->on_unity_pulse(nullptr, &pulse);
    assert(host->sample(&after) == PGR_RH_OK);
    assert(after.ticks_run == before.ticks_run + 1);
  }
  assert(host->stop(self_tid()) == PGR_RH_OK);
  assert(g_live_handles == 0 && "all GC handles must be freed on owner stop()");

  std::puts("PASS Routes host actual-library: ABI64, configure/reconfig, command parser + gate27 parity, cold-start throttle, metadata53 + leaves 43/44/45/46/50 real Dobby install, service capture + dica, current + boundSnapshot, discover/start/pause/complete + bjan + modal dismiss, tracking + nested location override + waypoint, transport52 opcodes 7/8/9/19, pulse48, stop drain");
  return 0;
}
