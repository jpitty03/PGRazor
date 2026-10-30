// End-to-end native fixture for Feature 64 (compiled camera production host:
// pgrazor/native/camera_host.{h,cpp}).
//
// Requires query64 in the actual core for ABI boundaries. Flow compiles unchanged
// production64 source with only59's resolver substituted by native fixture
// metadata, and uses actual41/47/48/52 + realDobby. Separate actual59 tests own
// metadata/prologue proof; this is not a live-game metadata or cutover claim.

#include "camera_host.h"
#include "camera_sampler.h"
#include "runtime_transport.h"
#include "pgrazor_native_api.h"
#include <dlfcn.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <thread>
#include <atomic>
#include "runtime_bootstrap.h"
#include "hook_engine.h"
#include "dobby.h"
static const pgrazor_native_api_v2 *fixture_core=nullptr;
int fixture_resolve(const PgrCameraMetaConfig*,PgrCameraMetaOutput*);
static const PgrCameraMetaApi fixture_metadata{1,sizeof(PgrCameraMetaApi),sizeof(PgrCameraMetaConfig),sizeof(PgrCameraMetaOutput),fixture_resolve};
const void *fixture_core_api(uint32_t v){return v==2?fixture_core:nullptr;}
const void *fixture_metadata_api(uint32_t){return &fixture_metadata;}
const void *fixture_sampler_api(uint32_t v){return fixture_core->query_feature(41,v);}
const void *fixture_transport_api(uint32_t v){return fixture_core->query_feature(52,v);}
const void *fixture_boot_api(uint32_t v){return fixture_core->query_feature(54,v);}
const void *fixture_scheduler_api(uint32_t v){return fixture_core->query_feature(48,v);}
int fixture_hook(void *target,void *replacement,void **original){return DobbyHook(target,replacement,original);}
int fixture_unhook(void *target){return DobbyDestroy(target);}
#define pgrazor_native_api fixture_core_api
#define pgrazor_camera_metadata_api fixture_metadata_api
#define pgrazor_camera_sampler_api fixture_sampler_api
#define pgrazor_runtime_transport_api fixture_transport_api
#define pgrazor_runtime_bootstrap_api fixture_boot_api
#define pgrazor_unity_scheduler_api fixture_scheduler_api
#define pgrazor_hook fixture_hook
#define pgrazor_unhook fixture_unhook
#define pgrazor_camera_host_api fixture_host_api
#include "camera_host.cpp"
#include <sys/mman.h>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace pgr_camera_schema;

struct Class;
struct Type {
  std::string name;
  Class *klass = nullptr;
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
  Type *params[2] = {};
  uint32_t count = 0;
  uint32_t flags = 0;
  bool generic = false;
  bool inflated = false;
};
static_assert(offsetof(Method, code) == 0, "MethodInfo code pointer first");

struct Image {
  const char *name;
};

struct FakeObj {
  void *klass;
};
struct FakeVector {
  void *klass;
  float x;
  float y;
  float z;
};
struct FakeBox {
  void *klass;
  int32_t value;
};

constexpr uint64_t kDebugMethodInfoRva = 0x1000ULL;
constexpr uint64_t kGcNewRva = 0x2000ULL;
constexpr uint64_t kGcNewBodyRva = 0x2100ULL;
constexpr uint64_t kGcGetRva = 0x2600ULL;
constexpr uint64_t kGcGetBodyRva = 0x2700ULL;
constexpr uint64_t kGcFreeRva = 0x2800ULL;
constexpr uint64_t kGcFreeBodyRva = 0x2900ULL;

constexpr uint32_t kAllOptionalExports =
    PGR_CAM_EXP_CLASS_GET_FIELD_FROM_NAME | PGR_CAM_EXP_FIELD_GET_TYPE |
    PGR_CAM_EXP_FIELD_GET_PARENT | PGR_CAM_EXP_FIELD_GET_FLAGS |
    PGR_CAM_EXP_RUNTIME_INVOKE | PGR_CAM_EXP_OBJECT_GET_CLASS |
    PGR_CAM_EXP_OBJECT_UNBOX | PGR_CAM_EXP_GCHANDLE_NEW |
    PGR_CAM_EXP_GCHANDLE_GET_TARGET | PGR_CAM_EXP_GCHANDLE_FREE |
    PGR_CAM_EXP_FIELD_GET_VALUE | PGR_CAM_EXP_GET_CORLIB |
    PGR_CAM_EXP_CLASS_IS_ASSIGNABLE_FROM | PGR_CAM_EXP_CLASS_IS_VALUETYPE |
    PGR_CAM_EXP_CLASS_VALUE_SIZE | PGR_CAM_EXP_DEBUG_GET_METHOD_INFO |
    PGR_CAM_EXP_METHOD_IS_GENERIC | PGR_CAM_EXP_METHOD_IS_INFLATED |
    PGR_CAM_EXP_METHOD_GET_PARAM | PGR_CAM_EXP_STRING_NEW;

std::vector<std::unique_ptr<Type>> g_types;
std::vector<std::unique_ptr<Class>> g_extra_classes;
std::vector<std::unique_ptr<Field>> g_fields;
std::vector<std::unique_ptr<Method>> g_methods;
Class g_classes[kClassCount];
Class g_other_class;
Field *g_vec_fields[kFieldCount] = {};
Method *g_slot_methods[kMethodCount] = {};

Image g_img_core{"UnityEngine.CoreModule.dll"};
Image g_img_holo{"holo-game.dll"};
Image g_img_corlib{"mscorlib.dll"};
void *g_default_assemblies[2] = {&g_img_core, &g_img_holo};
void **g_active_assemblies = g_default_assemblies;
size_t g_assembly_count = 2;

int g_domain_token = 1;
int g_thread_token = 1;
int g_owned_thread_token = 2;
bool g_domain_ready = true;
bool g_thread_attached = true;
bool g_attach_fails = false;
int g_attach_calls = 0;
int g_detach_calls = 0;
bool g_debug_info_succeeds = true;

unsigned char *g_module_base = nullptr;
uint64_t g_module_size = 0;

FakeObj g_rig = {};
FakeObj g_other_rig = {};
FakeObj g_camera = {};
FakeObj g_camera2 = {};
FakeObj g_transform = {};
FakeObj g_string = {};
FakeVector g_vector = {};
FakeBox g_layer_box = {};
FakeBox g_mask_box = {};
FakeBox g_id_box = {};

void *g_ret_main = nullptr;
void *g_ret_transform = nullptr;
void *g_ret_forward = nullptr;
void *g_ret_string = nullptr;
void *g_getter_return_camera = nullptr;
const void *g_throw_method = nullptr;
bool g_throw_field = false;
bool g_cleanup_throws = false;
bool g_unbox_null = false;
int g_invokes = 0;
int g_nametolayer_calls = 0;
std::function<void()> g_on_invoke;

std::map<uintptr_t, void *> g_handles;
uintptr_t g_next_handle = 0x1000;
int g_gcnew_calls = 0;
int g_gcfree_calls = 0;
int g_gcnew_fail_at = 0;
bool g_gcnew_throws = false;
bool g_gcnew_long_error = false;

double g_fake_now_ms = 100000.0;
bool g_clock_throws = false;
bool g_getter_from_clock = false;
uint64_t g_fake_tid = 0;

using MainCameraGetterFn=void *(*)(void*,const void*);
const PgrCameraHostApi *g_host=nullptr;
const PgrUnitySchedulerApi3 *g_scheduler=nullptr;
uint64_t g_generation=0;
unsigned g_getter_calls=0;bool g_getter_throws=false,g_execute_getter=true;
const void *g_expected_info=reinterpret_cast<void*>(0x776655);
void *g_last_return=nullptr;
std::map<uintptr_t,int> g_pinned;
__attribute__((noinline,optnone)) void *fixture_get_main_camera(void *rig,const void *info=nullptr){
 asm volatile(".rept 32; nop; .endr" ::: "memory");
 assert(rig==&g_rig&&info==g_expected_info);++g_getter_calls;
 if(g_getter_throws)throw std::runtime_error("original-getter");
 return g_getter_return_camera;
}
void call_getter(){MainCameraGetterFn volatile fn=&fixture_get_main_camera;g_last_return=fn(&g_rig,g_expected_info);}
double test_clock_fn(void *) {
  if(g_getter_from_clock){g_getter_from_clock=false;call_getter();}
  if (g_clock_throws) {
    throw std::runtime_error("Synthetic clock read failure");
  }
  return g_fake_now_ms;
}

uint64_t test_tid_fn(void *) { return g_fake_tid; }

Type *new_type(const std::string &name, Class *klass) {
  g_types.emplace_back(new Type{name, klass});
  return g_types.back().get();
}

Class *new_class(const std::string &name, bool value = false, int size = 0,
                 int align = 0) {
  g_extra_classes.emplace_back(new Class);
  Class *k = g_extra_classes.back().get();
  k->type = new_type(name, k);
  k->value = value;
  k->size = size;
  k->align = align;
  return k;
}

Type *find_type(const char *name) {
  for (const auto &t : g_types) {
    if (t->name == name) return t.get();
  }
  return new_class(name)->type;
}

void *fake_domain_get() { return g_domain_ready ? &g_domain_token : nullptr; }

void *fake_domain_get_assemblies(void *dom, size_t *count) {
  assert(dom == &g_domain_token);
  *count = g_assembly_count;
  return g_active_assemblies;
}

void *fake_assembly_get_image(void *a) { return a; }

const char *fake_image_get_name(void *img) {
  return img ? static_cast<Image *>(img)->name : nullptr;
}

void *fake_get_corlib() { return &g_img_corlib; }

void *fake_class_from_name(void *img, const char *ns, const char *name) {
  std::string full =
      (ns && *ns) ? (std::string(ns) + "." + name) : std::string(name);
  if (img == &g_img_core) {
    if (full == "UnityEngine.Camera") return &g_classes[C_CAMERA];
    if (full == "UnityEngine.Component") return &g_classes[C_COMPONENT];
    if (full == "UnityEngine.Transform") return &g_classes[C_TRANSFORM];
    if (full == "UnityEngine.UnitySynchronizationContext")
      return &g_classes[C_SYNCCTX];
    if (full == "UnityEngine.Object") return &g_classes[C_OBJECT];
    if (full == "UnityEngine.Vector3") return &g_classes[C_VECTOR3];
    if (full == "UnityEngine.LayerMask") return &g_classes[C_LAYERMASK];
  } else if (img == &g_img_holo) {
    if (full == "Niantic.Holoholo.CameraRig.MainCameraRig")
      return &g_classes[C_RIG];
    if (full == "Niantic.Holoholo.Cameras.CameraStack")
      return &g_classes[C_STACK];
  } else if (img == &g_img_corlib) {
    if (full == "System.Int32") return &g_classes[C_INT32];
    if (full == "System.String") return &g_classes[C_STRING];
  }
  return nullptr;
}

void *fake_class_get_type(void *k) { return static_cast<Class *>(k)->type; }

char *fake_type_get_name(void *t) {
  const std::string &s = static_cast<Type *>(t)->name;
  char *copy = static_cast<char *>(std::malloc(s.size() + 1));
  std::memcpy(copy, s.c_str(), s.size() + 1);
  return copy;
}

void fake_mem_free(void *p) { std::free(p); }

void *fake_class_get_field_from_name(void *k, const char *name) {
  for (const auto &f : g_fields) {
    if (f->lookup == k && f->name == name) return f.get();
  }
  return nullptr;
}

void *fake_field_get_parent(void *f) { return static_cast<Field *>(f)->owner; }
int fake_field_get_flags(void *f) {
  return static_cast<int>(static_cast<Field *>(f)->flags);
}
size_t fake_field_get_offset(void *f) {
  return static_cast<size_t>(static_cast<Field *>(f)->offset);
}
void *fake_field_get_type(void *f) { return static_cast<Field *>(f)->type; }
void *fake_class_from_type(void *t) { return static_cast<Type *>(t)->klass; }
bool fake_class_is_valuetype(void *k) { return static_cast<Class *>(k)->value; }
bool fake_class_is_enum(void *k) {
  return static_cast<Class *>(k)->enumeration;
}
void *fake_class_enum_basetype(void *k) {
  return static_cast<Class *>(k)->base;
}
int fake_class_value_size(void *k, int *align) {
  if (align) *align = static_cast<Class *>(k)->align;
  return static_cast<Class *>(k)->size;
}

void *fake_class_get_method_from_name(void *k, const char *name, int argc) {
  for (const auto &m : g_methods) {
    if (m->lookup == k && m->name == name &&
        static_cast<int>(m->count) == argc) {
      return m.get();
    }
  }
  return nullptr;
}

void *fake_method_get_class(void *m) { return static_cast<Method *>(m)->owner; }
const char *fake_method_get_name(void *m) {
  return static_cast<Method *>(m)->name.c_str();
}
uint32_t fake_method_get_flags(void *m, uint32_t *iflags) {
  if (iflags) *iflags = 0;
  return static_cast<Method *>(m)->flags;
}
uint32_t fake_method_get_param_count(void *m) {
  return static_cast<Method *>(m)->count;
}
void *fake_method_get_param(void *m, uint32_t index) {
  Method *fn = static_cast<Method *>(m);
  return index < fn->count ? fn->params[index] : nullptr;
}
void *fake_method_get_return_type(void *m) {
  return static_cast<Method *>(m)->result;
}
bool fake_method_is_generic(void *m) {
  return static_cast<Method *>(m)->generic;
}
bool fake_method_is_inflated(void *m) {
  return static_cast<Method *>(m)->inflated;
}
bool fake_debug_get_method_info(void *m, void *info) {
  if (!g_debug_info_succeeds) return false;
  *static_cast<void **>(info) = static_cast<Method *>(m)->code;
  return true;
}
bool fake_class_is_assignable_from(void *klass, void *other) {
  if (klass == other) return true;
  if (klass == &g_classes[C_STACK] && other == &g_classes[C_RIG]) return true;
  if (klass == &g_classes[C_COMPONENT] && other == &g_classes[C_CAMERA])
    return true;
  return false;
}
void *fake_class_get_methods(void *, void **) { return nullptr; }
void *fake_class_get_nested_types(void *, void **) { return nullptr; }
void *fake_class_get_declaring_type(void *) { return nullptr; }

void *fake_thread_current() {
  return g_thread_attached ? &g_thread_token : nullptr;
}
void *fake_thread_attach(const void *dom) {
  assert(dom == &g_domain_token);
  g_attach_calls++;
  if (g_attach_fails) return nullptr;
  g_thread_attached = true;
  return &g_owned_thread_token;
}
void fake_thread_detach(void *t) {
  assert(t == &g_owned_thread_token);
  g_detach_calls++;
  g_thread_attached = false;
}

uintptr_t fake_gchandle_new(void *obj, int pinned) {
  assert(pinned == 0||pinned == 1);
  if (g_gcnew_throws) {
    if (g_gcnew_long_error) {
      throw std::runtime_error(std::string(300, 'x'));
    }
    throw std::runtime_error("synthetic native handle failure");
  }
  g_gcnew_calls++;
  if (!obj) return 0;
  if (g_gcnew_fail_at > 0 && g_gcnew_calls == g_gcnew_fail_at) return 0;
  uintptr_t h = ++g_next_handle;
  g_handles[h] = obj;g_pinned[h]=pinned;
  return h;
}

void *fake_gchandle_get_target(uintptr_t h) {
  auto it = g_handles.find(h);
  return it == g_handles.end() ? nullptr : it->second;
}

void fake_gchandle_free(uintptr_t h) {
  assert(g_handles.count(h) == 1);
  g_handles.erase(h);g_pinned.erase(h);
  g_gcfree_calls++;
  if (g_cleanup_throws) {
    throw std::runtime_error("Synthetic cleanup error");
  }
}

void *fake_object_get_class(void *obj) {
  return obj ? *static_cast<void **>(obj) : nullptr;
}

void *fake_object_unbox(void *obj) {
  if (!obj || g_unbox_null) return nullptr;
  return static_cast<char *>(obj) + offsetof(FakeBox, value);
}

void *fake_string_new(const char *s) {
  assert(s && std::strcmp(s, "WorldMap") == 0);
  return g_ret_string;
}

void fake_field_get_value(void *obj, void *field, void *out) {
  assert(obj == &g_vector);
  if (g_throw_field) {
    throw std::runtime_error("Native field read failed");
  }
  size_t off = (field == g_vec_fields[F_X])   ? offsetof(FakeVector, x)
               : (field == g_vec_fields[F_Y]) ? offsetof(FakeVector, y)
                                              : offsetof(FakeVector, z);
  std::memcpy(out, static_cast<char *>(obj) + off, sizeof(float));
}

__attribute__((noinline,optnone)) void *fake_runtime_invoke(const void *method, void *obj, void **args,
                          void **exc) {
  asm volatile(".rept 32; nop; .endr" ::: "memory");
  if(method==g_slot_methods[M_EXECUTE_TASKS]){if(exc)*exc=nullptr;if(g_execute_getter)call_getter();return nullptr;}
  g_invokes++;
  if (exc) *exc = nullptr;
  if (method == g_throw_method) {
    if (exc) *exc = reinterpret_cast<void *>(0x1);
    return nullptr;
  }
  if (g_on_invoke) {
    g_on_invoke();
  }
  if (method == g_slot_methods[M_GET_MAIN]) {
    assert(obj == nullptr);
    return g_ret_main;
  }
  if (method == g_slot_methods[M_NAME_TO_LAYER]) {
    g_nametolayer_calls++;
    assert(obj == nullptr && args && args[0] == &g_string);
    return &g_layer_box;
  }
  if (method == g_slot_methods[M_GET_TRANSFORM]) {
    assert(obj == &g_camera||obj==&g_camera2);
    return g_ret_transform;
  }
  if (method == g_slot_methods[M_GET_CULLING_MASK]) {
    assert(obj == &g_camera||obj==&g_camera2);
    return &g_mask_box;
  }
  if (method == g_slot_methods[M_GET_INSTANCE_ID]) {
    assert(obj == &g_camera||obj==&g_camera2);
    return &g_id_box;
  }
  if (method == g_slot_methods[M_GET_FORWARD]) {
    assert(obj == &g_transform);
    return g_ret_forward;
  }
  assert(false && "unexpected method in fake_runtime_invoke");
  return nullptr;
}

__attribute__((noinline,optnone)) void *fake_convert(const void *m,void *o,void **p,int32_t,void **e){asm volatile(".rept 32; nop; .endr" ::: "memory");return fake_runtime_invoke(m,o,p,e);}
int verify_scheduler(const PgrUnitySchedulerConfig *c){return c&&c->version==2&&c->execute_tasks==g_slot_methods[M_EXECUTE_TASKS]&&c->runtime_invoke==reinterpret_cast<void*>(fake_runtime_invoke)&&c->runtime_invoke_convert_args==reinterpret_cast<void*>(fake_convert)&&c->exclusive_owner==1;}
void run_execute(){using Fn=decltype(&fake_runtime_invoke);Fn volatile invoke=&fake_runtime_invoke;void *exception=nullptr;assert(invoke(g_slot_methods[M_EXECUTE_TASKS],nullptr,nullptr,&exception)==nullptr&&!exception);}

int fake_seg_query(void *, const void *addr, uint64_t size,
                   PgrMetaSegment *out) {
  *out = {};
  uintptr_t a = reinterpret_cast<uintptr_t>(addr);
  uintptr_t base = reinterpret_cast<uintptr_t>(g_module_base);
  if (a < base || a - base > g_module_size ||
      size > g_module_size - (a - base)) {
    return 0;
  }
  out->found = 1;
  out->readable = 1;
  out->host_exec = 1;
  out->seg_base = g_module_base;
  out->seg_size = g_module_size;
  return 0;
}

void write_u32(uint64_t rva, uint32_t word) {
  std::memcpy(g_module_base + rva, &word, sizeof(word));
}

uint32_t encode_b_imm26(uint64_t from_rva, uint64_t to_rva) {
  int64_t diff_words =
      (static_cast<int64_t>(to_rva) - static_cast<int64_t>(from_rva)) / 4;
  uint32_t imm26 = static_cast<uint32_t>(diff_words) & 0x03ffffffu;
  return (5u << 26) | imm26;
}

void restore_module_fingerprints() {
  std::memcpy(g_module_base + kDebugMethodInfoRva, kDebugPrologue,
              kDebugPrologueLen);
  for (uint32_t i = 0; i < kGcCreateWordCount; ++i) {
    write_u32(kGcNewRva + i * 4u, kGcCreateWords[i]);
  }
  write_u32(kGcNewRva + kGcCreateBranchOff,
            encode_b_imm26(kGcNewRva + kGcCreateBranchOff, kGcNewBodyRva));
  write_u32(kGcNewBodyRva + kGcCreateBodyOff, kGcCreateBodyWord);

  write_u32(kGcGetRva, encode_b_imm26(kGcGetRva, kGcGetBodyRva));
  write_u32(kGcGetBodyRva, kGcGetHead);
  write_u32(kGcGetBodyRva + kGcGetTailOff, kGcGetTail[0]);
  write_u32(kGcGetBodyRva + kGcGetTailOff + 4u, kGcGetTail[1]);

  write_u32(kGcFreeRva, encode_b_imm26(kGcFreeRva, kGcFreeBodyRva));
  write_u32(kGcFreeBodyRva, kGcFreeHead);
  write_u32(kGcFreeBodyRva + kGcFreeTailOff, kGcFreeTail[0]);
  write_u32(kGcFreeBodyRva + kGcFreeTailOff + 4u, kGcFreeTail[1]);

  write_u32(PGR_CAM_MAIN_CAMERA_RVA, kGetMainCameraWords[0]);
  write_u32(PGR_CAM_MAIN_CAMERA_RVA + 4u, kGetMainCameraWords[1]);
}

PgrMetaExports g_meta_exports = {};
PgrCameraMetaExtra g_meta_extra = {};

void build_universe() {
  g_types.clear();
  g_extra_classes.clear();
  g_fields.clear();
  g_methods.clear();
  std::memset(g_classes, 0, sizeof(g_classes));

  struct CDef {
    ClassIndex idx;
    const char *name;
    bool value;
    int size;
  };
  const CDef cdefs[] = {
      {C_CAMERA, "UnityEngine.Camera", false, 0},
      {C_COMPONENT, "UnityEngine.Component", false, 0},
      {C_TRANSFORM, "UnityEngine.Transform", false, 0},
      {C_SYNCCTX, "UnityEngine.UnitySynchronizationContext", false, 0},
      {C_OBJECT, "UnityEngine.Object", false, 0},
      {C_VECTOR3, "UnityEngine.Vector3", true, 12},
      {C_INT32, "System.Int32", true, 4},
      {C_RIG, "Niantic.Holoholo.CameraRig.MainCameraRig", false, 0},
      {C_STACK, "Niantic.Holoholo.Cameras.CameraStack", false, 0},
      {C_STRING, "System.String", false, 0},
      {C_LAYERMASK, "UnityEngine.LayerMask", true, 4},
  };
  for (const auto &cd : cdefs) {
    Class &c = g_classes[cd.idx];
    c.type = new_type(cd.name, &c);
    c.value = cd.value;
    c.size = cd.size;
    c.align = cd.value ? 4 : 8;
  }
  g_other_class.type = new_type("OtherRig", &g_other_class);

  Type *single_t = new_class("System.Single", true, 4, 4)->type;
  for (uint32_t i = 0; i < kFieldCount; ++i) {
    g_fields.emplace_back(new Field{&g_classes[C_VECTOR3],
                                    &g_classes[C_VECTOR3], kVectorFieldNames[i],
                                    single_t, 0, i * 4u});
    g_vec_fields[i] = g_fields.back().get();
  }

  auto add_m = [&](MethodIndex idx, ClassIndex cidx, const char *name,
                   const char *ret, uint32_t is_static, uint32_t argc,
                   const char *p0, void *code) {
    g_methods.emplace_back(new Method);
    Method *m = g_methods.back().get();
    m->code = code;
    m->lookup = &g_classes[cidx];
    m->owner = &g_classes[cidx];
    m->name = name;
    m->result = find_type(ret);
    m->count = argc;
    if (argc == 1 && p0) m->params[0] = find_type(p0);
    m->flags = is_static ? 0x10u : 0u;
    g_slot_methods[idx] = m;
  };

  for (uint32_t i = 0; i < kTargetCount; ++i) {
    const auto &t = kTargets[i];
    add_m(static_cast<MethodIndex>(t.method_idx),
          static_cast<ClassIndex>(t.class_idx), t.method_name, t.return_type,
          t.is_static, 0, nullptr, nullptr);
  }
  add_m(M_GET_MAIN_CAMERA, C_STACK, "get_MainCamera", "UnityEngine.Camera", 0,
        0, nullptr, g_module_base + PGR_CAM_MAIN_CAMERA_RVA);
  add_m(M_GET_CULLING_MASK, C_CAMERA, "get_cullingMask", "System.Int32", 0, 0,
        nullptr, nullptr);
  add_m(M_NAME_TO_LAYER, C_LAYERMASK, "NameToLayer", "System.Int32", 1, 1,
        "System.String", nullptr);

  g_meta_exports = {
      reinterpret_cast<void *>(&fake_domain_get),
      reinterpret_cast<void *>(&fake_domain_get_assemblies),
      reinterpret_cast<void *>(&fake_assembly_get_image),
      reinterpret_cast<void *>(&fake_class_from_name),
      reinterpret_cast<void *>(&fake_class_get_type),
      reinterpret_cast<void *>(&fake_type_get_name),
      reinterpret_cast<void *>(&fake_mem_free),
      reinterpret_cast<void *>(&fake_class_get_field_from_name),
      reinterpret_cast<void *>(&fake_field_get_parent),
      reinterpret_cast<void *>(&fake_field_get_flags),
      reinterpret_cast<void *>(&fake_field_get_offset),
      reinterpret_cast<void *>(&fake_field_get_type),
      reinterpret_cast<void *>(&fake_class_from_type),
      reinterpret_cast<void *>(&fake_class_is_valuetype),
      reinterpret_cast<void *>(&fake_class_is_enum),
      reinterpret_cast<void *>(&fake_class_enum_basetype),
      reinterpret_cast<void *>(&fake_class_value_size),
      reinterpret_cast<void *>(&fake_class_get_method_from_name),
      reinterpret_cast<void *>(&fake_method_get_class),
      reinterpret_cast<void *>(&fake_method_get_name),
      reinterpret_cast<void *>(&fake_method_get_flags),
      reinterpret_cast<void *>(&fake_method_get_param_count),
      reinterpret_cast<void *>(&fake_method_get_param),
      reinterpret_cast<void *>(&fake_method_get_return_type),
      reinterpret_cast<void *>(&fake_method_is_generic),
      reinterpret_cast<void *>(&fake_method_is_inflated),
      reinterpret_cast<void*>(&fake_debug_get_method_info),
      reinterpret_cast<void *>(&fake_class_is_assignable_from),
      reinterpret_cast<void *>(&fake_class_get_methods),
      reinterpret_cast<void *>(&fake_class_get_nested_types),
      reinterpret_cast<void *>(&fake_class_get_declaring_type),
  };

  g_meta_extra = {};
  g_meta_extra.version = PGR_CAM_EXTRA_VERSION;
  g_meta_extra.size = sizeof(g_meta_extra);
  g_meta_extra.image_get_name = reinterpret_cast<void *>(&fake_image_get_name);
  g_meta_extra.get_corlib = reinterpret_cast<void *>(&fake_get_corlib);
  g_meta_extra.runtime_invoke = reinterpret_cast<void *>(&fake_runtime_invoke);
  g_meta_extra.object_get_class =
      reinterpret_cast<void *>(&fake_object_get_class);
  g_meta_extra.object_unbox = reinterpret_cast<void *>(&fake_object_unbox);
  g_meta_extra.gchandle_new = reinterpret_cast<void *>(&fake_gchandle_new);
  g_meta_extra.gchandle_get_target =
      reinterpret_cast<void *>(&fake_gchandle_get_target);
  g_meta_extra.gchandle_free = reinterpret_cast<void *>(&fake_gchandle_free);
  g_meta_extra.field_get_value =
      reinterpret_cast<void *>(&fake_field_get_value);
  g_meta_extra.string_new = reinterpret_cast<void *>(&fake_string_new);
  g_meta_extra.present = kAllOptionalExports;
}

void reset_fixture() {
  restore_module_fingerprints();
  g_rig = {&g_classes[C_RIG]};
  g_other_rig = {&g_other_class};
  g_camera = {&g_classes[C_CAMERA]};
  g_camera2 = {&g_classes[C_CAMERA]};
  g_transform = {&g_classes[C_TRANSFORM]};
  g_string = {&g_classes[C_STRING]};
  g_vector = {&g_classes[C_VECTOR3], 1.0f, 0.0f, 0.0f};
  g_layer_box = {&g_classes[C_INT32], 13};
  g_mask_box = {&g_classes[C_INT32], static_cast<int32_t>(0x80002000u)};
  g_id_box = {&g_classes[C_INT32], 1234};

  g_ret_main = &g_camera;
  g_ret_transform = &g_transform;
  g_ret_forward = &g_vector;
  g_ret_string = &g_string;
  g_getter_return_camera = &g_camera;
  g_throw_method = nullptr;
  g_throw_field = false;
  g_cleanup_throws = false;
  g_unbox_null = false;
  g_invokes = 0;
  g_on_invoke = nullptr;

  g_handles.clear();
  g_gcnew_calls = 0;
  g_gcfree_calls = 0;
  g_gcnew_fail_at = 0;
  g_gcnew_throws = false;
  g_gcnew_long_error = false;

  g_domain_ready = true;
  g_active_assemblies = g_default_assemblies;
  g_assembly_count = 2;
  g_thread_attached = true;
  g_attach_fails = false;
  g_attach_calls = 0;
  g_detach_calls = 0;
  g_debug_info_succeeds = true;

  g_fake_now_ms = 100000.0;
  g_clock_throws = false;
  g_fake_tid = static_cast<uint64_t>(static_cast<pid_t>(syscall(SYS_gettid)));
}

PgrCameraHostConfig make_host_config(uint32_t extra_flags = 0) {
  PgrCameraHostConfig cfg = {};
  cfg.version = PGR_CH_API_VERSION;
  cfg.size = sizeof(cfg);
  cfg.flags = PGR_CH_FLAG_FIXTURE_HOOK_OVERRIDE | extra_flags;
  cfg.expected_layer = 13;
  cfg.module_base = g_module_base;
  cfg.module_size = g_module_size;
  cfg.meta_exports = &g_meta_exports;
  cfg.meta_extra = &g_meta_extra;
  cfg.seg_query = &fake_seg_query;
  cfg.seg_ctx = nullptr;
  cfg.domain = &g_domain_token;
  cfg.thread_current = reinterpret_cast<void *>(&fake_thread_current);
  cfg.thread_attach = reinterpret_cast<void *>(&fake_thread_attach);
  cfg.thread_detach = reinterpret_cast<void *>(&fake_thread_detach);
  cfg.clock_fn = &test_clock_fn;
  cfg.clock_ctx = nullptr;
  cfg.tid_fn = nullptr;  // default to real OS gettid() so Feature 41 matches
  cfg.tid_ctx = nullptr;
  cfg.fixture_getter_code = reinterpret_cast<void *>(&fixture_get_main_camera);
  cfg.reserved0 = 0;
  return cfg;
}

std::vector<PgrCameraHostEvent> drain_all(const PgrCameraHostApi *api) {
  std::vector<PgrCameraHostEvent> out;
  for (;;) {
    PgrCameraHostEvent ev = {};
    ev.version = PGR_CH_API_VERSION;
    ev.size = sizeof(ev);
    int rc = api->next_event(&ev);
    assert(rc >= 0);
    if (rc == 0 || ev.kind == PGR_CH_EVENT_NONE) break;
    out.push_back(ev);
  }
  return out;
}

}  // namespace

int fixture_resolve(const PgrCameraMetaConfig *cfg,PgrCameraMetaOutput *out){
 assert(cfg&&out&&cfg->owner_tid==uint64_t(gettid()));*out={};out->version=1;out->size=sizeof(*out);out->vector_fields=PGR_CAM_VF_VERIFIED;out->target_count=5;
 for(unsigned i=0;i<kClassCount;++i)out->classes[i]=&g_classes[i];
 for(unsigned i=0;i<kFieldCount;++i)out->fields[i]=g_vec_fields[i];
 for(unsigned i=0;i<kMethodCount;++i)out->methods[i]=g_slot_methods[i];
 out->execute=g_slot_methods[M_EXECUTE_TASKS];out->get_main_camera_address=reinterpret_cast<void*>(fixture_get_main_camera);return 0;
}

int main(int argc, char **argv) {
  assert(argc >= 2 && "usage: camera_host_test <path/to/libpgrazor-core.so>");
  void *lib = dlopen(argv[1], RTLD_NOW);
  if (!lib) {
    std::fprintf(stderr, "dlopen failed: %s\n", dlerror());
    return 1;
  }

  using NativeApiFn = const void *(*)(uint32_t);
  auto get_native =
      reinterpret_cast<NativeApiFn>(dlsym(lib, "pgrazor_native_api"));
  assert(get_native != nullptr);
  const auto *core = static_cast<const pgrazor_native_api_v2 *>(get_native(2u));
  assert(core != nullptr && core->query_feature != nullptr);

  fixture_core=core;
  const auto *actual=static_cast<const PgrCameraHostApi *>(core->query_feature(64u,1u));assert(actual&&actual->size==112);
  const auto *api=static_cast<const PgrCameraHostApi *>(fixture_host_api(1));
  assert(api->version == PGR_CH_API_VERSION);
  assert(api->size == sizeof(PgrCameraHostApi));

  // Map synthetic 256 MiB module with active pages around debug/GC/getter RVAs.
  const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  g_module_size = 0x10000000ULL;
  void *mapped = mmap(nullptr, g_module_size, PROT_NONE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(mapped != MAP_FAILED);
  g_module_base = static_cast<unsigned char *>(mapped);
  uintptr_t low_end = (kGcFreeBodyRva + 0x1000ULL + page - 1) & ~(page - 1);
  assert(mprotect(g_module_base, low_end, PROT_READ | PROT_WRITE) == 0);
  uintptr_t get_page = PGR_CAM_MAIN_CAMERA_RVA & ~(page - 1);
  assert(mprotect(g_module_base + get_page, page * 2,
                  PROT_READ | PROT_WRITE) == 0);

  build_universe();
  reset_fixture();

  // Bind Feature 47 once with the canonical exports & segment resolver.
  const auto *md47 =
      static_cast<const PgrMetaApi *>(core->query_feature(47u, 1u));
  assert(md47 != nullptr);
  PgrMetaBindConfig bind_cfg = {};
  bind_cfg.version = PGR_MD_CONFIG_VERSION;
  bind_cfg.size = sizeof(bind_cfg);
  bind_cfg.exports = &g_meta_exports;
  bind_cfg.module_base = g_module_base;
  bind_cfg.module_size = g_module_size;
  bind_cfg.seg_query = &fake_seg_query;
  assert(md47->bind(&bind_cfg) == PGR_MD_OK);


  assert(!core->query_feature(64,0)&&!core->query_feature(64,2));
  PgrCameraHostConfig cfg=make_host_config();const bool bounded=argc>2&&std::strcmp(argv[2],"bounded")==0;const bool transport_mode=argc>2&&std::strcmp(argv[2],"transport")==0;
  const auto transport=static_cast<const PgrRuntimeTransportApi*>(core->query_feature(52,1));
  if(transport_mode){PgrRuntimeTransportConfig c{1,sizeof(c),1000,256,10000,65536,0,0};assert(transport->install(&c)==0);cfg.flags|=PGR_CH_FLAG_USE_TRANSPORT;}
  PgrCameraHostConfig bad=cfg;bad.version=2;assert(api->configure(&bad)==PGR_CH_ERR_VERSION);assert(actual->configure(&bad)==PGR_CH_ERR_VERSION);
  assert(api->configure(&cfg)==0);assert(api->start(nullptr)==PGR_CH_ERR_INVALID_ARG);
  g_host=api;g_scheduler=static_cast<const PgrUnitySchedulerApi3*>(core->query_feature(48,3));assert(g_scheduler);
  PgrUnitySchedulerConfig scheduler{};scheduler.version=2;scheduler.size=sizeof(scheduler);scheduler.runtime_invoke=reinterpret_cast<void*>(fake_runtime_invoke);scheduler.runtime_invoke_convert_args=reinterpret_cast<void*>(fake_convert);scheduler.execute_tasks=g_slot_methods[M_EXECUTE_TASKS];scheduler.thread_current=fake_thread_current;scheduler.verify_metadata=verify_scheduler;scheduler.expected_owner_tid=gettid();scheduler.exclusive_owner=1;scheduler.subscriber_count=1;scheduler.subscribers[0]={api->on_unity_begin,api->on_unity_end,nullptr};
  assert(g_scheduler->install(&scheduler)==0);g_generation=g_scheduler->activate();assert(g_generation);
  PgrCameraHostOptions opts{1,sizeof(opts),1,1,uint8_t(!bounded),1,250,0,0,0};assert(api->start(&opts)==0);drain_all(api);
  auto sample=[&](){PgrCameraHostSample v{};v.version=1;v.size=sizeof(v);assert(api->sample(&v)==0);return v;};
  auto renew=[&](){g_fake_now_ms+=bounded?500:100;opts.now_ms=uint64_t(g_fake_now_ms);assert(api->set_options(&opts)==0);};
  if(bounded){
   g_execute_getter=false;for(unsigned i=0;i<30&&sample().active;++i){renew();run_execute();}
   auto v=sample();fprintf(stderr,"bounded64 samples=%u active=%u roots=%zu attempts=%u matches=%u reason=%s failure=%s\n",v.sample_count,v.active,g_handles.size(),v.sample_attempts,v.matches,v.reason,v.failure_message);assert(v.sample_count==20&&!v.active&&g_handles.empty());
   puts("PASS compiled64 source+real48 bounded diagnostics driven by real48 post-original subscribers,20-sample limit and balanced roots");return 0;
  }
  auto natural_getter=[&](){run_execute();call_getter();api->tick(gettid());};
  if(transport_mode){
   uint64_t seq=0;unsigned heading_actions=0,telemetry_actions=0;auto frame=[&](bool enabled){std::vector<uint8_t> bytes(180,0);auto set=[&](size_t at,uint32_t v){memcpy(bytes.data()+at,&v,4);};set(0,PGR_TRANSPORT_SNAPSHOT_MAGIC);set(4,1);set(8,bytes.size());++seq;memcpy(bytes.data()+16,&seq,8);bytes[32+10]=enabled;uint32_t length=0;std::vector<uint8_t> output(65536);assert(transport->exchange(bytes.data(),bytes.size(),output.data(),output.size(),&length)==0);
    auto read=[&](size_t at){uint32_t v;memcpy(&v,output.data()+at,4);return v;};size_t at=32;for(unsigned n=0;n<read(12);++n){uint32_t op=read(at),ns=read(at+4),nt=read(at+8);at+=12+8*ns;heading_actions+=op==57;telemetry_actions+=op==19;for(unsigned j=0;j<nt;++j){uint32_t size=read(at);at+=4+size;}}assert(at==length);
   };
   frame(true);for(unsigned i=0;i<3;++i){renew();run_execute();}renew();natural_getter();assert(sample().sample_count==1);assert(g_handles.empty());
   for(unsigned i=0;i<130;++i){renew();natural_getter();frame(true);}assert(sample().sample_count==131&&heading_actions>=130&&telemetry_actions>10&&sample().queued_events==0&&drain_all(api).empty());
   // OFF snapshot and stale snapshot never renew a continuous camera lease.
   frame(false);g_fake_now_ms+=300;natural_getter();unsigned samples=sample().sample_count;assert(samples==131);
   frame(true);renew();natural_getter();assert(sample().sample_count==samples+1);samples++;
   usleep(1100000);renew();natural_getter();assert(sample().sample_count==samples&&g_handles.empty());
   // A newer52 snapshot arriving during41 allocation invalidates publication.
   frame(true);bool changed=false;g_on_invoke=[&](){if(!changed){changed=true;frame(false);}};renew();natural_getter();g_on_invoke=nullptr;assert(changed&&sample().sample_count==samples);
   // Queue saturation is terminal and never permits an unreported heading.
   frame(true);PgrRuntimeTransportString message{"fill",4};int fill=0;while(transport->emit(21,nullptr,0,&message,1)==0)++fill;assert(fill>0);
   renew();natural_getter();api->tick(gettid());assert(sample().stopping&&g_handles.empty());
   puts("PASS64 actual52 sustained131 headings/no adapter queue, coherent settings, OFF/stale gates, mid-sample snapshot revocation, queue backpressure stop");return 0;
  }
  // No fabricated manual or subscriber pulse authorizes continuous sampling.
  PgrUnityPulse invalid{g_generation+1,1,gettid(),0};api->on_unity_begin(nullptr,&invalid);api->on_unity_end(nullptr,&invalid);assert(sample().matches==0);
  // Warmup and subsequent getter calls inside original ExecuteTasks never sample.
  for(int i=0;i<4;++i){renew();run_execute();}assert(sample().sample_count==0);
  int before=g_invokes;
  unsigned pinned_checks=0;g_on_invoke=[&](){unsigned pinned=0;for(auto &h:g_pinned)pinned+=h.second!=0;assert(pinned>=2);++pinned_checks;};
  renew();natural_getter();assert(sample().sample_count==1&&pinned_checks&&g_handles.empty()&&g_last_return==&g_camera);g_on_invoke=nullptr;
  // Even a naturally nested getter during this host's subscriber clock work
  // forwards the original without calling managed camera methods.
  before=g_invokes;g_getter_from_clock=true;run_execute();assert(!g_getter_from_clock&&g_invokes==before);
  // Foreign/detached getter calls preserve the original and do no managed sampling.
  before=g_invokes;std::thread foreign([](){call_getter();});foreign.join();assert(g_invokes==before);
  g_thread_attached=false;renew();natural_getter();g_thread_attached=true;assert(g_invokes==before);
  // A relocating fixture updates all rooted camera references during41 work.
  bool moved=false;g_on_invoke=[&](){if(!moved){moved=true;for(auto &entry:g_handles)if(entry.second==&g_camera)entry.second=&g_camera2;}};
  renew();natural_getter();assert(moved&&g_last_return==&g_camera2&&g_handles.empty());g_on_invoke=nullptr;
  // Original exceptions propagate exactly; pinned receiver root unwinds.
  g_getter_throws=true;renew();bool threw=false;try{call_getter();}catch(const std::runtime_error&e){threw=std::strcmp(e.what(),"original-getter")==0;}g_getter_throws=false;assert(threw&&g_handles.empty());
  // Revoke during41 managed work: no late sample is published.
  const unsigned count=sample().sample_count;bool revoked_once=false;g_on_invoke=[&](){if(!revoked_once){revoked_once=true;assert(g_scheduler->revoke(g_generation)==0);}};
  renew();natural_getter();assert(sample().sample_count==count&&g_handles.empty());g_on_invoke=nullptr;g_generation=g_scheduler->activate();assert(g_generation);
  // A new active generation needs a fresh validated owner pulse before sampling.
  before=g_invokes;call_getter();assert(g_invokes==before);
  // Foreign-thread stop during synchronous managed work returns promptly (join
  // would deadlock if64 still held its old recursive mutex across41).
  bool stopped_once=false;g_on_invoke=[&](){if(!stopped_once){stopped_once=true;std::thread stopper([&](){assert(api->stop("foreign-stop")==0);});stopper.join();}};
  renew();natural_getter();g_on_invoke=nullptr;api->tick(gettid());assert(stopped_once&&!sample().active&&g_handles.empty());
  unsigned original_calls=g_getter_calls;call_getter();assert(g_getter_calls==original_calls+1&&sample().hook_installed);
  assert(api->start(&opts)==PGR_CH_ERR_RESTART_REQUIRED);assert(g_handles.empty());
  puts("PASS actual64 ABI+compiled64 source+real41 +real48 API3/Dobby: idle Unity getter boundary, in-ExecuteTasks/subscriber exclusion, generation and attachment gates, pinned roots, relocation refresh, original exception, revoke, foreign stop, permanent passthrough");
  if(argc==2)for(const char *mode:{"bounded","transport"}){pid_t child=fork();assert(child>=0);if(!child){execl(argv[0],argv[0],argv[1],mode,static_cast<char*>(nullptr));_exit(127);}int status=0;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);}
  return 0;
}
