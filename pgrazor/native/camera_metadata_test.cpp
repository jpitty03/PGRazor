// Actual production dlopen/query59 (API 1) fixture. Metadata exports are fake; the compiled
// camera-probe discover() resolver and the shared feature47 validators are REAL. A sparse
// page-aligned virtual module holds the real debug_get_method_info prologue, GC-handle prologue
// + body fingerprint bytes, and CameraStack.get_MainCamera pinned RVA instructions. Injected
// segment permissions are synthetic -- NOT a /proc/Houdini test and NO live-game claim.
#include "camera_metadata.h"
#include "pgrazor_native_api.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

namespace {
using namespace pgr_camera_schema;

struct Class;
struct Type {
  std::string name;
  Class *klass = nullptr;
  bool null_type_name = false;
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
  const char *raw_name_override = nullptr;
  bool force_null_name = false;
  std::string name;
  Type *result = nullptr;
  Type *params[2]{};
  uint32_t count = 0;
  uint32_t flags = 0;
  bool generic = false;
  bool inflated = false;
};
static_assert(offsetof(Method, code) == 0, "fixture MethodInfo code pointer first");

struct Image {
  const char *name;
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

std::vector<std::unique_ptr<Type>> types;
std::vector<std::unique_ptr<Class>> extra_classes;
std::vector<std::unique_ptr<Field>> fields;
std::vector<std::unique_ptr<Method>> methods;
Class classes[kClassCount];
Field *vec_fields[kFieldCount]{};
Method *slot_methods[kMethodCount]{};

Image img_core{"UnityEngine.CoreModule.dll"};
Image img_holo{"holo-game.dll"};
Image img_corlib{"mscorlib.dll"};
Image img_filler{"filler.dll"};
int domain_token = 1, thread_token = 1, stub_token = 1;
void *default_assemblies[2] = {&img_core, &img_holo};
void **active_assemblies = default_assemblies;
size_t assembly_count = 2;
bool domain_ready = true, corlib_ready = true, null_assemblies = false;
bool attached = true, reenter = false;
int hide_class = -1;
bool stack_assignable_from_rig = true;
bool debug_info_succeeds = true;
unsigned thread_checks = 0, detach_at = 0, names_allocated = 0, names_freed = 0, reentries = 0;
double fake_now_ms = 1000.0, fake_step_ms = 0.0;
unsigned char *module_base = nullptr;
uint64_t module_size = 0;
const PgrCameraMetaApi *api = nullptr;
PgrCameraMetaConfig config{};
PgrCameraMetaExtra extra_cfg{};

double test_clock_ms(void *) {
  double cur = fake_now_ms;
  fake_now_ms += fake_step_ms;
  return cur;
}

Type *new_type(const std::string &name, Class *klass) {
  types.emplace_back(new Type{name, klass, false});
  return types.back().get();
}
Class *new_class(const std::string &name, bool value = false, int size = 0, int align = 0) {
  extra_classes.emplace_back(new Class);
  Class *k = extra_classes.back().get();
  k->type = new_type(name, k);
  k->value = value;
  k->size = size;
  k->align = align;
  return k;
}
Type *type(const char *name) {
  assert(name);
  for (const auto &t : types)
    if (t->name == name) return t.get();
  return new_class(name)->type;
}

bool zero(const PgrCameraMetaOutput &o) {
  PgrCameraMetaOutput e{};
  return std::memcmp(&o, &e, sizeof(o)) == 0;
}

void *domain_get() { return domain_ready ? &domain_token : nullptr; }
void *domain_assemblies(void *dom, size_t *count) {
  if (reenter) {
    reenter = false;
    PgrCameraMetaOutput out;
    std::memset(&out, 0xa5, sizeof(out));
    assert(api->resolve(&config, &out) == PGR_MD_ERR_BUSY && zero(out));
    ++reentries;
  }
  assert(dom == &domain_token);
  *count = assembly_count;
  return null_assemblies ? nullptr : active_assemblies;
}
void *assembly_image(void *a) { return a; }
const char *image_get_name(void *img) {
  return img ? static_cast<Image *>(img)->name : nullptr;
}
void *get_corlib() { return corlib_ready ? &img_corlib : nullptr; }

struct ClassInfo {
  void *image;
  const char *ns;
  const char *name;
  const char *canonical;
};

void *class_from_name(void *image, const char *ns, const char *name) {
  const ClassInfo table[kClassCount] = {
    {&img_core,   "UnityEngine",                "Camera",                      "UnityEngine.Camera"},
    {&img_core,   "UnityEngine",                "Component",                   "UnityEngine.Component"},
    {&img_core,   "UnityEngine",                "Transform",                   "UnityEngine.Transform"},
    {&img_core,   "UnityEngine",                "UnitySynchronizationContext", "UnityEngine.UnitySynchronizationContext"},
    {&img_core,   "UnityEngine",                "Object",                      "UnityEngine.Object"},
    {&img_core,   "UnityEngine",                "Vector3",                     "UnityEngine.Vector3"},
    {&img_corlib, "System",                     "Int32",                       "System.Int32"},
    {&img_holo,   "Niantic.Holoholo.CameraRig", "MainCameraRig",               "Niantic.Holoholo.CameraRig.MainCameraRig"},
    {&img_holo,   "Niantic.Holoholo.Cameras",   "CameraStack",                 "Niantic.Holoholo.Cameras.CameraStack"},
    {&img_corlib, "System",                     "String",                      "System.String"},
    {&img_core,   "UnityEngine",                "LayerMask",                   "UnityEngine.LayerMask"},
  };
  for (size_t i = 0; i < kClassCount; i++) {
    if (static_cast<int>(i) == hide_class) continue;
    if (image == table[i].image && !std::strcmp(ns, table[i].ns) && !std::strcmp(name, table[i].name))
      return &classes[i];
  }
  return nullptr;
}
void *class_type(void *k) {
  assert(k);
  return static_cast<Class *>(k)->type;
}
char *type_name(void *raw) {
  assert(raw);
  auto *t = static_cast<Type *>(raw);
  if (t->null_type_name) return nullptr;
  const auto &n = t->name;
  char *copy = static_cast<char *>(std::malloc(n.size() + 1));
  assert(copy);
  std::memcpy(copy, n.c_str(), n.size() + 1);
  ++names_allocated;
  return copy;
}
void mem_free(void *p) {
  assert(p);
  ++names_freed;
  std::free(p);
}
void *field_by_name(void *owner, const char *name) {
  for (auto &f : fields)
    if (f->lookup == owner && f->name == name) return f.get();
  return nullptr;
}
void *field_parent(void *raw) { return static_cast<Field *>(raw)->owner; }
int field_flags(void *raw) { return static_cast<Field *>(raw)->flags; }
size_t field_offset(void *raw) { return static_cast<Field *>(raw)->offset; }
void *field_type(void *raw) { return static_cast<Field *>(raw)->type; }
void *from_type(void *raw) { return raw ? static_cast<Type *>(raw)->klass : nullptr; }
bool value_type(void *raw) { return static_cast<Class *>(raw)->value; }
bool is_enum(void *raw) { return static_cast<Class *>(raw)->enumeration; }
void *enum_base(void *raw) { return static_cast<Class *>(raw)->base; }
int value_size(void *raw, int *align) {
  auto *k = static_cast<Class *>(raw);
  if (align) *align = k->align;
  return k->size;
}
void *method_by_name(void *owner, const char *name, int count) {
  for (auto &m : methods)
    if (m->lookup == owner && m->name == name && int(m->count) == count) return m.get();
  return nullptr;
}
void *method_class(void *raw) { return static_cast<Method *>(raw)->owner; }
const char *method_name(void *raw) {
  auto *m = static_cast<Method *>(raw);
  if (m->force_null_name) return nullptr;
  if (m->raw_name_override) return m->raw_name_override;
  return m->name.c_str();
}
uint32_t method_flags(void *raw, uint32_t *impl) {
  if (impl) *impl = 0;
  return static_cast<Method *>(raw)->flags;
}
uint32_t param_count(void *raw) { return static_cast<Method *>(raw)->count; }
void *param(void *raw, uint32_t index) {
  assert(index < 2);
  return static_cast<Method *>(raw)->params[index];
}
void *return_type(void *raw) { return static_cast<Method *>(raw)->result; }
bool generic(void *raw) { return static_cast<Method *>(raw)->generic; }
bool inflated(void *raw) { return static_cast<Method *>(raw)->inflated; }
bool debug_info(void *raw, void *out) {
  if (!debug_info_succeeds) return false;
  static_cast<void **>(out)[0] = static_cast<Method *>(raw)->code;
  return true;
}
bool assignable(void *klass, void *subklass) {
  if (klass == &classes[C_STACK] && subklass == &classes[C_RIG])
    return stack_assignable_from_rig;
  return klass == subklass;
}
void *enumerate_empty(void *, void **) { return nullptr; }
void *declaring(void *) { return nullptr; }
void *thread_current() {
  ++thread_checks;
  return attached && (!detach_at || thread_checks < detach_at) ? &thread_token : nullptr;
}
int segment(void *, const void *address, uint64_t length, PgrMetaSegment *out) {
  *out = {};
  uintptr_t a = reinterpret_cast<uintptr_t>(address), base = reinterpret_cast<uintptr_t>(module_base);
  if (a < base || a - base > module_size || !length || length > module_size - (a - base)) return 0;
  out->found = out->readable = out->host_exec = out->writable = 1;
  out->seg_base = module_base;
  out->seg_size = module_size;
  return 0;
}

PgrMetaExports exports() {
  PgrMetaExports e{};
  e.domain_get = reinterpret_cast<void *>(domain_get);
  e.domain_get_assemblies = reinterpret_cast<void *>(domain_assemblies);
  e.assembly_get_image = reinterpret_cast<void *>(assembly_image);
  e.class_from_name = reinterpret_cast<void *>(class_from_name);
  e.class_get_type = reinterpret_cast<void *>(class_type);
  e.type_get_name = reinterpret_cast<void *>(type_name);
  e.mem_free = reinterpret_cast<void *>(mem_free);
  e.class_get_field_from_name = reinterpret_cast<void *>(field_by_name);
  e.field_get_parent = reinterpret_cast<void *>(field_parent);
  e.field_get_flags = reinterpret_cast<void *>(field_flags);
  e.field_get_offset = reinterpret_cast<void *>(field_offset);
  e.field_get_type = reinterpret_cast<void *>(field_type);
  e.class_from_type = reinterpret_cast<void *>(from_type);
  e.class_is_valuetype = reinterpret_cast<void *>(value_type);
  e.class_is_enum = reinterpret_cast<void *>(is_enum);
  e.class_enum_basetype = reinterpret_cast<void *>(enum_base);
  e.class_value_size = reinterpret_cast<void *>(value_size);
  e.class_get_method_from_name = reinterpret_cast<void *>(method_by_name);
  e.method_get_class = reinterpret_cast<void *>(method_class);
  e.method_get_name = reinterpret_cast<void *>(method_name);
  e.method_get_flags = reinterpret_cast<void *>(method_flags);
  e.method_get_param_count = reinterpret_cast<void *>(param_count);
  e.method_get_param = reinterpret_cast<void *>(param);
  e.method_get_return_type = reinterpret_cast<void *>(return_type);
  e.method_is_generic = reinterpret_cast<void *>(generic);
  e.method_is_inflated = reinterpret_cast<void *>(inflated);
  e.debug_get_method_info = reinterpret_cast<void *>(debug_info);
  e.class_is_assignable_from = reinterpret_cast<void *>(assignable);
  e.class_get_methods = reinterpret_cast<void *>(enumerate_empty);
  e.class_get_nested_types = reinterpret_cast<void *>(enumerate_empty);
  e.class_get_declaring_type = reinterpret_cast<void *>(declaring);
  return e;
}

void write_u32(uint64_t rva, uint32_t word) {
  std::memcpy(module_base + rva, &word, sizeof(word));
}

uint32_t encode_b(uint64_t from_rva, uint64_t to_rva) {
  int64_t diff = (static_cast<int64_t>(to_rva) - static_cast<int64_t>(from_rva)) / 4;
  assert(diff >= -(1 << 25) && diff < (1 << 25));
  return 0x14000000u | (static_cast<uint32_t>(diff) & 0x03ffffffu);
}

void write_code_blocks() {
  std::memcpy(module_base + kDebugMethodInfoRva, kDebugPrologue, kDebugPrologueLen);

  for (uint32_t i = 0; i < kGcCreateWordCount; i++)
    write_u32(kGcNewRva + i * 4, kGcCreateWords[i]);
  write_u32(kGcNewRva + kGcCreateBranchOff, encode_b(kGcNewRva + kGcCreateBranchOff, kGcNewBodyRva));
  write_u32(kGcNewBodyRva + kGcCreateBodyOff, kGcCreateBodyWord);

  write_u32(kGcGetRva, encode_b(kGcGetRva, kGcGetBodyRva));
  write_u32(kGcGetBodyRva, kGcGetHead);
  write_u32(kGcGetBodyRva + kGcGetTailOff, kGcGetTail[0]);
  write_u32(kGcGetBodyRva + kGcGetTailOff + 4, kGcGetTail[1]);

  write_u32(kGcFreeRva, encode_b(kGcFreeRva, kGcFreeBodyRva));
  write_u32(kGcFreeBodyRva, kGcFreeHead);
  write_u32(kGcFreeBodyRva + kGcFreeTailOff, kGcFreeTail[0]);
  write_u32(kGcFreeBodyRva + kGcFreeTailOff + 4, kGcFreeTail[1]);

  write_u32(PGR_CAM_MAIN_CAMERA_RVA, kGetMainCameraWords[0]);
  write_u32(PGR_CAM_MAIN_CAMERA_RVA + 4, kGetMainCameraWords[1]);
}

Method *add_method(Class *owner, const char *name, Type *ret, bool is_static,
                   Type *p0 = nullptr, void *code = nullptr) {
  uint32_t cnt = p0 ? 1u : 0u;
  methods.emplace_back(new Method{code, owner, owner, nullptr, false, name, ret, {p0, nullptr},
                                  cnt, is_static ? 0x10u : 0u, false, false});
  return methods.back().get();
}

void build_fixture() {
  assert(kClassCount == 11 && kFieldCount == 3 && kMethodCount == 8 && kTargetCount == 5);
  assert(PGR_CAM_MAIN_CAMERA_RVA == 0xae09554u);
  assert(kDebugPrologueLen == 80 && kGetMainCameraByteLen == 8);

  uint64_t top = PGR_CAM_MAIN_CAMERA_RVA + 64ULL;
  long page = sysconf(_SC_PAGESIZE);
  assert(page > 0);
  module_size = ((top + page - 1) / page) * page;
  void *mapped = mmap(nullptr, module_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(mapped != MAP_FAILED);
  module_base = static_cast<unsigned char *>(mapped);
  write_code_blocks();

  const char *canonicals[kClassCount] = {
    "UnityEngine.Camera",
    "UnityEngine.Component",
    "UnityEngine.Transform",
    "UnityEngine.UnitySynchronizationContext",
    "UnityEngine.Object",
    "UnityEngine.Vector3",
    "System.Int32",
    "Niantic.Holoholo.CameraRig.MainCameraRig",
    "Niantic.Holoholo.Cameras.CameraStack",
    "System.String",
    "UnityEngine.LayerMask",
  };
  for (size_t i = 0; i < kClassCount; i++)
    classes[i].type = new_type(canonicals[i], &classes[i]);
  classes[C_VECTOR3].value = true;
  classes[C_VECTOR3].size = 12;
  classes[C_VECTOR3].align = 4;
  classes[C_INT32].value = true;
  classes[C_INT32].size = 4;
  classes[C_INT32].align = 4;
  classes[C_LAYERMASK].value = true;
  classes[C_LAYERMASK].size = 4;
  classes[C_LAYERMASK].align = 4;

  Type *single_type = new_class("System.Single", true, 4, 4)->type;
  Type *void_type = new_class("System.Void", true, 0, 1)->type;

  for (uint32_t f = 0; f < kFieldCount; f++) {
    fields.emplace_back(new Field{&classes[C_VECTOR3], &classes[C_VECTOR3], kVectorFieldNames[f],
                                  single_type, 0u, f * 4ULL});
    vec_fields[f] = fields.back().get();
  }

  slot_methods[M_GET_MAIN] = add_method(&classes[C_CAMERA], "get_main", classes[C_CAMERA].type, true);
  slot_methods[M_GET_TRANSFORM] = add_method(&classes[C_COMPONENT], "get_transform", classes[C_TRANSFORM].type, false);
  slot_methods[M_GET_FORWARD] = add_method(&classes[C_TRANSFORM], "get_forward", classes[C_VECTOR3].type, false);
  slot_methods[M_EXECUTE_TASKS] = add_method(&classes[C_SYNCCTX], "ExecuteTasks", void_type, true);
  slot_methods[M_GET_INSTANCE_ID] = add_method(&classes[C_OBJECT], "GetInstanceID", classes[C_INT32].type, false);
  slot_methods[M_GET_MAIN_CAMERA] = add_method(&classes[C_STACK], "get_MainCamera", classes[C_CAMERA].type, false,
                                               nullptr, module_base + PGR_CAM_MAIN_CAMERA_RVA);
  slot_methods[M_GET_CULLING_MASK] = add_method(&classes[C_CAMERA], "get_cullingMask", classes[C_INT32].type, false);
  slot_methods[M_NAME_TO_LAYER] = add_method(&classes[C_LAYERMASK], "NameToLayer", classes[C_INT32].type, true,
                                             classes[C_STRING].type);
}

PgrCameraMetaOutput ok_passive() {
  auto c = config;
  c.sample_camera = 0;
  c.continuous = 0;
  PgrCameraMetaOutput out{};
  assert(api->resolve(&c, &out) == PGR_CAM_OK);
  assert(out.version == 1 && out.size == sizeof(out) && out.status == PGR_CAM_OK);
  assert(out.vector_fields == PGR_CAM_VF_VERIFIED && out.target_count == 4);
  assert(out.core_image == &img_core && out.game_image == nullptr && out.corlib_image == nullptr);
  assert(out.execute == slot_methods[M_EXECUTE_TASKS]);
  assert(out.debug_address == nullptr && out.get_main_camera_address == nullptr);
  for (uint32_t i = 0; i < 4; i++) assert(out.classes[i] == &classes[i]);
  assert(out.classes[C_OBJECT] == nullptr && out.classes[C_VECTOR3] == &classes[C_VECTOR3]);
  for (uint32_t i = C_INT32; i < kClassCount; i++) assert(out.classes[i] == nullptr);
  for (uint32_t f = 0; f < kFieldCount; f++) assert(out.fields[f] == vec_fields[f]);
  for (uint32_t m = 0; m < 4; m++) assert(out.methods[m] == slot_methods[m]);
  for (uint32_t m = 4; m < kMethodCount; m++) assert(out.methods[m] == nullptr);
  assert(out.reason[0] == '\0' && names_allocated == names_freed);
  return out;
}

PgrCameraMetaOutput ok_sample() {
  auto c = config;
  c.sample_camera = 1;
  c.continuous = 0;
  PgrCameraMetaOutput out{};
  assert(api->resolve(&c, &out) == PGR_CAM_OK);
  assert(out.version == 1 && out.size == sizeof(out) && out.status == PGR_CAM_OK);
  assert(out.vector_fields == PGR_CAM_VF_VERIFIED && out.target_count == 5);
  assert(out.core_image == &img_core && out.game_image == nullptr && out.corlib_image == &img_corlib);
  assert(out.execute == slot_methods[M_EXECUTE_TASKS]);
  assert(out.debug_address == nullptr && out.get_main_camera_address == nullptr);
  for (uint32_t i = 0; i <= C_INT32; i++) assert(out.classes[i] == &classes[i]);
  for (uint32_t i = C_RIG; i < kClassCount; i++) assert(out.classes[i] == nullptr);
  for (uint32_t f = 0; f < kFieldCount; f++) assert(out.fields[f] == vec_fields[f]);
  for (uint32_t m = 0; m <= M_GET_INSTANCE_ID; m++) assert(out.methods[m] == slot_methods[m]);
  for (uint32_t m = M_GET_MAIN_CAMERA; m < kMethodCount; m++) assert(out.methods[m] == nullptr);
  assert(out.reason[0] == '\0' && names_allocated == names_freed);
  return out;
}

PgrCameraMetaOutput ok_continuous() {
  auto c = config;
  c.sample_camera = 1;
  c.continuous = 1;
  PgrCameraMetaOutput out{};
  assert(api->resolve(&c, &out) == PGR_CAM_OK);
  assert(out.version == 1 && out.size == sizeof(out) && out.status == PGR_CAM_OK);
  assert(out.vector_fields == PGR_CAM_VF_VERIFIED && out.target_count == 5);
  assert(out.core_image == &img_core && out.game_image == &img_holo && out.corlib_image == &img_corlib);
  assert(out.execute == slot_methods[M_EXECUTE_TASKS]);
  assert(out.debug_address == module_base + kDebugMethodInfoRva);
  assert(out.get_main_camera_address == module_base + PGR_CAM_MAIN_CAMERA_RVA);
  for (uint32_t i = 0; i < kClassCount; i++) assert(out.classes[i] == &classes[i]);
  for (uint32_t f = 0; f < kFieldCount; f++) assert(out.fields[f] == vec_fields[f]);
  for (uint32_t m = 0; m < kMethodCount; m++) assert(out.methods[m] == slot_methods[m]);
  assert(out.reason[0] == '\0' && names_allocated == names_freed);
  return out;
}

void expect_retry(const char *expected_reason, const PgrCameraMetaConfig *custom = nullptr) {
  PgrCameraMetaOutput out;
  std::memset(&out, 0xa5, sizeof(out));
  assert(api->resolve(custom ? custom : &config, &out) == PGR_CAM_RETRY);
  assert(out.version == 1 && out.size == sizeof(out) && out.status == PGR_CAM_RETRY);
  assert(!std::strcmp(out.reason, expected_reason));
  assert(out.execute == nullptr && out.target_count == 0 && names_allocated == names_freed);
}

void expect_unavailable(const char *expected_reason, const PgrCameraMetaConfig *custom = nullptr) {
  PgrCameraMetaOutput out;
  std::memset(&out, 0xa5, sizeof(out));
  assert(api->resolve(custom ? custom : &config, &out) == PGR_CAM_UNAVAILABLE);
  assert(out.version == 1 && out.size == sizeof(out) && out.status == PGR_CAM_UNAVAILABLE);
  assert(!std::strcmp(out.reason, expected_reason));
  assert(out.execute == nullptr && out.target_count == 0 && names_allocated == names_freed);
}

void expect_fatal(int expected, const PgrCameraMetaConfig *custom = nullptr) {
  PgrCameraMetaOutput out;
  std::memset(&out, 0xa5, sizeof(out));
  assert(api->resolve(custom ? custom : &config, &out) == expected);
  assert(zero(out) && names_allocated == names_freed);
}
}  // namespace

int main(int argc, char **argv) {
  assert(argc == 2);
  void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  assert(library);
  auto get = reinterpret_cast<const void *(*)(uint32_t)>(dlsym(library, "pgrazor_native_api"));
  assert(get);
  auto *core = static_cast<const pgrazor_native_api_v2 *>(get(2));
  assert(core && core->query_feature);
  assert(core->query_feature(47, 1) != nullptr);
  assert(!core->query_feature(59, 0) && !core->query_feature(59, 2));
  api = static_cast<const PgrCameraMetaApi *>(core->query_feature(59, 1));
  assert(api && api->version == 1 && api->size == 24 &&
         api->config_size == sizeof(PgrCameraMetaConfig) &&
         api->output_size == sizeof(PgrCameraMetaOutput));

  build_fixture();
  PgrMetaExports e = exports();
  extra_cfg.version = 1;
  extra_cfg.size = sizeof(PgrCameraMetaExtra);
  extra_cfg.image_get_name = reinterpret_cast<void *>(image_get_name);
  extra_cfg.get_corlib = reinterpret_cast<void *>(get_corlib);
  extra_cfg.runtime_invoke = &stub_token;
  extra_cfg.object_get_class = &stub_token;
  extra_cfg.object_unbox = &stub_token;
  extra_cfg.gchandle_new = module_base + kGcNewRva;
  extra_cfg.gchandle_get_target = module_base + kGcGetRva;
  extra_cfg.gchandle_free = module_base + kGcFreeRva;
  extra_cfg.field_get_value = &stub_token;
  extra_cfg.string_new = &stub_token;
  extra_cfg.clock_ms = test_clock_ms;
  extra_cfg.clock_ctx = nullptr;
  extra_cfg.present = kAllOptionalExports;
  extra_cfg.reserved = 0;

  config.version = 1;
  config.size = sizeof(PgrCameraMetaConfig);
  config.binding.version = 1;
  config.binding.size = sizeof(PgrMetaBindConfig);
  config.binding.exports = &e;
  config.binding.module_base = module_base;
  config.binding.module_size = module_size;
  config.binding.seg_query = segment;
  config.binding.seg_ctx = nullptr;
  config.binding.debug_prologue = module_base + kDebugMethodInfoRva;
  config.binding.debug_prologue_len = 0;
  config.thread_current = reinterpret_cast<void *>(thread_current);
  config.owner_tid = gettid();
  config.domain = &domain_token;
  config.extra = &extra_cfg;
  config.sample_camera = 1;
  config.continuous = 1;

  // 1) All 3 modes succeed and produce deterministic handles.
  ok_passive();
  ok_sample();
  ok_continuous();
  ok_continuous();  // repeated calls preserve feature47 binding idempotently

  // Legacy passive discover does not require these feature47-only exports.
  const decltype(&PgrMetaExports::domain_get) unused[] = {
    &PgrMetaExports::class_get_type,&PgrMetaExports::field_get_offset,&PgrMetaExports::class_from_type,
    &PgrMetaExports::class_is_enum,&PgrMetaExports::class_enum_basetype,&PgrMetaExports::class_get_methods,
    &PgrMetaExports::class_get_nested_types,&PgrMetaExports::class_get_declaring_type};
  for(auto member:unused){auto partial=e;partial.*member=nullptr;auto c=config;
    c.binding.exports=&partial;c.sample_camera=0;c.continuous=0;PgrCameraMetaOutput out{};
    assert(api->resolve(&c,&out)==PGR_CAM_OK&&out.target_count==4&&out.execute);
  }
  // Continuous-only metadata absence must not disable bounded sampling.
  const decltype(&PgrMetaExports::domain_get) continuous_only[] = {
    &PgrMetaExports::method_is_generic,&PgrMetaExports::method_is_inflated,
    &PgrMetaExports::method_get_param,&PgrMetaExports::debug_get_method_info};
  for(auto member:continuous_only){auto partial=e;partial.*member=nullptr;auto c=config;
    c.binding.exports=&partial;c.sample_camera=1;c.continuous=0;PgrCameraMetaOutput out{};
    assert(api->resolve(&c,&out)==PGR_CAM_OK&&out.target_count==5&&out.fields[0]);
  }
  { auto partial=e;for(auto member:unused)partial.*member=nullptr;auto c=config;
    c.binding.exports=&partial;PgrCameraMetaOutput out{};
    assert(api->resolve(&c,&out)==PGR_CAM_OK&&out.get_main_camera_address);
    // Partial-export range validation must reject writable non-executable code
    // before dereferencing GC fingerprint bytes, despite valid metadata.
    c.binding.seg_query=[](void *,const void *address,uint64_t length,PgrMetaSegment *seg)->int{
      *seg={};seg->found=seg->readable=seg->writable=1;seg->seg_base=address;seg->seg_size=length;return 0;};
    expect_unavailable("gc-handle-abi-unverified",&c);
  }

  // 2) Envelope, attachment, extra block, and mandatory export validation.
  { PgrCameraMetaOutput out; std::memset(&out, 0xa5, sizeof(out));
    assert(api->resolve(nullptr, &out) == PGR_MD_ERR_BAD_VERSION && zero(out));
    assert(api->resolve(&config, nullptr) == PGR_MD_ERR_NULL_ARG); }
  { auto c = config; c.version = 2; expect_fatal(PGR_MD_ERR_BAD_VERSION, &c);
    c = config; c.size--; expect_fatal(PGR_MD_ERR_BAD_VERSION, &c);
    c = config; c.thread_current = nullptr; expect_fatal(PGR_MD_ERR_BAD_VERSION, &c);
    c = config; c.sample_camera = 2; expect_fatal(PGR_MD_ERR_BAD_VERSION, &c);
    c = config; c.continuous = 2; expect_fatal(PGR_MD_ERR_BAD_VERSION, &c); }
  { auto c = config; c.extra = nullptr; expect_fatal(PGR_CAM_ERR_EXTRA_MISSING, &c); }
  { auto x = extra_cfg; x.version = 2; auto c = config; c.extra = &x; expect_fatal(PGR_CAM_ERR_EXTRA_MISSING, &c); }
  { auto x = extra_cfg; x.image_get_name = nullptr; auto c = config; c.extra = &x; expect_fatal(PGR_CAM_ERR_EXTRA_MISSING, &c); }
  { auto e2 = e; e2.class_from_name = nullptr; auto c = config; c.binding.exports = &e2; expect_fatal(PGR_MD_ERR_MISSING_EXPORT, &c); }
  attached = false; expect_fatal(PGR_CAM_ERR_ATTACH); attached = true;
  { auto c = config; c.owner_tid = 0; expect_fatal(PGR_CAM_ERR_ATTACH, &c);
    c.owner_tid = config.owner_tid + 1; expect_fatal(PGR_CAM_ERR_ATTACH, &c); }
  { std::thread foreign([] { expect_fatal(PGR_CAM_ERR_ATTACH); }); foreign.join(); }
  thread_checks = 0; detach_at = 5; expect_fatal(PGR_CAM_ERR_ATTACH); detach_at = 0; ok_continuous();

  // 3) Retry conditions and assembly bound (128).
  { auto c = config; c.domain = nullptr; domain_ready = false; expect_retry("assemblies-not-ready", &c); domain_ready = true; }
  { assembly_count = 0; expect_retry("assemblies-not-ready"); assembly_count = 2; }
  { void *only_holo[1] = {&img_holo}; active_assemblies = only_holo; assembly_count = 1;
    expect_retry("core-module-not-found-within-bound");
    active_assemblies = default_assemblies; assembly_count = 2; }
  { void *only_core[1] = {&img_core}; active_assemblies = only_core; assembly_count = 1;
    expect_retry("holo-game-not-found-within-bound");
    // When continuous == 0, holo-game.dll is not required.
    ok_sample();
    active_assemblies = default_assemblies; assembly_count = 2; }
  { void *many[130]{};
    many[0] = &img_core;
    for (size_t i = 1; i < 128; i++) many[i] = &img_filler;
    many[128] = &img_holo;  // index 128 is past LIMITS.assemblies (128)
    active_assemblies = many; assembly_count = 130;
    expect_retry("holo-game-not-found-within-bound");
    many[127] = &img_holo;  // index 127 is within bound
    ok_continuous();
    active_assemblies = default_assemblies; assembly_count = 2; }

  // 4) Metadata scan budget (LIMITS.scanMs = 3000) via clock_ms seam.
  { fake_step_ms = 3000.0; expect_unavailable("metadata-scan-budget"); fake_step_ms = 0.0; ok_continuous(); }

  // 5) Fatal metadata faults during assembly/target/type inspection.
  { null_assemblies = true; expect_fatal(PGR_CAM_ERR_NULL_ASSEMBLIES); null_assemblies = false; }
  { const char *saved = img_core.name; img_core.name = nullptr; expect_fatal(PGR_CAM_ERR_NULL_STRING); img_core.name = saved; }
  { std::string long_name(256, 'A'); const char *saved = img_core.name; img_core.name = long_name.c_str();
    expect_fatal(PGR_MD_ERR_NAME_TOO_LONG); img_core.name = saved; }
  { Type *saved = slot_methods[M_GET_MAIN]->result; slot_methods[M_GET_MAIN]->result = nullptr;
    expect_fatal(PGR_CAM_ERR_NULL_TYPE); slot_methods[M_GET_MAIN]->result = saved; }
  { classes[C_CAMERA].type->null_type_name = true;
    expect_fatal(PGR_MD_ERR_NULL_TYPE_NAME);
    classes[C_CAMERA].type->null_type_name = false; }
  { std::string saved = classes[C_CAMERA].type->name; classes[C_CAMERA].type->name = std::string(256, 'T');
    expect_fatal(PGR_MD_ERR_NAME_TOO_LONG); classes[C_CAMERA].type->name = saved; }
  { slot_methods[M_GET_MAIN]->force_null_name = true;
    expect_fatal(PGR_CAM_ERR_NULL_STRING);
    slot_methods[M_GET_MAIN]->force_null_name = false; }

  // 6) Target class/method missing & signature mismatch.
  { hide_class = C_CAMERA; expect_unavailable("missing-class:Camera"); hide_class = -1; }
  { hide_class = C_OBJECT;
    ok_passive();  // Object is only resolved when sampleCamera == 1
    expect_unavailable("missing-class:Object");
    hide_class = -1; }
  { std::string saved = slot_methods[M_GET_FORWARD]->name; slot_methods[M_GET_FORWARD]->name = "missing";
    expect_unavailable("missing-method:Transform.get_forward");
    slot_methods[M_GET_FORWARD]->name = saved; }
  { uint32_t saved = slot_methods[M_GET_MAIN]->flags; slot_methods[M_GET_MAIN]->flags = 0u;
    expect_unavailable("signature-mismatch:Camera.get_main");
    slot_methods[M_GET_MAIN]->flags = saved; }
  { Type *saved = slot_methods[M_EXECUTE_TASKS]->result; slot_methods[M_EXECUTE_TASKS]->result = classes[C_INT32].type;
    expect_unavailable("signature-mismatch:UnitySynchronizationContext.ExecuteTasks");
    slot_methods[M_EXECUTE_TASKS]->result = saved; }
  { Class *saved = slot_methods[M_GET_TRANSFORM]->owner; slot_methods[M_GET_TRANSFORM]->owner = &classes[C_CAMERA];
    expect_unavailable("signature-mismatch:Component.get_transform");
    slot_methods[M_GET_TRANSFORM]->owner = saved; }
  { slot_methods[M_GET_INSTANCE_ID]->raw_name_override = "WrongName";
    expect_unavailable("signature-mismatch:Object.GetInstanceID");
    slot_methods[M_GET_INSTANCE_ID]->raw_name_override = nullptr; }

  // 7) Vector3 field preparatory checks: non-fatal in passive mode, unavailable in sampleCamera mode.
  { auto e2 = e; e2.class_get_field_from_name = nullptr;
    auto c = config; c.binding.exports = &e2; c.sample_camera = 0; c.continuous = 0;
    PgrCameraMetaOutput out{}; assert(api->resolve(&c, &out) == PGR_CAM_OK);
    assert(out.vector_fields == PGR_CAM_VF_EXPORTS_UNAVAILABLE);
    c.sample_camera = 1; expect_unavailable("vector-fields-exports-unavailable", &c); }
  { hide_class = C_VECTOR3;
    auto c = config; c.sample_camera = 0; c.continuous = 0;
    PgrCameraMetaOutput out{}; assert(api->resolve(&c, &out) == PGR_CAM_OK);
    assert(out.vector_fields == PGR_CAM_VF_CLASS_UNAVAILABLE);
    c.sample_camera = 1; expect_unavailable("vector-fields-class-unavailable", &c);
    hide_class = -1; }
  { uint32_t saved = vec_fields[F_Y]->flags; vec_fields[F_Y]->flags = 0x10u;
    auto c = config; c.sample_camera = 0; c.continuous = 0;
    PgrCameraMetaOutput out{}; assert(api->resolve(&c, &out) == PGR_CAM_OK);
    assert(out.vector_fields == PGR_CAM_VF_SIGNATURE_MISMATCH && out.fields[0] == nullptr);
    c.sample_camera = 1; expect_unavailable("vector-fields-signature-mismatch", &c);
    vec_fields[F_Y]->flags = saved; }

  // 8) sampleCamera gates: pointerHandleAbi, sampling exports, corlib, boxed value types.
  { module_base[kGcNewBodyRva + kGcCreateBodyOff] ^= 1;
    expect_unavailable("gc-handle-abi-unverified");
    module_base[kGcNewBodyRva + kGcCreateBodyOff] ^= 1; }
  { module_base[kGcFreeBodyRva + kGcFreeTailOff] ^= 1;
    expect_unavailable("gc-handle-abi-unverified");
    module_base[kGcFreeBodyRva + kGcFreeTailOff] ^= 1; }
  { auto x = extra_cfg; x.runtime_invoke = nullptr; auto c = config; c.extra = &x;
    expect_unavailable("sampling-export-missing:runtime_invoke", &c); }
  { auto x = extra_cfg; x.get_corlib = nullptr; auto c = config; c.extra = &x;
    expect_unavailable("sampling-export-missing:get_corlib", &c); }
  { auto e2 = e; e2.class_is_valuetype = nullptr; auto c = config; c.binding.exports = &e2;
    expect_unavailable("sampling-export-missing:class_is_valuetype", &c); }
  { corlib_ready = false; expect_unavailable("corlib-unavailable"); corlib_ready = true; }
  { int saved = classes[C_INT32].size; classes[C_INT32].size = 8;
    expect_unavailable("boxed-value-type-mismatch");
    classes[C_INT32].size = saved; }
  { bool saved = classes[C_VECTOR3].value; classes[C_VECTOR3].value = false;
    expect_unavailable("boxed-value-type-mismatch");
    classes[C_VECTOR3].value = saved; }

  // 9) Continuous map-gate checks: classes, exports, debug_get_method_info ABI, get_MainCamera RVA/code, cullingMask, NameToLayer.
  { hide_class = C_RIG; expect_unavailable("map-gate-class-mismatch"); hide_class = -1; }
  { stack_assignable_from_rig = false; expect_unavailable("map-gate-class-mismatch"); stack_assignable_from_rig = true; }
  { classes[C_STRING].value = true; expect_unavailable("map-gate-class-mismatch"); classes[C_STRING].value = false; }
  { auto e2 = e; e2.debug_get_method_info = nullptr; auto c = config; c.binding.exports = &e2;
    expect_unavailable("map-gate-export-missing:debug_get_method_info", &c); }
  { auto x = extra_cfg; x.string_new = nullptr; auto c = config; c.extra = &x;
    expect_unavailable("map-gate-export-missing:string_new", &c); }
  { module_base[kDebugMethodInfoRva] ^= 1;
    expect_unavailable("map-debug-info-abi-unverified");
    module_base[kDebugMethodInfoRva] ^= 1; }
  { std::string saved = slot_methods[M_GET_MAIN_CAMERA]->name; slot_methods[M_GET_MAIN_CAMERA]->name = "missing";
    expect_unavailable("map-gate-method-missing:get_MainCamera");
    slot_methods[M_GET_MAIN_CAMERA]->name = saved; }
  { slot_methods[M_GET_MAIN_CAMERA]->generic = true;
    expect_unavailable("map-gate-signature-mismatch:get_MainCamera");
    slot_methods[M_GET_MAIN_CAMERA]->generic = false; }
  { slot_methods[M_GET_MAIN_CAMERA]->inflated = true;
    expect_unavailable("map-gate-signature-mismatch:get_MainCamera");
    slot_methods[M_GET_MAIN_CAMERA]->inflated = false; }
  { debug_info_succeeds = false;
    expect_unavailable("map-gate-code-unavailable:get_MainCamera");
    debug_info_succeeds = true; }
  { void *saved = slot_methods[M_GET_MAIN_CAMERA]->code;
    slot_methods[M_GET_MAIN_CAMERA]->code = module_base + PGR_CAM_MAIN_CAMERA_RVA + 4;
    expect_unavailable("map-gate-code-mismatch:get_MainCamera");
    slot_methods[M_GET_MAIN_CAMERA]->code = saved; }
  { module_base[PGR_CAM_MAIN_CAMERA_RVA] ^= 1;
    expect_unavailable("map-camera-getter-code-unverified");
    module_base[PGR_CAM_MAIN_CAMERA_RVA] ^= 1; }
  { std::string saved = slot_methods[M_GET_CULLING_MASK]->name; slot_methods[M_GET_CULLING_MASK]->name = "missing";
    expect_unavailable("map-gate-method-missing:get_cullingMask");
    slot_methods[M_GET_CULLING_MASK]->name = saved; }
  { Type *saved = slot_methods[M_GET_CULLING_MASK]->result; slot_methods[M_GET_CULLING_MASK]->result = classes[C_STRING].type;
    expect_unavailable("map-gate-signature-mismatch:get_cullingMask");
    slot_methods[M_GET_CULLING_MASK]->result = saved; }
  { std::string saved = slot_methods[M_NAME_TO_LAYER]->name; slot_methods[M_NAME_TO_LAYER]->name = "missing";
    expect_unavailable("map-gate-method-missing:NameToLayer");
    slot_methods[M_NAME_TO_LAYER]->name = saved; }
  { Type *saved = slot_methods[M_NAME_TO_LAYER]->params[0]; slot_methods[M_NAME_TO_LAYER]->params[0] = classes[C_INT32].type;
    expect_unavailable("map-gate-signature-mismatch:NameToLayer");
    slot_methods[M_NAME_TO_LAYER]->params[0] = saved; }

  // 10) Module canonicalisation extent-diff guard (default resolver path rejected before thread checks).
  { auto c = config; c.binding.seg_query = nullptr; unsigned before = thread_checks;
    expect_fatal(PGR_MD_ERR_CODE_RANGE, &c); assert(thread_checks == before); }
  { auto c = config; c.binding.module_base = reinterpret_cast<void *>(UINTPTR_MAX - 3); c.binding.module_size = 8;
    unsigned before = thread_checks; expect_fatal(PGR_MD_ERR_CODE_RANGE, &c); assert(thread_checks == before); }

  // 11) Reentrant call returns BUSY with cleared output.
  reenter = true; ok_continuous(); assert(reentries == 1); ok_continuous();

  assert(names_allocated == names_freed);
  assert(munmap(module_base, module_size) == 0);
  std::puts("PASS camera metadata actual-library (API 1): 11 classes / 3 Vector3 fields / 8 methods, "
            "passive + sampleCamera + continuous modes, assembly bound + scan budget, GC pointer ABI, "
            "debug_get_method_info + get_MainCamera RVA code pins, attachment/owner/BUSY, balanced frees");
  return 0;
}
