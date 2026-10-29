// Actual production dlopen/query55 (API 2) fixture. Metadata exports are fake; the compiled
// quick-ball descriptor resolver and the shared feature47 validators are REAL. A sparse
// page-aligned virtual module holds the real method/caller/GC-handle prologue bytes at their
// pinned RVAs. Injected segment permissions are synthetic -- NOT a /proc/Houdini test and NO
// live-game claim.
#include "quickball_metadata.h"
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
using namespace pgr_quickball_schema;

struct Class;
struct Type { std::string name; Class *klass = nullptr; bool byref = false; };
struct Class { Type *type = nullptr; bool value = false, enumeration = false; Type *base = nullptr; int size = 0, align = 0; };
struct Field { Class *lookup, *owner; std::string name; Type *type; uint32_t flags; uint64_t offset; int32_t static_val = 0; };
struct Method {
  void *code;  // MethodInfo's first pointer is read by debug_get_method_info.
  Class *lookup, *owner; std::string name; Type *result; Type *params[2]; uint32_t count, flags;
  bool generic, inflated;
};
static_assert(offsetof(Method, code) == 0, "fixture MethodInfo code pointer first");

constexpr uint64_t kGcNewRva = kCallerCheckRva + 0x1000ULL;
constexpr uint64_t kGcNewBodyRva = kCallerCheckRva + 0x1100ULL;
constexpr uint64_t kGcGetRva = kCallerCheckRva + 0x1600ULL;
constexpr uint64_t kGcGetBodyRva = kCallerCheckRva + 0x1700ULL;
constexpr uint64_t kGcFreeRva = kCallerCheckRva + 0x1800ULL;
constexpr uint64_t kGcFreeBodyRva = kCallerCheckRva + 0x1900ULL;
constexpr uint32_t kAllOptionalExports =
    PGR_QB_EXP_TYPE_IS_BYREF | PGR_QB_EXP_CLASS_GET_FIELDS | PGR_QB_EXP_STRING_LENGTH |
    PGR_QB_EXP_STRING_CHARS | PGR_QB_EXP_OBJECT_NEW | PGR_QB_EXP_FIELD_SET_VALUE |
    PGR_QB_EXP_OBJECT_GET_VIRTUAL | PGR_QB_EXP_STRING_NEW | PGR_QB_EXP_TYPE_GET_OBJECT;

std::vector<std::unique_ptr<Type>> types;
std::vector<std::unique_ptr<Class>> extra_classes;
std::vector<std::unique_ptr<Field>> fields;
std::vector<std::unique_ptr<Method>> methods;
Class classes[kClassCount];
Class *enum_class = nullptr, *dup_class = nullptr;
Field *outcome_fields[PGR_QB_OUTCOME_MAX]{};
Field *overflow_field = nullptr, *alignment_field = nullptr, *scheduler_current_field = nullptr;
Method *get_bool_int = nullptr, *get_integer_int = nullptr;
Type *vec3_byref_type = nullptr;
int domain_token, image_a, image_b, thread_token;
void *assemblies[] = {&image_a, &image_b};
bool attached = true, reenter = false, domain_ready = true;
size_t assembly_count = 2;
int dup_index = -1, hide_index = -1;  // optional-class ambiguity / absence injection
unsigned thread_checks = 0, detach_at = 0, names_allocated = 0, names_freed = 0, reentries = 0;
unsigned char *module_base = nullptr;
uint64_t module_size = 0;
const PgrQuickballMetaApi *api = nullptr;
PgrQuickballMetaConfig config{};
PgrQuickballExtra extra_cfg{};

Type *new_type(const std::string &name, Class *klass, bool byref = false) {
  types.emplace_back(new Type{name, klass, byref});
  return types.back().get();
}
Class *new_class(const std::string &name, bool value = false, int size = 0, int align = 0) {
  extra_classes.emplace_back(new Class);
  Class *k = extra_classes.back().get();
  k->type = new_type(name, k); k->value = value; k->size = size; k->align = align; return k;
}
Type *type(const char *name) {
  assert(name);
  for (const auto &t : types) if (t->name == name && !t->byref) return t.get();
  return new_class(name)->type;
}

bool zero(const PgrQuickballMetaOutput &o);
void *domain_get() {
  if (reenter) {  // a metadata callback reentering resolve() must get BUSY with cleared output.
    reenter = false;
    PgrQuickballMetaOutput out;
    std::memset(&out, 0xa5, sizeof(out));
    assert(api->resolve(&config, &out) == -27 && zero(out));
    ++reentries;
  }
  return domain_ready ? &domain_token : nullptr;
}
void *domain_assemblies(void *, size_t *count) { *count = assembly_count; return assemblies; }
void *assembly_image(void *a) { return a; }
void *class_from_name(void *image, const char *ns, const char *name) {
  for (size_t i = 0; i < kClassCount; i++)
    if (!std::strcmp(ns, kClasses[i].name_space) && !std::strcmp(name, kClasses[i].name)) {
      if (static_cast<int>(i) == hide_index) return nullptr;
      if (image == &image_a) return &classes[i];
      if (image == &image_b && static_cast<int>(i) == dup_index) return dup_class;
      return nullptr;
    }
  return nullptr;
}
void *class_type(void *k) { assert(k); return static_cast<Class *>(k)->type; }
char *type_name(void *raw) {
  assert(raw);
  const auto &n = static_cast<Type *>(raw)->name;
  char *copy = static_cast<char *>(std::malloc(n.size() + 1));
  assert(copy); std::memcpy(copy, n.c_str(), n.size() + 1); ++names_allocated; return copy;
}
void mem_free(void *p) { assert(p); ++names_freed; std::free(p); }
void *field_by_name(void *owner, const char *name) {
  for (auto &f : fields) if (f->lookup == owner && f->name == name) return f.get();
  return nullptr;
}
void *field_parent(void *raw) { return static_cast<Field *>(raw)->owner; }
int field_flags(void *raw) { return static_cast<Field *>(raw)->flags; }
size_t field_offset(void *raw) { return static_cast<Field *>(raw)->offset; }
void *field_type(void *raw) { return static_cast<Field *>(raw)->type; }
void field_static_get_value(void *raw, void *out) {
  assert(raw && out);
  *static_cast<int32_t *>(out) = static_cast<Field *>(raw)->static_val;
}
bool type_is_byref(void *raw) { assert(raw); return static_cast<Type *>(raw)->byref; }
void *from_type(void *raw) { return raw ? static_cast<Type *>(raw)->klass : nullptr; }
bool value_type(void *raw) { return static_cast<Class *>(raw)->value; }
bool is_enum(void *raw) { return static_cast<Class *>(raw)->enumeration; }
void *enum_base(void *raw) { return static_cast<Class *>(raw)->base; }
int value_size(void *raw, int *align) { auto *k = static_cast<Class *>(raw); *align = k->align; return k->size; }
void *method_by_name(void *owner, const char *name, int count) {
  for (auto &m : methods) if (m->lookup == owner && m->name == name && int(m->count) == count) return m.get();
  return nullptr;
}
void *method_class(void *raw) { return static_cast<Method *>(raw)->owner; }
char *method_name(void *raw) { return const_cast<char *>(static_cast<Method *>(raw)->name.c_str()); }
uint32_t method_flags(void *raw, uint32_t *impl) { *impl = 0; return static_cast<Method *>(raw)->flags; }
uint32_t param_count(void *raw) { return static_cast<Method *>(raw)->count; }
void *param(void *raw, uint32_t index) { assert(index < 2); return static_cast<Method *>(raw)->params[index]; }
void *return_type(void *raw) { return static_cast<Method *>(raw)->result; }
bool generic(void *raw) { return static_cast<Method *>(raw)->generic; }
bool inflated(void *raw) { return static_cast<Method *>(raw)->inflated; }
bool debug_info(void *raw, void *out) { static_cast<void **>(out)[0] = static_cast<Method *>(raw)->code; return true; }
bool assignable(void *, void *) { return false; }
void *enumerate_methods(void *owner, void **iter) {
  size_t idx = reinterpret_cast<uintptr_t>(*iter);
  while (idx < methods.size()) {
    Method *m = methods[idx++].get();
    if (m->lookup == owner) {
      *iter = reinterpret_cast<void *>(idx);
      return m;
    }
  }
  *iter = reinterpret_cast<void *>(idx);
  return nullptr;
}
void *enumerate_fields(void *owner, void **iter) {
  size_t idx = reinterpret_cast<uintptr_t>(*iter);
  while (idx < fields.size()) {
    Field *f = fields[idx++].get();
    if (f->lookup == owner) {
      *iter = reinterpret_cast<void *>(idx);
      return f;
    }
  }
  *iter = reinterpret_cast<void *>(idx);
  return nullptr;
}
void *enumerate_empty(void *, void **) { return nullptr; }
void *declaring(void *) { return nullptr; }
void *thread_current() { ++thread_checks; return attached && (!detach_at || thread_checks < detach_at) ? &thread_token : nullptr; }
int segment(void *, const void *address, uint64_t length, PgrMetaSegment *out) {
  *out = {};
  uintptr_t a = reinterpret_cast<uintptr_t>(address), base = reinterpret_cast<uintptr_t>(module_base);
  if (a < base || a - base > module_size || !length || length > module_size - (a - base)) return 0;
  out->found = out->readable = out->host_exec = out->writable = 1;
  out->seg_base = module_base; out->seg_size = module_size; return 0;
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
  e.class_get_methods = reinterpret_cast<void *>(enumerate_methods);
  e.class_get_nested_types = reinterpret_cast<void *>(enumerate_empty);
  e.class_get_declaring_type = reinterpret_cast<void *>(declaring);
  return e;
}

int decode_hex(const char *hex, unsigned char *out, int cap) {
  int n = static_cast<int>(std::strlen(hex));
  assert(n > 0 && !(n & 1) && n / 2 <= cap);
  for (int i = 0; i < n; i++) {
    char ch = hex[i];
    int v = (ch >= '0' && ch <= '9') ? ch - '0' : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10 : -1;
    assert(v >= 0);
    if (i & 1) out[i / 2] |= v; else out[i / 2] = v << 4;
  }
  return n / 2;
}

void write_u32(uint64_t rva, uint32_t word) {
  std::memcpy(module_base + rva, &word, sizeof(word));
}

uint32_t encode_b(uint64_t from_rva, uint64_t to_rva) {
  int64_t diff = (static_cast<int64_t>(to_rva) - static_cast<int64_t>(from_rva)) / 4;
  assert(diff >= -(1 << 25) && diff < (1 << 25));
  return 0x14000000u | (static_cast<uint32_t>(diff) & 0x03ffffffu);
}

void write_gc_handle_code() {
  const uint32_t create_head[5] = {0xaa0003e8u, 0x52800049u, 0x7200003fu, 0x1a890520u, 0xaa0803e1u};
  for (int i = 0; i < 5; i++) write_u32(kGcNewRva + i * 4, create_head[i]);
  write_u32(kGcNewRva + 20, encode_b(kGcNewRva + 20, kGcNewBodyRva));
  write_u32(kGcNewBodyRva + 0x384, 0x9a953120u);

  write_u32(kGcGetRva, encode_b(kGcGetRva, kGcGetBodyRva));
  write_u32(kGcGetBodyRva, 0xf81b0ffeu);
  write_u32(kGcGetBodyRva + 20, 0x9273c817u);
  write_u32(kGcGetBodyRva + 24, 0x394082e8u);

  write_u32(kGcFreeRva, encode_b(kGcFreeRva, kGcFreeBodyRva));
  write_u32(kGcFreeBodyRva, 0xf81b0ffeu);
  write_u32(kGcFreeBodyRva + 24, 0x9273c816u);
  write_u32(kGcFreeBodyRva + 28, 0x394082c8u);
}

void build_fixture() {
  // Independent facts from quick-ball.js prepare(), not generated success expectations alone.
  assert(kClassCount == 42 && kMandatoryClassCount == 8 && kFieldCount == 65 && kMethodCount == 51 && kGroupCount == 14);
  assert(!std::strcmp(kClasses[C_BALL].canonical, "Niantic.Holoholo.Encounter.Pokeball"));
  assert(!std::strcmp(kClasses[C_RESPONSE].canonical, "Holoholo.Rpc.CatchPokemonOutProto"));
  assert(kFields[0].owner == C_BALL && kFields[0].offset == 0xb0);
  assert(kFields[2].owner == C_RESPONSE && !std::strcmp(kFields[2].name, "Status") && kFields[2].kind == PGR_MD_FIELD_ENUM_I32);
  assert(kMethods[1].owner == C_BALL && !std::strcmp(kMethods[1].name, "btxu") && kMethods[1].rva == 0xad58170ULL);
  assert(kMethods[4].is_static == 1 && !std::strcmp(kMethods[4].name, "op_Implicit"));

  // Size the virtual module to cover the highest pinned RVA + prologue, caller check, and GC handle blocks.
  uint64_t top = kGcFreeBodyRva + 64;
  for (uint32_t i = 0; i < kMethodCount; i++)
    if (kMethods[i].prologue) {
      uint64_t end = kMethods[i].rva + std::strlen(kMethods[i].prologue) / 2;
      if (end > top) top = end;
    }
  long page = sysconf(_SC_PAGESIZE);
  assert(page > 0);
  module_size = ((top + page - 1) / page) * page;
  void *mapped = mmap(nullptr, module_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(mapped != MAP_FAILED);
  module_base = static_cast<unsigned char *>(mapped);

  for (size_t i = 0; i < kClassCount; i++) classes[i].type = new_type(kClasses[i].canonical, &classes[i]);
  for (size_t i = 0; i < kValueSizeCount; i++) {
    Class &c = classes[kValueSizes[i].klass];
    c.value = true; c.size = kValueSizes[i].size; c.align = kValueSizes[i].align;
  }
  Type *int32 = classes[C_INT32].type;
  enum_class = new_class(kStatusEnumName, true, 4, 4);
  enum_class->enumeration = true; enum_class->base = int32;
  dup_class = new_class("Niantic.Holoholo.Map.MapViewHandler");  // same canonical as MAPVIEW, different handle
  vec3_byref_type = new_type("UnityEngine.Vector3&", &classes[C_VEC3], true);

  for (size_t i = 0; i < kFieldCount; i++) {
    const auto &s = kFields[i];
    Type *t = (s.owner == C_RESPONSE && s.name == std::string("Status")) ? enum_class->type
              : s.type_name ? type(s.type_name) : int32;
    fields.emplace_back(new Field{&classes[s.owner], &classes[s.owner], s.name, t, s.required_mask, s.offset, 0});
  }
  // Status enum literals (CatchError=0, CatchSuccess=1, CatchEscape=2, CatchFlee=3, CatchMissed=4, CatchItemReplacement=5).
  const int32_t literal_vals[PGR_QB_OUTCOME_MAX] = {0, 1, 2, 3, 4, 5};
  for (size_t i = 0; i < PGR_QB_OUTCOME_MAX; i++) {
    fields.emplace_back(new Field{enum_class, enum_class, kOutcomeLiterals[i], enum_class->type, 0x8050u, 0, literal_vals[i]});
    outcome_fields[i] = fields.back().get();
  }
  // IV enum literals.
  fields.emplace_back(new Field{&classes[C_HWRAP], &classes[C_HWRAP], "Overflow", classes[C_HWRAP].type, 0x8050u, 0, 1});
  overflow_field = fields.back().get();
  fields.emplace_back(new Field{&classes[C_TEXTANCHOR], &classes[C_TEXTANCHOR], "MiddleCenter", classes[C_TEXTANCHOR].type, 0x8050u, 0, 4});
  alignment_field = fields.back().get();
  // SchedulerStateMachine current-state field scanned by class_get_fields at offset 0x18.
  fields.emplace_back(new Field{&classes[C_SCHEDULER], &classes[C_SCHEDULER], "currentState_",
                                type("Niantic.Holoholo.SchedulerState"), 0u, 0x18u, 0});
  scheduler_current_field = fields.back().get();

  for (size_t i = 0; i < kMethodCount; i++) {
    const auto &s = kMethods[i];
    const char *ret = (s.owner == C_MAPPOKEMON && s.name == std::string("TryCapture"))
                          ? "Niantic.Promises.IPromise`1<Holoholo.Rpc.CatchPokemonOutProto>"
                          : s.return_type;
    Type *p0 = s.param0 ? type(s.param0) : nullptr;
    Type *p1 = (s.owner == C_MAPVIEW && s.name == std::string("GetGroundPosition"))
                   ? vec3_byref_type
                   : (s.param1 ? type(s.param1) : nullptr);
    methods.emplace_back(new Method{nullptr, &classes[s.owner], &classes[s.owner], s.name,
                                    ret ? type(ret) : nullptr,
                                    {p0, p1},
                                    s.param_count, s.is_static ? 0x10u : 0u, false, false});
    if (s.prologue) {
      methods.back()->code = module_base + s.rva;
      decode_hex(s.prologue, module_base + s.rva, PGR_MD_PROLOGUE_MAX);
    }
  }
  // Animator overload candidates for GetBool / GetInteger (String distractors + Int32 matches).
  methods.emplace_back(new Method{nullptr, &classes[C_ANIMATOR], &classes[C_ANIMATOR], "GetBool",
                                  classes[C_BOOL].type, {classes[C_STRING].type, nullptr}, 1, 0, false, false});
  methods.emplace_back(new Method{nullptr, &classes[C_ANIMATOR], &classes[C_ANIMATOR], "GetBool",
                                  classes[C_BOOL].type, {classes[C_INT32].type, nullptr}, 1, 0, false, false});
  get_bool_int = methods.back().get();
  methods.emplace_back(new Method{nullptr, &classes[C_ANIMATOR], &classes[C_ANIMATOR], "GetInteger",
                                  classes[C_INT32].type, {classes[C_STRING].type, nullptr}, 1, 0, false, false});
  methods.emplace_back(new Method{nullptr, &classes[C_ANIMATOR], &classes[C_ANIMATOR], "GetInteger",
                                  classes[C_INT32].type, {classes[C_INT32].type, nullptr}, 1, 0, false, false});
  get_integer_int = methods.back().get();

  unsigned char caller[PGR_MD_PROLOGUE_MAX];
  int clen = decode_hex(kCallerCheckPrologue, caller, sizeof(caller));
  std::memcpy(module_base + kCallerCheckRva, caller, clen);
  write_gc_handle_code();
}

bool zero(const PgrQuickballMetaOutput &o) { PgrQuickballMetaOutput e{}; return std::memcmp(&o, &e, sizeof(o)) == 0; }

PgrQuickballMetaOutput success() {
  PgrQuickballMetaOutput out{};
  assert(api->resolve(&config, &out) == 0 && out.version == 2 && out.size == sizeof(out));
  for (size_t i = 0; i < kClassCount; i++) { assert(out.classes[i] == &classes[i]); assert(out.class_conflict[i] == 0); }
  assert(out.enum_class == enum_class);
  for (size_t i = 0; i < kFieldCount; i++) assert(out.fields[i] == fields[i].get());
  for (size_t i = 0; i < kMethodCount; i++) {
    assert(out.methods[i] == methods[i].get());
    assert(out.method_code[i] == methods[i]->code);
  }
  assert(out.trace_params_bool == get_bool_int);
  assert(out.trace_params_int == get_integer_int);
  assert(out.alwayshit_current == scheduler_current_field);
  assert(out.success == 1);
  for (size_t i = 0; i < PGR_QB_OUTCOME_MAX; i++) assert(out.outcomes_values[i] == int32_t(i));
  assert(out.iv_overflow_value == 1 && out.iv_alignment_value == 4);
  for (uint32_t g = 0; g < kGroupCount; g++) { assert(out.groups[g].available == 1); assert(out.groups[g].reason[0] == '\0'); }
  assert(names_allocated == names_freed);
  return out;
}
void failure(int expected, const PgrQuickballMetaConfig *c = nullptr) {
  PgrQuickballMetaOutput out;
  std::memset(&out, 0xa5, sizeof(out));
  assert(api->resolve(c ? c : &config, &out) == expected);
  assert(zero(out) && names_allocated == names_freed);
}
// Resolve succeeds but a named optional group is unavailable with a reason substring.
PgrQuickballMetaOutput group_down(uint32_t g, const char *needle) {
  PgrQuickballMetaOutput out{};
  assert(api->resolve(&config, &out) == 0);
  assert(out.groups[g].available == 0 && std::strstr(out.groups[g].reason, needle));
  assert(names_allocated == names_freed);
  return out;
}
int field_at(uint32_t group, const char *name) {
  for (uint32_t i = 0; i < kFieldCount; i++) if (kFields[i].group == group && kFields[i].name == std::string(name)) return int(i);
  return -1;
}
int method_at(const char *name) {
  for (uint32_t i = 0; i < kMethodCount; i++) if (kMethods[i].name == std::string(name)) return int(i);
  return -1;
}
}  // namespace

int main(int argc, char **argv) {
  assert(argc == 2);
  void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  assert(library);
  auto get = reinterpret_cast<const void *(*)(uint32_t)>(dlsym(library, "pgrazor_native_api"));
  assert(get);
  auto *core = static_cast<const pgrazor_native_api_v2 *>(get(2));
  assert(core && core->query_feature && !core->query_feature(55, 1) && !core->query_feature(55, 3));
  api = static_cast<const PgrQuickballMetaApi *>(core->query_feature(55, 2));
  assert(api && api->version == 2 && api->size == 24 && api->config_size == 96 &&
         api->output_size == sizeof(PgrQuickballMetaOutput));

  build_fixture();
  PgrMetaExports e = exports();
  extra_cfg.version = 2; extra_cfg.size = sizeof(PgrQuickballExtra);
  extra_cfg.field_static_get_value = reinterpret_cast<void *>(field_static_get_value);
  extra_cfg.type_is_byref = reinterpret_cast<void *>(type_is_byref);
  extra_cfg.class_get_fields = reinterpret_cast<void *>(enumerate_fields);
  extra_cfg.gchandle_new = module_base + kGcNewRva;
  extra_cfg.gchandle_get_target = module_base + kGcGetRva;
  extra_cfg.gchandle_free = module_base + kGcFreeRva;
  extra_cfg.present = kAllOptionalExports;
  extra_cfg.reserved = 0;

  config.version = 2; config.size = sizeof(config);
  config.binding.version = 1; config.binding.size = sizeof(PgrMetaBindConfig); config.binding.exports = &e;
  config.binding.module_base = module_base; config.binding.module_size = module_size; config.binding.seg_query = segment;
  config.thread_current = reinterpret_cast<void *>(thread_current); config.owner_tid = gettid();
  config.extra = &extra_cfg;

  success(); success();  // immutable binding + repeated accessor pointer identity

  // Envelope / attachment / extra block validation.
  { auto c = config; c.version = 1; failure(-2, &c); c = config; c.size--; failure(-2, &c); c = config; c.thread_current = nullptr; failure(-2, &c); }
  { auto c = config; c.extra = nullptr; failure(PGR_QB_ERR_EXTRA_MISSING, &c); }
  { auto x = extra_cfg; x.version = 1; auto c = config; c.extra = &x; failure(PGR_QB_ERR_EXTRA_MISSING, &c); }
  { auto x = extra_cfg; x.field_static_get_value = nullptr; auto c = config; c.extra = &x; failure(PGR_QB_ERR_EXTRA_MISSING, &c); }
  { auto x = extra_cfg; x.gchandle_new = nullptr; auto c = config; c.extra = &x; failure(PGR_QB_ERR_EXTRA_MISSING, &c); }
  { PgrQuickballMetaOutput out; std::memset(&out, 0xa5, sizeof(out)); assert(api->resolve(nullptr, &out) == -2 && zero(out)); assert(api->resolve(&config, nullptr) == -1); }
  attached = false; failure(-30); attached = true;
  { auto c = config; c.owner_tid = 0; failure(-30, &c); c.owner_tid = config.owner_tid + 1; failure(-30, &c); }
  { std::thread foreign([] { failure(-30); }); foreign.join(); }
  thread_checks = 0; detach_at = 6; failure(-30); detach_at = 0; success();

  // Only class discovery readiness is retriable, never missing core members.
  { hide_index=C_BALL; failure(PGR_QB_NOT_READY); hide_index=-1; success(); }
  { assembly_count=0; failure(PGR_QB_NOT_READY); assembly_count=2; success(); }
  { domain_ready=false; failure(PGR_QB_NOT_READY); domain_ready=true; success(); }
  { auto name=fields[0]->name; fields[0]->name="hidden-core-field";
    failure(PGR_MD_ERR_NOT_FOUND); fields[0]->name=name; success(); }
  { int m=method_at("btxu"); auto name=methods[m]->name; methods[m]->name="hidden-core-method";
    failure(PGR_MD_ERR_NOT_FOUND); methods[m]->name=name; success(); }

  // Mandatory (CORE) structural + value + enum + GC pointer ABI faults are fatal.
  { auto s = fields[0]->offset; fields[0]->offset++; failure(PGR_MD_ERR_OFFSET); fields[0]->offset = s; }
  { auto s = fields[2]->flags; fields[2]->flags = 0x10; failure(PGR_MD_ERR_FLAGS); fields[2]->flags = s; }
  { auto s = outcome_fields[1]->flags; outcome_fields[1]->flags = 0x10; failure(PGR_QB_ERR_SUCCESS_LIT); outcome_fields[1]->flags = s; }
  { auto s = classes[C_SINGLE].size; classes[C_SINGLE].size = 8; failure(PGR_MD_ERR_SIZE); classes[C_SINGLE].size = s; }
  { auto s = classes[C_BOOL].value; classes[C_BOOL].value = false; failure(PGR_MD_ERR_KIND); classes[C_BOOL].value = s; }
  { module_base[kGcNewBodyRva + 0x384] ^= 1; failure(PGR_QB_ERR_GC_POINTER); module_base[kGcNewBodyRva + 0x384] ^= 1; }
  { module_base[kGcFreeBodyRva + 24] ^= 1; failure(PGR_QB_ERR_GC_POINTER); module_base[kGcFreeBodyRva + 24] ^= 1; }
  { int m = method_at("op_Implicit"); methods[m]->flags = 0; failure(PGR_MD_ERR_STATIC); methods[m]->flags = 0x10; }
  { int m = method_at("btxu"); methods[m]->generic = true; failure(PGR_MD_ERR_GENERIC); methods[m]->generic = false; }
  { int m = method_at("btxu"); methods[m]->inflated = true; failure(PGR_MD_ERR_INFLATED); methods[m]->inflated = false; }
  { int m = method_at("set_speed"); auto s = methods[m]->params[0]; methods[m]->params[0] = classes[C_INT32].type; failure(PGR_MD_ERR_TYPE); methods[m]->params[0] = s; }
  { int m = method_at("btxu"); unsigned char *p = module_base + kMethods[m].rva; *p ^= 1; failure(PGR_MD_ERR_PROLOGUE); *p ^= 1; }
  { module_base[kCallerCheckRva] ^= 1; failure(PGR_MD_ERR_PROLOGUE); module_base[kCallerCheckRva] ^= 1; }
  // Duplicate a MANDATORY class across images -> fatal DUPLICATE.
  { Class *keep = dup_class; dup_class = new_class("Niantic.Holoholo.Encounter.Pokeball"); dup_index = C_BALL; failure(PGR_MD_ERR_DUPLICATE); dup_index = -1; dup_class = keep; }

  // GROUP_OUTCOMES: duplicate literal value or missing literal disables only outcomes, never core.
  { int32_t s = outcome_fields[2]->static_val; outcome_fields[2]->static_val = outcome_fields[0]->static_val;
    group_down(GROUP_OUTCOMES, "outcome-literal-duplicate:CatchEscape"); outcome_fields[2]->static_val = s; success(); }
  { uint32_t s = outcome_fields[3]->flags; outcome_fields[3]->flags = 0;
    group_down(GROUP_OUTCOMES, "outcome-literal:CatchFlee"); outcome_fields[3]->flags = s; success(); }

  // Optional bare-class value-size checks disable only the affected optional group.
  { int s = classes[C_INFO].size; classes[C_INFO].size = 40;
    group_down(GROUP_TRACE, "trace-abi:UnityEngine.AnimatorStateInfo"); classes[C_INFO].size = s; success(); }
  { int s = classes[C_THROW].size; classes[C_THROW].size = 16;
    auto out = group_down(GROUP_THROWS, "throw-value-abi");
    assert(out.groups[GROUP_ALWAYSHIT].available == 0 && out.groups[GROUP_ARTHROWS].available == 0);
    classes[C_THROW].size = s; success(); }
  { int s = classes[C_LATLNG].align; classes[C_LATLNG].align = 4;
    group_down(GROUP_MAPTAP, "map-value-abi"); classes[C_LATLNG].align = s; success(); }
  { int s = classes[C_COLOR].size; classes[C_COLOR].size = 12;
    group_down(GROUP_IVOUTLINE, "iv-outline-value-abi"); classes[C_COLOR].size = s; success(); }

  // GROUP_TRACE_PARAMS overload disambiguation (missing / duplicate / mismatch).
  { auto s = get_bool_int->params[0]; get_bool_int->params[0] = classes[C_STRING].type;
    auto out = group_down(GROUP_TRACE_PARAMS, "overload-missing:GetBool");
    assert(out.groups[GROUP_TRACE].available == 1 && out.trace_params_bool == nullptr);
    get_bool_int->params[0] = s; success(); }
  { methods.emplace_back(new Method{nullptr, &classes[C_ANIMATOR], &classes[C_ANIMATOR], "GetInteger",
                                    classes[C_INT32].type, {classes[C_INT32].type, nullptr}, 1, 0, false, false});
    group_down(GROUP_TRACE_PARAMS, "overload-duplicate:GetInteger");
    methods.pop_back(); success(); }

  // GROUP_ALWAYSHIT current-state field scan at offset 0x18.
  { uint64_t s = scheduler_current_field->offset; scheduler_current_field->offset = 0x20;
    auto out = group_down(GROUP_ALWAYSHIT, "always-hit-current-state");
    assert(out.alwayshit_current == nullptr);
    scheduler_current_field->offset = s; success(); }

  // GROUP_MAPTAP GetGroundPosition param1 byref check.
  { vec3_byref_type->byref = false;
    group_down(GROUP_MAPTAP, "map-ground-byref");
    vec3_byref_type->byref = true; success(); }

  // GROUP_IV static enum literal check.
  { uint32_t s = overflow_field->flags; overflow_field->flags = 0;
    auto out = group_down(GROUP_IV, "iv-enum:Overflow");
    assert(out.groups[GROUP_IVTEXT].available == 0 && out.groups[GROUP_IVOWNED].available == 0);
    overflow_field->flags = s; success(); }

  // Optional export presence bitmask gates.
  { extra_cfg.present &= ~PGR_QB_EXP_STRING_LENGTH;
    auto out = group_down(GROUP_TRACE_NAME, "trace-export:string");
    assert(out.groups[GROUP_IVTEXT].available == 0 && std::strstr(out.groups[GROUP_IVTEXT].reason, "iv-text-api"));
    assert(out.groups[GROUP_IVOWNED].available == 0);
    extra_cfg.present = kAllOptionalExports; success(); }
  { extra_cfg.present &= ~PGR_QB_EXP_OBJECT_NEW;
    group_down(GROUP_ARTHROWS, "ar-throw-api-unavailable");
    extra_cfg.present = kAllOptionalExports; success(); }
  { extra_cfg.present &= ~PGR_QB_EXP_TYPE_GET_OBJECT;
    group_down(GROUP_IVOUTLINE, "iv-outline-type-api");
    extra_cfg.present = kAllOptionalExports; success(); }
  { extra_cfg.present &= ~PGR_QB_EXP_OBJECT_GET_VIRTUAL;
    auto out = group_down(GROUP_IV, "iv-export-unavailable");
    assert(out.groups[GROUP_IVTEXT].available == 0 && out.groups[GROUP_IVOWNED].available == 0);
    extra_cfg.present = kAllOptionalExports; success(); }

  // Optional groups fail CLOSED and INDEPENDENTLY; resolve still returns 0.
  { int f = field_at(GROUP_THROWS, "ReticleSize"); auto s = fields[f]->offset; fields[f]->offset++; group_down(GROUP_THROWS, "field-mismatch:ReticleSize"); fields[f]->offset = s; success(); }
  // Dependent groups propagate: breaking THROWS takes down alwaysHit + arThrows via dep.
  { int f = field_at(GROUP_THROWS, "Missed"); auto s = fields[f]->type; fields[f]->type = classes[C_INT32].type;
    auto out = group_down(GROUP_THROWS, "field-mismatch:Missed");
    assert(out.groups[GROUP_ALWAYSHIT].available == 0 && std::strstr(out.groups[GROUP_ALWAYSHIT].reason, "always-hit-api"));
    assert(out.groups[GROUP_ARTHROWS].available == 0 && std::strstr(out.groups[GROUP_ARTHROWS].reason, "ar-throw-api-unavailable"));
    fields[f]->type = s; success(); }
  // Breaking an IV method takes down ivText + ivOwned via the dep chain.
  { int m = method_at("SetPokemonUI"); auto s = methods[m]->result; methods[m]->result = classes[C_INT32].type;
    auto out = group_down(GROUP_IV, "method-mismatch:SetPokemonUI");
    assert(out.groups[GROUP_IVTEXT].available == 0 && std::strstr(out.groups[GROUP_IVTEXT].reason, "iv-text-api"));
    assert(out.groups[GROUP_IVOWNED].available == 0 && std::strstr(out.groups[GROUP_IVOWNED].reason, "iv-text-unavailable"));
    methods[m]->result = s; success(); }
  // A MISSING optional class disables only its group, with trace-class-missing + no conflict flag.
  { hide_index = C_MAPVIEW; auto out = group_down(GROUP_MAPTAP, "trace-class-missing:Niantic.Holoholo.Map.MapViewHandler");
    assert(out.classes[C_MAPVIEW] == nullptr && out.class_conflict[C_MAPVIEW] == 0); hide_index = -1; success(); }
  // An AMBIGUOUS optional class sets conflict + disables its group with trace-class-mismatch, never fatal.
  { dup_index = C_MAPVIEW; auto out = group_down(GROUP_MAPTAP, "trace-class-mismatch:Niantic.Holoholo.Map.MapViewHandler");
    assert(out.classes[C_MAPVIEW] == nullptr && out.class_conflict[C_MAPVIEW] == 1); dup_index = -1; success(); }

  // Module canonicalisation extent-diff guard (default resolver path, no fixture seg_query).
  { auto c = config; c.binding.seg_query = nullptr; unsigned before = thread_checks; failure(PGR_MD_ERR_CODE_RANGE, &c); assert(thread_checks == before); }
  { auto c = config; c.binding.module_base = reinterpret_cast<void *>(UINTPTR_MAX - 3); c.binding.module_size = 8;
    unsigned before = thread_checks; failure(PGR_MD_ERR_CODE_RANGE, &c); assert(thread_checks == before); }

  // Reentrant/concurrent call returns BUSY without touching the result (output cleared).
  reenter = true; success(); assert(reentries == 1); success();

  assert(names_allocated == names_freed);
  assert(munmap(module_base, module_size) == 0);
  std::puts("PASS quick-ball metadata actual-library (API 2): 42 classes / 65 fields / 51 methods + caller pin, "
            "enum literals, 9 value-type sizes, GC pointer ABI, overloads/field-scan/byref/optional-export gates, "
            "independent optional groups + dep propagation, attachment/owner/BUSY, balanced frees");
  return 0;
}
