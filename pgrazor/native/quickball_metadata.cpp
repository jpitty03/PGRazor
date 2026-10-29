#include "quickball_metadata.h"
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unistd.h>

namespace {
using namespace pgr_quickball_schema;

std::mutex g_resolving;

const PgrMetaApi *metadata() {
  return static_cast<const PgrMetaApi *>(pgrazor_runtime_metadata_api(1));
}

bool attached(const PgrQuickballMetaConfig &c) {
  return c.owner_tid && c.owner_tid == static_cast<uint64_t>(gettid()) &&
         reinterpret_cast<void *(*)()>(c.thread_current)() != nullptr;
}

bool read_name(const char *p, std::string *out) {
  if (!p) return false;
  size_t n = 0;
  while (n < PGR_MD_NAME_MAX && p[n] != '\0') ++n;
  if (n == PGR_MD_NAME_MAX) return false;
  out->assign(p, n);
  return true;
}

bool type_name(const PgrQuickballMetaConfig &c, const void *type, std::string *out) {
  if (!type) return false;
  auto get = reinterpret_cast<char *(*)(const void *)>(c.binding.exports->type_get_name);
  auto freep = reinterpret_cast<void (*)(void *)>(c.binding.exports->mem_free);
  char *raw = get(type);
  if (!raw) return false;
  bool ok = read_name(raw, out);
  freep(raw);
  return ok;
}

// Decode up to cap bytes of hex. Returns the byte length, or -1 on a malformed/overlong string.
int decode_hex(const char *hex, unsigned char *out, int cap) {
  if (!hex) return -1;
  int n = static_cast<int>(std::strlen(hex));
  if (n == 0 || (n & 1) || n / 2 > cap) return -1;
  for (int i = 0; i < n; i++) {
    char ch = hex[i];
    int v = (ch >= '0' && ch <= '9') ? ch - '0'
            : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10
            : (ch >= 'A' && ch <= 'F') ? ch - 'A' + 10
                                       : -1;
    if (v < 0) return -1;
    if (i & 1) out[i / 2] |= static_cast<unsigned char>(v);
    else out[i / 2] = static_cast<unsigned char>(v << 4);
  }
  return n / 2;
}

// Validate one schema method (signature + optional pinned code address + prologue) via feature47.
int validate_method(const PgrMetaApi *md, const PgrQuickballMetaConfig &c, const MethodSpec &s,
                    const void *owner, const void **out, const void **code_out) {
  PgrMetaMethodQuery q{};
  q.version = 1;
  q.size = sizeof(q);
  q.owner = owner;
  q.name = s.name;
  q.return_type = s.return_type;
  q.return_match = s.return_type ? s.return_match : PGR_MD_MATCH_ANY;
  q.param_count = s.param_count;
  if (s.param_count >= 1) { q.param_types[0] = s.param0; q.param_match[0] = PGR_MD_MATCH_EXACT; }
  if (s.param_count >= 2) {
    q.param_types[1] = s.param1;
    // GetGroundPosition param1 is checked for exact Vector3(&) + type_is_byref by check_map_ground_byref.
    q.param_match[1] = (s.owner == C_MAPVIEW && std::strcmp(s.name, "GetGroundPosition") == 0)
                           ? PGR_MD_MATCH_PREFIX
                           : PGR_MD_MATCH_EXACT;
  }
  q.is_static = s.is_static;
  q.allow_inflated = 0;  // generic always rejected; quick-ball never resolves inflated here
  unsigned char prologue[PGR_MD_PROLOGUE_MAX];
  if (s.prologue) {
    int len = decode_hex(s.prologue, prologue, sizeof(prologue));
    if (len <= 0) return -32;
    const uintptr_t b = reinterpret_cast<uintptr_t>(c.binding.module_base);
    if (b > UINTPTR_MAX - s.rva) return -33;
    q.expected_code = reinterpret_cast<const void *>(b + s.rva);
    q.prologue = prologue;
    q.prologue_len = static_cast<uint32_t>(len);
  }
  PgrMetaMethodResult r{};
  int rc = md->validate_method(&q, &r);
  if (rc) return rc;
  // TryCapture return must match /^Niantic\.Promises\.IPromise(?:`1)?<Holoholo\.Rpc\.CatchPokemonOutProto>$/.
  if (s.owner == C_MAPPOKEMON && std::strcmp(s.name, "TryCapture") == 0) {
    auto get_ret = reinterpret_cast<void *(*)(const void *)>(c.binding.exports->method_get_return_type);
    std::string rn;
    if (!type_name(c, get_ret(r.method), &rn)) return PGR_MD_ERR_NULL_TYPE_NAME;
    if (rn != "Niantic.Promises.IPromise<Holoholo.Rpc.CatchPokemonOutProto>" &&
        rn != "Niantic.Promises.IPromise`1<Holoholo.Rpc.CatchPokemonOutProto>")
      return PGR_MD_ERR_TYPE;
  }
  *out = r.method;
  if (code_out) *code_out = r.code;
  return 0;
}

int validate_field(const PgrMetaApi *md, const FieldSpec &s, const void *owner, const void **out,
                   const void **type_class) {
  PgrMetaFieldQuery q{};
  q.version = 1;
  q.size = sizeof(q);
  q.owner = owner;
  q.name = s.name;
  q.offset = s.offset;
  q.forbidden_mask = s.forbidden_mask;
  q.required_mask = s.required_mask;
  q.kind = s.kind;
  q.value_size = s.value_size;
  q.type_name = s.type_name;
  q.type_match = s.type_name ? PGR_MD_MATCH_EXACT : PGR_MD_MATCH_ANY;
  PgrMetaFieldResult r{};
  int rc = md->validate_field(&q, &r);
  if (!rc) { *out = r.field; if (type_class) *type_class = r.type_class; }
  return rc;
}

void set_reason(PgrQuickballMetaGroup &g, const char *fmt, const char *arg) {
  g.available = 0;
  std::snprintf(g.reason, sizeof(g.reason), fmt, arg ? arg : "");
}

// Bare-class value-type ABI check: class_is_valuetype(klass) && class_value_size(klass, &align) == size && align == expected.
int check_value_size(const PgrQuickballMetaConfig &c, const void *klass, uint32_t size, uint32_t expected_align) {
  auto is_vt = reinterpret_cast<bool (*)(const void *)>(c.binding.exports->class_is_valuetype);
  auto val_sz = reinterpret_cast<int (*)(const void *, int *)>(c.binding.exports->class_value_size);
  if (!klass || !is_vt(klass)) return PGR_MD_ERR_KIND;
  int align = 0;
  int sz = val_sz(klass, &align);
  if (sz != static_cast<int>(size) || align != static_cast<int>(expected_align)) return PGR_MD_ERR_SIZE;
  return 0;
}

// Static enum literal validation + value read (prepare lines 165..167, 172..174, 276):
// field_from_name(owner, name) non-null, parent == owner, (flags & 0x8050) == 0x8050, typeName == expected_type,
// then field_static_get_value(f, out_val).
bool read_enum_literal(const PgrQuickballMetaConfig &c, const void *owner, const char *expected_type,
                       const char *lit_name, int32_t *out_val) {
  auto field_by_name = reinterpret_cast<void *(*)(const void *, const char *)>(c.binding.exports->class_get_field_from_name);
  auto field_parent = reinterpret_cast<void *(*)(const void *)>(c.binding.exports->field_get_parent);
  auto field_flags = reinterpret_cast<int (*)(const void *)>(c.binding.exports->field_get_flags);
  auto field_type = reinterpret_cast<void *(*)(const void *)>(c.binding.exports->field_get_type);
  auto static_get = reinterpret_cast<void (*)(const void *, void *)>(c.extra->field_static_get_value);
  if (!owner) return false;
  void *f = field_by_name(owner, lit_name);
  if (!f || field_parent(f) != owner || (static_cast<uint32_t>(field_flags(f)) & 0x8050u) != 0x8050u)
    return false;
  std::string tn;
  if (!type_name(c, field_type(f), &tn) || tn != expected_type) return false;
  int32_t v = 0;
  static_get(f, &v);
  *out_val = v;
  return true;
}

const void *add_off(const void *p, uint64_t off) {
  uintptr_t a = reinterpret_cast<uintptr_t>(p);
  if (!p || a > UINTPTR_MAX - off) return nullptr;
  return reinterpret_cast<const void *>(a + off);
}

bool code_readable(const PgrMetaApi *md, const void *addr, uint64_t len) {
  if (!addr) return false;
  PgrMetaCodeQuery q{};
  q.version = 1;
  q.size = sizeof(q);
  q.address = addr;
  q.length = len;
  return md->validate_code(&q) == 0;
}

bool code_words(const PgrMetaApi *md, const void *addr, const uint32_t *words, uint32_t count) {
  if (!addr || !words || !count) return false;
  PgrMetaCodeQuery q{};
  q.version = 1;
  q.size = sizeof(q);
  q.address = addr;
  q.length = static_cast<uint64_t>(count) * 4u;
  q.prologue = words;
  q.prologue_len = count * 4u;
  return md->validate_code(&q) == 0;
}

const void *branch_target(const PgrMetaApi *md, const void *addr) {
  if (!code_readable(md, addr, 4)) return nullptr;
  uint32_t word = *reinterpret_cast<const uint32_t *>(addr);
  if ((word >> 26) != 5u) return nullptr;  // AArch64 B imm26, never BL or indirect
  int64_t imm = static_cast<int64_t>(word & 0x03ffffffu);
  if (imm & 0x02000000LL) imm -= 0x04000000LL;
  int64_t byte_off = imm * 4;
  uintptr_t base = reinterpret_cast<uintptr_t>(addr);
  uintptr_t target = 0;
  if (byte_off >= 0) {
    uint64_t uoff = static_cast<uint64_t>(byte_off);
    if (base > UINTPTR_MAX - uoff) return nullptr;
    target = base + static_cast<uintptr_t>(uoff);
  } else {
    uint64_t uoff = static_cast<uint64_t>(-byte_off);
    if (base < uoff) return nullptr;
    target = base - static_cast<uintptr_t>(uoff);
  }
  const void *tptr = reinterpret_cast<const void *>(target);
  return code_readable(md, tptr, 4) ? tptr : nullptr;
}

// pointerHandleAbi (prepare lines 109..140): read-only code fingerprint of il2cpp_gchandle_new /
// _get_target / _free. No GC handle is created.
bool pointer_handle_abi(const PgrMetaApi *md, const PgrQuickballExtra *ex) {
  if (!code_words(md, ex->gchandle_new, kGcCreateWords, kGcCreateWordCount)) return false;
  const void *create_body = branch_target(md, add_off(ex->gchandle_new, kGcCreateBranchOff));
  const void *get_body = branch_target(md, ex->gchandle_get_target);
  const void *free_body = branch_target(md, ex->gchandle_free);
  if (!create_body || !get_body || !free_body) return false;
  return code_words(md, add_off(create_body, kGcCreateBodyOff), &kGcCreateBodyWord, 1) &&
         code_words(md, get_body, &kGcGetHead, 1) &&
         code_words(md, add_off(get_body, kGcGetTailOff), kGcGetTail, 2) &&
         code_words(md, free_body, &kGcFreeHead, 1) &&
         code_words(md, add_off(free_body, kGcFreeTailOff), kGcFreeTail, 2);
}

// GetBool(Int32) / GetInteger(Int32) overload disambiguation (prepare lines 196..203).
bool resolve_overload(const PgrQuickballMetaConfig &c, const void *owner, const char *want_name,
                      const char *want_ret, PgrQuickballMetaGroup &grp, const void **out_method) {
  auto get_methods = reinterpret_cast<void *(*)(const void *, void **)>(c.binding.exports->class_get_methods);
  auto get_pcount = reinterpret_cast<uint32_t (*)(const void *)>(c.binding.exports->method_get_param_count);
  auto get_mname = reinterpret_cast<const char *(*)(const void *)>(c.binding.exports->method_get_name);
  auto get_param = reinterpret_cast<void *(*)(const void *, uint32_t)>(c.binding.exports->method_get_param);
  auto get_mclass = reinterpret_cast<void *(*)(const void *)>(c.binding.exports->method_get_class);
  auto get_mflags = reinterpret_cast<uint32_t (*)(const void *, uint32_t *)>(c.binding.exports->method_get_flags);
  auto is_generic = reinterpret_cast<bool (*)(const void *)>(c.binding.exports->method_is_generic);
  auto is_inflated = reinterpret_cast<bool (*)(const void *)>(c.binding.exports->method_is_inflated);
  auto get_ret = reinterpret_cast<void *(*)(const void *)>(c.binding.exports->method_get_return_type);

  void *iter = nullptr;
  const void *found = nullptr;
  for (int i = 0;; i++) {
    if (i >= 2048) { set_reason(grp, "method-limit:%s", want_name); return false; }
    void *m = get_methods(owner, &iter);
    if (!m) break;
    if (get_pcount(m) != 1) continue;
    std::string mname;
    if (!read_name(get_mname(m), &mname)) { set_reason(grp, "name-limit:%s", want_name); return false; }
    if (mname != want_name) continue;
    std::string pname;
    if (!type_name(c, get_param(m, 0), &pname)) { set_reason(grp, "null-type-name:%s", want_name); return false; }
    if (pname != "System.Int32") continue;
    if (found) { set_reason(grp, "overload-duplicate:%s", want_name); return false; }
    found = m;
  }
  if (!found) { set_reason(grp, "overload-missing:%s", want_name); return false; }
  uint32_t impl = 0;
  std::string rname;
  if (get_mclass(found) != owner || (get_mflags(found, &impl) & 16u) != 0 ||
      is_generic(found) || is_inflated(found) ||
      !type_name(c, get_ret(found), &rname) || rname != want_ret) {
    set_reason(grp, "method-mismatch:%s", want_name);
    return false;
  }
  *out_method = found;
  return true;
}

// SchedulerStateMachine current-state field scan (prepare lines 235..238).
bool scan_scheduler_current(const PgrQuickballMetaConfig &c, const void *machine,
                            PgrQuickballMetaGroup &grp, const void **out_field) {
  auto get_fields = reinterpret_cast<void *(*)(const void *, void **)>(c.extra->class_get_fields);
  auto get_offset = reinterpret_cast<size_t (*)(const void *)>(c.binding.exports->field_get_offset);
  auto get_flags = reinterpret_cast<int (*)(const void *)>(c.binding.exports->field_get_flags);
  auto get_ftype = reinterpret_cast<void *(*)(const void *)>(c.binding.exports->field_get_type);

  void *iter = nullptr;
  const void *current = nullptr;
  for (int i = 0; i < 128; i++) {
    void *f = get_fields(machine, &iter);
    if (!f) break;
    if (static_cast<uint64_t>(get_offset(f)) == 0x18ULL && (static_cast<uint32_t>(get_flags(f)) & 0x50u) == 0) {
      std::string tn;
      if (!type_name(c, get_ftype(f), &tn)) { set_reason(grp, "always-hit-current-state", nullptr); return false; }
      if (tn.compare(0, 17, "Niantic.Holoholo.") == 0) { current = f; break; }
    }
  }
  if (!current) { set_reason(grp, "always-hit-current-state", nullptr); return false; }
  *out_field = current;
  return true;
}

// MapViewHandler.GetGroundPosition out-param byref check (prepare lines 263..266).
bool check_map_ground_byref(const PgrQuickballMetaConfig &c, const void *map_class,
                            PgrQuickballMetaGroup &grp) {
  auto get_method = reinterpret_cast<void *(*)(const void *, const char *, int)>(c.binding.exports->class_get_method_from_name);
  auto get_pcount = reinterpret_cast<uint32_t (*)(const void *)>(c.binding.exports->method_get_param_count);
  auto get_param = reinterpret_cast<void *(*)(const void *, uint32_t)>(c.binding.exports->method_get_param);
  void *gm = get_method(map_class, "GetGroundPosition", 2);
  if (!gm || !(c.extra->present & PGR_QB_EXP_TYPE_IS_BYREF) || !c.extra->type_is_byref) {
    set_reason(grp, "map-ground-signature", nullptr);
    return false;
  }
  if (get_pcount(gm) < 2) {
    set_reason(grp, "map-ground-byref", nullptr);
    return false;
  }
  auto is_byref = reinterpret_cast<bool (*)(const void *)>(c.extra->type_is_byref);
  void *out_type = get_param(gm, 1);
  std::string out_name;
  if (!out_type || !type_name(c, out_type, &out_name)) {
    set_reason(grp, "map-ground-byref", nullptr);
    return false;
  }
  if ((out_name != "UnityEngine.Vector3" && out_name != "UnityEngine.Vector3&") || !is_byref(out_type)) {
    set_reason(grp, "map-ground-byref", nullptr);
    return false;
  }
  return true;
}

// Required optional classes per group, in exact source need() order (prepare lines 205..324).
struct GroupClassList { const ClassIndex *indices; uint32_t count; };
inline constexpr ClassIndex kTraceClasses[]         = {C_INFO, C_INT32};
inline constexpr ClassIndex kTraceIdentityClasses[] = {C_RAC};
inline constexpr ClassIndex kTraceNameClasses[]     = {C_STRING};
inline constexpr ClassIndex kThrowsClasses[]        = {C_THROW, C_ENCOUNTER};
inline constexpr ClassIndex kAlwaysHitClasses[]     = {C_SCHEDULER, C_ACTION};
inline constexpr ClassIndex kArThrowsClasses[]      = {C_ARVALUES, C_MAPPOKEMON, C_WILDMAP};
inline constexpr ClassIndex kMapTapClasses[]        = {C_GROUND, C_MAPVIEW, C_LATLNG, C_VEC3};
inline constexpr ClassIndex kIvClasses[]            = {C_NAMEPLATE, C_PK, C_MAPPK, C_UITEXT, C_LEVELS, C_INT32,
                                                       C_STRING, C_WILDMAP, C_WILDPROTO, C_ENCOUNTEROUT,
                                                       C_TAPPABLE, C_HWRAP, C_TEXTANCHOR};
inline constexpr ClassIndex kIvTextClasses[]        = {C_UITEXT, C_HWRAP, C_TEXTANCHOR};
inline constexpr ClassIndex kIvOwnedClasses[]       = {C_PANEL};
inline constexpr ClassIndex kIvOutlineClasses[]     = {C_COMPONENT, C_GAMEOBJECT, C_OUTLINE, C_SHADOW,
                                                       C_BEHAVIOUR, C_COLOR, C_VECTOR2};

GroupClassList group_needed_classes(uint32_t g) {
  switch (g) {
    case GROUP_TRACE:          return {kTraceClasses, 2};
    case GROUP_TRACE_IDENTITY: return {kTraceIdentityClasses, 1};
    case GROUP_TRACE_NAME:     return {kTraceNameClasses, 1};
    case GROUP_THROWS:         return {kThrowsClasses, 2};
    case GROUP_ALWAYSHIT:      return {kAlwaysHitClasses, 2};
    case GROUP_ARTHROWS:       return {kArThrowsClasses, 3};
    case GROUP_MAPTAP:         return {kMapTapClasses, 4};
    case GROUP_IV:             return {kIvClasses, 13};
    case GROUP_IVTEXT:         return {kIvTextClasses, 3};
    case GROUP_IVOWNED:        return {kIvOwnedClasses, 1};
    case GROUP_IVOUTLINE:      return {kIvOutlineClasses, 7};
    default:                   return {nullptr, 0};
  }
}

int resolve(const PgrQuickballMetaConfig *input, PgrQuickballMetaOutput *out) {
  if (!out) return -1;
  std::memset(out, 0, sizeof(*out));
  if (!input || input->version != PGR_QB_CFG_VERSION || input->size != sizeof(*input) || !input->thread_current)
    return -2;
  if (!input->extra || input->extra->version != PGR_QB_CFG_VERSION ||
      input->extra->size != sizeof(PgrQuickballExtra) || input->extra->reserved != 0 ||
      !input->extra->field_static_get_value || !input->extra->gchandle_new ||
      !input->extra->gchandle_get_target || !input->extra->gchandle_free)
    return PGR_QB_ERR_EXTRA_MISSING;
  std::unique_lock<std::mutex> lock(g_resolving, std::try_to_lock);
  if (!lock.owns_lock()) return -27;

  PgrQuickballMetaConfig c = *input;
  PgrQuickballExtra extra_copy = *input->extra;
  c.extra = &extra_copy;

  const uintptr_t module_base = reinterpret_cast<uintptr_t>(c.binding.module_base);
  const uintptr_t thread_address = reinterpret_cast<uintptr_t>(c.thread_current);
  if (!module_base || !c.binding.module_size || c.binding.module_size > UINTPTR_MAX - module_base)
    return PGR_MD_ERR_CODE_RANGE;
  // Production path (default resolver): the attached-thread accessor must sit inside the module.
  // An explicit fixture seg_query is the feature47 fixture boundary and may supply outside exports.
  if (!c.binding.seg_query &&
      (thread_address < module_base || thread_address - module_base > c.binding.module_size ||
        4 > c.binding.module_size - (thread_address - module_base)))
    return PGR_MD_ERR_CODE_RANGE;
  if (!attached(c)) return -30;
  const auto *md = metadata();
  if (!md || md->version != 1 || md->size != sizeof(PgrMetaApi)) return -31;

  if (!c.binding.seg_query) {
    // Frida reports a page-rounded mapping; the compiled resolver reports the PT_LOAD extent. Bind
    // the canonical raw extent (matching feature53/feature54/feature56); a diff >= one page is incompatible.
    const void *canonical = nullptr;
    uint64_t extent = 0;
    int status = md->resolve_module("libil2cpp.so", &canonical, &extent);
    if (status) return status;
    const long page = sysconf(_SC_PAGESIZE);
    if (canonical != c.binding.module_base || !extent || extent > c.binding.module_size || page <= 0 ||
        c.binding.module_size - extent >= static_cast<uint64_t>(page))
      return PGR_MD_ERR_CODE_RANGE;
    c.binding.module_size = extent;
    if (!c.binding.debug_prologue && c.binding.debug_prologue_len == 0) {
      c.binding.debug_prologue = kDebugPrologue;
      c.binding.debug_prologue_len = kDebugPrologueLen;
    }
  }
  int rc = md->bind(&c.binding);
  if (rc) return rc;

  PgrQuickballMetaOutput result{};
  result.version = PGR_QB_OUT_VERSION;
  result.size = sizeof(result);
  for (uint32_t g = 0; g < kGroupCount; g++) result.groups[g].available = 1;

  // --- classes: mandatory fatal (mismatch/duplicate takes precedence over not-found), optional conflict-tracked ---
  int mandatory_missing = 0;
  for (uint32_t i = 0; i < kClassCount; i++) {
    if (!attached(c)) return -30;
    const auto &s = kClasses[i];
    PgrMetaClassQuery q{};
    q.version = 1;
    q.size = sizeof(q);
    q.name_space = s.name_space;
    q.name = s.name;
    q.canonical = s.canonical;
    q.canonical_match = PGR_MD_MATCH_EXACT;
    PgrMetaClassResult r{};
    rc = md->resolve_class(&q, &r);
    if (!rc) {
      result.classes[i] = r.klass;
    } else if (!s.optional) {
      if (rc != PGR_MD_ERR_NOT_FOUND) return rc;
      mandatory_missing = rc;
    } else {
      if (rc == PGR_MD_ERR_LIMIT) return rc;
      if (rc != PGR_MD_ERR_NOT_FOUND) result.class_conflict[i] = 1;  // ambiguous/type-mismatched optional class
    }
  }
  if (mandatory_missing) return PGR_QB_NOT_READY;

  const auto class_ok = [&](uint32_t idx) {
    return idx < kClassCount && result.classes[idx] && !result.class_conflict[idx];
  };

  // --- process groups in dependency order (deps precede dependents in the enum) ---
  for (uint32_t g = 0; g < kGroupCount; g++) {
    PgrQuickballMetaGroup &grp = result.groups[g];
    if (g == GROUP_CORE) {
      // 1) Mandatory fields (animator, bread, Status).
      for (uint32_t i = 0; i < kFieldCount; i++) {
        if (kFields[i].group != GROUP_CORE) continue;
        if (!attached(c)) return -30;
        const void *type_class = nullptr;
        rc = validate_field(md, kFields[i], result.classes[kFields[i].owner], &result.fields[i], &type_class);
        if (rc) return rc;
        if (kFields[i].owner == C_RESPONSE && !std::strcmp(kFields[i].name, "Status")) {
          if (!type_class) return PGR_MD_ERR_KIND;
          result.enum_class = type_class;
        }
      }
      // 2) Mandatory CatchSuccess static enum literal (prepare lines 165..167).
      if (!attached(c)) return -30;
      if (!read_enum_literal(c, result.enum_class, kStatusEnumName, "CatchSuccess", &result.success))
        return PGR_QB_ERR_SUCCESS_LIT;
      // 3) Mandatory boxed value-type ABI checks: Single=4/4, Boolean=1/1 (prepare line 177).
      for (uint32_t i = 0; i < kValueSizeCount; i++) {
        if (kValueSizes[i].group != GROUP_CORE) continue;
        if (!attached(c)) return -30;
        rc = check_value_size(c, result.classes[kValueSizes[i].klass], kValueSizes[i].size, kValueSizes[i].align);
        if (rc) return rc;
      }
      // 4) Mandatory pointerHandleAbi fingerprint (prepare line 178).
      if (!attached(c)) return -30;
      if (!pointer_handle_abi(md, c.extra)) return PGR_QB_ERR_GC_POINTER;
      // 5) Mandatory methods (prepare line 188).
      for (uint32_t i = 0; i < kMethodCount; i++) {
        if (kMethods[i].group != GROUP_CORE) continue;
        if (!attached(c)) return -30;
        rc = validate_method(md, c, kMethods[i], result.classes[kMethods[i].owner],
                             &result.methods[i], &result.method_code[i]);
        if (rc) return rc;
      }
      // 6) Mandatory standalone ordinary-ball caller code check (prepare line 189).
      unsigned char prologue[PGR_MD_PROLOGUE_MAX];
      int len = decode_hex(kCallerCheckPrologue, prologue, sizeof(prologue));
      if (len <= 0) return -32;
      const uintptr_t b = reinterpret_cast<uintptr_t>(c.binding.module_base);
      if (b > UINTPTR_MAX - kCallerCheckRva) return -33;
      PgrMetaCodeQuery code{};
      code.version = 1;
      code.size = sizeof(code);
      code.address = reinterpret_cast<const void *>(b + kCallerCheckRva);
      code.length = kCallerCheckLen;
      code.prologue = prologue;
      code.prologue_len = static_cast<uint32_t>(len);
      if ((rc = md->validate_code(&code))) return rc;
      continue;
    }

    if (g == GROUP_OUTCOMES) {
      // Outcome reporting needs all 6 Status literals with distinct values and CatchSuccess == result.success.
      for (uint32_t j = 0; j < PGR_QB_OUTCOME_MAX; j++) {
        if (!attached(c)) return -30;
        int32_t v = 0;
        if (!read_enum_literal(c, result.enum_class, kStatusEnumName, kOutcomeLiterals[j], &v)) {
          set_reason(grp, "outcome-literal:%s", kOutcomeLiterals[j]);
          std::memset(result.outcomes_values, 0, sizeof(result.outcomes_values));
          break;
        }
        bool dup = false;
        for (uint32_t k = 0; k < j; k++) {
          if (result.outcomes_values[k] == v) { dup = true; break; }
        }
        if (dup) {
          set_reason(grp, "outcome-literal-duplicate:%s", kOutcomeLiterals[j]);
          std::memset(result.outcomes_values, 0, sizeof(result.outcomes_values));
          break;
        }
        result.outcomes_values[j] = v;
      }
      if (grp.available && result.outcomes_values[1] != result.success) {
        set_reason(grp, "outcome-success-mismatch", nullptr);
        std::memset(result.outcomes_values, 0, sizeof(result.outcomes_values));
      }
      continue;
    }

    // --- Optional sub-feature groups (TRACE .. IVOUTLINE) ---
    int dep = kGroupDep[g];
    if (dep >= 0 && !result.groups[dep].available) {
      const char *reason = g == GROUP_ALWAYSHIT ? "always-hit-api" :
                           g == GROUP_ARTHROWS ? "ar-throw-api-unavailable" :
                           g == GROUP_IVTEXT ? "iv-text-api" :
                           g == GROUP_IVOWNED ? "iv-text-unavailable" : "trace-unavailable";
      set_reason(grp, "%s", reason);
      continue;
    }

    // Export gates checked before need() in prepare().
    if (g == GROUP_ALWAYSHIT && (!(c.extra->present & PGR_QB_EXP_CLASS_GET_FIELDS) || !c.extra->class_get_fields)) {
      set_reason(grp, "always-hit-api", nullptr);
      continue;
    }
    if (g == GROUP_ARTHROWS &&
        (c.extra->present & (PGR_QB_EXP_OBJECT_NEW | PGR_QB_EXP_FIELD_SET_VALUE)) !=
            (PGR_QB_EXP_OBJECT_NEW | PGR_QB_EXP_FIELD_SET_VALUE)) {
      set_reason(grp, "ar-throw-api-unavailable", nullptr);
      continue;
    }
    if (g == GROUP_IVTEXT &&
        (c.extra->present & (PGR_QB_EXP_STRING_LENGTH | PGR_QB_EXP_STRING_CHARS)) !=
            (PGR_QB_EXP_STRING_LENGTH | PGR_QB_EXP_STRING_CHARS)) {
      set_reason(grp, "iv-text-api", nullptr);
      continue;
    }

    // Every class referenced by this group (need() calls + field/method owners) must be resolved and unambiguous.
    bool blocked = false;
    const GroupClassList needed = group_needed_classes(g);
    for (uint32_t i = 0; i < needed.count && !blocked; i++) {
      uint32_t cls = needed.indices[i];
      if (!class_ok(cls)) {
        set_reason(grp, result.class_conflict[cls] ? "trace-class-mismatch:%s" : "trace-class-missing:%s",
                   kClasses[cls].canonical);
        blocked = true;
      }
    }
    for (uint32_t i = 0; i < kFieldCount && !blocked; i++)
      if (kFields[i].group == g && !class_ok(kFields[i].owner)) {
        set_reason(grp, result.class_conflict[kFields[i].owner] ? "trace-class-mismatch:%s" : "trace-class-missing:%s",
                   kClasses[kFields[i].owner].canonical);
        blocked = true;
      }
    for (uint32_t i = 0; i < kMethodCount && !blocked; i++)
      if (kMethods[i].group == g && !class_ok(kMethods[i].owner)) {
        set_reason(grp, result.class_conflict[kMethods[i].owner] ? "trace-class-mismatch:%s" : "trace-class-missing:%s",
                   kClasses[kMethods[i].owner].canonical);
        blocked = true;
      }
    if (blocked) continue;

    // Post-class export gates.
    if (g == GROUP_TRACE_NAME &&
        (c.extra->present & (PGR_QB_EXP_STRING_LENGTH | PGR_QB_EXP_STRING_CHARS)) !=
            (PGR_QB_EXP_STRING_LENGTH | PGR_QB_EXP_STRING_CHARS)) {
      set_reason(grp, "trace-export:string", nullptr);
      continue;
    }
    if (g == GROUP_IV &&
        (c.extra->present & (PGR_QB_EXP_OBJECT_GET_VIRTUAL | PGR_QB_EXP_STRING_NEW)) !=
            (PGR_QB_EXP_OBJECT_GET_VIRTUAL | PGR_QB_EXP_STRING_NEW)) {
      set_reason(grp, "iv-export-unavailable", nullptr);
      continue;
    }

    // Bare-class value-type ABI checks for this group (kValueSizes).
    bool failed = false;
    for (uint32_t i = 0; i < kValueSizeCount && !failed; i++) {
      if (kValueSizes[i].group != g) continue;
      if (!attached(c)) return -30;
      if (check_value_size(c, result.classes[kValueSizes[i].klass], kValueSizes[i].size, kValueSizes[i].align) != 0) {
        if (g == GROUP_TRACE) set_reason(grp, "trace-abi:%s", kClasses[kValueSizes[i].klass].canonical);
        else if (g == GROUP_THROWS) set_reason(grp, "throw-value-abi", nullptr);
        else if (g == GROUP_MAPTAP) set_reason(grp, "map-value-abi", nullptr);
        else if (g == GROUP_IVOUTLINE) set_reason(grp, "iv-outline-value-abi", nullptr);
        else set_reason(grp, "value-abi:%s", kClasses[kValueSizes[i].klass].canonical);
        failed = true;
      }
    }
    if (failed) continue;

    if (g == GROUP_IVOUTLINE && !(c.extra->present & PGR_QB_EXP_TYPE_GET_OBJECT)) {
      set_reason(grp, "iv-outline-type-api", nullptr);
      continue;
    }

    // Group-specific scans before field/method checks.
    if (g == GROUP_ALWAYSHIT) {
      if (!attached(c)) return -30;
      if (!scan_scheduler_current(c, result.classes[C_SCHEDULER], grp, &result.alwayshit_current))
        continue;
    }
    if (g == GROUP_TRACE_PARAMS) {
      if (!attached(c)) return -30;
      if (!resolve_overload(c, result.classes[C_ANIMATOR], "GetBool", "System.Boolean", grp, &result.trace_params_bool) ||
          !resolve_overload(c, result.classes[C_ANIMATOR], "GetInteger", "System.Int32", grp, &result.trace_params_int)) {
        result.trace_params_bool = nullptr;
        result.trace_params_int = nullptr;
      }
      continue;
    }

    for (uint32_t i = 0; i < kFieldCount && !failed; i++) {
      if (kFields[i].group != g) continue;
      if (!attached(c)) return -30;
      rc = validate_field(md, kFields[i], result.classes[kFields[i].owner], &result.fields[i], nullptr);
      if (rc) {
        set_reason(grp, kFields[i].required_mask ? "static-mismatch:%s" : "field-mismatch:%s", kFields[i].name);
        failed = true;
        break;
      }
    }

    if (g == GROUP_MAPTAP && !failed) {
      if (!attached(c)) return -30;
      if (!check_map_ground_byref(c, result.classes[C_MAPVIEW], grp)) failed = true;
    }

    for (uint32_t i = 0; i < kMethodCount && !failed; i++) {
      if (kMethods[i].group != g) continue;
      if (!attached(c)) return -30;
      rc = validate_method(md, c, kMethods[i], result.classes[kMethods[i].owner],
                           &result.methods[i], &result.method_code[i]);
      if (rc) {
        set_reason(grp, "method-mismatch:%s", kMethods[i].name);
        failed = true;
        break;
      }
    }

    if (g == GROUP_IV && !failed) {
      if (!attached(c)) return -30;
      if (!read_enum_literal(c, result.classes[C_HWRAP], kClasses[C_HWRAP].canonical, "Overflow",
                             &result.iv_overflow_value)) {
        set_reason(grp, "iv-enum:%s", "Overflow");
        failed = true;
      } else if (!read_enum_literal(c, result.classes[C_TEXTANCHOR], kClasses[C_TEXTANCHOR].canonical, "MiddleCenter",
                                    &result.iv_alignment_value)) {
        set_reason(grp, "iv-enum:%s", "MiddleCenter");
        failed = true;
      }
    }

    if (failed) {
      // Clear this group's partially populated handles/scalars so a failed group leaks no stale state.
      for (uint32_t i = 0; i < kFieldCount; i++) if (kFields[i].group == g) result.fields[i] = nullptr;
      for (uint32_t i = 0; i < kMethodCount; i++)
        if (kMethods[i].group == g) { result.methods[i] = nullptr; result.method_code[i] = nullptr; }
      if (g == GROUP_ALWAYSHIT) result.alwayshit_current = nullptr;
      if (g == GROUP_IV) { result.iv_overflow_value = 0; result.iv_alignment_value = 0; }
    }
  }

  if (!attached(c)) return -30;
  *out = result;
  return 0;
}

const PgrQuickballMetaApi g_api = {PGR_QB_API_VERSION, sizeof(PgrQuickballMetaApi), sizeof(PgrQuickballMetaConfig),
                                   sizeof(PgrQuickballMetaOutput), resolve};
}  // namespace

extern "C" const void *pgrazor_quickball_metadata_api(uint32_t version) {
  return version == PGR_QB_API_VERSION ? &g_api : nullptr;
}
