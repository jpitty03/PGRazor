#include "runtime_metadata.h"
#include "pgrazor_native_api.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <vector>
#include <thread>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

// Actual-library fixture harness for feature 47 (reusable IL2CPP metadata discovery/validation). It
// resolves the table through the exported pgrazor_native_api v2 registry (query_feature(47,1) -- the
// coordinator registers the hidden getter there) and drives bind + resolve_class / validate_field /
// validate_method / validate_code against a FAKE IL2CPP metadata universe: classes/fields/methods as
// tokens, fixture il2cpp exports over them, and a fixture segment resolver over a REAL page-aligned
// fake-module buffer holding REAL prologue bytes. It asserts the success paths and EACH classified
// failure. It uses NO game process, performs NO managed invoke, and proves nothing about live
// translated-ARM64 resolution under Houdini (see the contract).

namespace {

using GetApiFn = const void *(*)(uint32_t);

// ---- fake metadata model ----------------------------------------------------------------------
struct FType { std::string name; void *klass; };
struct FClass { FType *type; bool valuetype; bool isenum; FType *enumbase; int valsize; int align; };
struct FField { std::string name; FClass *parent; FType *type; int flags; size_t offset; };
struct FMethod {
  std::string name; FClass *klass; uint32_t flags; uint32_t iflags; uint32_t np;
  FType *ret; FType *params[PGR_MD_PARAM_MAX]; bool generic; bool inflated; void *code;
};

struct ClassReg { void *image; std::string ns, name; FClass *klass; };
std::vector<ClassReg> g_classes;
std::vector<FField *> g_fields;
std::vector<FMethod *> g_methods;

const PgrMetaApi *g_api_test=nullptr;
bool g_reenter=false;
int g_domain_tok = 0;
int g_imgA = 0, g_imgB = 0;
void *IMGA = &g_imgA, *IMGB = &g_imgB;
void *g_asms[300];
size_t g_asm_count = 2;

// ---- fixture il2cpp exports -------------------------------------------------------------------
void *fx_domain_get() {
 if(g_reenter){
   PgrMetaSample sample{};sample.bound=9;
   assert(g_api_test->sample(&sample)==PGR_MD_ERR_BUSY&&sample.bound==0);
   std::thread concurrent([]{PgrMetaSample other{};assert(g_api_test->sample(&other)==PGR_MD_ERR_BUSY);});
   concurrent.join();
 }
 return &g_domain_tok;
}
void *fx_domain_assemblies(void *, size_t *c) { *c = g_asm_count; return reinterpret_cast<void *>(g_asms); }
void *fx_assembly_image(void *a) { return a; }
void *fx_class_from_name(void *image, const char *ns, const char *name) {
  for (auto &cr : g_classes)
    if (cr.image == image && cr.ns == ns && cr.name == name) return cr.klass;
  return nullptr;
}
void *fx_class_get_type(void *k) { return static_cast<FClass *>(k)->type; }
char *fx_type_get_name(void *t) {
  const std::string &s = static_cast<FType *>(t)->name;
  char *p = static_cast<char *>(std::malloc(s.size() + 1));
  std::memcpy(p, s.c_str(), s.size() + 1);
  return p;
}
void fx_mem_free(void *p) { std::free(p); }
void *fx_field_from_name(void *k, const char *name) {
  for (auto *f : g_fields)
    if (f->parent == static_cast<FClass *>(k) && f->name == name) return f;
  return nullptr;
}
void *fx_field_parent(void *f) { return static_cast<FField *>(f)->parent; }
int fx_field_flags(void *f) { return static_cast<FField *>(f)->flags; }
size_t fx_field_offset(void *f) { return static_cast<FField *>(f)->offset; }
void *fx_field_type(void *f) { return static_cast<FField *>(f)->type; }
void *fx_class_from_type(void *t) { return static_cast<FType *>(t)->klass; }
bool fx_is_valuetype(void *k) { return static_cast<FClass *>(k)->valuetype; }
bool fx_is_enum(void *k) { return static_cast<FClass *>(k)->isenum; }
void *fx_enum_base(void *k) { return static_cast<FClass *>(k)->enumbase; }
int fx_value_size(void *k, int *al) { *al = static_cast<FClass *>(k)->align; return static_cast<FClass *>(k)->valsize; }
void *fx_method_from_name(void *k, const char *name, int argc) {
  for (auto *m : g_methods)
    if (m->klass == static_cast<FClass *>(k) && m->name == name && static_cast<int>(m->np) == argc) return m;
  return nullptr;
}
void *fx_method_class(void *m) { return static_cast<FMethod *>(m)->klass; }
char *fx_method_name(void *m) { return const_cast<char *>(static_cast<FMethod *>(m)->name.c_str()); }
uint32_t fx_method_flags(void *m, uint32_t *i) { *i = static_cast<FMethod *>(m)->iflags; return static_cast<FMethod *>(m)->flags; }
uint32_t fx_method_pcount(void *m) { return static_cast<FMethod *>(m)->np; }
void *fx_method_param(void *m, uint32_t i) { return static_cast<FMethod *>(m)->params[i]; }
void *fx_method_return(void *m) { return static_cast<FMethod *>(m)->ret; }
bool fx_is_generic(void *m) { return static_cast<FMethod *>(m)->generic; }
bool fx_is_inflated(void *m) { return static_cast<FMethod *>(m)->inflated; }
bool fx_debug_info(void *m, void *info) { static_cast<void **>(info)[0] = static_cast<FMethod *>(m)->code; return true; }
bool fx_assignable(void *, void *) { return false; }
void *fx_get_methods(void *, void **) { return nullptr; }
void *fx_get_nested(void *, void **) { return nullptr; }
void *fx_declaring(void *) { return nullptr; }

PgrMetaExports make_exports() {
  PgrMetaExports e{};
  e.domain_get = reinterpret_cast<void *>(&fx_domain_get);
  e.domain_get_assemblies = reinterpret_cast<void *>(&fx_domain_assemblies);
  e.assembly_get_image = reinterpret_cast<void *>(&fx_assembly_image);
  e.class_from_name = reinterpret_cast<void *>(&fx_class_from_name);
  e.class_get_type = reinterpret_cast<void *>(&fx_class_get_type);
  e.type_get_name = reinterpret_cast<void *>(&fx_type_get_name);
  e.mem_free = reinterpret_cast<void *>(&fx_mem_free);
  e.class_get_field_from_name = reinterpret_cast<void *>(&fx_field_from_name);
  e.field_get_parent = reinterpret_cast<void *>(&fx_field_parent);
  e.field_get_flags = reinterpret_cast<void *>(&fx_field_flags);
  e.field_get_offset = reinterpret_cast<void *>(&fx_field_offset);
  e.field_get_type = reinterpret_cast<void *>(&fx_field_type);
  e.class_from_type = reinterpret_cast<void *>(&fx_class_from_type);
  e.class_is_valuetype = reinterpret_cast<void *>(&fx_is_valuetype);
  e.class_is_enum = reinterpret_cast<void *>(&fx_is_enum);
  e.class_enum_basetype = reinterpret_cast<void *>(&fx_enum_base);
  e.class_value_size = reinterpret_cast<void *>(&fx_value_size);
  e.class_get_method_from_name = reinterpret_cast<void *>(&fx_method_from_name);
  e.method_get_class = reinterpret_cast<void *>(&fx_method_class);
  e.method_get_name = reinterpret_cast<void *>(&fx_method_name);
  e.method_get_flags = reinterpret_cast<void *>(&fx_method_flags);
  e.method_get_param_count = reinterpret_cast<void *>(&fx_method_pcount);
  e.method_get_param = reinterpret_cast<void *>(&fx_method_param);
  e.method_get_return_type = reinterpret_cast<void *>(&fx_method_return);
  e.method_is_generic = reinterpret_cast<void *>(&fx_is_generic);
  e.method_is_inflated = reinterpret_cast<void *>(&fx_is_inflated);
  e.debug_get_method_info = reinterpret_cast<void *>(&fx_debug_info);
  e.class_is_assignable_from = reinterpret_cast<void *>(&fx_assignable);
  e.class_get_methods = reinterpret_cast<void *>(&fx_get_methods);
  e.class_get_nested_types = reinterpret_cast<void *>(&fx_get_nested);
  e.class_get_declaring_type = reinterpret_cast<void *>(&fx_declaring);
  return e;
}

// ---- fixture module + segment resolver --------------------------------------------------------
alignas(64) unsigned char g_mod_buf[4096];
bool g_readable = true, g_host_exec = true, g_elf_exec = false,g_writable=false;
int g_segment_mode=0;
int fake_seg(void *, const void *addr, uint64_t size, PgrMetaSegment *out) {
  std::memset(out, 0, sizeof(*out));
  uintptr_t a = reinterpret_cast<uintptr_t>(addr), base = reinterpret_cast<uintptr_t>(g_mod_buf);
  if (a >= base && a + size <= base + sizeof(g_mod_buf)) {
    out->found = 1;
    out->readable = g_readable ? 1u : 0u;
    out->host_exec = g_host_exec ? 1u : 0u;
    out->elf_exec = g_elf_exec ? 1u : 0u;
    out->seg_base = g_mod_buf;
    out->seg_size = sizeof(g_mod_buf);out->writable=g_writable?1:0;
    if(g_segment_mode==1){out->seg_base=reinterpret_cast<void *>(a+4);out->seg_size=64;}
    if(g_segment_mode==2)out->seg_size=UINT64_MAX;
    if(g_segment_mode==3)out->found=2;
    if(g_segment_mode==4)out->readable=2;
    if(g_segment_mode==5)out->host_exec=2;
    if(g_segment_mode==6)out->elf_exec=2;
    if(g_segment_mode==7)out->writable=2;
  }
  return 0;
}

// helpers
PgrMetaFieldQuery fq(const void *owner, const char *name, uint64_t off, uint32_t forb, uint32_t req,
                     uint32_t kind, uint32_t vsize, const char *tn, uint32_t tm) {
  PgrMetaFieldQuery q{};
  q.version = 1; q.size = sizeof(q); q.owner = owner; q.name = name; q.offset = off;
  q.forbidden_mask = forb; q.required_mask = req; q.kind = kind; q.value_size = vsize;
  q.type_name = tn; q.type_match = tm;
  return q;
}
PgrMetaClassQuery cq(const char *ns, const char *name, const char *canon) {
  PgrMetaClassQuery q{};
  q.version = 1; q.size = sizeof(q); q.name_space = ns; q.name = name; q.canonical = canon;
  return q;
}

}  // namespace

int main(int argc, char **argv) {
  const char *so = argc > 1 ? argv[1] : "./libpgrazor.so";
  void *lib = ::dlopen(so, RTLD_NOW | RTLD_LOCAL);
  if (!lib) { std::fprintf(stderr, "dlopen(%s) failed: %s\n", so, ::dlerror()); return 1; }
  auto getter = reinterpret_cast<GetApiFn>(::dlsym(lib, "pgrazor_native_api"));
  assert(getter);
  const auto *core = static_cast<const pgrazor_native_api_v2 *>(getter(2));
  assert(core && core->query_feature);
  assert(core->query_feature(47, 99) == nullptr);
  const auto *api = static_cast<const PgrMetaApi *>(core->query_feature(47, 1));
  assert(api && api->version == 1 && api->size == sizeof(PgrMetaApi));g_api_test=api;
  assert(sizeof(PgrMetaSegment)==40);

  std::printf("ABI sizes: exports=%zu bind=%zu classQ=%zu fieldQ=%zu methodQ=%zu codeQ=%zu table=%zu\n",
              sizeof(PgrMetaExports), sizeof(PgrMetaBindConfig), sizeof(PgrMetaClassQuery),
              sizeof(PgrMetaFieldQuery), sizeof(PgrMetaMethodQuery), sizeof(PgrMetaCodeQuery),
              sizeof(PgrMetaApi));
  assert(sizeof(PgrMetaExports) == 248 && sizeof(PgrMetaBindConfig) == 64 &&
         sizeof(PgrMetaMethodQuery) == 264 && sizeof(PgrMetaApi) == 64);

  // ---- fake type/class universe ----
  FType t_int32{"System.Int32", nullptr};
  FType t_void{"System.Void", nullptr};
  FType t_obj{"UnityEngine.Object", nullptr};

  FClass c_enum{nullptr, true, true, &t_int32, 4, 4};
  FType t_enum{"SomeEnum", &c_enum}; c_enum.type = &t_enum;
  FClass c_u64{nullptr, true, false, nullptr, 8, 8};
  FType t_u64{"System.UInt64", &c_u64}; c_u64.type = &t_u64;
  FClass c_str{nullptr, false, false, nullptr, 0, 0};
  FType t_str{"System.String", &c_str}; c_str.type = &t_str;
  FClass c_bool{nullptr, true, false, nullptr, 1, 1};
  FType t_bool{"System.Boolean", &c_bool}; c_bool.type = &t_bool;

  FClass c_gui{nullptr, false, false, nullptr, 0, 0};
  FType t_gui{"Niantic.Holoholo.NUI.GuiController", &c_gui}; c_gui.type = &t_gui;

  FClass c_dupA{nullptr, false, false, nullptr, 0, 0};
  FType t_dupA{"DupNs.Dup", &c_dupA}; c_dupA.type = &t_dupA;
  FClass c_dupB{nullptr, false, false, nullptr, 0, 0};
  FType t_dupB{"DupNs.Dup", &c_dupB}; c_dupB.type = &t_dupB;

  FClass c_weird{nullptr, false, false, nullptr, 0, 0};
  FType t_weird{"Totally.Different", &c_weird}; c_weird.type = &t_weird;

  g_asms[0] = IMGA; g_asms[1] = IMGB; g_asm_count = 2;
  g_classes = {
      {IMGA, "Niantic.Holoholo.NUI", "GuiController", &c_gui},
      {IMGA, "DupNs", "Dup", &c_dupA},
      {IMGB, "DupNs", "Dup", &c_dupB},
      {IMGA, "Bad.Ns", "Weird", &c_weird},
  };

  FField f_state{"currentState", &c_gui, &t_enum, 0x0, 0x70};
  FField f_caught{"caughtId", &c_gui, &t_u64, 0x0, 0x20};
  FField f_ref{"someRef", &c_gui, &t_str, 0x0, 0xA0};
  FField f_boolv{"boolField", &c_gui, &t_bool, 0x0, 0x30};
  FField f_lit{"litField", &c_gui, &t_enum, 0x8050, 0x40};
  g_fields = {&f_state, &f_caught, &f_ref, &f_boolv, &f_lit};

  // prologue bytes in the real fake-module buffer
  const unsigned char PRO[8] = {0xfe, 0x0f, 0x1d, 0xf8, 0xf6, 0x57, 0x01, 0xa9};
  void *CODE = g_mod_buf + 64;  // 4-byte aligned, in module
  std::memcpy(CODE, PRO, sizeof(PRO));

  FMethod m_dismiss{"Dismiss", &c_gui, 0, 0, 0, &t_void, {}, false, false, CODE};
  FMethod m_impl{"op_Implicit", &c_gui, 0x10, 0, 1, &t_bool, {&t_obj}, false, false, nullptr};
  FMethod m_gen{"Generic", &c_gui, 0, 0, 0, &t_void, {}, true, false, nullptr};
  FMethod m_inf{"Inflated", &c_gui, 0, 0, 0, &t_void, {}, false, true, nullptr};
  g_methods = {&m_dismiss, &m_impl, &m_gen, &m_inf};

  PgrMetaExports exports = make_exports();
  PgrMetaBindConfig cfg{};
  cfg.version = 1; cfg.size = sizeof(cfg); cfg.exports = &exports;
  cfg.module_base = g_mod_buf; cfg.module_size = sizeof(g_mod_buf);
  cfg.seg_query = &fake_seg; cfg.seg_ctx = nullptr;

  // 1. bind header/export/idempotency/mismatch.
  { PgrMetaClassQuery q = cq("Niantic.Holoholo.NUI", "GuiController", nullptr); PgrMetaClassResult r{};
    assert(api->resolve_class(&q, &r) == PGR_MD_ERR_NOT_BOUND); }
  assert(api->bind(nullptr) == PGR_MD_ERR_NULL_ARG);
  { PgrMetaBindConfig b = cfg; b.version = 2; assert(api->bind(&b) == PGR_MD_ERR_BAD_VERSION); }
  { PgrMetaBindConfig b = cfg; b.size = sizeof(b) - 1; assert(api->bind(&b) == PGR_MD_ERR_BAD_SIZE); }
  { PgrMetaExports e2 = exports; e2.type_get_name = nullptr; PgrMetaBindConfig b = cfg; b.exports = &e2;
    assert(api->bind(&b) == PGR_MD_ERR_MISSING_EXPORT); }
  { PgrMetaBindConfig b = cfg; b.module_base = nullptr; assert(api->bind(&b) == PGR_MD_ERR_MISSING_EXPORT); }
  {PgrMetaBindConfig b=cfg;b.module_base=reinterpret_cast<void *>(UINTPTR_MAX-3);b.module_size=8;
   assert(api->bind(&b)==PGR_MD_ERR_CODE_RANGE);}
  // Separate process proves bootstrap-first then schema-specific validation.
  {pid_t child=fork();assert(child>=0);if(child==0){
    PgrMetaExports local=exports;local.debug_get_method_info=CODE;
    PgrMetaBindConfig b=cfg;b.exports=&local;
    assert(api->bind(&b)==PGR_MD_OK);
    b.debug_prologue=PRO;b.debug_prologue_len=sizeof(PRO);
    assert(api->bind(&b)==PGR_MD_OK);
    unsigned char copy[sizeof(PRO)];std::memcpy(copy,PRO,sizeof(copy));b.debug_prologue=copy;
    assert(api->bind(&b)==PGR_MD_OK);copy[0]^=1;
    assert(api->bind(&b)==PGR_MD_ERR_PROLOGUE);
    copy[0]^=1;assert(api->bind(&b)==PGR_MD_OK);
    static_cast<unsigned char *>(CODE)[0]^=1;assert(api->bind(&b)==PGR_MD_ERR_PROLOGUE);static_cast<unsigned char *>(CODE)[0]^=1;
    b.debug_prologue_len=0;assert(api->bind(&b)==PGR_MD_OK);_exit(0);
   }int status=0;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);}
  assert(api->bind(&cfg) == PGR_MD_OK);
  assert(api->bind(&cfg) == PGR_MD_OK);  // idempotent
  { PgrMetaBindConfig b = cfg; b.module_size = 1; assert(api->bind(&b) == PGR_MD_ERR_BOUND_MISMATCH); }

  // 2. resolve_class success / canonical / not-found / duplicate / limits.
  { PgrMetaClassQuery q = cq("Niantic.Holoholo.NUI", "GuiController", nullptr); PgrMetaClassResult r{};
    assert(api->resolve_class(&q, &r) == PGR_MD_OK && r.klass == &c_gui && r.type == &t_gui); }
  { PgrMetaClassQuery q = cq("Niantic.Holoholo.NUI", "GuiController",
                             "Niantic.Holoholo.NUI.GuiController"); PgrMetaClassResult r{};
    assert(api->resolve_class(&q, &r) == PGR_MD_OK); }
  { PgrMetaClassQuery q = cq("Niantic.Holoholo.NUI", "Missing", nullptr); PgrMetaClassResult r{};
    assert(api->resolve_class(&q, &r) == PGR_MD_ERR_NOT_FOUND); }
  { PgrMetaClassQuery q = cq("Bad.Ns", "Weird", nullptr); PgrMetaClassResult r{};  // full name mismatches
    assert(api->resolve_class(&q, &r) == PGR_MD_ERR_NAME_MISMATCH); }
  { PgrMetaClassQuery q = cq("DupNs", "Dup", nullptr); PgrMetaClassResult r{};  // two images, two handles
    assert(api->resolve_class(&q, &r) == PGR_MD_ERR_DUPLICATE); }
  { std::string big(300, 'x'); PgrMetaClassQuery q = cq("Ns", big.c_str(), nullptr); PgrMetaClassResult r{};
    assert(api->resolve_class(&q, &r) == PGR_MD_ERR_NAME_TOO_LONG); }
  { g_asm_count = PGR_MD_ASM_MAX + 1; for (size_t i = 2; i < g_asm_count; ++i) g_asms[i] = IMGA;
    PgrMetaClassQuery q = cq("Niantic.Holoholo.NUI", "GuiController", nullptr); PgrMetaClassResult r{};
    assert(api->resolve_class(&q, &r) == PGR_MD_ERR_LIMIT); g_asm_count = 2; }

  // 3. validate_field: enum-int32 / value / ref / literal + every classified failure.
  { PgrMetaFieldResult r{};
    auto q = fq(&c_gui, "currentState", 0x70, 0x50, 0, PGR_MD_FIELD_ENUM_I32, 0, nullptr, 0);
    assert(api->validate_field(&q, &r) == PGR_MD_OK && r.field == &f_state && r.value_size == 4);
    q = fq(&c_gui, "caughtId", 0x20, 0x50, 0, PGR_MD_FIELD_VALUE, 8, "System.UInt64", PGR_MD_MATCH_EXACT);
    assert(api->validate_field(&q, &r) == PGR_MD_OK && r.value_size == 8);
    q = fq(&c_gui, "someRef", 0xA0, 0x50, 0, PGR_MD_FIELD_REF, 0, nullptr, 0);
    assert(api->validate_field(&q, &r) == PGR_MD_OK);
    q = fq(&c_gui, "litField", 0x40, 0, 0x8050, PGR_MD_FIELD_ANY, 0, nullptr, 0);  // static literal
    assert(api->validate_field(&q, &r) == PGR_MD_OK);
    // failures:
    q = fq(&c_gui, "nope", 0x70, 0x50, 0, PGR_MD_FIELD_ANY, 0, nullptr, 0);
    assert(api->validate_field(&q, &r) == PGR_MD_ERR_NOT_FOUND);
    q = fq(&c_gui, "currentState", 0x71, 0x50, 0, PGR_MD_FIELD_ENUM_I32, 0, nullptr, 0);
    assert(api->validate_field(&q, &r) == PGR_MD_ERR_OFFSET);
    q = fq(&c_gui, "litField", 0x40, 0x50, 0, PGR_MD_FIELD_ANY, 0, nullptr, 0);  // 0x8050&0x50 forbidden
    assert(api->validate_field(&q, &r) == PGR_MD_ERR_FLAGS);
    q = fq(&c_gui, "currentState", 0x70, 0x50, 0x8000, PGR_MD_FIELD_ENUM_I32, 0, nullptr, 0);  // req unmet
    assert(api->validate_field(&q, &r) == PGR_MD_ERR_FLAGS);
    q = fq(&c_gui, "someRef", 0xA0, 0x50, 0, PGR_MD_FIELD_VALUE, 0, nullptr, 0);  // ref is not value
    assert(api->validate_field(&q, &r) == PGR_MD_ERR_KIND);
    q = fq(&c_gui, "caughtId", 0x20, 0x50, 0, PGR_MD_FIELD_REF, 0, nullptr, 0);  // value is not ref
    assert(api->validate_field(&q, &r) == PGR_MD_ERR_KIND);
    q = fq(&c_gui, "boolField", 0x30, 0x50, 0, PGR_MD_FIELD_VALUE, 0, nullptr, 0);  // size 1 not 4/8
    assert(api->validate_field(&q, &r) == PGR_MD_ERR_SIZE);
    q = fq(&c_gui, "caughtId", 0x20, 0x50, 0, PGR_MD_FIELD_VALUE, 4, nullptr, 0);  // require 4, is 8
    assert(api->validate_field(&q, &r) == PGR_MD_ERR_SIZE);
    q = fq(&c_gui, "caughtId", 0x20, 0x50, 0, PGR_MD_FIELD_VALUE, 8, "System.Int64", PGR_MD_MATCH_EXACT);
    assert(api->validate_field(&q, &r) == PGR_MD_ERR_TYPE); }

  // 4. validate_method: instance+code, static, and every classified failure.
  { PgrMetaMethodResult r{};
    PgrMetaMethodQuery q{}; q.version = 1; q.size = sizeof(q); q.owner = &c_gui; q.name = "Dismiss";
    q.return_type = "System.Void"; q.return_match = PGR_MD_MATCH_EXACT; q.param_count = 0; q.is_static = 0;
    q.expected_code = CODE; q.prologue = PRO; q.prologue_len = sizeof(PRO);
    assert(api->validate_method(&q, &r) == PGR_MD_OK && r.method == &m_dismiss && r.code == CODE);

    PgrMetaMethodQuery s{}; s.version = 1; s.size = sizeof(s); s.owner = &c_gui; s.name = "op_Implicit";
    s.return_type = "System.Boolean"; s.return_match = PGR_MD_MATCH_EXACT; s.param_count = 1; s.is_static = 1;
    s.param_types[0] = "UnityEngine.Object"; s.param_match[0] = PGR_MD_MATCH_EXACT;
    assert(api->validate_method(&s, &r) == PGR_MD_OK);
    s.param_types[0] = "Object"; s.param_match[0] = PGR_MD_MATCH_SUFFIX;  // suffix predicate also OK
    assert(api->validate_method(&s, &r) == PGR_MD_OK);

    // failures reuse Dismiss with one field perturbed each:
    auto base = [&]() { PgrMetaMethodQuery b{}; b.version = 1; b.size = sizeof(b); b.owner = &c_gui;
      b.name = "Dismiss"; b.return_type = "System.Void"; b.return_match = PGR_MD_MATCH_EXACT;
      b.param_count = 0; b.is_static = 0; return b; };
    { auto b = base(); b.name = "Ghost"; assert(api->validate_method(&b, &r) == PGR_MD_ERR_NOT_FOUND); }
    { auto b = base(); b.is_static = 1; assert(api->validate_method(&b, &r) == PGR_MD_ERR_STATIC); }
    { auto b = base(); b.return_type = "System.Int32"; assert(api->validate_method(&b, &r) == PGR_MD_ERR_TYPE); }
    { auto b = base(); b.name = "Generic"; assert(api->validate_method(&b, &r) == PGR_MD_ERR_GENERIC); }
    { auto b = base(); b.name = "Inflated"; assert(api->validate_method(&b, &r) == PGR_MD_ERR_INFLATED);
      b.allow_inflated = 1; assert(api->validate_method(&b, &r) == PGR_MD_OK); }
    { auto b = base(); b.param_count = 1; assert(api->validate_method(&b, &r) == PGR_MD_ERR_NOT_FOUND); }
    { auto b = base(); b.param_count = PGR_MD_PARAM_MAX + 1; assert(api->validate_method(&b, &r) == PGR_MD_ERR_LIMIT); }
    { auto b = base(); b.expected_code = g_mod_buf + 68; b.prologue = PRO; b.prologue_len = sizeof(PRO);
      assert(api->validate_method(&b, &r) == PGR_MD_ERR_CODE_RANGE); }  // debug addr != expected
    { auto b = base(); const unsigned char WRONG[8] = {0, 1, 2, 3, 4, 5, 6, 7};
      b.expected_code = CODE; b.prologue = WRONG; b.prologue_len = sizeof(WRONG);
      assert(api->validate_method(&b, &r) == PGR_MD_ERR_PROLOGUE); } }

  // 5. validate_code: success, host-exec off + elf-exec on (Houdini guest), and each failure.
  { PgrMetaCodeQuery q{}; q.version = 1; q.size = sizeof(q); q.address = CODE; q.length = sizeof(PRO);
    q.prologue = PRO; q.prologue_len = sizeof(PRO);
    assert(api->validate_code(&q) == PGR_MD_OK);
    g_host_exec = false; g_elf_exec = true;  // translated ARM64 guest code mapped read-only
    assert(api->validate_code(&q) == PGR_MD_OK);
    g_host_exec = false; g_elf_exec = false;  // neither -> rejected
    assert(api->validate_code(&q) == PGR_MD_ERR_SEGMENT);
    g_readable = false; g_elf_exec = true;     // not readable -> rejected
    assert(api->validate_code(&q) == PGR_MD_ERR_SEGMENT);
    g_readable = true; g_host_exec = true; g_elf_exec = false;  // restore
    PgrMetaCodeQuery bad = q; bad.address = g_mod_buf + 65;  // misaligned
    assert(api->validate_code(&bad) == PGR_MD_ERR_CODE_RANGE);
    bad = q; bad.address = g_mod_buf + sizeof(g_mod_buf) - 4;  // aligned but runs past module
    assert(api->validate_code(&bad) == PGR_MD_ERR_CODE_RANGE);
    bad = q; bad.length = PGR_MD_CODE_MAX + 1; assert(api->validate_code(&bad) == PGR_MD_ERR_CODE_RANGE);
    unsigned char WRONG[8] = {9, 9, 9, 9, 9, 9, 9, 9};
    bad = q; bad.prologue = WRONG; assert(api->validate_code(&bad) == PGR_MD_ERR_PROLOGUE); }

  // Corrective regressions: segment lower bounds/overflow/boolean validation.
  {PgrMetaCodeQuery q{};q.version=1;q.size=sizeof(q);q.address=CODE;q.length=sizeof(PRO);
   for(int mode=1;mode<=7;mode++){g_segment_mode=mode;assert(api->validate_code(&q)==PGR_MD_ERR_SEGMENT);}
   g_segment_mode=0;g_host_exec=false;g_elf_exec=true;g_writable=true;
   assert(api->validate_code(&q)==PGR_MD_ERR_SEGMENT);
   g_writable=false;assert(api->validate_code(&q)==PGR_MD_OK);
   g_host_exec=true;g_elf_exec=false;
   q.address=reinterpret_cast<void *>(UINTPTR_MAX-3);q.length=8;
   assert(api->validate_code(&q)==PGR_MD_ERR_CODE_RANGE);
  }
  // Failure never leaves stale successful metadata outputs, including bad headers.
  {PgrMetaClassQuery q=cq("Niantic.Holoholo.NUI","GuiController",nullptr);PgrMetaClassResult r{};
   assert(api->resolve_class(&q,&r)==0&&r.klass);q.version=2;
   assert(api->resolve_class(&q,&r)==PGR_MD_ERR_BAD_VERSION&&!r.klass&&!r.type&&r.version==0);
   PgrMetaFieldResult f{};std::memset(&f,0xa5,sizeof(f));assert(api->validate_field(nullptr,&f)==PGR_MD_ERR_NULL_ARG&&!f.field&&!f.type);
   PgrMetaMethodResult m{};std::memset(&m,0xa5,sizeof(m));assert(api->validate_method(nullptr,&m)==PGR_MD_ERR_NULL_ARG&&!m.method&&!m.code);
  }
  // Guard page immediately follows the NAME_MAX-byte nonterminated input.
  {long page=sysconf(_SC_PAGESIZE);assert(page>=PGR_MD_NAME_MAX);
   void *mapping=mmap(nullptr,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(mapping!=MAP_FAILED);
   assert(mprotect(static_cast<char *>(mapping)+page,page,PROT_NONE)==0);
   char *name=static_cast<char *>(mapping)+page-PGR_MD_NAME_MAX;std::memset(name,'x',PGR_MD_NAME_MAX);
   PgrMetaClassQuery q=cq("Ns",name,nullptr);PgrMetaClassResult r{};
   assert(api->resolve_class(&q,&r)==PGR_MD_ERR_NAME_TOO_LONG);
   auto f=fq(&c_gui,"caughtId",0x20,0x50,0,PGR_MD_FIELD_VALUE,8,name,PGR_MD_MATCH_EXACT);PgrMetaFieldResult fr{};
   assert(api->validate_field(&f,&fr)==PGR_MD_ERR_TYPE&&!fr.field);
   assert(munmap(mapping,page*2)==0);
  }
  // Reentrant and competing-thread callbacks return BUSY without shared counter writes.
  {PgrMetaSample before{},after{};assert(api->sample(&before)==0);g_reenter=true;
   PgrMetaClassQuery q=cq("Niantic.Holoholo.NUI","GuiController",nullptr);PgrMetaClassResult r{};
   assert(api->resolve_class(&q,&r)==0);g_reenter=false;assert(api->sample(&after)==0);
   assert(after.failures==before.failures&&after.classes_resolved==before.classes_resolved+1);
  }
  // Exact basename/full path only, never substring/prefix; failed outputs cleared.
  {Dl_info info{};assert(dladdr(reinterpret_cast<void *>(getter),&info)&&info.dli_fname);
   std::string full=info.dli_fname;std::string base=full.substr(full.find_last_of('/')+1);
   const void *address=reinterpret_cast<void *>(1);uint64_t size=99;
   assert(api->resolve_module(base.c_str(),&address,&size)==0&&address&&size);
   assert(api->resolve_module(full.c_str(),&address,&size)==0&&address&&size);
   std::string wrong=base+".suffix";
   assert(api->resolve_module(wrong.c_str(),&address,&size)==PGR_MD_ERR_NOT_FOUND&&!address&&!size);
   assert(api->resolve_module(".so",&address,&size)==PGR_MD_ERR_NOT_FOUND&&!address&&!size);
   // Optional second same-basename real library copy supplied by coordinator.
   if(argc>2){void *duplicate=dlopen(argv[2],RTLD_NOW|RTLD_LOCAL);assert(duplicate);
    assert(api->resolve_module(base.c_str(),&address,&size)==PGR_MD_ERR_DUPLICATE&&!address&&!size);
    dlclose(duplicate);}
  }

  // 6. telemetry is coherent (bound, counts advanced, failures recorded).
  { PgrMetaSample s{}; assert(api->sample(&s) == PGR_MD_OK);
    assert(s.bound == 1 && s.classes_resolved >= 2 && s.fields_validated >= 4 &&
           s.methods_validated >= 3 && s.code_checks >= 3 && s.failures > 0); }

  std::puts("PASS runtime metadata: ABI(248/64/264/64), bind header/export/idempotent/mismatch, "
            "resolve_class success/canonical/not-found/name-mismatch/duplicate/name-limit/asm-limit, "
            "validate_field enum-i32/value/ref/literal + offset/flags/kind/size/type, validate_method "
            "instance+code/static/suffix + not-found/static/type/generic/inflated/param/limit/"
            "code-range/prologue, validate_code host-exec+guest-elf-exec + segment/readable/align/"
            "range/length/prologue, telemetry");
  dlclose(lib);
  return 0;
}
