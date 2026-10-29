// Feature 47 implementation: reusable IL2CPP metadata discovery/validation. See runtime_metadata.h
// and .artifacts/native-migration/runtime-metadata-contract.txt. Pure structural validation: NO
// managed invokes, NO hooks, NO GC handles, NO thread attach. Metadata pointers are stable; this
// module roots nothing and calls nothing back into JS.

#include "runtime_metadata.h"

#include <cstring>
#include <string>
#include <mutex>
#include <limits>

#if defined(__linux__)
#include <cstdio>
#include <cstdlib>
#include <link.h>
#endif

namespace {

// ---- resolved il2cpp export signatures (cast from the void* config slots) ----------------------
using fn_domain_get = void *(*)();
using fn_domain_assemblies = void *(*)(void *, size_t *);
using fn_assembly_image = void *(*)(void *);
using fn_class_from_name = void *(*)(void *, const char *, const char *);
using fn_class_get_type = void *(*)(void *);
using fn_type_get_name = char *(*)(void *);
using fn_mem_free = void (*)(void *);
using fn_field_from_name = void *(*)(void *, const char *);
using fn_field_parent = void *(*)(void *);
using fn_field_flags = int (*)(void *);
using fn_field_offset = size_t (*)(void *);
using fn_field_type = void *(*)(void *);
using fn_class_from_type = void *(*)(void *);
using fn_class_bool = bool (*)(void *);
using fn_class_enum_base = void *(*)(void *);
using fn_class_value_size = int (*)(void *, int *);
using fn_method_from_name = void *(*)(void *, const char *, int);
using fn_method_class = void *(*)(void *);
using fn_method_name = char *(*)(void *);
using fn_method_flags = uint32_t (*)(void *, uint32_t *);
using fn_method_param_count = uint32_t (*)(void *);
using fn_method_param = void *(*)(void *, uint32_t);
using fn_method_return = void *(*)(void *);
using fn_method_is = bool (*)(void *);
using fn_debug_method_info = bool (*)(void *, void *);

// ---- bound resolver singleton (metadata is read-only + process-lifetime stable) ---------------
struct State {
  bool bound = false;
  PgrMetaExports exports{};
  const void *module_base = nullptr;
  uint64_t module_size = 0;
  pgr_meta_seg_fn seg_query = nullptr;
  void *seg_ctx = nullptr;

  PgrMetaSample sample{};
};

State g;
std::mutex api_mutex;
thread_local bool in_api=false;
struct ApiGuard {
  std::unique_lock<std::mutex> lock{api_mutex,std::defer_lock};
  ApiGuard(){if(!in_api&&lock.try_lock())in_api=true;}
  ~ApiGuard(){if(lock.owns_lock())in_api=false;}
  explicit operator bool() const{return lock.owns_lock();}
};
bool range(uintptr_t base,uint64_t size){return size&&size<=UINTPTR_MAX-base;}
bool contains(uintptr_t base,uint64_t size,uintptr_t address,uint64_t length){
 return range(base,size)&&range(address,length)&&address>=base&&address-base<=size&&length<=size-(address-base);
}

int fail(int code) { ++g.sample.failures; return code; }

// Bounded UTF8 read matching quick-close text(): refuse at the NAME_MAX ceiling (name-limit).
bool read_name(const char *p, std::string *out) {
  if (p == nullptr) return false;
  size_t n = 0;
  while (n < PGR_MD_NAME_MAX && p[n] != '\0') ++n;
  if (n == PGR_MD_NAME_MAX) return false;  // over the ceiling
  out->assign(p, n);
  return true;
}

// typeName(): type_get_name + bounded read + il2cpp_free. Returns false on a null name pointer.
bool type_name(void *type, std::string *out) {
  auto get = reinterpret_cast<fn_type_get_name>(g.exports.type_get_name);
  auto freep = reinterpret_cast<fn_mem_free>(g.exports.mem_free);
  char *raw = get(type);
  if (raw == nullptr) return false;
  bool ok = read_name(raw, out);
  freep(raw);
  return ok;
}

bool match(const std::string &actual, const char *expected, uint32_t mode) {
  if (mode == PGR_MD_MATCH_ANY || expected == nullptr) return true;
  std::string want;
  if(!read_name(expected,&want))return false;
  switch (mode) {
    case PGR_MD_MATCH_EXACT:    return actual == want;
    case PGR_MD_MATCH_PREFIX:   return actual.size() >= want.size() && actual.compare(0, want.size(), want) == 0;
    case PGR_MD_MATCH_SUFFIX:   return actual.size() >= want.size() &&
                                       actual.compare(actual.size() - want.size(), want.size(), want) == 0;
    case PGR_MD_MATCH_CONTAINS: return actual.find(want) != std::string::npos;
    default:                    return false;
  }
}

// ---- default segment resolver: /proc/self/maps (readable/host_exec/containment) + dl_iterate_phdr
//      (ELF PT_LOAD PF_X for translated guest code the host mapped read-only). NOT exercised by the
//      fixture harness; UNVERIFIED on the real Houdini mapping (contract note 4).
#if defined(__linux__)
struct PhdrCtx { uintptr_t addr; uintptr_t end; bool exec; };
int phdr_cb(struct dl_phdr_info *info, size_t, void *data) {
  auto *c = static_cast<PhdrCtx *>(data);
  for (int i = 0; i < info->dlpi_phnum; ++i) {
    const ElfW(Phdr) &ph = info->dlpi_phdr[i];
    if (ph.p_type != PT_LOAD) continue;
    uintptr_t base=static_cast<uintptr_t>(info->dlpi_addr);
    if(ph.p_vaddr>UINTPTR_MAX-base)continue;
    uintptr_t lo=base+ph.p_vaddr;
    if(contains(lo,ph.p_memsz,c->addr,c->end-c->addr)&&(ph.p_flags&PF_X)){c->exec=true;return 1;}
  }
  return 0;
}
int default_seg(void *, const void *addr, uint64_t size, PgrMetaSegment *out) {
  std::memset(out, 0, sizeof(*out));
  uintptr_t a = reinterpret_cast<uintptr_t>(addr);
  if(!range(a,size))return -1;
  uintptr_t end = a + size;
  FILE *fp = std::fopen("/proc/self/maps", "re");
  if (fp == nullptr) return -1;
  char line[512];
  while (std::fgets(line, sizeof(line), fp)) {
    uintptr_t lo = 0, hi = 0;
    char perms[8] = {0};
    if (std::sscanf(line, "%zx-%zx %7s", &lo, &hi, perms) != 3) continue;
    if (a < lo || end > hi) continue;
    out->found = 1;
    out->readable = (perms[0] == 'r') ? 1u : 0u;
    out->writable = (perms[1] == 'w') ? 1u : 0u;
    out->host_exec = (perms[2] == 'x') ? 1u : 0u;
    out->seg_base = reinterpret_cast<const void *>(lo);
    out->seg_size = hi - lo;
    break;
  }
  std::fclose(fp);
  if (!out->found) return 0;
  if (!out->host_exec && !out->writable) {  // guest code under Houdini: confirm an ELF PT_LOAD PF_X covers the range
    PhdrCtx c{a, end, false};
    dl_iterate_phdr(phdr_cb, &c);
    out->elf_exec = c.exec ? 1u : 0u;
  }
  return 0;
}
#else
int default_seg(void *, const void *, uint64_t, PgrMetaSegment *out) {
  std::memset(out, 0, sizeof(*out));
  return -1;  // no supported resolver off-Linux; callers must inject seg_query
}
#endif

// ---- core executable + prologue check (quick-close executable() + checkBytes) ------------------
int check_code(const void *address, uint64_t length, const void *prologue, uint32_t prologue_len) {
  if (address == nullptr || length == 0 || length > PGR_MD_CODE_MAX) return PGR_MD_ERR_CODE_RANGE;
  uintptr_t a = reinterpret_cast<uintptr_t>(address);
  if ((a & 0x3u) != 0) return PGR_MD_ERR_CODE_RANGE;  // 4-byte instruction alignment
  uintptr_t base = reinterpret_cast<uintptr_t>(g.module_base);
  if (!contains(base,g.module_size,a,length)) return PGR_MD_ERR_CODE_RANGE;

  PgrMetaSegment seg{};
  if (g.seg_query(g.seg_ctx, address, length, &seg) != 0) return PGR_MD_ERR_SEGMENT;
  if(seg.found!=1||seg.readable!=1||seg.host_exec>1||seg.elf_exec>1||seg.writable>1||seg.reserved||
     !(seg.host_exec||(!seg.writable&&seg.elf_exec)))return PGR_MD_ERR_SEGMENT;
  uintptr_t sbase = reinterpret_cast<uintptr_t>(seg.seg_base);
  if (!contains(sbase,seg.seg_size,a,length)) return PGR_MD_ERR_SEGMENT;

  if (prologue_len != 0) {
    if (prologue == nullptr || prologue_len > PGR_MD_PROLOGUE_MAX || prologue_len > length)
      return PGR_MD_ERR_PROLOGUE;
    if (std::memcmp(address, prologue, prologue_len) != 0) return PGR_MD_ERR_PROLOGUE;
  }
  return PGR_MD_OK;
}

// ================================================================================================
int api_bind(const PgrMetaBindConfig *config) {
  ApiGuard guard;if(!guard)return PGR_MD_ERR_BUSY;
  if (config == nullptr || config->exports == nullptr) return fail(PGR_MD_ERR_NULL_ARG);
  if (config->version != PGR_MD_CONFIG_VERSION) return fail(PGR_MD_ERR_BAD_VERSION);
  if (config->size != sizeof(PgrMetaBindConfig)) return fail(PGR_MD_ERR_BAD_SIZE);
  if (config->module_base == nullptr || config->module_size == 0) return fail(PGR_MD_ERR_MISSING_EXPORT);
  if(!range(reinterpret_cast<uintptr_t>(config->module_base),config->module_size))return fail(PGR_MD_ERR_CODE_RANGE);
  if(config->reserved)return fail(PGR_MD_ERR_BAD_SIZE);
  if(config->debug_prologue_len>PGR_MD_PROLOGUE_MAX||
    (config->debug_prologue_len&&!config->debug_prologue))return fail(PGR_MD_ERR_PROLOGUE);

  const void *const *slots = reinterpret_cast<const void *const *>(config->exports);
  const size_t count = sizeof(PgrMetaExports) / sizeof(void *);
  for (size_t i = 0; i < count; ++i)
    if (slots[i] == nullptr) return fail(PGR_MD_ERR_MISSING_EXPORT);

  pgr_meta_seg_fn seg = config->seg_query ? config->seg_query : &default_seg;

  if (g.bound) {  // immutable runtime identity; optional code proof belongs to this call
    if (std::memcmp(&g.exports, config->exports, sizeof(PgrMetaExports)) != 0 ||
        g.module_base != config->module_base || g.module_size != config->module_size ||
        g.seg_query != seg || g.seg_ctx != config->seg_ctx)
      return fail(PGR_MD_ERR_BOUND_MISMATCH);
    // Bootstrap54 binds early without a game-version fingerprint. Later schema
    // owners must be able to demand their own proof without rebinding exports.
    // Revalidate every supplied proof, including identical repeated requests.
    if(config->debug_prologue_len){
      const int rc=check_code(config->exports->debug_get_method_info,config->debug_prologue_len,
                              config->debug_prologue,config->debug_prologue_len);
      if(rc)return fail(rc);
    }
    return PGR_MD_OK;
  }

  const State previous=g;
  g.exports = *config->exports;
  g.module_base = config->module_base;
  g.module_size = config->module_size;
  g.seg_query = seg;
  g.seg_ctx = config->seg_ctx;

  // Optional: prove debug_get_method_info itself is an in-module executable segment with the
  // supplied bytes (quick-close executable(module,debugAddress,80) + DEBUG word compare).
  if (config->debug_prologue_len != 0) {
    int rc = check_code(config->exports->debug_get_method_info, config->debug_prologue_len,
                        config->debug_prologue, config->debug_prologue_len);
    if (rc != PGR_MD_OK) { g = previous; return fail(rc); }
  }

  g.bound = true;
  g.sample.bound = 1;
  return PGR_MD_OK;
}

int api_resolve_class(const PgrMetaClassQuery *q, PgrMetaClassResult *out) {
  if(out)*out={};
  ApiGuard guard;if(!guard)return PGR_MD_ERR_BUSY;
  if (q == nullptr || out == nullptr) return fail(PGR_MD_ERR_NULL_ARG);
  if (q->version != PGR_MD_QUERY_VERSION) return fail(PGR_MD_ERR_BAD_VERSION);
  if (q->size != sizeof(PgrMetaClassQuery)) return fail(PGR_MD_ERR_BAD_SIZE);
  if (!g.bound) return fail(PGR_MD_ERR_NOT_BOUND);

  std::string ns, name;
  if (!read_name(q->name_space ? q->name_space : "", &ns) || !read_name(q->name, &name))
    return fail(PGR_MD_ERR_NAME_TOO_LONG);

  std::string expected;
  uint32_t expected_mode;
  if (q->canonical != nullptr) {
    if (!read_name(q->canonical, &expected)) return fail(PGR_MD_ERR_NAME_TOO_LONG);
    expected_mode = q->canonical_match ? q->canonical_match : PGR_MD_MATCH_EXACT;
  } else {
    expected = ns.empty() ? name : ns + "." + name;
    expected_mode = PGR_MD_MATCH_EXACT;
  }

  auto domain_get = reinterpret_cast<fn_domain_get>(g.exports.domain_get);
  auto assemblies = reinterpret_cast<fn_domain_assemblies>(g.exports.domain_get_assemblies);
  auto asm_image = reinterpret_cast<fn_assembly_image>(g.exports.assembly_get_image);
  auto from_name = reinterpret_cast<fn_class_from_name>(g.exports.class_from_name);
  auto get_type = reinterpret_cast<fn_class_get_type>(g.exports.class_get_type);

  void *domain = domain_get();
  if (domain == nullptr) return fail(PGR_MD_ERR_NOT_FOUND);
  size_t total = 0;
  void **list = static_cast<void **>(assemblies(domain, &total));
  if (total == 0 || list == nullptr) return fail(PGR_MD_ERR_NOT_FOUND);
  if (total > PGR_MD_ASM_MAX) return fail(PGR_MD_ERR_LIMIT);

  void *found = nullptr;
  for (size_t i = 0; i < total; ++i) {
    void *image = asm_image(list[i]);
    if (image == nullptr) continue;
    void *k = from_name(image, ns.c_str(), name.c_str());
    if (k == nullptr) continue;
    std::string full;
    if (!type_name(get_type(k), &full)) return fail(PGR_MD_ERR_NULL_TYPE_NAME);
    if (!match(full, expected.c_str(), expected_mode)) return fail(PGR_MD_ERR_NAME_MISMATCH);
    if (found != nullptr && found != k) return fail(PGR_MD_ERR_DUPLICATE);
    found = k;
  }
  if (found == nullptr) return fail(PGR_MD_ERR_NOT_FOUND);

  out->version = PGR_MD_QUERY_VERSION;
  out->size = sizeof(PgrMetaClassResult);
  out->klass = found;
  out->type = get_type(found);
  ++g.sample.classes_resolved;
  return PGR_MD_OK;
}

int api_validate_field(const PgrMetaFieldQuery *q, PgrMetaFieldResult *out) {
  if(out)*out={};
  ApiGuard guard;if(!guard)return PGR_MD_ERR_BUSY;
  if (q == nullptr || out == nullptr || q->owner == nullptr || q->name == nullptr)
    return fail(PGR_MD_ERR_NULL_ARG);
  if (q->version != PGR_MD_QUERY_VERSION) return fail(PGR_MD_ERR_BAD_VERSION);
  if (q->size != sizeof(PgrMetaFieldQuery)) return fail(PGR_MD_ERR_BAD_SIZE);
  if (!g.bound) return fail(PGR_MD_ERR_NOT_BOUND);

  std::string name;
  if (!read_name(q->name, &name)) return fail(PGR_MD_ERR_NAME_TOO_LONG);

  auto field_from_name = reinterpret_cast<fn_field_from_name>(g.exports.class_get_field_from_name);
  auto field_parent = reinterpret_cast<fn_field_parent>(g.exports.field_get_parent);
  auto field_flags = reinterpret_cast<fn_field_flags>(g.exports.field_get_flags);
  auto field_offset = reinterpret_cast<fn_field_offset>(g.exports.field_get_offset);
  auto field_type = reinterpret_cast<fn_field_type>(g.exports.field_get_type);
  auto class_from_type = reinterpret_cast<fn_class_from_type>(g.exports.class_from_type);
  auto is_valuetype = reinterpret_cast<fn_class_bool>(g.exports.class_is_valuetype);
  auto is_enum = reinterpret_cast<fn_class_bool>(g.exports.class_is_enum);
  auto enum_base = reinterpret_cast<fn_class_enum_base>(g.exports.class_enum_basetype);
  auto value_size = reinterpret_cast<fn_class_value_size>(g.exports.class_value_size);

  void *owner = const_cast<void *>(q->owner);
  void *f = field_from_name(owner, name.c_str());
  if (f == nullptr) return fail(PGR_MD_ERR_NOT_FOUND);
  if (field_parent(f) != owner) return fail(PGR_MD_ERR_OWNER_MISMATCH);

  uint32_t flags = static_cast<uint32_t>(field_flags(f));
  if ((flags & q->forbidden_mask) != 0) return fail(PGR_MD_ERR_FLAGS);
  if ((flags & q->required_mask) != q->required_mask) return fail(PGR_MD_ERR_FLAGS);

  uint64_t offset = static_cast<uint64_t>(field_offset(f));
  if (offset != q->offset) return fail(PGR_MD_ERR_OFFSET);

  void *type = field_type(f);
  void *type_class = class_from_type(type);
  uint32_t vsize = 0;

  switch (q->kind) {
    case PGR_MD_FIELD_ANY:
      break;
    case PGR_MD_FIELD_REF:
      if (type_class == nullptr || is_valuetype(type_class)) return fail(PGR_MD_ERR_KIND);
      break;
    case PGR_MD_FIELD_VALUE: {
      if (type_class == nullptr || !is_valuetype(type_class)) return fail(PGR_MD_ERR_KIND);
      int align = 0;
      int sz = value_size(type_class, &align);
      if (q->value_size != 0) { if (sz != static_cast<int>(q->value_size)) return fail(PGR_MD_ERR_SIZE); }
      else if (sz != 4 && sz != 8) return fail(PGR_MD_ERR_SIZE);
      vsize = static_cast<uint32_t>(sz);
      break;
    }
    case PGR_MD_FIELD_ENUM_I32: {
      if (type_class == nullptr || !is_enum(type_class)) return fail(PGR_MD_ERR_KIND);
      std::string base;
      if (!type_name(enum_base(type_class), &base)) return fail(PGR_MD_ERR_NULL_TYPE_NAME);
      if (base != "System.Int32") return fail(PGR_MD_ERR_KIND);
      int align = 0;
      if (value_size(type_class, &align) != 4 || align != 4) return fail(PGR_MD_ERR_SIZE);
      vsize = 4;
      break;
    }
    default:
      return fail(PGR_MD_ERR_KIND);
  }

  if (q->type_name != nullptr && q->type_match != PGR_MD_MATCH_ANY) {
    std::string tn;
    if (!type_name(type, &tn)) return fail(PGR_MD_ERR_NULL_TYPE_NAME);
    if (!match(tn, q->type_name, q->type_match)) return fail(PGR_MD_ERR_TYPE);
  }

  out->version = PGR_MD_QUERY_VERSION;
  out->size = sizeof(PgrMetaFieldResult);
  out->field = f;
  out->type = type;
  out->type_class = type_class;
  out->offset = offset;
  out->flags = flags;
  out->value_size = vsize;
  ++g.sample.fields_validated;
  return PGR_MD_OK;
}

int api_validate_method(const PgrMetaMethodQuery *q, PgrMetaMethodResult *out) {
  if(out)*out={};
  ApiGuard guard;if(!guard)return PGR_MD_ERR_BUSY;
  if (q == nullptr || out == nullptr || q->owner == nullptr || q->name == nullptr)
    return fail(PGR_MD_ERR_NULL_ARG);
  if (q->version != PGR_MD_QUERY_VERSION) return fail(PGR_MD_ERR_BAD_VERSION);
  if (q->size != sizeof(PgrMetaMethodQuery)) return fail(PGR_MD_ERR_BAD_SIZE);
  if (!g.bound) return fail(PGR_MD_ERR_NOT_BOUND);
  if (q->param_count > PGR_MD_PARAM_MAX) return fail(PGR_MD_ERR_LIMIT);

  std::string name;
  if (!read_name(q->name, &name)) return fail(PGR_MD_ERR_NAME_TOO_LONG);

  auto method_from_name = reinterpret_cast<fn_method_from_name>(g.exports.class_get_method_from_name);
  auto method_class = reinterpret_cast<fn_method_class>(g.exports.method_get_class);
  auto method_name = reinterpret_cast<fn_method_name>(g.exports.method_get_name);
  auto method_flags = reinterpret_cast<fn_method_flags>(g.exports.method_get_flags);
  auto method_pcount = reinterpret_cast<fn_method_param_count>(g.exports.method_get_param_count);
  auto method_param = reinterpret_cast<fn_method_param>(g.exports.method_get_param);
  auto method_return = reinterpret_cast<fn_method_return>(g.exports.method_get_return_type);
  auto method_is_generic = reinterpret_cast<fn_method_is>(g.exports.method_is_generic);
  auto method_is_inflated = reinterpret_cast<fn_method_is>(g.exports.method_is_inflated);
  auto debug_info = reinterpret_cast<fn_debug_method_info>(g.exports.debug_get_method_info);

  void *owner = const_cast<void *>(q->owner);
  void *m = method_from_name(owner, name.c_str(), static_cast<int>(q->param_count));
  if (m == nullptr) return fail(PGR_MD_ERR_NOT_FOUND);
  if (method_class(m) != owner) return fail(PGR_MD_ERR_OWNER_MISMATCH);

  std::string actual_name;
  if (!read_name(method_name(m), &actual_name)) return fail(PGR_MD_ERR_NAME_TOO_LONG);
  if (actual_name != name) return fail(PGR_MD_ERR_NAME_MISMATCH);

  uint32_t iflags = 0;
  uint32_t flags = method_flags(m, &iflags);
  bool is_static = (flags & 0x10u) != 0;  // METHOD_ATTRIBUTE_STATIC
  if (is_static != (q->is_static != 0)) return fail(PGR_MD_ERR_STATIC);
  if (method_pcount(m) != q->param_count) return fail(PGR_MD_ERR_PARAM_COUNT);
  if (method_is_generic(m)) return fail(PGR_MD_ERR_GENERIC);          // always rejected
  if (q->allow_inflated == 0 && method_is_inflated(m)) return fail(PGR_MD_ERR_INFLATED);

  if (q->return_type != nullptr && q->return_match != PGR_MD_MATCH_ANY) {
    std::string rn;
    if (!type_name(method_return(m), &rn)) return fail(PGR_MD_ERR_NULL_TYPE_NAME);
    if (!match(rn, q->return_type, q->return_match)) return fail(PGR_MD_ERR_TYPE);
  }
  for (uint32_t i = 0; i < q->param_count; ++i) {
    if (q->param_types[i] == nullptr || q->param_match[i] == PGR_MD_MATCH_ANY) continue;
    std::string pn;
    if (!type_name(method_param(m, i), &pn)) return fail(PGR_MD_ERR_NULL_TYPE_NAME);
    if (!match(pn, q->param_types[i], q->param_match[i])) return fail(PGR_MD_ERR_TYPE);
  }

  const void *code = nullptr;
  if (q->expected_code != nullptr) {
    if (q->prologue == nullptr || q->prologue_len == 0) return fail(PGR_MD_ERR_NULL_ARG);
    void *info[3] = {nullptr, nullptr, nullptr};  // >= 24 bytes (quick-close info buffer); addr at [0]
    if (!debug_info(m, info)) return fail(PGR_MD_ERR_CODE_RANGE);
    code = info[0];
    if (code != q->expected_code) return fail(PGR_MD_ERR_CODE_RANGE);
    int rc = check_code(code, q->prologue_len, q->prologue, q->prologue_len);
    if (rc != PGR_MD_OK) return fail(rc);
    ++g.sample.code_checks;
  }

  out->version = PGR_MD_QUERY_VERSION;
  out->size = sizeof(PgrMetaMethodResult);
  out->method = m;
  out->code = code;
  out->flags = flags;
  out->iflags = iflags;
  out->param_count = q->param_count;
  ++g.sample.methods_validated;
  return PGR_MD_OK;
}

int api_validate_code(const PgrMetaCodeQuery *q) {
  ApiGuard guard;if(!guard)return PGR_MD_ERR_BUSY;
  if (q == nullptr) return fail(PGR_MD_ERR_NULL_ARG);
  if (q->version != PGR_MD_QUERY_VERSION) return fail(PGR_MD_ERR_BAD_VERSION);
  if (q->size != sizeof(PgrMetaCodeQuery)) return fail(PGR_MD_ERR_BAD_SIZE);
  if (!g.bound) return fail(PGR_MD_ERR_NOT_BOUND);
  int rc = check_code(q->address, q->length, q->prologue, q->prologue_len);
  if (rc != PGR_MD_OK) return fail(rc);
  ++g.sample.code_checks;
  return PGR_MD_OK;
}

int api_resolve_module(const char *soname, const void **base, uint64_t *size) {
  if(base)*base=nullptr;
  if(size)*size=0;
  ApiGuard guard;if(!guard)return PGR_MD_ERR_BUSY;
  if (soname == nullptr || base == nullptr || size == nullptr) return fail(PGR_MD_ERR_NULL_ARG);
  *base = nullptr;
  *size = 0;
  std::string requested;if(!read_name(soname,&requested)||requested.empty())return fail(PGR_MD_ERR_NAME_TOO_LONG);
#if defined(__linux__)
  struct Ctx {const std::string *name;const void *base;uint64_t size;unsigned matches;bool invalid;} ctx{&requested,nullptr,0,0,false};
  dl_iterate_phdr([](struct dl_phdr_info *info,size_t,void *data)->int{
    auto *c=static_cast<Ctx *>(data);if(!info->dlpi_name)return 0;
    // Loader-owned path; bound even this string before exact comparison.
    size_t n=strnlen(info->dlpi_name,4096);if(n==4096)return 0;
    std::string full(info->dlpi_name,n);size_t slash=full.find_last_of('/');
    std::string basename=slash==std::string::npos?full:full.substr(slash+1);
    if(*c->name!=full&&*c->name!=basename)return 0;
    ++c->matches;uintptr_t lo=UINTPTR_MAX,hi=0;
    for(int i=0;i<info->dlpi_phnum;i++){
      const auto &ph=info->dlpi_phdr[i];if(ph.p_type!=PT_LOAD||!ph.p_memsz)continue;
      uintptr_t bias=static_cast<uintptr_t>(info->dlpi_addr);
      if(ph.p_vaddr>UINTPTR_MAX-bias){c->invalid=true;continue;}
      uintptr_t s=bias+ph.p_vaddr;
      if(!range(s,ph.p_memsz)){c->invalid=true;continue;}
      if(s<lo)lo=s;if(s+ph.p_memsz>hi)hi=s+ph.p_memsz;
    }
    if(hi>lo){c->base=reinterpret_cast<const void *>(lo);c->size=hi-lo;}else c->invalid=true;
    return 0; // scan ALL loaded images, never silently select first duplicate
  },&ctx);
  if(ctx.matches>1)return fail(PGR_MD_ERR_DUPLICATE);
  if(ctx.invalid)return fail(PGR_MD_ERR_CODE_RANGE);
  if(!ctx.base)return fail(PGR_MD_ERR_NOT_FOUND);
  *base=ctx.base;*size=ctx.size;return PGR_MD_OK;
#else
  return fail(PGR_MD_ERR_NOT_FOUND);
#endif
}

int api_sample(PgrMetaSample *out) {
  if(out)*out={};
  ApiGuard guard;if(!guard)return PGR_MD_ERR_BUSY;
  if (out == nullptr) return PGR_MD_ERR_NULL_ARG;
  g.sample.version = PGR_MD_SAMPLE_VERSION;
  g.sample.size = sizeof(PgrMetaSample);
  *out = g.sample;
  return PGR_MD_OK;
}

const PgrMetaApi g_api = {
    PGR_MD_API_VERSION,
    sizeof(PgrMetaApi),
    api_bind,
    api_resolve_class,
    api_validate_field,
    api_validate_method,
    api_validate_code,
    api_resolve_module,
    api_sample,
};

}  // namespace

const void *pgrazor_runtime_metadata_api(uint32_t version) {
  return version == PGR_MD_API_VERSION ? &g_api : nullptr;
}
