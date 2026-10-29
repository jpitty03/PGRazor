// Actual production dlopen/query56 fixture. Metadata exports are fake; the compiled quick-close
// required-core descriptor resolver and the shared feature-47 validators are REAL. A sparse virtual
// module reservation holds the real required prologue bytes at their exact RVAs. Injected segment
// permissions are synthetic: this is NOT a /proc/Houdini test and proves nothing about live resolution.
#include "shared_metadata.h"
#include "managed_handle_abi.h"
#include "pgrazor_native_api.h"
#include <cassert>
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
using namespace pgr_shared_meta;

struct Class;
struct Type { std::string name; Class *klass = nullptr; };
struct Class { Type *type = nullptr; bool value = false, enumeration = false; Type *base = nullptr; int size = 0, align = 0; };
struct Field { Class *lookup, *owner; std::string name; Type *type; uint32_t flags; uint64_t offset; };
struct Method {
  void *code; // MethodInfo's first pointer; debug_get_method_info returns it.
  Class *lookup, *owner; std::string name; Type *result, *param0, *param1; uint32_t count, flags = 0;
  bool generic = false, inflated = false;
};
static_assert(offsetof(Method, code) == 0, "fixture MethodInfo code pointer");

std::vector<std::unique_ptr<Type>> types;
std::vector<std::unique_ptr<Class>> extra_classes;
std::vector<std::unique_ptr<Field>> fields;
std::vector<std::unique_ptr<Method>> methods;
Class classes[kClassCount];
Class *enum_class, *uint64_class, *ref_class, *bool_value_class, *duplicate_class;
Type *int32_type, *void_type;
int domain_token, image_a, image_b, thread_token;
void *assemblies[] = {&image_a, &image_b};
bool attached = true, duplicate = false, reenter = false;
int hide = -1; // index of a class class_from_name should pretend is absent
unsigned thread_checks = 0, detach_at = 0, names_allocated = 0, names_freed = 0, reentries = 0;
unsigned char *module_base = nullptr; uint64_t module_size = 0;
const PgrSharedMetadataApi *api = nullptr;
PgrSharedMetadataConfig config{};
PgrSharedMetaExtraExports extra{};
constexpr uint64_t gc_new_rva=0x1000,gc_get_rva=0x2000,gc_free_rva=0x3000;
unsigned char debug_trampoline[16]{};
void static_value(void *raw,void *out){assert(static_cast<Field*>(raw)->name=="CatchSuccess");*static_cast<int32_t*>(out)=1;}
void write_word(uint64_t offset,uint32_t word){std::memcpy(module_base+offset,&word,4);}
uint32_t branch(uint64_t from,uint64_t to){return 0x14000000u|(uint32_t((int64_t(to)-int64_t(from))/4)&0x3ffffffu);}
void gc_code(){
 const uint32_t head[]={0xaa0003e8,0x52800049,0x7200003f,0x1a890520,0xaa0803e1};
 for(unsigned i=0;i<5;++i)write_word(gc_new_rva+4*i,head[i]);
 write_word(gc_new_rva+20,branch(gc_new_rva+20,0x4000));write_word(0x4384,0x9a953120);
 write_word(gc_get_rva,branch(gc_get_rva,0x5000));write_word(0x5000,0xf81b0ffe);
 write_word(0x5014,0x9273c817);write_word(0x5018,0x394082e8);
 write_word(gc_free_rva,branch(gc_free_rva,0x6000));write_word(0x6000,0xf81b0ffe);
 write_word(0x6018,0x9273c816);write_word(0x601c,0x394082c8);
}

Type *new_type(const std::string &name, Class *klass) { types.emplace_back(new Type{name, klass}); return types.back().get(); }
Class *new_class(const std::string &name, bool value = false, int size = 0, int align = 0) {
  extra_classes.emplace_back(new Class); Class *k = extra_classes.back().get();
  k->type = new_type(name, k); k->value = value; k->size = size; k->align = align; return k;
}
Type *type(const char *name) {
  assert(name); for (const auto &c : types) if (c->name == name) return c.get();
  return new_class(name)->type;
}
Method *new_method(Class *owner, const char *name, Type *result, Type *p0 = nullptr, Type *p1 = nullptr) {
  uint32_t n = uint32_t((p0 ? 1 : 0) + (p1 ? 1 : 0));
  methods.emplace_back(new Method{nullptr, owner, owner, name, result, p0, p1, n, 0, false, false});
  return methods.back().get();
}
bool zero(const PgrSharedMetadataOutput &out) { PgrSharedMetadataOutput e{}; return std::memcmp(&out, &e, sizeof(out)) == 0; }

void *domain_get() {
  if (reenter) { reenter = false; PgrSharedMetadataOutput out; std::memset(&out, 0xa5, sizeof(out));
    assert(api->resolve(&config, &out) == PGR_MD_ERR_BUSY && zero(out)); ++reentries; }
  return &domain_token;
}
void *domain_assemblies(void *, size_t *count) { *count = 2; return assemblies; }
void *assembly_image(void *a) { return a; }
void *class_from_name(void *image, const char *ns, const char *name) {
  for (size_t i = 0; i < kClassCount; i++)
    if (!std::strcmp(ns, kClasses[i].name_space) && !std::strcmp(name, kClasses[i].name)) {
      if (int(i) == hide) return nullptr;
      if (image == &image_a) return &classes[i];
      if (image == &image_b && duplicate && i == 3) return duplicate_class;
    }
  return nullptr;
}
void *class_type(void *k) { assert(k); return static_cast<Class *>(k)->type; }
char *type_name(void *raw) { assert(raw); const auto &n = static_cast<Type *>(raw)->name;
  char *c = static_cast<char *>(std::malloc(n.size() + 1)); assert(c); std::memcpy(c, n.c_str(), n.size() + 1); ++names_allocated; return c; }
void mem_free(void *p) { assert(p); ++names_freed; std::free(p); }
void *field_by_name(void *owner, const char *name) { for (auto &f : fields) if (f->lookup == owner && f->name == name) return f.get(); return nullptr; }
void *field_parent(void *raw) { return static_cast<Field *>(raw)->owner; }
int field_flags(void *raw) { return static_cast<Field *>(raw)->flags; }
size_t field_offset(void *raw) { return static_cast<Field *>(raw)->offset; }
void *field_type(void *raw) { return static_cast<Field *>(raw)->type; }
void *from_type(void *raw) { return raw ? static_cast<Type *>(raw)->klass : nullptr; }
bool value_type(void *raw) { return static_cast<Class *>(raw)->value; }
bool is_enum(void *raw) { return static_cast<Class *>(raw)->enumeration; }
void *enum_base(void *raw) { return static_cast<Class *>(raw)->base; }
int value_size(void *raw, int *align) { auto *k = static_cast<Class *>(raw); *align = k->align; return k->size; }
void *method_by_name(void *owner, const char *name, int count) {
  for (auto &m : methods) if (m->lookup == owner && m->name == name && int(m->count) == count) return m.get(); return nullptr;
}
void *method_class(void *raw) { return static_cast<Method *>(raw)->owner; }
char *method_name(void *raw) { return const_cast<char *>(static_cast<Method *>(raw)->name.c_str()); }
uint32_t method_flags(void *raw, uint32_t *impl) { *impl = 0; return static_cast<Method *>(raw)->flags; }
uint32_t param_count(void *raw) { return static_cast<Method *>(raw)->count; }
void *param(void *raw, uint32_t index) { auto *m = static_cast<Method *>(raw); return index == 0 ? m->param0 : m->param1; }
void *return_type(void *raw) { return static_cast<Method *>(raw)->result; }
bool generic(void *raw) { return static_cast<Method *>(raw)->generic; }
bool inflated(void *raw) { return static_cast<Method *>(raw)->inflated; }
bool debug_info(void *raw, void *out) { static_cast<void **>(out)[0] = static_cast<Method *>(raw)->code; return true; }
bool assignable(void *, void *) { return false; }
void *enumerate(void *, void **) { return nullptr; }
void *declaring(void *) { return nullptr; }
void *thread_current() { ++thread_checks; return attached && (!detach_at || thread_checks < detach_at) ? &thread_token : nullptr; }
int segment(void *, const void *address, uint64_t length, PgrMetaSegment *out) {
  *out = {}; uintptr_t a = reinterpret_cast<uintptr_t>(address), base = reinterpret_cast<uintptr_t>(module_base);
  if (a < base || a - base > module_size || !length || length > module_size - (a - base)) return 0;
  out->found = out->readable = out->host_exec = out->writable = 1; out->seg_base = module_base; out->seg_size = module_size; return 0;
}
PgrMetaExports exports() {
  PgrMetaExports e{};
  e.domain_get = reinterpret_cast<void *>(domain_get); e.domain_get_assemblies = reinterpret_cast<void *>(domain_assemblies);
  e.assembly_get_image = reinterpret_cast<void *>(assembly_image); e.class_from_name = reinterpret_cast<void *>(class_from_name);
  e.class_get_type = reinterpret_cast<void *>(class_type); e.type_get_name = reinterpret_cast<void *>(type_name); e.mem_free = reinterpret_cast<void *>(mem_free);
  e.class_get_field_from_name = reinterpret_cast<void *>(field_by_name); e.field_get_parent = reinterpret_cast<void *>(field_parent);
  e.field_get_flags = reinterpret_cast<void *>(field_flags); e.field_get_offset = reinterpret_cast<void *>(field_offset);
  e.field_get_type = reinterpret_cast<void *>(field_type); e.class_from_type = reinterpret_cast<void *>(from_type);
  e.class_is_valuetype = reinterpret_cast<void *>(value_type); e.class_is_enum = reinterpret_cast<void *>(is_enum);
  e.class_enum_basetype = reinterpret_cast<void *>(enum_base); e.class_value_size = reinterpret_cast<void *>(value_size);
  e.class_get_method_from_name = reinterpret_cast<void *>(method_by_name); e.method_get_class = reinterpret_cast<void *>(method_class);
  e.method_get_name = reinterpret_cast<void *>(method_name); e.method_get_flags = reinterpret_cast<void *>(method_flags);
  e.method_get_param_count = reinterpret_cast<void *>(param_count); e.method_get_param = reinterpret_cast<void *>(param);
  e.method_get_return_type = reinterpret_cast<void *>(return_type); e.method_is_generic = reinterpret_cast<void *>(generic);
  e.method_is_inflated = reinterpret_cast<void *>(inflated); e.debug_get_method_info = reinterpret_cast<void *>(debug_info);
  e.class_is_assignable_from = reinterpret_cast<void *>(assignable); e.class_get_methods = reinterpret_cast<void *>(enumerate);
  e.class_get_nested_types = reinterpret_cast<void *>(enumerate); e.class_get_declaring_type = reinterpret_cast<void *>(declaring);
  return e;
}

void build_fixture() {
  // Independent facts from quick-close.js required core, not generated success expectations alone.
  assert(kClassCount == 7 && kFieldCount == 6 && kMethodCount == 7 && kOptionalCount == 21);
  assert(kStatusFieldIndex == 5);
  assert(!std::strcmp(kClasses[3].canonical, "Holoholo.Rpc.CatchPokemonOutProto"));
  assert(kFields[0].owner == 3 && kFields[0].offset == 0x20 && kFields[0].kind == PGR_MD_FIELD_VALUE && kFields[0].value_size == 8);
  assert(!std::strcmp(kFields[0].type_name, "System.UInt64"));
  assert(kFields[1].owner == 0 && kFields[1].offset == 0x70 && kFields[1].kind == PGR_MD_FIELD_ENUM_I32);
  assert(kFields[2].owner == 1 && kFields[2].offset == 0x2a8 && kFields[2].kind == PGR_MD_FIELD_REF);
  assert(kFields[5].owner == 3 && kFields[5].offset == 0x10 && kFields[5].kind == PGR_MD_FIELD_ENUM_I32);
  assert(kMethods[3].param_count == 2 && !std::strcmp(kMethods[3].param1, "Holoholo.Rpc.Item"));
  assert(kMethods[5].is_static == 1 && kMethods[6].is_static == 1 && kMethods[6].prologue == nullptr);

  // Reserve a sparse module big enough for the largest RVA; only prologue pages are touched.
  uint64_t top = 0;
  for (size_t i = 0; i < kMethodCount; i++) if (kMethods[i].prologue) { uint64_t end = kMethods[i].code_rva + kMethods[i].prologue_len; if (end > top) top = end; }
  long page = sysconf(_SC_PAGESIZE); assert(page > 0);
  module_size = ((top + page - 1) / page) * page;
  void *mapped = mmap(nullptr, module_size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); assert(mapped != MAP_FAILED);
  module_base = static_cast<unsigned char *>(mapped);
  const uintptr_t debug_target=reinterpret_cast<uintptr_t>(&debug_info);
#if defined(__x86_64__)
  debug_trampoline[0]=0x48;debug_trampoline[1]=0xb8;
  std::memcpy(debug_trampoline+2,&debug_target,8);debug_trampoline[10]=0xff;debug_trampoline[11]=0xe0;
#elif defined(__aarch64__)
  const uint32_t jump[]={0x58000050,0xd61f0200};
  std::memcpy(debug_trampoline,jump,8);std::memcpy(debug_trampoline+8,&debug_target,8);
#else
#error Unsupported fixture ISA
#endif
  std::memcpy(module_base+0x7000,debug_trampoline,sizeof(debug_trampoline));
  __builtin___clear_cache(reinterpret_cast<char*>(module_base+0x7000),reinterpret_cast<char*>(module_base+0x7010));

  for (size_t i = 0; i < kClassCount; i++) classes[i].type = new_type(kClasses[i].canonical, &classes[i]);
  int32_type = new_class("System.Int32", true, 4, 4)->type;
  void_type = new_class("System.Void", true, 0, 1)->type;
  uint64_class = new_class("System.UInt64", true, 8, 8);
  enum_class = new_class("Holoholo.Rpc.CatchPokemonOutProto.Types.Status", true, 4, 4); enum_class->enumeration = true; enum_class->base = int32_type;
  ref_class = new_class("Fixture.Reference");
  bool_value_class = new_class("System.Boolean.Value", true, 1, 1); // not the required BOOL class; a value for params
  duplicate_class = new_class("Holoholo.Rpc.CatchPokemonOutProto");
  classes[6].value=true;classes[6].size=1;classes[6].align=1;

  for (size_t i = 0; i < kFieldCount; i++) {
    const auto &s = kFields[i];
    Type *t = s.kind == PGR_MD_FIELD_VALUE ? uint64_class->type
            : s.kind == PGR_MD_FIELD_ENUM_I32 ? enum_class->type : ref_class->type;
    fields.emplace_back(new Field{&classes[s.owner], &classes[s.owner], s.name, t, 0, s.offset});
  }
  fields.emplace_back(new Field{enum_class,enum_class,"CatchSuccess",enum_class->type,0x8050,0});
  gc_code();
  for (size_t i = 0; i < kMethodCount; i++) {
    const auto &s = kMethods[i];
    Type *p0 = s.param0 ? type(s.param0) : nullptr, *p1 = s.param1 ? type(s.param1) : nullptr;
    Method *m = new_method(&classes[s.owner], s.name, s.return_type ? type(s.return_type) : nullptr, p0, p1);
    if (s.is_static) m->flags = 0x10;
    if (s.prologue) { m->code = module_base + s.code_rva; std::memcpy(module_base + s.code_rva, s.prologue, s.prologue_len); }
  }
}

PgrSharedMetadataOutput success() {
  PgrSharedMetadataOutput out{}; const int rc=api->resolve(&config,&out);if(rc)std::fprintf(stderr,"shared56 resolve status=%d\n",rc);
  assert(rc == 0 && out.version == 2 && out.size == sizeof(out));
  for (size_t i = 0; i < kClassCount; i++) assert(out.classes[i] == &classes[i]);
  for (size_t i = 0; i < kFieldCount; i++) assert(out.fields[i] == fields[i].get());
  for (size_t i = 0; i < kMethodCount; i++) assert(out.methods[i] == methods[i].get());
  for (size_t i = 0; i < kMethodCount; i++)
    assert(kMethods[i].prologue ? out.method_code[i] == module_base + kMethods[i].code_rva : out.method_code[i] == nullptr);
  assert(out.field_type_classes[kStatusFieldIndex] == enum_class); // JS CatchSuccess-literal owner handle
  assert(out.field_type_classes[0] == uint64_class);
  for (size_t i = 0; i < kOptionalCount; i++) assert(out.optional[i] == PGR_SHARED_OPT_UNAVAILABLE);
  assert(out.success==1&&out.caught_id_size==8&&out.field_meta_count>=6);
  for(size_t i=0;i<6;++i){assert(out.field_meta[i].offset==kFields[i].offset);assert(out.field_meta[i].kind==kFields[i].kind);assert(!std::strcmp(out.field_meta[i].name,kFields[i].name));}
  assert(out.enrichment_flags == 0 && names_allocated == names_freed);
  return out;
}
void failure(int expected, const PgrSharedMetadataConfig *c = nullptr) {
  PgrSharedMetadataOutput out; std::memset(&out, 0xa5, sizeof(out));
  assert(api->resolve(c ? c : &config, &out) == expected);
  assert(zero(out) && names_allocated == names_freed);
}
} // namespace

int main(int argc, char **argv) {
  assert(argc == 2); void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL); assert(library);
  auto get = reinterpret_cast<const void *(*)(uint32_t)>(dlsym(library, "pgrazor_native_api")); assert(get);
  auto *core = static_cast<const pgrazor_native_api_v2 *>(get(2)); assert(core && core->query_feature && !core->query_feature(56, 1));
  api = static_cast<const PgrSharedMetadataApi *>(core->query_feature(56, 2));
  assert(api && api->version == 2 && api->size == 24 && api->config_size == 96 && api->output_size == 67304);

  build_fixture();
  PgrMetaExports e = exports();
  e.debug_get_method_info=module_base+0x7000;
  config.version = 2; config.size = sizeof(config);
  extra.field_static_get_value=reinterpret_cast<void*>(static_value);
  extra.gchandle_new=module_base+gc_new_rva;extra.gchandle_get_target=module_base+gc_get_rva;extra.gchandle_free=module_base+gc_free_rva;
  config.extra=&extra;assert(sizeof(extra)==112);
  config.binding.version = 1; config.binding.size = sizeof(PgrMetaBindConfig); config.binding.exports = &e;
  config.binding.module_base = module_base; config.binding.module_size = module_size; config.binding.seg_query = segment;
  config.binding.debug_prologue=debug_trampoline;config.binding.debug_prologue_len=sizeof(debug_trampoline);
  config.thread_current = reinterpret_cast<void *>(thread_current); config.owner_tid = gettid();

  success(); success(); // repeated resolve yields identical validated pointers

  // Config / argument validation.
  { auto c = config; c.version = 1; failure(PGR_SHARED_ERR_CONFIG, &c); c = config; c.size--; failure(PGR_SHARED_ERR_CONFIG, &c);
    c = config; c.thread_current = nullptr; failure(PGR_SHARED_ERR_CONFIG, &c); }
  {auto c=config;c.extra=nullptr;failure(PGR_SHARED_ERR_CONFIG,&c);
   auto x=extra;c=config;c.extra=&x;x.field_static_get_value=nullptr;failure(PGR_SHARED_ERR_CONFIG,&c);}
  { PgrSharedMetadataOutput out; std::memset(&out, 0xa5, sizeof(out));
    assert(api->resolve(nullptr, &out) == PGR_SHARED_ERR_CONFIG && zero(out));
    assert(api->resolve(&config, nullptr) == PGR_SHARED_ERR_NULL_ARG); }

  // Thread ownership / attachment.
  attached = false; failure(PGR_SHARED_ERR_ATTACH); attached = true;
  { auto c = config; c.owner_tid = 0; failure(PGR_SHARED_ERR_ATTACH, &c); c.owner_tid = config.owner_tid + 1; failure(PGR_SHARED_ERR_ATTACH, &c); }
  { std::thread foreign([] { failure(PGR_SHARED_ERR_ATTACH); }); foreign.join(); }
  thread_checks = 0; detach_at = 4; failure(PGR_SHARED_ERR_ATTACH); detach_at = 0; success();

  // Class resolution: not found (absent in every image) and cross-image duplicate with a different handle.
  hide = 5; failure(PGR_SHARED_NOT_READY); hide = -1; // SYNC absent
  {auto saved=fields[2]->name;fields[2]->name="missing";failure(PGR_SHARED_ERR_MEMBER_NOT_FOUND);fields[2]->name=saved;}
  {auto saved=methods[1]->name;methods[1]->name="missing";failure(PGR_SHARED_ERR_MEMBER_NOT_FOUND);methods[1]->name=saved;}
  module_base[0x4384]^=1;failure(PGR_SHARED_ERR_GC_ABI);module_base[0x4384]^=1;
  {auto &literal=*fields[kFieldCount];auto saved=literal.name;literal.name="missing";failure(PGR_SHARED_ERR_MEMBER_NOT_FOUND);literal.name=saved;
   literal.flags=0x10;failure(PGR_MD_ERR_FLAGS);literal.flags=0x8050;}
  classes[6].align=4;failure(PGR_MD_ERR_SIZE);classes[6].align=1;
  duplicate = true; failure(PGR_MD_ERR_DUPLICATE); duplicate = false;

  // Field faults (precise feature-47 classifications).
  { auto saved = fields[2]->offset; fields[2]->offset++; failure(PGR_MD_ERR_OFFSET); fields[2]->offset = saved; }
  fields[2]->flags = 0x10; failure(PGR_MD_ERR_FLAGS); fields[2]->flags = 0;
  { auto saved = fields[2]->type; fields[2]->type = uint64_class->type; failure(PGR_MD_ERR_KIND); fields[2]->type = saved; } // ref expected, value given
  { auto saved = fields[0]->type; fields[0]->type = ref_class->type; failure(PGR_MD_ERR_KIND); fields[0]->type = saved; } // value expected, ref given
  { auto saved = enum_class->base; enum_class->base = void_type; failure(PGR_MD_ERR_KIND); enum_class->base = saved; } // enum basetype != Int32
  { auto saved = enum_class->size; enum_class->size = 8; failure(PGR_MD_ERR_SIZE); enum_class->size = saved; } // enum size != 4
  { auto saved = uint64_class->size; uint64_class->size = 4; failure(PGR_MD_ERR_SIZE); uint64_class->size = saved; } // captured id must be 8
  { auto saved = fields[0]->type; auto *other = new_class("System.Int64", true, 8, 8); fields[0]->type = other->type;
    failure(PGR_MD_ERR_TYPE); fields[0]->type = saved; } // UInt64 name required

  // Method faults.
  { auto saved = methods[1]->result; methods[1]->result = int32_type; failure(PGR_MD_ERR_TYPE); methods[1]->result = saved; }
  { auto saved = methods[1]->owner; methods[1]->owner = &classes[2]; failure(PGR_MD_ERR_OWNER_MISMATCH); methods[1]->owner = saved; }
  methods[1]->flags = 0x10; failure(PGR_MD_ERR_STATIC); methods[1]->flags = 0;          // dismiss is instance
  methods[5]->flags = 0; failure(PGR_MD_ERR_STATIC); methods[5]->flags = 0x10;          // op_Implicit is static
  methods[1]->generic = true; failure(PGR_MD_ERR_GENERIC); methods[1]->generic = false;
  methods[1]->inflated = true; failure(PGR_MD_ERR_INFLATED); methods[1]->inflated = false;
  { auto saved = methods[3]->param1; methods[3]->param1 = int32_type; failure(PGR_MD_ERR_TYPE); methods[3]->param1 = saved; }
  { void *saved = methods[1]->code; methods[1]->code = module_base + kMethods[1].code_rva + 4; failure(PGR_MD_ERR_CODE_RANGE); methods[1]->code = saved; }
  { uint64_t rva = kMethods[1].code_rva; module_base[rva] ^= 1; failure(PGR_MD_ERR_PROLOGUE); module_base[rva] ^= 1; }

  // Canonical-bind preconditions (default resolver path rejected before resolve_module here).
  { auto c = config; c.binding.seg_query = nullptr; unsigned before = thread_checks; failure(PGR_MD_ERR_CODE_RANGE, &c); assert(thread_checks == before); }
  { auto c = config; c.binding.module_base = reinterpret_cast<void *>(UINTPTR_MAX - 3); c.binding.module_size = 8;
    unsigned before = thread_checks; failure(PGR_MD_ERR_CODE_RANGE, &c); assert(thread_checks == before); }

  // Reentrant resolve returns BUSY without touching shared state.
  reenter = true; success(); assert(reentries == 1); success();

  assert(names_allocated == names_freed); assert(munmap(module_base, module_size) == 0);
  std::puts("PASS shared_metadata actual-library: 7 classes/6 fields/7 methods required core, source-known "
            "descriptors, CatchSuccess literal + BOOL ABI + GC fingerprints, 21 optional unavailable, cleared failures, "
            "owner/attachment, offset/flags/kind/size/type/static/generic/inflated/param/code/prologue "
            "faults, duplicate class, canonical-bind preconditions, reentrant BUSY, balanced name frees");
  // No hooks installed. Keep the production library mapped until process exit; no live runtime claim.
  return 0;
}
