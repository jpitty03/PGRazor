#include "shared_metadata.h"
#include "managed_handle_abi.h"
#include "shared_gift_code.h"
#include <initializer_list>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unistd.h>

namespace pgr_shared_meta {
namespace {
constexpr uint32_t kApiVersion=2;

std::mutex resolving;

const PgrMetaApi *metadata() { return static_cast<const PgrMetaApi *>(pgrazor_runtime_metadata_api(1)); }

bool attached(const PgrSharedMetadataConfig &c) {
  return c.owner_tid && c.owner_tid == static_cast<uint64_t>(gettid()) &&
         reinterpret_cast<void *(*)()>(c.thread_current)() != nullptr;
}

struct Ctx {
  const PgrMetaApi *md;
  const PgrMetaExports *core;
  const PgrSharedMetaExtraExports *ex;
  uintptr_t module_base;
  PgrSharedMetadataOutput *out;
  const void *seen_code[256];
  size_t seen_code_count;
  char reason[kReasonMax];
};

int set_err(Ctx &c, int rc, const char *prefix, const char *detail = nullptr) {
  if (detail && detail[0])
    std::snprintf(c.reason, sizeof(c.reason), "Error: %s:%s", prefix, detail);
  else
    std::snprintf(c.reason, sizeof(c.reason), "Error: %s", prefix);
  return rc;
}

bool read_utf8(const char *p, std::string *out) {
  if (!p) return false;
  size_t n = 0;
  while (n < PGR_MD_NAME_MAX && p[n] != '\0') ++n;
  if (n == PGR_MD_NAME_MAX) return false;
  out->assign(p, n);
  return true;
}

bool get_type_name(Ctx &c, const void *type, std::string *out) {
  if (!type) return false;
  auto fn_get = reinterpret_cast<char *(*)(const void *)>(c.core->type_get_name);
  auto fn_free = reinterpret_cast<void (*)(void *)>(c.core->mem_free);
  char *raw = fn_get(type);
  if (!raw) return false;
  bool ok = read_utf8(raw, out);
  fn_free(raw);
  return ok;
}

bool get_class_name(Ctx &c, const void *klass, std::string *out) {
  if (!klass) return false;
  auto fn_ct = reinterpret_cast<void *(*)(const void *)>(c.core->class_get_type);
  return get_type_name(c, fn_ct(klass), out);
}

void record_field_meta(Ctx &c, const void *owner, const char *name, uint64_t offset, uint32_t kind, uint32_t size) {
  if (c.out->field_meta_count >= kFieldMetaMax) return;
  std::string owner_name;
  if (!get_class_name(c, owner, &owner_name)) return;
  auto &e = c.out->field_meta[c.out->field_meta_count++];
  e.offset = offset;
  e.kind = kind;
  e.size = size;
  std::snprintf(e.name, sizeof(e.name), "%s", name ? name : "");
  std::snprintf(e.owner, sizeof(e.owner), "%s", owner_name.c_str());
}

bool register_code_addr(Ctx &c, const void *code) {
  if (!code) return true;
  for (size_t i = 0; i < c.seen_code_count; ++i)
    if (c.seen_code[i] == code) return false;
  if (c.seen_code_count >= 256) return false;
  c.seen_code[c.seen_code_count++] = code;
  return true;
}

bool sep_suffix(const char *name, const char *suffix) {
  if (!name || !suffix) return false;
  size_t n = std::strlen(name), s = std::strlen(suffix);
  if (n < s + 1 || std::strcmp(name + (n - s), suffix) != 0) return false;
  char sep = name[n - s - 1];
  return sep == '.' || sep == '+' || sep == '/';
}

bool ends_with(const std::string &s, const char *suffix) {
  size_t sl = std::strlen(suffix);
  return s.size() >= sl && s.compare(s.size() - sl, sl, suffix) == 0;
}

bool starts_with(const std::string &s, const char *prefix) {
  size_t pl = std::strlen(prefix);
  return s.size() >= pl && s.compare(0, pl, prefix) == 0;
}

bool match_generic_1(const std::string &s, const char *outer, const char *inner) {
  std::string a = std::string(outer) + "<" + inner + ">";
  if (s == a) return true;
  std::string b = std::string(outer) + "`1<" + inner + ">";
  return s == b;
}

// Type-predicate callbacks for generic/tuple/action parameter & return types in quick-close.js.
using TypePred = bool (*)(const std::string &);

bool pred_award_list(const std::string &s) {
  return match_generic_1(s, "System.Collections.Generic.List", "Niantic.Holoholo.UI.AwardBubbleData");
}
bool pred_dismiss_action(const std::string &s) {
  return starts_with(s, "System.Action") && s.find("DismissParameters") != std::string::npos;
}
bool pred_release_promise(const std::string &s) {
  return match_generic_1(s, "Niantic.Promises.IPromise", "Holoholo.Rpc.ReleasePokemonOutProto");
}
bool pred_item_set(const std::string &s) {
  return match_generic_1(s, "System.Collections.Generic.ISet", "Holoholo.Rpc.Item");
}
bool pred_item_tuple(const std::string &s) {
  return s == "System.ValueTuple<Holoholo.Rpc.Item, Holoholo.Rpc.HoloItemType>" ||
         s == "System.ValueTuple<Holoholo.Rpc.Item,Holoholo.Rpc.HoloItemType>" ||
         s == "System.ValueTuple`2<Holoholo.Rpc.Item, Holoholo.Rpc.HoloItemType>" ||
         s == "System.ValueTuple`2<Holoholo.Rpc.Item,Holoholo.Rpc.HoloItemType>";
}
bool pred_item_filter(const std::string &s) {
  const char *p0 = "System.Func<", *p1 = "System.Func`2<";
  size_t off = starts_with(s, p0) ? std::strlen(p0) : (starts_with(s, p1) ? std::strlen(p1) : 0);
  if (!off) return false;
  const char *s0 = ", System.Boolean>", *s1 = ",System.Boolean>";
  size_t sl = ends_with(s, s0) ? std::strlen(s0) : (ends_with(s, s1) ? std::strlen(s1) : 0);
  if (!sl || s.size() <= off + sl) return false;
  return pred_item_tuple(s.substr(off, s.size() - off - sl));
}
bool pred_item_func_bool(const std::string &s) {
  return s == "System.Func<Holoholo.Rpc.Item, System.Boolean>" ||
         s == "System.Func<Holoholo.Rpc.Item,System.Boolean>" ||
         s == "System.Func`2<Holoholo.Rpc.Item, System.Boolean>" ||
         s == "System.Func`2<Holoholo.Rpc.Item,System.Boolean>";
}
bool pred_item_action(const std::string &s) {
  return match_generic_1(s, "System.Action", "Holoholo.Rpc.Item");
}
bool pred_item_data_list(const std::string &s) {
  const char *p0 = "System.Collections.Generic.List<", *p1 = "System.Collections.Generic.List`1<";
  size_t off = starts_with(s, p0) ? std::strlen(p0) : (starts_with(s, p1) ? std::strlen(p1) : 0);
  if (!off || !ends_with(s, "ItemData>")) return false;
  return s.size() > off + std::strlen("ItemData>");
}
bool pred_recycle_promise(const std::string &s) {
  return match_generic_1(s, "Niantic.Promises.IPromise", "Holoholo.Rpc.RecycleItemOutProto");
}
bool pred_direction_suffix(const std::string &s) {
  return sep_suffix(s.c_str(), "Direction");
}
bool pred_put_egg_promise(const std::string &s) {
  return match_generic_1(s, "Niantic.Promises.IPromise", "Holoholo.Rpc.UseItemEggIncubatorOutProto");
}
bool pred_incubators_enum(const std::string &s) {
  return match_generic_1(s, "System.Collections.Generic.IEnumerable", "Holoholo.Rpc.EggIncubatorProto");
}
bool pred_all_pokemon_coll(const std::string &s) {
  return match_generic_1(s, "System.Collections.Generic.ICollection", "Holoholo.Rpc.PokemonProto");
}
bool pred_buddy_feed_promise(const std::string &s) {
  return match_generic_1(s, "Niantic.Promises.IPromise", "Holoholo.Rpc.BuddyFeedingOutProto");
}
bool pred_dot_data_suffix(const std::string &s) {
  return ends_with(s, ".Data");
}
bool pred_dot_init_suffix(const std::string &s) {
  return ends_with(s, ".Initializer");
}
bool pred_sticker_list(const std::string &s) {
  return match_generic_1(s, "System.Collections.Generic.List", "Holoholo.Rpc.StickerProto");
}
bool pred_send_gift_promise(const std::string &s) {
  return match_generic_1(s, "Niantic.Promises.IPromise", "Holoholo.Rpc.SendGiftOutProto");
}
bool pred_sendable_gifts_enum(const std::string &s) {
  return match_generic_1(s, "System.Collections.Generic.IEnumerable", "Holoholo.Rpc.GiftBoxProto");
}
bool pred_gift_box_enumerator(const std::string &s) {
  return match_generic_1(s, "System.Collections.Generic.IEnumerator", "Holoholo.Rpc.GiftBoxProto");
}
bool pred_vgp_encounter_resp(const std::string &s) {
  return s.find("<Niantic.Holoholo.Map.PokemonEncounterResponse>") != std::string::npos;
}
bool pred_vgp_capture_resp(const std::string &s) {
  return s.find("<Holoholo.Rpc.CatchPokemonOutProto>") != std::string::npos;
}

// ---------------------------------------------------------------------------------------------------
// Field, method, enum, and nested-type helpers.
// ---------------------------------------------------------------------------------------------------
int raw_field(Ctx &c, const void *owner, const char *name, uint64_t off, const void **field_out,
              const void **type_out = nullptr, const void **tc_out = nullptr) {
  if (!owner) return set_err(c, PGR_MD_ERR_NOT_FOUND, "field-mismatch", name);
  PgrMetaFieldQuery q{}; q.version = 1; q.size = sizeof(q);
  q.owner = owner; q.name = name; q.offset = off;
  q.forbidden_mask = 0x50; q.required_mask = 0; q.kind = PGR_MD_FIELD_ANY;
  PgrMetaFieldResult r{};
  int rc = c.md->validate_field(&q, &r);
  if (rc) return set_err(c, rc, "field-mismatch", name);
  if (field_out) *field_out = r.field;
  if (type_out) *type_out = r.type;
  if (tc_out) *tc_out = r.type_class;
  return 0;
}

int ref_field(Ctx &c, const void *owner, const char *name, uint64_t off, const void **field_out,
              const void **tc_out = nullptr) {
  if (!owner) return set_err(c, PGR_MD_ERR_NOT_FOUND, "field-mismatch", name);
  PgrMetaFieldQuery q{}; q.version = 1; q.size = sizeof(q);
  q.owner = owner; q.name = name; q.offset = off;
  q.forbidden_mask = 0x50; q.required_mask = 0; q.kind = PGR_MD_FIELD_REF;
  PgrMetaFieldResult r{};
  int rc = c.md->validate_field(&q, &r);
  if (rc) return set_err(c, rc, rc == PGR_MD_ERR_KIND ? "field-not-reference" : "field-mismatch", name);
  record_field_meta(c, owner, name, off, PGR_MD_FIELD_REF, 0);
  if (field_out) *field_out = r.field;
  if (tc_out) *tc_out = r.type_class;
  return 0;
}

int enum_i32_field(Ctx &c, const void *owner, const char *name, uint64_t off, const void **field_out,
                   const void **tc_out = nullptr) {
  if (!owner) return set_err(c, PGR_MD_ERR_NOT_FOUND, "field-mismatch", name);
  PgrMetaFieldQuery q{}; q.version = 1; q.size = sizeof(q);
  q.owner = owner; q.name = name; q.offset = off;
  q.forbidden_mask = 0x50; q.required_mask = 0; q.kind = PGR_MD_FIELD_ENUM_I32;
  PgrMetaFieldResult r{};
  int rc = c.md->validate_field(&q, &r);
  if (rc) return set_err(c, rc, (rc == PGR_MD_ERR_KIND || rc == PGR_MD_ERR_SIZE) ? "enum-abi" : "field-mismatch", name);
  record_field_meta(c, owner, name, off, PGR_MD_FIELD_ENUM_I32, 0);
  if (field_out) *field_out = r.field;
  if (tc_out) *tc_out = r.type_class;
  return 0;
}

int typed_field(Ctx &c, const void *owner, const char *name, uint64_t off, const char *type_name,
                const char *err_prefix, const void **field_out, const void **tc_out = nullptr) {
  if (!owner) return set_err(c, PGR_MD_ERR_NOT_FOUND, err_prefix, name);
  PgrMetaFieldQuery q{}; q.version = 1; q.size = sizeof(q);
  q.owner = owner; q.name = name; q.offset = off;
  q.forbidden_mask = 0x50; q.required_mask = 0; q.kind = PGR_MD_FIELD_ANY;
  q.type_name = type_name; q.type_match = PGR_MD_MATCH_EXACT;
  PgrMetaFieldResult r{};
  int rc = c.md->validate_field(&q, &r);
  if (rc) return set_err(c, rc, rc == PGR_MD_ERR_TYPE ? err_prefix : "field-mismatch", name);
  if (field_out) *field_out = r.field;
  if (tc_out) *tc_out = r.type_class;
  return 0;
}

int scalar_field(Ctx &c, const void *owner, const char *name, uint64_t off, const char *type_name,
                 uint32_t sz, const char *err_prefix, const void **field_out) {
  if (!owner) return set_err(c, PGR_MD_ERR_NOT_FOUND, err_prefix, name);
  PgrMetaFieldQuery q{}; q.version = 1; q.size = sizeof(q);
  q.owner = owner; q.name = name; q.offset = off;
  q.forbidden_mask = 0x50; q.required_mask = 0; q.kind = PGR_MD_FIELD_VALUE; q.value_size = sz;
  q.type_name = type_name; q.type_match = PGR_MD_MATCH_EXACT;
  PgrMetaFieldResult r{};
  int rc = c.md->validate_field(&q, &r);
  if (rc) return set_err(c, rc, err_prefix, name);
  if (field_out) *field_out = r.field;
  return 0;
}

struct MParam {
  const char *exact = nullptr;
  TypePred pred = nullptr;
};

int check_method_ex(Ctx &c, const void *owner, const char *name, const char *ret_exact, TypePred ret_pred,
                    uint32_t pc, const MParam *params, bool is_static, bool allow_inflated,
                    uint64_t rva, const unsigned char *pro, uint32_t plen,
                    const void **m_out, const void **code_out = nullptr) {
  if (!owner) return set_err(c, PGR_MD_ERR_NOT_FOUND, "method-mismatch", name);
  PgrMetaMethodQuery q{}; q.version = 1; q.size = sizeof(q);
  q.owner = owner; q.name = name;
  q.return_type = ret_exact;
  q.return_match = ret_exact ? PGR_MD_MATCH_EXACT : PGR_MD_MATCH_ANY;
  q.param_count = pc;
  for (uint32_t i = 0; i < pc && i < PGR_MD_PARAM_MAX; ++i) {
    q.param_types[i] = params[i].exact;
    q.param_match[i] = params[i].exact ? PGR_MD_MATCH_EXACT : PGR_MD_MATCH_ANY;
  }
  q.is_static = is_static ? 1u : 0u;
  q.allow_inflated = allow_inflated ? 1u : 0u;
  if (pro && plen) {
    if (c.module_base > UINTPTR_MAX - rva) return set_err(c, PGR_MD_ERR_CODE_RANGE, "method-address", name);
    q.expected_code = reinterpret_cast<const void *>(c.module_base + rva);
    q.prologue = pro; q.prologue_len = plen;
  }
  PgrMetaMethodResult r{};
  int rc = c.md->validate_method(&q, &r);
  if (rc) return set_err(c, rc, "method-mismatch", name);

  if (ret_pred) {
    auto fn_ret = reinterpret_cast<void *(*)(const void *)>(c.core->method_get_return_type);
    std::string rn;
    if (!get_type_name(c, fn_ret(r.method), &rn)) return set_err(c, PGR_MD_ERR_NULL_TYPE_NAME, "null-type-name");
    if (!ret_pred(rn)) return set_err(c, PGR_MD_ERR_TYPE, "method-mismatch", name);
  }
  auto fn_param = reinterpret_cast<void *(*)(const void *, uint32_t)>(c.core->method_get_param);
  for (uint32_t i = 0; i < pc; ++i) {
    if (!params[i].pred) continue;
    std::string pn;
    if (!get_type_name(c, fn_param(r.method, i), &pn)) return set_err(c, PGR_MD_ERR_NULL_TYPE_NAME, "null-type-name");
    if (!params[i].pred(pn)) return set_err(c, PGR_MD_ERR_TYPE, "parameter-mismatch", name);
  }
  if (pro && plen && r.code && !register_code_addr(c, r.code))
    return set_err(c, PGR_MD_ERR_CODE_RANGE, "method-address", name);

  if (m_out) *m_out = r.method;
  if (code_out) *code_out = r.code;
  return 0;
}

int check_method(Ctx &c, const void *owner, const char *name, const char *ret, uint32_t pc,
                 const char *p0, const char *p1, bool is_static, bool allow_inflated,
                 uint64_t rva, const unsigned char *pro, uint32_t plen,
                 const void **m_out, const void **code_out = nullptr) {
  MParam params[2] = {{p0, nullptr}, {p1, nullptr}};
  return check_method_ex(c, owner, name, ret, nullptr, pc, params, is_static, allow_inflated,
                         rva, pro, plen, m_out, code_out);
}

int enum_literal(Ctx &c, const void *enum_class, const char *name, bool check_parent,
                 const char *err_prefix, int32_t *out) {
  if (!enum_class) return set_err(c, PGR_MD_ERR_NOT_FOUND, err_prefix, name);
  auto field_from_name = reinterpret_cast<void *(*)(const void *, const char *)>(c.core->class_get_field_from_name);
  auto field_parent = reinterpret_cast<void *(*)(const void *)>(c.core->field_get_parent);
  auto field_flags = reinterpret_cast<int (*)(const void *)>(c.core->field_get_flags);
  auto static_get = reinterpret_cast<void (*)(const void *, void *)>(c.ex->field_static_get_value);
  void *f = field_from_name(enum_class, name);
  if (!f) return set_err(c, PGR_MD_ERR_NOT_FOUND, err_prefix, name);
  if (check_parent && field_parent(f) != enum_class) return set_err(c, PGR_MD_ERR_OWNER_MISMATCH, err_prefix, name);
  if ((static_cast<uint32_t>(field_flags(f)) & 0x8050u) != 0x8050u) return set_err(c, PGR_MD_ERR_FLAGS, err_prefix, name);
  int32_t v = 0; static_get(f, &v); *out = v; return 0;
}

int nested_suffix(Ctx &c, const void *owner, const char *suffix, uint32_t limit, bool check_declaring,
                  const char *err_name, const void **out) {
  if (!owner) return set_err(c, PGR_MD_ERR_NOT_FOUND, err_name);
  auto nested = reinterpret_cast<void *(*)(const void *, void **)>(c.core->class_get_nested_types);
  auto declaring = reinterpret_cast<void *(*)(const void *)>(c.core->class_get_declaring_type);
  void *iter = nullptr;
  for (uint32_t i = 0; i < limit; i++) {
    void *k = nested(owner, &iter);
    if (!k) break;
    std::string nm;
    if (!get_class_name(c, k, &nm)) return set_err(c, PGR_MD_ERR_NULL_TYPE_NAME, "null-type-name");
    if (sep_suffix(nm.c_str(), suffix) && (!check_declaring || declaring(k) == owner)) {
      *out = k; return 0;
    }
  }
  return set_err(c, PGR_MD_ERR_NOT_FOUND, err_name);
}

// ---------------------------------------------------------------------------------------------------
// Clean pre-hook method prologues for all optional subfeatures.
// ---------------------------------------------------------------------------------------------------
#define P24(b0,b1,b2,b3,b4,b5,b6,b7,b8,b9,b10,b11,b12,b13,b14,b15,b16,b17,b18,b19,b20,b21,b22,b23) \
  {0x##b0,0x##b1,0x##b2,0x##b3,0x##b4,0x##b5,0x##b6,0x##b7,0x##b8,0x##b9,0x##b10,0x##b11, \
   0x##b12,0x##b13,0x##b14,0x##b15,0x##b16,0x##b17,0x##b18,0x##b19,0x##b20,0x##b21,0x##b22,0x##b23}
#define P32(b0,b1,b2,b3,b4,b5,b6,b7,b8,b9,b10,b11,b12,b13,b14,b15,b16,b17,b18,b19,b20,b21,b22,b23,b24,b25,b26,b27,b28,b29,b30,b31) \
  {0x##b0,0x##b1,0x##b2,0x##b3,0x##b4,0x##b5,0x##b6,0x##b7,0x##b8,0x##b9,0x##b10,0x##b11, \
   0x##b12,0x##b13,0x##b14,0x##b15,0x##b16,0x##b17,0x##b18,0x##b19,0x##b20,0x##b21,0x##b22,0x##b23, \
   0x##b24,0x##b25,0x##b26,0x##b27,0x##b28,0x##b29,0x##b30,0x##b31}

// catchCanvas + catchDiscovery
constexpr unsigned char kCanvasSet[32] = P32(fe,0f,1c,f8,f8,5f,01,a9,f6,57,02,a9,f4,4f,03,a9,d5,6a,02,d0,f3,03,01,2a,f4,03,00,aa,a8,ce,51,39);
constexpr unsigned char kTopGui[32]    = P32(ff,83,02,d1,fd,7b,04,a9,fc,6f,05,a9,fa,67,06,a9,f8,5f,07,a9,f6,57,08,a9,f4,4f,09,a9,93,6a,02,f0);

// details + directPresentation
constexpr unsigned char kDetPresent[32]= P32(ff,03,02,d1,fd,7b,02,a9,fc,6f,03,a9,fa,67,04,a9,f8,5f,05,a9,f6,57,06,a9,f4,4f,07,a9,57,19,02,f0);
constexpr unsigned char kDetExit[32]   = P32(fe,0f,1e,f8,f4,4f,01,a9,08,6c,40,f9,f3,03,00,aa,a8,00,00,b4,09,0d,40,f9,00,21,40,f9,01,15,40,f9);
constexpr unsigned char kDetOpen[32]   = P32(fe,5f,bd,a9,f6,57,01,a9,f4,4f,02,a9,f4,b4,02,f0,f3,03,00,aa,88,f6,50,39,28,02,00,36,f5,8a,02,f0);
constexpr unsigned char kDetInit[32]   = P32(ff,83,01,d1,fe,67,02,a9,f8,5f,03,a9,f6,57,04,a9,f4,4f,05,a9,f5,b4,02,90,f3,03,01,aa,f4,03,00,aa);
constexpr unsigned char kDetClick[32]  = P32(fe,57,be,a9,f4,4f,01,a9,94,b4,02,d0,f3,03,00,aa,88,a6,51,39,e8,00,00,36,94,b4,02,d0,88,d6,50,39);
constexpr unsigned char kDpShow[32]    = P32(ff,83,02,d1,fd,7b,04,a9,fc,6f,05,a9,fa,67,06,a9,f8,5f,07,a9,f6,57,08,a9,f4,4f,09,a9,73,b2,02,d0);

// transfer
constexpr unsigned char kTrResult[24]  = P24(ff,03,02,d1,fe,5f,05,a9,f6,57,06,a9,f4,4f,07,a9,d5,83,02,90,f3,03,01,aa);
constexpr unsigned char kTrError[24]   = P24(fe,0f,1d,f8,f6,57,01,a9,f4,4f,02,a9,d5,83,02,90,f3,03,01,aa,f4,03,00,aa);
constexpr unsigned char kTrRelease[32] = P32(ff,43,02,d1,fe,23,00,f9,fa,67,05,a9,f8,5f,06,a9,f6,57,07,a9,f4,4f,08,a9,f3,83,02,d0,f5,03,01,aa);

// spin + spinCamera
constexpr unsigned char kSpinEnter[24]  = P24(ff,c3,02,d1,fd,7b,05,a9,fc,6f,06,a9,fa,67,07,a9,f8,5f,08,a9,f6,57,09,a9);
constexpr unsigned char kSpinExit[24]   = P24(fd,7b,ba,a9,fc,6f,01,a9,fa,67,02,a9,f8,5f,03,a9,f6,57,04,a9,f4,4f,05,a9);
constexpr unsigned char kSpinResult[24] = P24(ff,c3,01,d1,fe,6f,02,a9,fa,67,03,a9,f8,5f,04,a9,f6,57,05,a9,f4,4f,06,a9);
constexpr unsigned char kSpinClose[24]  = P24(fe,57,be,a9,f4,4f,01,a9,34,5f,02,f0,f3,03,00,aa,88,f2,67,39,08,01,00,37);
constexpr unsigned char kSpinSpin[24]   = P24(fe,4f,bf,a9,f3,03,00,aa,51,bc,ff,97,7f,c2,03,39,fe,4f,c1,a8,c0,03,5f,d6);
constexpr unsigned char kSpinRequest[24]= P24(fe,0f,1d,f8,f6,57,01,a9,f4,4f,02,a9,34,93,02,f0,f3,03,00,aa,88,ce,61,39);
constexpr unsigned char kSpinCan[24]    = P24(fe,57,be,a9,f4,4f,01,a9,d4,93,02,b0,f3,03,00,aa,88,32,60,39,08,01,00,37);
constexpr unsigned char kSpinActive[24] = P24(00,00,42,39,c0,03,5f,d6,fe,0f,1d,f8,f6,57,01,a9,f4,4f,02,a9,b5,45,02,b0);
constexpr unsigned char kScMove[24]     = P24(ff,c3,02,d1,ea,33,00,fd,e9,a3,06,6d,fe,3f,00,f9,f8,5f,08,a9,f6,57,09,a9);
constexpr unsigned char kScAppEnter[24] = P24(ff,c3,01,d1,fe,23,00,f9,f6,57,05,a9,f4,4f,06,a9,d4,5f,02,f0,f3,03,00,aa);
constexpr unsigned char kScAppExit[24]  = P24(fe,57,be,a9,f4,4f,01,a9,d4,5f,02,d0,f3,03,00,aa,88,9e,66,39,08,01,00,37);
constexpr unsigned char kScResEnter[24] = P24(fe,57,be,a9,f4,4f,01,a9,d4,5d,02,d0,f3,03,00,aa,88,ce,6c,39,08,01,00,37);
constexpr unsigned char kScResExit[24]  = P24(fe,57,be,a9,f4,4f,01,a9,d4,5d,02,b0,f3,03,00,aa,88,d2,6c,39,08,01,00,37);

// items + itemsLookup
constexpr unsigned char kItCjpz[24]     = P24(ff,03,07,d1,ea,a3,00,fd,e9,23,15,6d,fd,7b,16,a9,fc,6f,17,a9,fa,67,18,a9);
constexpr unsigned char kItCjpm[24]     = P24(fe,5f,bd,a9,f6,57,01,a9,f4,4f,02,a9,16,aa,02,f0,34,fc,60,d3,f5,03,01,aa);
constexpr unsigned char kItCjpd[24]     = P24(20,00,80,52,c0,03,5f,d6,fe,5f,bd,a9,f6,57,01,a9,f4,4f,02,a9,16,aa,02,d0);
constexpr unsigned char kItCjpx[24]     = P24(ff,83,02,d1,fd,7b,04,a9,fc,6f,05,a9,fa,67,06,a9,f8,5f,07,a9,f6,57,08,a9);
constexpr unsigned char kItExpiring[24] = P24(00,5c,40,f9,c0,03,5f,d6,ff,83,02,d1,fd,7b,04,a9,fc,6f,05,a9,fa,67,06,a9);
constexpr unsigned char kItGetCount[24] = P24(fe,0f,1d,f8,f6,57,01,a9,f4,4f,02,a9,74,86,02,b0,f3,03,00,aa,88,8a,48,39);
constexpr unsigned char kItRecycle[24]  = P24(ff,c3,02,d1,fd,7b,05,a9,fc,6f,06,a9,fa,67,07,a9,f8,5f,08,a9,f6,57,09,a9);
constexpr unsigned char kItZid[24]      = P24(ff,03,02,d1,fe,5f,05,a9,f6,57,06,a9,f4,4f,07,a9,f5,85,02,d0,f3,03,01,aa);
constexpr unsigned char kItZie[24]      = P24(fe,0f,1d,f8,f6,57,01,a9,f4,4f,02,a9,f5,85,02,d0,f3,03,01,aa,f4,03,00,aa);

// rocket (RVCode, RMCode, ROCKETCODE)
constexpr unsigned char kRvUpdate[24]   = P24(e8,0f,1b,fc,fe,07,00,f9,fa,67,01,a9,f8,5f,02,a9,f6,57,03,a9,f4,4f,04,a9);
constexpr unsigned char kRvClose[24]    = P24(fe,57,be,a9,f4,4f,01,a9,54,f7,02,d0,f3,03,00,aa,88,e2,65,39,08,01,00,37);
constexpr unsigned char kRvBlocked[24]  = P24(fe,5f,bd,a9,f6,57,01,a9,f4,4f,02,a9,54,f7,02,b0,f3,03,00,aa,88,ee,65,39);
constexpr unsigned char kRvFast[24]     = P24(ff,43,01,d1,fe,0b,00,f9,f8,5f,02,a9,f6,57,03,a9,f4,4f,04,a9,94,f1,02,f0);
constexpr unsigned char kRvObserve[24]  = P24(fe,0f,1a,f8,fc,6f,01,a9,fa,67,02,a9,f8,5f,03,a9,f6,57,04,a9,f4,4f,05,a9);
constexpr unsigned char kRvCharge[24]   = P24(e8,0f,1a,fc,fe,07,00,f9,fc,6f,01,a9,fa,67,02,a9,f8,5f,03,a9,f6,57,04,a9);
constexpr unsigned char kRvCharged[24]  = P24(00,b0,48,39,c0,03,5f,d6,01,b0,08,39,c0,03,5f,d6,00,b4,48,39,c0,03,5f,d6);
constexpr unsigned char kRvVisible[24]  = P24(fe,0f,1d,f8,f6,57,01,a9,f4,4f,02,a9,08,bc,48,39,28,08,00,34,e1,03,1f,aa);
constexpr unsigned char kRmUpdate[24]   = P24(ff,c3,01,d1,ea,0b,00,fd,e9,23,02,6d,fe,67,03,a9,f8,5f,04,a9,f6,57,05,a9);
constexpr unsigned char kRmExit[24]     = P24(ff,43,02,d1,eb,2b,02,6d,e9,23,03,6d,fe,23,00,f9,fa,67,05,a9,f8,5f,06,a9);
constexpr unsigned char kRmHide[24]     = P24(fe,0f,1c,f8,f8,5f,01,a9,f6,57,02,a9,f4,4f,03,a9,74,30,02,d0,f3,03,00,aa);
constexpr unsigned char kRmDestroy[24]  = P24(fe,5f,bd,a9,f6,57,01,a9,f4,4f,02,a9,74,30,02,d0,f3,03,00,aa,88,c2,67,39);
constexpr unsigned char kRkEnter[24]    = P24(fe,0f,1c,f8,f8,5f,01,a9,f6,57,02,a9,f4,4f,03,a9,56,2e,02,90,f4,03,02,aa);
constexpr unsigned char kRkExit[24]     = P24(fe,0f,1d,f8,f6,57,01,a9,f4,4f,02,a9,34,2e,02,90,f3,03,00,aa,88,02,71,39);
constexpr unsigned char kRkUpdate[24]   = P24(fe,5f,bd,a9,f6,57,01,a9,f4,4f,02,a9,15,cc,40,f9,75,0d,00,b4,96,2f,02,d0);
constexpr unsigned char kRkUi[24]       = P24(ef,3b,b8,6d,ed,33,01,6d,eb,2b,02,6d,e9,23,03,6d,fe,67,04,a9,f8,5f,05,a9);
constexpr unsigned char kRkClose[24]    = P24(fe,0f,1c,f8,f8,5f,01,a9,f6,57,02,a9,f4,4f,03,a9,b4,2e,02,f0,f3,03,00,aa);
constexpr unsigned char kRkDirector[24] = P24(fe,57,be,a9,f4,4f,01,a9,d4,2e,02,b0,f3,03,00,aa,88,6a,6d,39,08,01,00,37);
constexpr unsigned char kRkActive[24]   = P24(fe,57,be,a9,f4,4f,01,a9,74,38,02,90,f3,03,00,aa,88,ee,51,39,e8,00,00,36);
constexpr unsigned char kRkFast[24]     = P24(fe,0f,1d,f8,f6,57,01,a9,f4,4f,02,a9,54,38,02,b0,f3,03,00,aa,88,06,52,39);
constexpr unsigned char kRkCharge[24]   = P24(ff,83,01,d1,fe,67,02,a9,f8,5f,03,a9,f6,57,04,a9,f4,4f,05,a9,34,2f,02,f0);
constexpr unsigned char kRkVisible[24]  = P24(ff,03,01,d1,fe,0b,00,f9,f6,57,02,a9,f4,4f,03,a9,e1,03,1f,aa,f3,03,00,aa);
constexpr unsigned char kRkPrompt[24]   = P24(fe,67,bc,a9,f8,5f,01,a9,f6,57,02,a9,f4,4f,03,a9,95,30,02,d0,f3,03,01,2a);
constexpr unsigned char kRkShield[24]   = P24(ff,c3,01,d1,fe,13,00,f9,fa,67,03,a9,f8,5f,04,a9,f6,57,05,a9,f4,4f,06,a9);
constexpr unsigned char kRkShClose[24]  = P24(fe,57,be,a9,f4,4f,01,a9,93,30,02,90,f4,03,00,aa,68,9a,67,39,08,01,00,37);

// ---------------------------------------------------------------------------------------------------
// Optional subfeature resolvers 0..10.
// ---------------------------------------------------------------------------------------------------
int do_catch_canvas(Ctx &c) {
  const void **H = c.out->opt_handles;
  return check_method(c, c.out->classes[0], "set_CanvasGroupVisible", "System.Void", 1, "System.Boolean", nullptr,
                      false, false, 0xa372cb8ULL, kCanvasSet, sizeof(kCanvasSet), &H[H_CC_METHOD], &H[H_CC_CODE]);
}

int do_catch_discovery(Ctx &c) {
  const void **H = c.out->opt_handles; int rc;
  const void *klass = c.out->opt_classes[OC_GUILAYER];
  if (!klass) return set_err(c, PGR_MD_ERR_NOT_FOUND, "gui-layer-class");
  H[H_CD_CLASS] = klass;
  if ((rc = ref_field(c, c.out->classes[0], "guiLayerService", 0xa0, &H[H_CD_LAYER]))) return rc;
  return check_method(c, klass, "TopGui", "Niantic.Holoholo.NUI.IGuiController", 0, nullptr, nullptr,
                      false, false, 0xa3797c0ULL, kTopGui, sizeof(kTopGui), &H[H_CD_TOP], &H[H_CD_TOP_CODE]);
}

int do_catch_scene(Ctx &c) {
  const void **H = c.out->opt_handles;
  auto enum_methods = reinterpret_cast<void *(*)(const void *, void **)>(c.core->class_get_methods);
  auto m_name = reinterpret_cast<char *(*)(const void *)>(c.core->method_get_name);
  auto m_generic = reinterpret_cast<bool (*)(const void *)>(c.core->method_is_generic);
  auto m_pcount = reinterpret_cast<uint32_t (*)(const void *)>(c.core->method_get_param_count);
  auto m_param = reinterpret_cast<void *(*)(const void *, uint32_t)>(c.core->method_get_param);
  auto m_ret = reinterpret_cast<void *(*)(const void *)>(c.core->method_get_return_type);
  void *iter = nullptr, *find = nullptr;
  for (int i = 0; i < 256; i++) {
    void *q = enum_methods(c.out->classes[4], &iter);
    if (!q) break;
    std::string nm;
    if (!read_utf8(m_name(q), &nm)) return set_err(c, PGR_MD_ERR_NAME_TOO_LONG, "name-limit");
    if (nm == "FindObjectsOfType" && !m_generic(q) && m_pcount(q) == 1) {
      std::string p0, rn;
      if (!get_type_name(c, m_param(q, 0), &p0) || !get_type_name(c, m_ret(q), &rn))
        return set_err(c, PGR_MD_ERR_NULL_TYPE_NAME, "null-type-name");
      if (p0 == "System.Type" && rn == "UnityEngine.Object[]") { find = q; break; }
    }
  }
  if (!find) return set_err(c, PGR_MD_ERR_NOT_FOUND, "catch-find-objects");
  if (!c.ex->type_get_object || !c.ex->array_length)
    return set_err(c, PGR_MD_ERR_MISSING_EXPORT, "catch-scene-exports");
  H[H_CS_FIND] = find;
  H[H_CS_TYPE_OBJ] = c.ex->type_get_object;
  H[H_CS_ARRAY_LEN] = c.ex->array_length;
  return 0;
}

int do_details(Ctx &c) {
  const void **H = c.out->opt_handles; int rc;
  const void *dgui = c.out->opt_classes[OC_DGUI], *data = c.out->opt_classes[OC_DATA],
             *view = c.out->opt_classes[OC_VIEW], *pk = c.out->opt_classes[OC_PK],
             *int_c = c.out->opt_classes[OC_INT], *del = c.out->opt_classes[OC_DELEGATE],
             *act = c.out->opt_classes[OC_ACTION];
  if (!dgui || !data || !view || !pk || !int_c || !del || !act)
    return set_err(c, PGR_MD_ERR_NOT_FOUND, "details-classes-unavailable");

  if ((rc = ref_field(c, del, "m_target", 0x20, &H[H_DET_DELEGATE_TARGET]))) return rc;
  if ((rc = ref_field(c, view, "dsvw", 0xe0, &H[H_DET_VIEW_RESP]))) return rc;
  if ((rc = ref_field(c, dgui, "egbx", 0x3a0, &H[H_DET_DATA]))) return rc;
  if ((rc = ref_field(c, dgui, "egbu", 0x388, &H[H_DET_CALLBACK]))) return rc;
  if ((rc = ref_field(c, data, "OnScreenDismissed", 0x28, &H[H_DET_ON_DISMISSED]))) return rc;
  if ((rc = ref_field(c, data, "PokemonList", 0x10, &H[H_DET_LIST], &H[H_DET_LIST_CLASS]))) return rc;
  if ((rc = scalar_field(c, data, "StartingIndex", 0x18, "System.Int32", 4, "details-field", &H[H_DET_INDEX]))) return rc;
  if ((rc = scalar_field(c, data, "IsPostEncounter", 0x39, "System.Boolean", 1, "details-field", &H[H_DET_POST]))) return rc;
  if ((rc = scalar_field(c, pk, "id_", 0x20, "System.UInt64", 8, "details-field", &H[H_DET_ID]))) return rc;

  MParam pres_p[3] = {{"Holoholo.Rpc.PokemonProto", nullptr}, {nullptr, pred_award_list}, {"System.Boolean", nullptr}};
  if ((rc = check_method_ex(c, view, "buji", "System.Void", nullptr, 3, pres_p, false, false,
                            0xada4ac8ULL, kDetPresent, sizeof(kDetPresent), &H[H_DET_PRESENT], &H[H_DET_PRESENT_CODE]))) return rc;
  if ((rc = check_method(c, view, "ExitState", "System.Void", 0, nullptr, nullptr, false, false,
                         0xada5814ULL, kDetExit, sizeof(kDetExit), &H[H_DET_EXIT], &H[H_DET_EXIT_CODE]))) return rc;
  if ((rc = check_method(c, dgui, "Initialize", "System.Void", 1, "Niantic.Holoholo.UI.Pokemon.PokemonInfoData", nullptr, false, false,
                         0x9a2d004ULL, kDetInit, sizeof(kDetInit), &H[H_DET_INIT], &H[H_DET_INIT_CODE]))) return rc;
  if ((rc = check_method(c, dgui, "HandleOpenComplete", "System.Void", 0, nullptr, nullptr, false, false,
                         0x9a2a4b0ULL, kDetOpen, sizeof(kDetOpen), &H[H_DET_OPEN], &H[H_DET_OPEN_CODE]))) return rc;
  if ((rc = check_method(c, dgui, "OnDismissOrContinueClicked", "System.Void", 0, nullptr, nullptr, false, false,
                         0x9a3702cULL, kDetClick, sizeof(kDetClick), &H[H_DET_CLICK], &H[H_DET_CLICK_CODE]))) return rc;
  if ((rc = check_method(c, H[H_DET_LIST_CLASS], "get_Count", "System.Int32", 0, nullptr, nullptr, false, true,
                         0, nullptr, 0, &H[H_DET_COUNT], nullptr))) return rc;
  if ((rc = check_method(c, H[H_DET_LIST_CLASS], "get_Item", "Holoholo.Rpc.PokemonProto", 1, "System.Int32", nullptr, false, true,
                         0, nullptr, 0, &H[H_DET_ITEM], nullptr))) return rc;
  return 0;
}

int do_direct_presentation(Ctx &c) {
  if (c.out->optional[OPT_DETAILS] != PGR_SHARED_OPT_AVAILABLE)
    return set_err(c, PGR_MD_ERR_NOT_FOUND, "direct-details-unavailable");
  const void **H = c.out->opt_handles; int rc;
  const void *gs = c.out->opt_classes[OC_GAMESTATE], *svc = c.out->opt_classes[OC_PKINFO_SERVICE],
             *act = c.out->opt_classes[OC_ACTION];
  if ((rc = typed_field(c, gs, "<IsActiveState>k__BackingField", 0x50, "System.Boolean", "direct-active-type", &H[H_DP_ACTIVE]))) return rc;
  H[H_DP_SERVICE] = svc;
  MParam sp[10] = {
    {"Holoholo.Rpc.PokemonProto", nullptr}, {"System.Boolean", nullptr}, {"System.Boolean", nullptr},
    {"Niantic.Holoholo.UI.Pokemon.OpenHyperTrainingState", nullptr}, {"System.Action", nullptr},
    {"System.Action", nullptr}, {nullptr, pred_dismiss_action},
    {"System.Boolean", nullptr}, {"System.Boolean", nullptr}, {"System.Boolean", nullptr}
  };
  if ((rc = check_method_ex(c, svc, "ShowPokemonInfo", "System.Void", nullptr, 10, sp, false, false,
                            0x9a7ba18ULL, kDpShow, sizeof(kDpShow), &H[H_DP_SHOW], &H[H_DP_SHOW_CODE]))) return rc;
  return check_method(c, act, "Invoke", "System.Void", 0, nullptr, nullptr, false, false,
                      0, nullptr, 0, &H[H_DP_CALLBACK], nullptr);
}

int do_transfer(Ctx &c) {
  const void *bag = c.out->opt_classes[OC_BAG], *disp = c.out->opt_classes[OC_DISPLAY],
             *rel = c.out->opt_classes[OC_RELEASE], *view = c.out->opt_classes[OC_VIEW],
             *pk = c.out->opt_classes[OC_PK], *csz = c.out->opt_classes[OC_CHAR_SIZE];
  if (c.out->optional[OPT_DETAILS] != PGR_SHARED_OPT_AVAILABLE || !bag || !disp || !rel)
    return set_err(c, PGR_MD_ERR_NOT_FOUND, "transfer-classes-unavailable");
  const void **H = c.out->opt_handles; int rc;
  const void *res_enum = nullptr, *costume_enum = nullptr, *art_enum = nullptr, *asset_enum = nullptr;

  if ((rc = enum_i32_field(c, rel, "Status", 0x10, &H[H_TR_RESULT_STATUS], &res_enum))) return rc;
  if ((rc = typed_field(c, view, "dsvj", 0x70, "Niantic.Holoholo.IPokemonBag", "transfer-field", &H[H_TR_BAG]))) return rc;
  if ((rc = typed_field(c, view, "dsvo", 0x98, "IGameMasterData", "transfer-field", &H[H_TR_GM]))) return rc;
  if ((rc = typed_field(c, pk, "pokemonId_", 0x28, "System.Int32", "transfer-field", &H[H_TR_SPECIES]))) return rc;
  if ((rc = typed_field(c, pk, "individualAttack_", 0x70, "System.Int32", "transfer-field", &H[H_TR_ATTACK]))) return rc;
  if ((rc = typed_field(c, pk, "individualDefense_", 0x74, "System.Int32", "transfer-field", &H[H_TR_DEFENSE]))) return rc;
  if ((rc = typed_field(c, pk, "individualStamina_", 0x78, "System.Int32", "transfer-field", &H[H_TR_STAMINA]))) return rc;
  if ((rc = typed_field(c, pk, "favorite_", 0xb0, "System.Boolean", "transfer-field", &H[H_TR_FAVORITE]))) return rc;
  if ((rc = typed_field(c, pk, "isEgg_", 0x50, "System.Boolean", "transfer-field", &H[H_TR_EGG]))) return rc;
  if ((rc = typed_field(c, pk, "pokemonDisplay_", 0xd8, "Holoholo.Rpc.PokemonDisplayProto", "transfer-field", &H[H_TR_DISPLAY]))) return rc;
  if ((rc = ref_field(c, pk, "originEvents_", 0x188, &H[H_TR_EVENTS], &H[H_TR_EVENT_CLASS]))) return rc;
  if ((rc = typed_field(c, pk, "limitedPokemonIdentifier_", 0x130, "System.String", "transfer-field", &H[H_TR_LIMITED]))) return rc;
  if ((rc = enum_i32_field(c, disp, "Costume", 0x10, &H[H_TR_COSTUME], &costume_enum))) return rc;
  if ((rc = enum_i32_field(c, disp, "OriginalCostume", 0x40, &H[H_TR_ORIG_COSTUME]))) return rc;
  if ((rc = ref_field(c, disp, "LocationCard", 0x58, &H[H_TR_BACKGROUND]))) return rc;
  if ((rc = enum_i32_field(c, disp, "NaturalArtType", 0x70, &H[H_TR_ART], &art_enum))) return rc;
  if ((rc = enum_i32_field(c, disp, "NaturalArtBackgroundAsset", 0x74, &H[H_TR_ART_ASSET], &asset_enum))) return rc;

  const void *cb = nullptr;
  if ((rc = nested_suffix(c, bag, "ya", 256, true, "transfer-callback-class", &cb))) return rc;
  H[H_TR_CB_CLASS] = cb;
  if ((rc = typed_field(c, cb, "pokemon", 0x20, "Holoholo.Rpc.PokemonProto", "transfer-field", &H[H_TR_CB_POKEMON]))) return rc;
  if ((rc = typed_field(c, cb, "<>4__this", 0x10, "Niantic.Holoholo.Internal.PokemonBagImpl", "transfer-field", &H[H_TR_CB_BAG]))) return rc;

  static const char *const sz_names[5] = {"Xxs", "Xs", "M", "Xl", "Xxl"};
  for (int i = 0; i < 5; i++)
    if ((rc = enum_literal(c, csz, sz_names[i], false, "transfer-size-enum", &c.out->opt_ints[I_TR_SIZE_XXS + i]))) return rc;
  if ((rc = enum_literal(c, res_enum, "Success", true, "transfer-enum", &c.out->opt_ints[I_TR_RESULT_SUCCESS]))) return rc;
  if ((rc = enum_literal(c, costume_enum, "Unset", true, "transfer-enum", &c.out->opt_ints[I_TR_COSTUME_NONE]))) return rc;
  if ((rc = enum_literal(c, art_enum, "NaturalArtUnset", true, "transfer-enum", &c.out->opt_ints[I_TR_ART_NONE]))) return rc;
  if ((rc = enum_literal(c, asset_enum, "NaturalBackgroundUnset", true, "transfer-enum", &c.out->opt_ints[I_TR_ASSET_NONE]))) return rc;

  if ((rc = check_method(c, cb, "zlc", "System.Void", 1, "Holoholo.Rpc.ReleasePokemonOutProto", nullptr, false, false,
                         0xa0534a0ULL, kTrResult, sizeof(kTrResult), &H[H_TR_RESULT_M], &H[H_TR_RESULT_CODE]))) return rc;
  if ((rc = check_method(c, cb, "zld", "System.Void", 1, "System.String", nullptr, false, false,
                         0xa0538b4ULL, kTrError, sizeof(kTrError), &H[H_TR_ERROR_M], &H[H_TR_ERROR_CODE]))) return rc;
  if ((rc = check_method(c, pk, "PokemonCharacterSize", "Holoholo.Rpc.CharacterSize", 1, "IGameMasterData", nullptr, false, false,
                         0, nullptr, 0, &H[H_TR_SIZE_M], nullptr))) return rc;
  if ((rc = check_method(c, pk, "IsLegendaryOrMythicOrUltraBeast", "System.Boolean", 1, "IGameMasterData", nullptr, false, false,
                         0, nullptr, 0, &H[H_TR_RARE_M], nullptr))) return rc;
  if ((rc = check_method(c, pk, "get_IsShinyPokemon", "System.Boolean", 0, nullptr, nullptr, false, false,
                         0, nullptr, 0, &H[H_TR_SHINY_M], nullptr))) return rc;
  if ((rc = check_method(c, H[H_TR_EVENT_CLASS], "get_Count", "System.Int32", 0, nullptr, nullptr, false, true,
                         0, nullptr, 0, &H[H_TR_EVENT_COUNT_M], nullptr))) return rc;
  if ((rc = check_method(c, bag, "GetPokemon", "Holoholo.Rpc.PokemonProto", 1, "System.UInt64", nullptr, false, false,
                         0, nullptr, 0, &H[H_TR_GET_M], nullptr))) return rc;
  MParam rel_p[1] = {{"Holoholo.Rpc.PokemonProto", nullptr}};
  return check_method_ex(c, bag, "ReleasePokemon", nullptr, pred_release_promise, 1, rel_p, false, false,
                         0xa04d064ULL, kTrRelease, sizeof(kTrRelease), &H[H_TR_RELEASE_M], &H[H_TR_RELEASE_CODE]);
}

int do_spin(Ctx &c) {
  const void **H = c.out->opt_handles; int rc;
  static const int ids[6] = {OC_SPINSTATE, OC_SPINGUI, OC_GESTURE, OC_SPINNER, OC_SPINBASE, OC_SPINRESULT};
  const void *cls[6];
  for (int i = 0; i < 6; i++) {
    cls[i] = c.out->opt_classes[ids[i]];
    if (!cls[i]) return set_err(c, PGR_MD_ERR_NOT_FOUND, "spin-classes-unavailable");
    H[H_SPIN_SPINSTATE + i] = cls[i];
  }
  const void *spinstate = cls[0], *spingui = cls[1], *gesture = cls[2], *spinner = cls[3], *spinbase = cls[4], *spinresult = cls[5];

  if ((rc = typed_field(c, spinstate, "deyd", 0x128, "Niantic.Holoholo.Pokestop.IPokestopSpinnerInteractionGuiController", "spin-field", &H[H_SPIN_GUI_F]))) return rc;
  if ((rc = typed_field(c, spinstate, "deyc", 0x120, "Niantic.Holoholo.GameInput.ItemSpinnerGestureHandler", "spin-field", &H[H_SPIN_GESTURE_F]))) return rc;
  if ((rc = typed_field(c, gesture, "dkbf", 0x60, "Niantic.Holoholo.Map.IPoiSpinner", "spin-field", &H[H_SPIN_SPINNER_F]))) return rc;
  const void *result_enum = nullptr;
  if ((rc = enum_i32_field(c, spinresult, "Result", 0x10, &H[H_SPIN_RESULT_F], &result_enum))) return rc;
  H[H_SPIN_RESULT_ENUM] = result_enum;
  if ((rc = enum_literal(c, result_enum, "Success", true, "spin-success-literal", &c.out->opt_ints[I_SPIN_SUCCESS]))) return rc;

  if ((rc = check_method(c, spinstate, "EnterState", "System.Void", 0, nullptr, nullptr, false, false, 0xa4ea31cULL, kSpinEnter, sizeof(kSpinEnter), &H[H_SPIN_ENTER], &H[H_SPIN_ENTER_CODE]))) return rc;
  if ((rc = check_method(c, spinstate, "ExitState", "System.Void", 0, nullptr, nullptr, false, false, 0xa4eb840ULL, kSpinExit, sizeof(kSpinExit), &H[H_SPIN_EXIT], &H[H_SPIN_EXIT_CODE]))) return rc;
  if ((rc = check_method(c, spinstate, "bgff", "System.Void", 1, "Holoholo.Rpc.FortSearchOutProto", nullptr, false, false, 0xa4ec468ULL, kSpinResult, sizeof(kSpinResult), &H[H_SPIN_RESULT_M], &H[H_SPIN_RESULT_CODE]))) return rc;
  if ((rc = check_method(c, spingui, "ClickExit", "System.Void", 0, nullptr, nullptr, false, false, 0xa4e5da4ULL, kSpinClose, sizeof(kSpinClose), &H[H_SPIN_CLOSE], &H[H_SPIN_CLOSE_CODE]))) return rc;
  if ((rc = check_method(c, spinner, "Spin", "System.Void", 1, "System.Single", nullptr, false, false, 0x9e63b34ULL, kSpinSpin, sizeof(kSpinSpin), &H[H_SPIN_SPIN_M], &H[H_SPIN_SPIN_CODE]))) return rc;
  if ((rc = check_method(c, spinner, "cnqj", "System.Void", 0, nullptr, nullptr, false, false, 0x9e63b54ULL, kSpinRequest, sizeof(kSpinRequest), &H[H_SPIN_REQUEST], &H[H_SPIN_REQUEST_CODE]))) return rc;
  if ((rc = check_method(c, spinbase, "get_CanSpin", "System.Boolean", 0, nullptr, nullptr, false, false, 0x9e514e8ULL, kSpinCan, sizeof(kSpinCan), &H[H_SPIN_CAN], &H[H_SPIN_CAN_CODE]))) return rc;
  return check_method(c, gesture, "get_IsActive", "System.Boolean", 0, nullptr, nullptr, false, false, 0xa818d40ULL, kSpinActive, sizeof(kSpinActive), &H[H_SPIN_ACTIVE], &H[H_SPIN_ACTIVE_CODE]);
}

int do_spin_camera(Ctx &c) {
  const void *approach = c.out->opt_classes[OC_APPROACH], *resolve = c.out->opt_classes[OC_RESOLVE], *spline = c.out->opt_classes[OC_SPLINE];
  if (c.out->optional[OPT_SPIN] != PGR_SHARED_OPT_AVAILABLE || !approach || !resolve || !spline)
    return set_err(c, PGR_MD_ERR_NOT_FOUND, "spin-camera-classes");
  const void **H = c.out->opt_handles; int rc;
  const char *const SPLINE_NAME = "Niantic.Holoholo.CameraRig.SplineCameraController";

  const void *iterator = nullptr;
  if ((rc = nested_suffix(c, spline, "cso", 256, true, "spin-camera-iterator", &iterator))) return rc;
  H[H_SC_ITER] = iterator;
  if ((rc = typed_field(c, iterator, "<>4__this", 0x28, SPLINE_NAME, "spin-camera-field", &H[H_SC_THIS_F]))) return rc;
  if ((rc = typed_field(c, iterator, "djjp", 0x10, "System.Int32", "spin-camera-field", &H[H_SC_STATE_F]))) return rc;
  if ((rc = typed_field(c, iterator, "djjv", 0x50, "System.Single", "spin-camera-field", nullptr))) return rc;
  if ((rc = check_method(c, iterator, "MoveNext", "System.Boolean", 0, nullptr, nullptr, false, false, 0xa7b9460ULL, kScMove, sizeof(kScMove), &H[H_SC_MOVE], &H[H_SC_MOVE_CODE]))) return rc;

  H[H_SC_APPROACH] = approach;
  if ((rc = typed_field(c, approach, "splineCam", 0x78, SPLINE_NAME, "spin-camera-field", &H[H_SC_APP_CAM]))) return rc;
  if ((rc = check_method(c, approach, "EnterState", "System.Void", 0, nullptr, nullptr, false, false, 0xa4d166cULL, kScAppEnter, sizeof(kScAppEnter), &H[H_SC_APP_ENTER], &H[H_SC_APP_ENTER_CODE]))) return rc;
  if ((rc = check_method(c, approach, "ExitState", "System.Void", 0, nullptr, nullptr, false, false, 0xa4d2174ULL, kScAppExit, sizeof(kScAppExit), &H[H_SC_APP_EXIT], &H[H_SC_APP_EXIT_CODE]))) return rc;

  H[H_SC_RESOLVE] = resolve;
  if ((rc = typed_field(c, resolve, "splineCam", 0x58, SPLINE_NAME, "spin-camera-field", &H[H_SC_RES_CAM]))) return rc;
  if ((rc = check_method(c, resolve, "EnterState", "System.Void", 0, nullptr, nullptr, false, false, 0xa512b30ULL, kScResEnter, sizeof(kScResEnter), &H[H_SC_RES_ENTER], &H[H_SC_RES_ENTER_CODE]))) return rc;
  return check_method(c, resolve, "ExitState", "System.Void", 0, nullptr, nullptr, false, false, 0xa51329cULL, kScResExit, sizeof(kScResExit), &H[H_SC_RES_EXIT], &H[H_SC_RES_EXIT_CODE]);
}

void format_item_enum_name(const std::string &raw, char *out, size_t cap) {
  size_t start = starts_with(raw, "Item") ? 4 : 0;
  std::string res;
  for (size_t i = start; i < raw.size(); ++i) {
    char ch = raw[i];
    if (i > start) {
      char prev = raw[i - 1];
      bool prev_lower_or_digit = (prev >= 'a' && prev <= 'z') || (prev >= '0' && prev <= '9');
      bool cur_upper = (ch >= 'A' && ch <= 'Z');
      if (prev_lower_or_digit && cur_upper) res.push_back(' ');
    }
    res.push_back(ch);
  }
  std::snprintf(out, cap, "%s", res.c_str());
}

void upsert_item_name(PgrSharedMetadataOutput *out, int32_t val, const char *name) {
  for (uint32_t i = 0; i < out->item_name_count; ++i) {
    if (out->item_names[i].value == val) {
      std::snprintf(out->item_names[i].name, sizeof(out->item_names[i].name), "%s", name);
      return;
    }
  }
  if (out->item_name_count < kItemNameMax) {
    auto &e = out->item_names[out->item_name_count++];
    e.value = val;
    std::snprintf(e.name, sizeof(e.name), "%s", name);
  }
}

int select_method_by_sig(Ctx &c, const void *owner, const char *name, const char *ret,
                         uint32_t pc, const char *const *params, bool is_static,
                         const char *err_prefix, const void **m_out) {
  if (!owner) return set_err(c, PGR_MD_ERR_NOT_FOUND, err_prefix, name);
  auto enum_methods = reinterpret_cast<void *(*)(const void *, void **)>(c.core->class_get_methods);
  auto m_name = reinterpret_cast<char *(*)(const void *)>(c.core->method_get_name);
  auto m_generic = reinterpret_cast<bool (*)(const void *)>(c.core->method_is_generic);
  auto m_pcount = reinterpret_cast<uint32_t (*)(const void *)>(c.core->method_get_param_count);
  auto m_param = reinterpret_cast<void *(*)(const void *, uint32_t)>(c.core->method_get_param);
  auto m_ret = reinterpret_cast<void *(*)(const void *)>(c.core->method_get_return_type);
  auto m_flags = reinterpret_cast<uint32_t (*)(const void *, uint32_t *)>(c.core->method_get_flags);
  void *iter = nullptr;
  for (int i = 0; i < 4096; i++) {
    void *m = enum_methods(owner, &iter);
    if (!m) break;
    std::string nm;
    if (!read_utf8(m_name(m), &nm)) return set_err(c, PGR_MD_ERR_NAME_TOO_LONG, "name-limit");
    if (nm != name || m_generic(m) || m_pcount(m) != pc) continue;
    uint32_t impl = 0, fl = m_flags(m, &impl);
    if (((fl & 16u) != 0) != is_static) continue;
    std::string rn;
    if (!get_type_name(c, m_ret(m), &rn) || rn != ret) continue;
    bool ok = true;
    for (uint32_t j = 0; j < pc; ++j) {
      std::string pn;
      if (!get_type_name(c, m_param(m, j), &pn) || pn != params[j]) { ok = false; break; }
    }
    if (ok) { *m_out = m; return 0; }
  }
  return set_err(c, PGR_MD_ERR_NOT_FOUND, err_prefix, name);
}

int do_items_lookup(Ctx &c) {
  const void **H = c.out->opt_handles; int rc;
  const void *map = c.out->opt_classes[OC_MAP_SCENE_CTX], *ctx_c = c.out->opt_classes[OC_SCENE_CTX],
             *container = c.out->opt_classes[OC_DICONTAINER], *contract = c.out->opt_classes[OC_IITEM_SERVICE];
  if (!map || !ctx_c || !container || !contract)
    return set_err(c, PGR_MD_ERR_NOT_FOUND, "item-lookup-classes");
  const void *gui = c.out->opt_classes[OC_ITEM_GUI_SERVICE], *gui_contract = c.out->opt_classes[OC_IITEM_GUI_SERVICE];
  if (!gui || !gui_contract) return set_err(c, PGR_MD_ERR_NOT_FOUND, "item-gui-service");

  const void *singleton = nullptr;
  if ((rc = nested_suffix(c, gui, "<>c", 256, true, "item-filter-singleton", &singleton))) return rc;
  MParam tp[1] = {{nullptr, pred_item_tuple}};
  if ((rc = check_method_ex(c, gui, "cjpm", "System.Boolean", nullptr, 1, tp, false, false,
                            163081096ULL, kItCjpm, sizeof(kItCjpm), &H[H_IL_USABLE_M], &H[H_IL_USABLE_CODE]))) return rc;
  if ((rc = check_method_ex(c, singleton, "cjpd", "System.Boolean", nullptr, 1, tp, false, false,
                            163083736ULL, kItCjpd, sizeof(kItCjpd), &H[H_IL_SHOW_M], &H[H_IL_SHOW_CODE]))) return rc;

  auto field_from_name = reinterpret_cast<void *(*)(const void *, const char *)>(c.core->class_get_field_from_name);
  auto field_parent = reinterpret_cast<void *(*)(const void *)>(c.core->field_get_parent);
  auto field_flags = reinterpret_cast<int (*)(const void *)>(c.core->field_get_flags);
  void *inst = field_from_name(singleton, "<>9");
  if (!inst || field_parent(inst) != singleton || !(field_flags(inst) & 16))
    return set_err(c, PGR_MD_ERR_FLAGS, "item-filter-instance");

  if (!c.ex->method_get_object || !c.ex->runtime_class_init || !c.ex->type_get_object)
    return set_err(c, PGR_MD_ERR_MISSING_EXPORT, "item-lookup-exports");

  auto m_param = reinterpret_cast<void *(*)(const void *, uint32_t)>(c.core->method_get_param);
  H[H_IL_MAP] = map; H[H_IL_CONTRACT] = contract; H[H_IL_GUI] = gui; H[H_IL_GUI_CONTRACT] = gui_contract;
  H[H_IL_SINGLETON] = singleton; H[H_IL_INSTANCE] = inst;
  H[H_IL_FUNC_TYPE] = m_param(H[H_IT_LIST_M], 0);
  H[H_IL_REFLECT_EXP] = c.ex->method_get_object;
  H[H_IL_INIT_CLASS_EXP] = c.ex->runtime_class_init;
  H[H_IL_TYPE_OBJ_EXP] = c.ex->type_get_object;

  static const char *const cd_p[3] = {"System.Type", "System.Object", "System.Reflection.MethodInfo"};
  if ((rc = select_method_by_sig(c, c.out->opt_classes[OC_DELEGATE], "CreateDelegate", "System.Delegate",
                                 3, cd_p, true, "item-lookup-method", &H[H_IL_CREATE_DEL_M]))) return rc;
  static const char *const fo_p[1] = {"System.Type"};
  if ((rc = select_method_by_sig(c, c.out->classes[4], "FindObjectOfType", "UnityEngine.Object",
                                 1, fo_p, true, "item-lookup-method", &H[H_IL_FIND_M]))) return rc;
  if ((rc = check_method(c, ctx_c, "get_Container", "Zenject.DiContainer", 0, nullptr, nullptr, false, false,
                         0, nullptr, 0, &H[H_IL_CONTAINER_M], nullptr))) return rc;
  return select_method_by_sig(c, container, "Resolve", "System.Object", 1, fo_p, false, "item-lookup-method", &H[H_IL_RESOLVE_M]);
}

int do_items(Ctx &c) {
  const void *ibag = c.out->opt_classes[OC_IBAG], *isvc = c.out->opt_classes[OC_ISERVICE],
             *iresp = c.out->opt_classes[OC_IRESPONSE], *item = c.out->opt_classes[OC_ITEM],
             *del = c.out->opt_classes[OC_DELEGATE];
  if (!ibag || !isvc || !iresp || !item)
    return set_err(c, PGR_MD_ERR_NOT_FOUND, "item-classes");
  const void **H = c.out->opt_handles; int rc;

  MParam lp[5] = {
    {nullptr, pred_item_filter}, {"System.Boolean", nullptr}, {nullptr, pred_item_filter},
    {nullptr, pred_item_func_bool}, {nullptr, pred_item_action}
  };
  if ((rc = check_method_ex(c, isvc, "cjpz", nullptr, pred_item_data_list, 5, lp, false, false,
                            163091344ULL, kItCjpz, sizeof(kItCjpz), &H[H_IT_LIST_M], &H[H_IT_LIST_CODE]))) return rc;

  auto m_ret = reinterpret_cast<void *(*)(const void *)>(c.core->method_get_return_type);
  auto c_from_t = reinterpret_cast<void *(*)(const void *)>(c.core->class_from_type);
  auto m_from_name = reinterpret_cast<void *(*)(const void *, const char *, int)>(c.core->class_get_method_from_name);
  void *lk = c_from_t(m_ret(H[H_IT_LIST_M]));
  void *get_m = lk ? m_from_name(lk, "get_Item", 1) : nullptr;
  void *dk = get_m ? c_from_t(m_ret(get_m)) : nullptr;
  std::string dn;
  if (!dk || !get_class_name(c, dk, &dn)) return set_err(c, PGR_MD_ERR_NOT_FOUND, "item-data-class");
  if (!sep_suffix(dn.c_str(), "ItemData") && dn != "ItemData")
    return set_err(c, PGR_MD_ERR_TYPE, "item-data-class");
  H[H_IT_DK] = dk;

  const void *cb = nullptr;
  if ((rc = nested_suffix(c, ibag, "wy", 256, true, "item-callback-class", &cb))) return rc;
  H[H_IT_CB] = cb;

  const void *pk = nullptr;
  if ((rc = typed_field(c, cb, "prediction", 0x18, "Holoholo.Rpc.HoloInventoryItemProto", "item-field", &H[H_IT_PREDICTION], &pk))) return rc;
  if ((rc = check_method(c, pk, "get_Item", "Holoholo.Rpc.ItemProto", 0, nullptr, nullptr, false, false,
                         0, nullptr, 0, &H[H_IT_PROTO_M], nullptr))) return rc;
  void *ik = c_from_t(m_ret(H[H_IT_PROTO_M]));

  if ((rc = raw_field(c, del, "method", 0x28, &H[H_IT_DEL_METHOD]))) return rc;
  if ((rc = raw_field(c, del, "m_target", 0x20, &H[H_IT_DEL_TARGET]))) return rc;
  if ((rc = typed_field(c, isvc, "ehsf", 0x60, "Niantic.Holoholo.IItemBag", "item-field", &H[H_IT_BAG]))) return rc;
  if ((rc = enum_i32_field(c, dk, "item", 0x10, &H[H_IT_ID]))) return rc;
  if ((rc = typed_field(c, dk, "count", 0x1c, "System.Int32", "item-field", &H[H_IT_COUNT_F]))) return rc;
  if ((rc = typed_field(c, dk, "recyclable", 0x21, "System.Boolean", "item-field", &H[H_IT_RECYCLABLE]))) return rc;
  if ((rc = typed_field(c, cb, "<>4__this", 0x10, "Niantic.Holoholo.Internal.ItemBagImpl", "item-field", &H[H_IT_CB_BAG]))) return rc;
  if ((rc = typed_field(c, ik, "Item", 0x10, "Holoholo.Rpc.Item", "item-field", &H[H_IT_PROTO_ID]))) return rc;
  const void *status_enum = nullptr;
  if ((rc = enum_i32_field(c, iresp, "Result", 0x10, &H[H_IT_STATUS], &status_enum))) return rc;
  if ((rc = enum_literal(c, status_enum, "Success", false, "item-success", &c.out->opt_ints[I_ITEMS_SUCCESS]))) return rc;

  if (!c.ex->class_get_fields || !c.ex->field_get_name)
    return set_err(c, PGR_MD_ERR_MISSING_EXPORT, "item-enum-exports");
  auto get_fields = reinterpret_cast<void *(*)(const void *, void **)>(c.ex->class_get_fields);
  auto get_fname = reinterpret_cast<const char *(*)(const void *)>(c.ex->field_get_name);
  auto field_flags = reinterpret_cast<int (*)(const void *)>(c.core->field_get_flags);
  auto static_get = reinterpret_cast<void (*)(const void *, void *)>(c.ex->field_static_get_value);
  void *ei = nullptr;
  c.out->item_name_count = 0;
  for (int i = 0; i < 2048; i++) {
    void *f = get_fields(item, &ei);
    if (!f) break;
    if ((static_cast<uint32_t>(field_flags(f)) & 0x8050u) != 0x8050u) continue;
    int32_t v = 0; static_get(f, &v);
    std::string raw_nm;
    if (!read_utf8(get_fname(f), &raw_nm)) return set_err(c, PGR_MD_ERR_NAME_TOO_LONG, "name-limit");
    char formatted[60];
    format_item_enum_name(raw_nm, formatted, sizeof(formatted));
    upsert_item_name(c.out, v, formatted);
  }
  upsert_item_name(c.out, 1, "Poke Ball");
  upsert_item_name(c.out, 401, "Incense");
  upsert_item_name(c.out, 501, "Lure Module");

  // Nested optional: items.lookup (failure is isolated and never aborts items).
  char saved_reason[kReasonMax];
  std::memcpy(saved_reason, c.reason, sizeof(saved_reason));
  int lrc = do_items_lookup(c);
  c.out->optional[OPT_ITEMS_LOOKUP] = lrc ? PGR_SHARED_OPT_UNAVAILABLE : PGR_SHARED_OPT_AVAILABLE;
  c.out->optional_error[OPT_ITEMS_LOOKUP] = lrc;
  if (lrc) std::snprintf(c.out->optional_reason[OPT_ITEMS_LOOKUP], kReasonMax, "%s", c.reason);
  std::memcpy(c.reason, saved_reason, sizeof(saved_reason));

  if ((rc = check_method(c, isvc, "cjpx", "System.Void", 0, nullptr, nullptr, false, false,
                         163086416ULL, kItCjpx, sizeof(kItCjpx), &H[H_IT_INIT_M], &H[H_IT_INIT_CODE]))) return rc;
  if ((rc = check_method(c, lk, "get_Count", "System.Int32", 0, nullptr, nullptr, false, true,
                         0, nullptr, 0, &H[H_IT_COUNT_M], nullptr))) return rc;
  if ((rc = check_method(c, lk, "get_Item", dn.c_str(), 1, "System.Int32", nullptr, false, true,
                         0, nullptr, 0, &H[H_IT_GET_M], nullptr))) return rc;
  if ((rc = check_method_ex(c, isvc, "get_ExpiringItemsCopy", nullptr, pred_item_set, 0, nullptr, false, false,
                            163086408ULL, kItExpiring, sizeof(kItExpiring), &H[H_IT_EXPIRING_M], &H[H_IT_EXPIRING_CODE]))) return rc;
  if ((rc = check_method(c, ibag, "GetItemCount", "System.Int32", 1, "Holoholo.Rpc.Item", nullptr, false, false,
                         167765300ULL, kItGetCount, sizeof(kItGetCount), &H[H_IT_QUANTITY_M], &H[H_IT_QUANTITY_CODE]))) return rc;
  MParam rec_p[3] = {{dn.c_str(), nullptr}, {"System.Int32", nullptr}, {nullptr, pred_item_set}};
  if ((rc = check_method_ex(c, ibag, "RecycleItem", nullptr, pred_recycle_promise, 3, rec_p, false, false,
                            167800040ULL, kItRecycle, sizeof(kItRecycle), &H[H_IT_RECYCLE_M], &H[H_IT_RECYCLE_CODE]))) return rc;
  if ((rc = check_method(c, cb, "zid", "System.Void", 1, "Holoholo.Rpc.RecycleItemOutProto", nullptr, false, false,
                         167826240ULL, kItZid, sizeof(kItZid), &H[H_IT_RESULT_M], &H[H_IT_RESULT_CODE]))) return rc;
  return check_method(c, cb, "zie", "System.Void", 1, "System.String", nullptr, false, false,
                      167827064ULL, kItZie, sizeof(kItZie), &H[H_IT_ERROR_M], &H[H_IT_ERROR_CODE]);
}

int do_rocket(Ctx &c) {
  const void *rocket = c.out->opt_classes[OC_ROCKET], *rui = c.out->opt_classes[OC_RUI],
             *rdir = c.out->opt_classes[OC_RDIR], *rcharge = c.out->opt_classes[OC_RCHARGE],
             *rshield = c.out->opt_classes[OC_RSHIELD], *sel = c.out->opt_classes[OC_SELECTABLE];
  if (!rocket || !rui || !rdir || !rcharge || !rshield || !sel)
    return set_err(c, PGR_MD_ERR_NOT_FOUND, "rocket-classes-unavailable");
  const void **H = c.out->opt_handles; int rc;
  const void *rvui = c.out->opt_classes[OC_RVUI], *rvgest = c.out->opt_classes[OC_RVGEST],
             *rvbase = c.out->opt_classes[OC_RVBASE], *rvcharge = c.out->opt_classes[OC_RVCHARGE];

  if ((rc = ref_field(c, rvui, "eovv", 0x260, &H[H_RK_VN_GESTURE]))) return rc;
  if ((rc = typed_field(c, rvbase, "FastAttackInputEnabled", 0x9a, "System.Boolean", "rocket-field", &H[H_RK_VN_ENABLED]))) return rc;
  if ((rc = ref_field(c, rvbase, "tapGesture", 0x70, &H[H_RK_VN_TAP]))) return rc;
  if ((rc = ref_field(c, rvcharge, "attackButton", 0x28, &H[H_RK_VN_BUTTON]))) return rc;
  if ((rc = check_method(c, rvui, "UpdateUiWidgets", "System.Void", 1, "Niantic.Holoholo.VNext.BattleState", nullptr, false, false, 152955364ULL, kRvUpdate, sizeof(kRvUpdate), &H[H_RK_VN_UPDATE_M], &H[H_RK_VN_UPDATE_CODE]))) return rc;
  if ((rc = check_method(c, rvui, "HandleDismiss", "System.Void", 0, nullptr, nullptr, false, false, 152947860ULL, kRvClose, sizeof(kRvClose), &H[H_RK_VN_CLOSE_M], &H[H_RK_VN_CLOSE_CODE]))) return rc;
  if ((rc = check_method(c, rvui, "get_IsInputBlocked", "System.Boolean", 0, nullptr, nullptr, false, false, 152950684ULL, kRvBlocked, sizeof(kRvBlocked), &H[H_RK_VN_BLOCKED_M], &H[H_RK_VN_BLOCKED_CODE]))) return rc;
  if ((rc = check_method(c, rvgest, "cqul", "System.Void", 1, "System.Object", nullptr, false, false, 153694288ULL, kRvFast, sizeof(kRvFast), &H[H_RK_VN_FAST_M], &H[H_RK_VN_FAST_CODE]))) return rc;
  if ((rc = check_method(c, rvcharge, "ObserveBattleState", "System.Void", 1, "Niantic.Holoholo.VNext.BattleState", nullptr, false, false, 154064256ULL, kRvObserve, sizeof(kRvObserve), &H[H_RK_VN_OBSERVE_M], &H[H_RK_VN_OBSERVE_CODE]))) return rc;
  if ((rc = check_method(c, rvcharge, "ChargeAttackButtonTapped", "System.Void", 0, nullptr, nullptr, false, false, 154083804ULL, kRvCharge, sizeof(kRvCharge), &H[H_RK_VN_CHARGE_M], &H[H_RK_VN_CHARGE_CODE]))) return rc;
  if ((rc = check_method(c, rvcharge, "get_IsCharged", "System.Boolean", 0, nullptr, nullptr, false, false, 154057736ULL, kRvCharged, sizeof(kRvCharged), &H[H_RK_VN_CHARGED_M], &H[H_RK_VN_CHARGED_CODE]))) return rc;
  if ((rc = check_method(c, rvcharge, "get_IsVisible", "System.Boolean", 0, nullptr, nullptr, false, false, 154057800ULL, kRvVisible, sizeof(kRvVisible), &H[H_RK_VN_VISIBLE_M], &H[H_RK_VN_VISIBLE_CODE]))) return rc;

  const void *vec3 = c.out->opt_classes[OC_VECTOR3];
  int align = 0;
  auto val_size = reinterpret_cast<int (*)(const void *, int *)>(c.core->class_value_size);
  if (!vec3 || val_size(vec3, &align) != 12) return set_err(c, PGR_MD_ERR_SIZE, "minigame-vector-abi");

  const void *rmini = c.out->opt_classes[OC_RMINI], *rminibase = c.out->opt_classes[OC_RMINIBASE],
             *comp = c.out->opt_classes[OC_COMPONENT], *trans = c.out->opt_classes[OC_TRANSFORM],
             *go = c.out->opt_classes[OC_GAMEOBJECT];
  if ((rc = ref_field(c, rmini, "tapCollider", 0xd8, &H[H_RK_MN_COLLIDER]))) return rc;
  if ((rc = typed_field(c, rminibase, "combatIsRunning", 0xb0, "System.Boolean", "rocket-field", &H[H_RK_MN_RUNNING]))) return rc;
  if ((rc = check_method(c, rmini, "UpdateMinigame", "System.Void", 1, "System.Int64", nullptr, false, false, 179051108ULL, kRmUpdate, sizeof(kRmUpdate), &H[H_RK_MN_UPDATE_M], &H[H_RK_MN_UPDATE_CODE]))) return rc;
  if ((rc = check_method(c, rmini, "ExitGame", "System.Void", 0, nullptr, nullptr, false, false, 179057384ULL, kRmExit, sizeof(kRmExit), &H[H_RK_MN_EXIT_M], &H[H_RK_MN_EXIT_CODE]))) return rc;
  if ((rc = check_method(c, rmini, "Hide", "System.Void", 0, nullptr, nullptr, false, false, 179046660ULL, kRmHide, sizeof(kRmHide), &H[H_RK_MN_HIDE_M], &H[H_RK_MN_HIDE_CODE]))) return rc;
  if ((rc = check_method(c, rmini, "OnDestroy", "System.Void", 0, nullptr, nullptr, false, false, 179047376ULL, kRmDestroy, sizeof(kRmDestroy), &H[H_RK_MN_DESTROY_M], &H[H_RK_MN_DESTROY_CODE]))) return rc;
  if ((rc = check_method(c, comp, "get_transform", "UnityEngine.Transform", 0, nullptr, nullptr, false, false, 0, nullptr, 0, &H[H_RK_MN_TRANSFORM_M], nullptr))) return rc;
  if ((rc = check_method(c, comp, "get_gameObject", "UnityEngine.GameObject", 0, nullptr, nullptr, false, false, 0, nullptr, 0, &H[H_RK_MN_GAMEOBJ_M], nullptr))) return rc;
  if ((rc = check_method(c, trans, "get_localScale", "UnityEngine.Vector3", 0, nullptr, nullptr, false, false, 0, nullptr, 0, &H[H_RK_MN_SCALE_M], nullptr))) return rc;
  if ((rc = check_method(c, trans, "set_localScale", "System.Void", 1, "UnityEngine.Vector3", nullptr, false, false, 0, nullptr, 0, &H[H_RK_MN_SETSCALE_M], nullptr))) return rc;
  if ((rc = check_method(c, go, "get_activeSelf", "System.Boolean", 0, nullptr, nullptr, false, false, 0, nullptr, 0, &H[H_RK_MN_ACTIVE_M], nullptr))) return rc;
  if ((rc = check_method(c, go, "SetActive", "System.Void", 1, "System.Boolean", nullptr, false, false, 0, nullptr, 0, &H[H_RK_MN_SETACTIVE_M], nullptr))) return rc;

  if ((rc = ref_field(c, rui, "dofo", 0x2c0, &H[H_RK_FIRST]))) return rc;
  if ((rc = ref_field(c, rui, "dofp", 0x2c8, &H[H_RK_SECOND]))) return rc;
  if ((rc = typed_field(c, rcharge, "<IsCharged>k__BackingField", 0x1a0, "System.Boolean", "rocket-field", &H[H_RK_CHARGED]))) return rc;
  if ((rc = typed_field(c, rcharge, "<InputEnabled>k__BackingField", 0x1a3, "System.Boolean", "rocket-field", &H[H_RK_INPUT]))) return rc;
  if ((rc = typed_field(c, rshield, "dnve", 0x178, "System.Int32", "rocket-field", &H[H_RK_SHIELDS]))) return rc;
  if ((rc = ref_field(c, rshield, "shieldButton", 0x118, &H[H_RK_BUTTON]))) return rc;

  if ((rc = check_method(c, rocket, "EnterState", "System.Void", 2, "Niantic.Platform.GameStates.StateLogic.IStateApi", "Niantic.Holoholo.Combat.Contexts.InvasionCombatState.Data", false, false, 179334704ULL, kRkEnter, sizeof(kRkEnter), &H[H_RK_ENTER_M], &H[H_RK_ENTER_CODE]))) return rc;
  if ((rc = check_method(c, rocket, "ExitState", "System.Void", 0, nullptr, nullptr, false, false, 179351272ULL, kRkExit, sizeof(kRkExit), &H[H_RK_EXIT_M], &H[H_RK_EXIT_CODE]))) return rc;
  if ((rc = check_method(c, rui, "UpdateActionsUI", "System.Void", 1, "System.Boolean", nullptr, false, false, 179283360ULL, kRkUpdate, sizeof(kRkUpdate), &H[H_RK_UPDATE_M], &H[H_RK_UPDATE_CODE]))) return rc;
  if ((rc = check_method(c, rui, "Initialize", "System.Void", 1, "Niantic.Holoholo.Combat.UI.CombatUI.Data", nullptr, false, false, 179268616ULL, kRkUi, sizeof(kRkUi), &H[H_RK_UI_M], &H[H_RK_UI_CODE]))) return rc;
  if ((rc = check_method(c, rui, "HandleDismiss", "System.Void", 0, nullptr, nullptr, false, false, 179270220ULL, kRkClose, sizeof(kRkClose), &H[H_RK_CLOSE_M], &H[H_RK_CLOSE_CODE]))) return rc;
  if ((rc = check_method(c, rui, "bpro", "Niantic.Holoholo.Combat.ICombatDirectorV2", 0, nullptr, nullptr, false, false, 179262080ULL, kRkDirector, sizeof(kRkDirector), &H[H_RK_DIRECTOR_M], &H[H_RK_DIRECTOR_CODE]))) return rc;
  if ((rc = check_method(c, rdir, "get_IsInActiveState", "System.Boolean", 0, nullptr, nullptr, false, false, 178007452ULL, kRkActive, sizeof(kRkActive), &H[H_RK_ACTIVE_M], &H[H_RK_ACTIVE_CODE]))) return rc;
  if ((rc = check_method(c, rdir, "TryFastAttack", "Holoholo.Rpc.CombatActionProto", 0, nullptr, nullptr, false, false, 178019108ULL, kRkFast, sizeof(kRkFast), &H[H_RK_FAST_M], &H[H_RK_FAST_CODE]))) return rc;
  if ((rc = check_method(c, rcharge, "OnTap", "System.Void", 0, nullptr, nullptr, false, false, 179204872ULL, kRkCharge, sizeof(kRkCharge), &H[H_RK_CHARGE_M], &H[H_RK_CHARGE_CODE]))) return rc;
  if ((rc = check_method(c, rcharge, "get_IsVisible", "System.Boolean", 0, nullptr, nullptr, false, false, 179194140ULL, kRkVisible, sizeof(kRkVisible), &H[H_RK_VISIBLE_M], &H[H_RK_VISIBLE_CODE]))) return rc;
  if ((rc = check_method(c, rshield, "ShowShieldPrompt", "System.Void", 1, "System.Boolean", nullptr, false, false, 179031204ULL, kRkPrompt, sizeof(kRkPrompt), &H[H_RK_PROMPT_M], &H[H_RK_PROMPT_CODE]))) return rc;
  if ((rc = check_method(c, rshield, "ShieldUiClicked", "System.Void", 0, nullptr, nullptr, false, false, 179036768ULL, kRkShield, sizeof(kRkShield), &H[H_RK_SHIELD_M], &H[H_RK_SHIELD_CODE]))) return rc;
  if ((rc = check_method(c, rshield, "HandleDismiss", "System.Void", 0, nullptr, nullptr, false, false, 179036540ULL, kRkShClose, sizeof(kRkShClose), &H[H_RK_SHIELD_CLOSE_M], &H[H_RK_SHIELD_CLOSE_CODE]))) return rc;
  return check_method(c, sel, "IsInteractable", "System.Boolean", 0, nullptr, nullptr, false, false, 0, nullptr, 0, &H[H_RK_INTERACTABLE_M], nullptr);
}

// ---------------------------------------------------------------------------------------------------
// Prologues + resolvers for battles, berries, incubator, buddy (11..14).
// ---------------------------------------------------------------------------------------------------
constexpr unsigned char kBt9182En[24]    = P24(ff,83,01,d1,fe,13,00,f9,f8,5f,03,a9,f6,57,04,a9,f4,4f,05,a9,d4,0e,02,f0);
constexpr unsigned char kBt9182Dis[24]   = P24(fe,0f,1d,f8,f6,57,01,a9,f4,4f,02,a9,d4,0e,02,f0,f3,03,00,aa,88,4e,44,39);
constexpr unsigned char kBt9182Btns[24]  = P24(68,20,02,b0,01,0c,0b,f8,08,c1,44,b9,a8,01,00,34,fe,0f,1f,f8,09,80,52,d3);
constexpr unsigned char kBt9182Bwqr[24]  = P24(ff,03,03,d1,ea,23,00,fd,e9,23,05,6d,fd,7b,06,a9,fc,6f,07,a9,fa,67,08,a9);
constexpr unsigned char kBt9308Chgd[24]  = P24(00,e0,46,39,c0,03,5f,d6,01,e0,06,39,c0,03,5f,d6,00,e4,46,39,c0,03,5f,d6);
constexpr unsigned char kBt9308Init[24]  = P24(ff,83,02,d1,ea,13,00,fd,e9,23,03,6d,fd,7b,04,a9,fc,6f,05,a9,fa,67,06,a9);
constexpr unsigned char kBt9308Tap[24]   = P24(e8,0f,19,fc,fd,7b,01,a9,fc,6f,02,a9,fa,67,03,a9,f8,5f,04,a9,f6,57,05,a9);
constexpr unsigned char kBt10591En[24]   = P24(fd,7b,ba,a9,fc,6f,01,a9,fa,67,02,a9,f8,5f,03,a9,f6,57,04,a9,f4,4f,05,a9);
constexpr unsigned char kBt10591Ex[24]   = P24(fd,7b,ba,a9,fc,6f,01,a9,fa,67,02,a9,f8,5f,03,a9,f6,57,04,a9,f4,4f,05,a9);
constexpr unsigned char kBt31244En[24]   = P24(ff,03,02,d1,fd,7b,02,a9,fc,6f,03,a9,fa,67,04,a9,f8,5f,05,a9,f6,57,06,a9);
constexpr unsigned char kBt31244Ex[24]   = P24(fe,57,be,a9,f4,4f,01,a9,d4,f4,02,d0,f3,03,00,aa,88,ae,6a,39,a8,01,00,37);
constexpr unsigned char kBt31280Cln[24]  = P24(fe,0f,1c,f8,f8,5f,01,a9,f6,57,02,a9,f4,4f,03,a9,54,f4,02,f0,f3,03,00,aa);
constexpr unsigned char kBt31280Ref[24]  = P24(08,00,40,f9,02,39,41,f9,03,35,41,f9,60,00,1f,d6,e1,00,00,b4,08,18,41,b9);
constexpr unsigned char kBt31285Cqjz[24] = P24(fe,4f,bf,a9,f3,03,00,aa,1e,6a,00,94,68,fa,40,b9,08,01,00,34,08,0d,00,51);
constexpr unsigned char kBt31351Btns[24] = P24(48,05,03,b0,01,8c,0e,f8,08,c1,44,b9,a8,01,00,34,fe,0f,1f,f8,09,80,52,d3);
constexpr unsigned char kBt31351En[24]   = P24(fe,0f,1c,f8,f8,5f,01,a9,f6,57,02,a9,f4,4f,03,a9,74,f3,02,90,f3,03,00,aa);
constexpr unsigned char kBt31351Dis[24]  = P24(fe,0f,1d,f8,f6,57,01,a9,f4,4f,02,a9,74,f3,02,90,f3,03,00,aa,88,62,6e,39);
constexpr unsigned char kBt31499Cqjz[24] = P24(fe,4f,bf,a9,f3,03,00,aa,e8,b9,fe,97,e0,03,13,aa,e1,03,1f,aa,fe,4f,c1,a8);
constexpr unsigned char kBt31514En[24]   = P24(fe,0f,1b,f8,fa,67,01,a9,f8,5f,02,a9,f6,57,03,a9,f4,4f,04,a9,b6,f0,02,f0);
constexpr unsigned char kBt31514Ex[24]   = P24(fe,57,be,a9,f4,4f,01,a9,94,f0,02,f0,f3,03,00,aa,88,3a,75,39,a8,01,00,37);
constexpr unsigned char kBt31351Cqpd[24] = P24(fe,5f,bd,a9,f6,57,01,a9,f4,4f,02,a9,55,f3,02,d0,f3,03,01,2a,f4,03,00,aa);
constexpr unsigned char kBt9182Bwqu[24]  = P24(e8,0f,1d,fc,fe,07,00,f9,f6,57,01,a9,f4,4f,02,a9,d5,0e,02,d0,f4,03,01,aa);
constexpr unsigned char kBt31623Crlu[24] = P24(fe,57,be,a9,f4,4f,01,a9,74,ee,02,b0,f3,03,00,aa,88,8a,7a,39,48,02,00,37);
constexpr unsigned char kBt31263Cqim[24] = P24(e8,0f,1e,fc,fe,4f,01,a9,03,40,40,bd,08,40,20,1e,01,e4,00,2f,02,10,2e,1e);
constexpr unsigned char kBt9283Move[24]  = P24(ff,83,01,d1,e9,23,02,6d,fe,1b,00,f9,f6,57,04,a9,f4,4f,05,a9,14,0d,02,d0);
constexpr unsigned char kBt31569Cur[24]  = P24(00,88,40,f9,c0,03,5f,d6,00,60,44,39,c0,03,5f,d6,01,60,04,39,c0,03,5f,d6);
constexpr unsigned char kBt31351Cqpa[24] = P24(fe,67,bc,a9,f8,5f,01,a9,f6,57,02,a9,f4,4f,03,a9,53,f3,02,f0,f4,03,00,aa);
constexpr unsigned char kBt31351Dst[24]  = P24(fe,5f,bd,a9,f6,57,01,a9,f4,4f,02,a9,74,f3,02,b0,f3,03,00,aa,88,5e,6e,39);
constexpr unsigned char kBt9322Show[32]  = P32(fe,0f,1d,f8,f6,57,01,a9,f4,4f,02,a9,74,0c,02,f0,f3,03,00,aa,88,fe,4a,39,08,01,00,37,60,e2,01,b0);
constexpr unsigned char kBt9322Dst[32]   = P32(fe,5f,bd,a9,f6,57,01,a9,f4,4f,02,a9,54,0c,02,d0,f3,03,00,aa,88,52,4a,39,e8,02,00,37,a0,e0,01,d0);
constexpr unsigned char kBt9322Open[32]  = P32(fe,0f,1c,f8,f8,5f,01,a9,f6,57,02,a9,f4,4f,03,a9,54,0c,02,d0,f3,03,00,aa,88,56,4a,39,08,01,00,37);
constexpr unsigned char kBt9322Dism[32]  = P32(fe,0f,1d,f8,f6,57,01,a9,f4,4f,02,a9,74,0c,02,f0,f3,03,00,aa,88,06,4b,39,08,01,00,37,60,e2,01,b0);
constexpr unsigned char kBt31337Show[32] = P32(00,44,42,39,c0,03,5f,d6,01,44,02,39,c0,03,5f,d6,ff,03,01,d1,fe,57,02,a9,f4,4f,03,a9,94,f3,02,d0);
constexpr unsigned char kBt31337Cln[32]  = P32(ff,03,01,d1,fe,57,02,a9,f4,4f,03,a9,94,f3,02,d0,f3,03,00,aa,88,e6,6d,39,88,03,00,37,60,cb,02,b0);
constexpr unsigned char kBt31337Open[32] = P32(fe,0f,1c,f8,f8,5f,01,a9,f6,57,02,a9,f4,4f,03,a9,94,f3,02,d0,f3,03,00,aa,88,ee,6d,39,08,01,00,37);
constexpr unsigned char kBt31337Hide[32] = P32(fe,5f,bd,a9,f6,57,01,a9,f4,4f,02,a9,94,f3,02,b0,f3,03,00,aa,88,fa,6d,39,08,01,00,37,20,c9,02,f0);
constexpr unsigned char kBt31337Dism[32] = P32(07,ff,ff,17,fe,0f,1d,f8,f6,57,01,a9,f4,4f,02,a9,94,f3,02,b0,f3,03,00,aa,88,fe,6d,39,48,02,00,37);
constexpr unsigned char kBt31639Show[32] = P32(fe,0f,1d,f8,f6,57,01,a9,f4,4f,02,a9,54,ee,02,b0,f3,03,00,aa,88,02,7c,39,08,01,00,37,80,c4,02,b0);
constexpr unsigned char kBt31639Dst[32]  = P32(fe,0f,1c,f8,f8,5f,01,a9,f6,57,02,a9,f4,4f,03,a9,14,ee,02,90,f3,03,00,aa,88,6e,7b,39,a8,01,00,37);
constexpr unsigned char kBt31639Open[32] = P32(fe,67,bc,a9,f8,5f,01,a9,f6,57,02,a9,f4,4f,03,a9,14,ee,02,90,f3,03,00,aa,88,06,7c,39,08,06,00,37);
constexpr unsigned char kBt31639Dism[32] = P32(fe,0f,1d,f8,f6,57,01,a9,f4,4f,02,a9,54,ee,02,b0,f3,03,00,aa,88,0e,7c,39,08,01,00,37,80,c4,02,b0);

// berries (BERRY_CODE + BERRY_THROW_CODE)
constexpr unsigned char kBrCan[24]       = P24(08,18,42,b9,1f,01,00,71,e0,17,9f,1a,c0,03,5f,d6,00,b0,4a,39,c0,03,5f,d6);
constexpr unsigned char kBrPhoto[24]     = P24(fe,57,be,a9,f4,4f,01,a9,d4,1e,02,d0,f3,03,00,aa,88,d2,59,39,08,01,00,37);
constexpr unsigned char kBrBall[24]      = P24(fe,57,be,a9,f4,4f,01,a9,d4,1e,02,d0,f3,03,00,aa,88,06,5c,39,08,01,00,37);
constexpr unsigned char kBrSelect[24]    = P24(fe,0f,1b,f8,fa,67,01,a9,f8,5f,02,a9,f6,57,03,a9,f4,4f,04,a9,b4,1e,02,b0);
constexpr unsigned char kBrEnter[24]     = P24(fe,0f,1c,f8,f8,5f,01,a9,f6,57,02,a9,f4,4f,03,a9,b4,1e,02,b0,f3,03,00,aa);
constexpr unsigned char kBrExit[24]      = P24(fe,67,bc,a9,f8,5f,01,a9,f6,57,02,a9,f4,4f,03,a9,94,1e,02,b0,f3,03,00,aa);
constexpr unsigned char kBrConfirm[24]   = P24(fe,67,bc,a9,f8,5f,01,a9,f6,57,02,a9,f4,4f,03,a9,94,ab,02,d0,f3,03,00,aa);
constexpr unsigned char kBrBtdx[24]      = P24(fe,67,bc,a9,f8,5f,01,a9,f6,57,02,a9,f4,4f,03,a9,55,1e,02,f0,f4,03,01,2a);
constexpr unsigned char kBrBtdy[24]      = P24(e8,0f,1c,fc,fe,07,00,f9,f8,5f,01,a9,f6,57,02,a9,f4,4f,03,a9,55,1e,02,d0);
constexpr unsigned char kBrBtdz[24]      = P24(fe,57,be,a9,f4,4f,01,a9,08,08,40,f9,28,05,00,b4,14,1e,02,f0,13,fd,40,f9);
constexpr unsigned char kBrBread[24]     = P24(00,00,49,39,c0,03,5f,d6,01,00,09,39,c0,03,5f,d6,ff,03,01,d1,e8,0b,00,fd);
constexpr unsigned char kBrBallType[24]  = P24(00,74,43,b9,c0,03,5f,d6,01,74,03,b9,c0,03,5f,d6,e1,03,1f,aa,ae,05,b4,14);
constexpr unsigned char kBrSubmit[24]    = P24(fe,5f,bd,a9,f6,57,01,a9,f4,4f,02,a9,96,1b,02,90,f5,03,02,aa,f4,03,01,aa);
constexpr unsigned char kBrPeek[24]      = P24(fe,0f,1d,f8,f6,57,01,a9,f4,4f,02,a9,b4,1b,02,f0,f3,03,00,aa,88,86,62,39);

// incubator
constexpr unsigned char kIncPut[32]      = P32(ff,c3,01,d1,fe,13,00,f9,fa,67,03,a9,f8,5f,04,a9,f6,57,05,a9,f4,4f,06,a9,14,a7,02,d0,f6,03,02,aa);
constexpr unsigned char kIncList[32]     = P32(fe,57,be,a9,f4,4f,01,a9,94,34,02,d0,f3,03,00,aa,88,12,5c,39,08,01,00,37,20,0b,02,b0,00,80,1b,91);
constexpr unsigned char kIncEggs[32]     = P32(fe,0f,1d,f8,f6,57,01,a9,f4,4f,02,a9,14,84,02,90,f3,03,00,aa,88,9a,4f,39,08,01,00,37,e0,5b,02,f0);

// buddy (BUDDY_CODE)
constexpr unsigned char kBdRem[24]       = P24(fe,57,be,a9,f4,4f,01,a9,d4,27,02,b0,f3,03,00,aa,88,06,41,39,08,01,00,37);
constexpr unsigned char kBdFull[24]      = P24(00,e4,00,2f,80,00,00,b4,08,20,40,f9,48,00,00,b4,00,19,40,bd,c0,03,5f,d6);
constexpr unsigned char kBdCool[24]      = P24(fe,57,be,a9,f4,4f,01,a9,d4,27,02,b0,f3,03,00,aa,88,0a,41,39,08,01,00,37);
constexpr unsigned char kBdFeed[24]      = P24(fe,0f,1c,f8,f8,5f,01,a9,f6,57,02,a9,f4,4f,03,a9,d5,27,02,90,f3,03,02,2a);
constexpr unsigned char kBdPk[24]        = P24(00,b8,40,f9,c0,03,5f,d6,48,34,02,f0,01,b8,00,f9,08,c1,44,b9,c8,01,00,34);
constexpr unsigned char kBdHas[24]       = P24(00,e0,45,39,c0,03,5f,d6,01,e0,05,39,c0,03,5f,d6,fe,57,be,a9,f4,4f,01,a9);
constexpr unsigned char kBdBrhd[24]      = P24(fe,0f,1d,f8,f6,57,01,a9,f4,4f,02,a9,b5,27,02,f0,f3,03,01,aa,f4,03,00,aa);
constexpr unsigned char kBdStats[24]     = P24(00,38,40,f9,c0,03,5f,d6,48,39,02,d0,01,0c,07,f8,08,c1,44,b9,a8,01,00,34);

int do_battles(Ctx &c) {
  if (c.out->optional[OPT_ROCKET] != PGR_SHARED_OPT_AVAILABLE)
    return set_err(c, PGR_MD_ERR_NOT_FOUND, "vnext-attack-support");
  const void **H = c.out->opt_handles; int rc;
  const void *b31514 = c.out->opt_classes[OC_BATTLE_31514], *b10591 = c.out->opt_classes[OC_BATTLE_10591],
             *b31244 = c.out->opt_classes[OC_BATTLE_31244], *b31280 = c.out->opt_classes[OC_BATTLE_31280],
             *b31351 = c.out->opt_classes[OC_RVBASE],       *b9284  = c.out->opt_classes[OC_BATTLE_9284],
             *b44226 = c.out->opt_classes[OC_BATTLE_44226], *b9322  = c.out->opt_classes[OC_BATTLE_9322],
             *b31639 = c.out->opt_classes[OC_BATTLE_31639], *b31337 = c.out->opt_classes[OC_BATTLE_31337],
             *b9182  = c.out->opt_classes[OC_BATTLE_9182],  *b9308  = c.out->opt_classes[OC_BATTLE_9308],
             *b31569 = c.out->opt_classes[OC_BATTLE_31569], *b31623 = c.out->opt_classes[OC_BATTLE_31623],
             *b31263 = c.out->opt_classes[OC_BATTLE_31263], *b31499 = c.out->opt_classes[OC_BATTLE_31499],
             *b31285 = c.out->opt_classes[OC_BATTLE_31285];

  H[H_BT_SC0_CLASS] = b31514;
  if ((rc = check_method(c, b31514, "EnterState", "System.Void", 2, "Niantic.Platform.GameStates.StateLogic.IStateApi", "Niantic.Holoholo.Raid.RaidBattleLaunchData", false, false, 153812088ULL, kBt31514En, sizeof(kBt31514En), &H[H_BT_SC0_ENTER_M], &H[H_BT_SC0_ENTER_CODE]))) return rc;
  if ((rc = check_method(c, b31514, "ExitState", "System.Void", 0, nullptr, nullptr, false, false, 153828484ULL, kBt31514Ex, sizeof(kBt31514Ex), &H[H_BT_SC0_EXIT_M], &H[H_BT_SC0_EXIT_CODE]))) return rc;

  H[H_BT_SC1_CLASS] = b10591;
  if ((rc = check_method(c, b10591, "EnterState", "System.Void", 0, nullptr, nullptr, false, false, 156310396ULL, kBt10591En, sizeof(kBt10591En), &H[H_BT_SC1_ENTER_M], &H[H_BT_SC1_ENTER_CODE]))) return rc;
  if ((rc = check_method(c, b10591, "ExitState", "System.Void", 0, nullptr, nullptr, false, false, 156312036ULL, kBt10591Ex, sizeof(kBt10591Ex), &H[H_BT_SC1_EXIT_M], &H[H_BT_SC1_EXIT_CODE]))) return rc;

  H[H_BT_SC2_CLASS] = b31244;
  if ((rc = check_method(c, b31244, "EnterState", "System.Void", 2, "Niantic.Platform.GameStates.StateLogic.IStateApi", "Niantic.Holoholo.Bread.BreadLaunchData", false, false, 153265176ULL, kBt31244En, sizeof(kBt31244En), &H[H_BT_SC2_ENTER_M], &H[H_BT_SC2_ENTER_CODE]))) return rc;
  if ((rc = check_method(c, b31244, "ExitState", "System.Void", 0, nullptr, nullptr, false, false, 153275788ULL, kBt31244Ex, sizeof(kBt31244Ex), &H[H_BT_SC2_EXIT_M], &H[H_BT_SC2_EXIT_CODE]))) return rc;

  const void *max_enum = nullptr;
  if ((rc = enum_i32_field(c, b31280, "<ActiveBreadMoveType>k__BackingField", 0x134, &H[H_BT_F_MAX_TYPE], &max_enum))) return rc;
  if ((rc = enum_literal(c, max_enum, "BreadAttack", false, "max-attack-literal", &c.out->opt_ints[I_BAT_MAX_ATTACK]))) return rc;

  MParam dp[1] = {{nullptr, pred_direction_suffix}};
  if ((rc = check_method_ex(c, b31351, "cqpd", "System.Void", nullptr, 1, dp, false, false, 153469604ULL, kBt31351Cqpd, sizeof(kBt31351Cqpd), &H[H_BT_M_DODGE], &H[H_BT_M_DODGE_CODE]))) return rc;
  auto m_param = reinterpret_cast<void *(*)(const void *, uint32_t)>(c.core->method_get_param);
  auto c_from_t = reinterpret_cast<void *(*)(const void *)>(c.core->class_from_type);
  auto is_enum = reinterpret_cast<bool (*)(const void *)>(c.core->class_is_enum);
  auto enum_base = reinterpret_cast<void *(*)(const void *)>(c.core->class_enum_basetype);
  void *dir_cls = c_from_t(m_param(H[H_BT_M_DODGE], 0));
  std::string dir_base;
  if (!dir_cls || !is_enum(dir_cls) || !get_type_name(c, enum_base(dir_cls), &dir_base) || dir_base != "System.Int32")
    return set_err(c, PGR_MD_ERR_KIND, "dodge-direction-type");
  if ((rc = enum_literal(c, dir_cls, "Left", false, "dodge-left-literal", &c.out->opt_ints[I_BAT_DODGE_LEFT]))) return rc;

  const void *gym_iter = nullptr;
  if ((rc = nested_suffix(c, b9284, "epg", 64, false, "gym-warning-iterator", &gym_iter))) return rc;
  H[H_BT_GYM_ITER] = gym_iter;
  if ((rc = check_method(c, gym_iter, "MoveNext", "System.Boolean", 0, nullptr, nullptr, false, false, 183692812ULL, kBt9283Move, sizeof(kBt9283Move), &H[H_BT_M_GYM_WARN], &H[H_BT_M_GYM_WARN_CODE]))) return rc;
  if ((rc = raw_field(c, gym_iter, "duwi", 0x10, &H[H_BT_GYM_WARN_STATE]))) return rc;

  const void *status_cls = nullptr;
  if ((rc = enum_i32_field(c, b44226, "BattleStatus", 0x64, &H[H_BT_F_BATTLE_STATUS], &status_cls))) return rc;

  // swaps[0..2]
  H[H_BT_SW0_CLASS] = b9322;
  if ((rc = check_method(c, b9322, "Open", "System.Void", 0, nullptr, nullptr, false, false, 183790892ULL, kBt9322Open, sizeof(kBt9322Open), &H[H_BT_SW0_OPEN_M], &H[H_BT_SW0_OPEN_CODE]))) return rc;
  if ((rc = check_method(c, b9322, "Dismiss", "System.Void", 0, nullptr, nullptr, false, false, 183769212ULL, kBt9322Dism, sizeof(kBt9322Dism), &H[H_BT_SW0_CL0_M], &H[H_BT_SW0_CL0_CODE]))) return rc;
  if ((rc = check_method(c, b9322, "OnDestroy", "System.Void", 0, nullptr, nullptr, false, false, 183790408ULL, kBt9322Dst, sizeof(kBt9322Dst), &H[H_BT_SW0_CL1_M], &H[H_BT_SW0_CL1_CODE]))) return rc;
  if ((rc = check_method(c, b9322, "get_IsOpen", "System.Boolean", 0, nullptr, nullptr, false, false, 183768912ULL, kBt9322Show, sizeof(kBt9322Show), &H[H_BT_SW0_SHOW_M], &H[H_BT_SW0_SHOW_CODE]))) return rc;

  H[H_BT_SW1_CLASS] = b31639;
  if ((rc = check_method(c, b31639, "Open", "System.Void", 0, nullptr, nullptr, false, false, 154168180ULL, kBt31639Open, sizeof(kBt31639Open), &H[H_BT_SW1_OPEN_M], &H[H_BT_SW1_OPEN_CODE]))) return rc;
  if ((rc = check_method(c, b31639, "Dismiss", "System.Void", 0, nullptr, nullptr, false, false, 154130228ULL, kBt31639Dism, sizeof(kBt31639Dism), &H[H_BT_SW1_CL0_M], &H[H_BT_SW1_CL0_CODE]))) return rc;
  if ((rc = check_method(c, b31639, "OnDestroy", "System.Void", 0, nullptr, nullptr, false, false, 154167704ULL, kBt31639Dst, sizeof(kBt31639Dst), &H[H_BT_SW1_CL1_M], &H[H_BT_SW1_CL1_CODE]))) return rc;
  if ((rc = check_method(c, b31639, "get_IsOpen", "System.Boolean", 0, nullptr, nullptr, false, false, 154129928ULL, kBt31639Show, sizeof(kBt31639Show), &H[H_BT_SW1_SHOW_M], &H[H_BT_SW1_SHOW_CODE]))) return rc;

  H[H_BT_SW2_CLASS] = b31337;
  if ((rc = check_method(c, b31337, "OpenSwapMenu", "System.Void", 0, nullptr, nullptr, false, false, 153439844ULL, kBt31337Open, sizeof(kBt31337Open), &H[H_BT_SW2_OPEN_M], &H[H_BT_SW2_OPEN_CODE]))) return rc;
  if ((rc = check_method(c, b31337, "HideSwapMenu", "System.Void", 0, nullptr, nullptr, false, false, 153440928ULL, kBt31337Hide, sizeof(kBt31337Hide), &H[H_BT_SW2_CL0_M], &H[H_BT_SW2_CL0_CODE]))) return rc;
  if ((rc = check_method(c, b31337, "Cleanup", "System.Void", 0, nullptr, nullptr, false, false, 153438084ULL, kBt31337Cln, sizeof(kBt31337Cln), &H[H_BT_SW2_CL1_M], &H[H_BT_SW2_CL1_CODE]))) return rc;
  if ((rc = check_method(c, b31337, "get_IsShowing", "System.Boolean", 0, nullptr, nullptr, false, false, 153438068ULL, kBt31337Show, sizeof(kBt31337Show), &H[H_BT_SW2_SHOW_M], &H[H_BT_SW2_SHOW_CODE]))) return rc;

  if ((rc = enum_literal(c, status_cls, "Active", false, "battle-status", &c.out->opt_ints[I_BAT_STATUS_ACTIVE]))) return rc;
  if ((rc = enum_literal(c, status_cls, "Unset", false, "battle-status", &c.out->opt_ints[I_BAT_STATUS_UNSET]))) return rc;

  if ((rc = ref_field(c, b31244, "epkk", 0x70, &H[H_BT_F_MAX_GESTURE]))) return rc;
  if ((rc = ref_field(c, b31351, "epus", 0x20, &H[H_BT_F_DIRECTOR]))) return rc;
  if ((rc = ref_field(c, b9182, "horizontalFlickGesture", 0x60, &H[H_BT_F_LEG_FLICK]))) return rc;
  if ((rc = ref_field(c, b9182, "tapGesture", 0x58, &H[H_BT_F_LEG_TAP]))) return rc;
  if ((rc = ref_field(c, b9308, "attackButton", 0x28, &H[H_BT_F_LEG_BUTTON]))) return rc;
  if ((rc = ref_field(c, b31280, "button", 0x98, &H[H_BT_F_MAX_BUTTON]))) return rc;

  if ((rc = check_method(c, b31569, "get_CurrentState", "Niantic.Holoholo.VNext.BattleState", 0, nullptr, nullptr, false, false, 153929856ULL, kBt31569Cur, sizeof(kBt31569Cur), &H[H_BT_M_CUR_STATE], &H[H_BT_M_CUR_STATE_CODE]))) return rc;
  if ((rc = check_method(c, b31351, "cqpa", "System.Void", 0, nullptr, nullptr, false, false, 153467264ULL, kBt31351Cqpa, sizeof(kBt31351Cqpa), &H[H_BT_M_RAID_FAST_ACT], &H[H_BT_M_RAID_FAST_ACT_CODE]))) return rc;
  if ((rc = check_method(c, b31351, "OnDestroy", "System.Void", 0, nullptr, nullptr, false, false, 153460632ULL, kBt31351Dst, sizeof(kBt31351Dst), &H[H_BT_M_DESTROY], &H[H_BT_M_DESTROY_CODE]))) return rc;
  if ((rc = check_method(c, b31623, "crlu", "System.Void", 0, nullptr, nullptr, false, false, 154115704ULL, kBt31623Crlu, sizeof(kBt31623Crlu), &H[H_BT_M_RAID_WARN], &H[H_BT_M_RAID_WARN_CODE]))) return rc;
  if ((rc = check_method(c, b31263, "cqim", "System.Void", 1, "System.Single", nullptr, false, false, 153315360ULL, kBt31263Cqim, sizeof(kBt31263Cqim), &H[H_BT_M_MAX_WARN], &H[H_BT_M_MAX_WARN_CODE]))) return rc;
  if ((rc = check_method(c, b9182, "bwqu", "System.Void", 2, "System.Object", "System.EventArgs", false, false, 183462720ULL, kBt9182Bwqu, sizeof(kBt9182Bwqu), &H[H_BT_M_LEG_DODGE], &H[H_BT_M_LEG_DODGE_CODE]))) return rc;
  if ((rc = check_method(c, b31351, "OnEnable", "System.Void", 0, nullptr, nullptr, false, false, 153460988ULL, kBt31351En, sizeof(kBt31351En), &H[H_BT_M_ENABLE], &H[H_BT_M_ENABLE_CODE]))) return rc;
  if ((rc = check_method(c, b31351, "OnDisable", "System.Void", 0, nullptr, nullptr, false, false, 153462380ULL, kBt31351Dis, sizeof(kBt31351Dis), &H[H_BT_M_DISABLE], &H[H_BT_M_DISABLE_CODE]))) return rc;
  if ((rc = check_method(c, b31351, "SetChargeAttackButtons", "System.Void", 1, "Niantic.Holoholo.VNext.ChargeAttackDisplayVNext[]", nullptr, false, false, 153460496ULL, kBt31351Btns, sizeof(kBt31351Btns), &H[H_BT_M_BUTTONS], &H[H_BT_M_BUTTONS_CODE]))) return rc;
  if ((rc = check_method(c, b31499, "cqjz", "System.Void", 2, "System.Object", "System.EventArgs", false, false, 153796784ULL, kBt31499Cqjz, sizeof(kBt31499Cqjz), &H[H_BT_M_RAID_FAST], &H[H_BT_M_RAID_FAST_CODE]))) return rc;
  if ((rc = check_method(c, b31285, "cqjz", "System.Void", 2, "System.Object", "System.EventArgs", false, false, 153354200ULL, kBt31285Cqjz, sizeof(kBt31285Cqjz), &H[H_BT_M_MAX_FAST], &H[H_BT_M_MAX_FAST_CODE]))) return rc;
  if ((rc = check_method(c, b9182, "OnEnable", "System.Void", 0, nullptr, nullptr, false, false, 183456908ULL, kBt9182En, sizeof(kBt9182En), &H[H_BT_M_LEG_ENABLE], &H[H_BT_M_LEG_ENABLE_CODE]))) return rc;
  if ((rc = check_method(c, b9182, "OnDisable", "System.Void", 0, nullptr, nullptr, false, false, 183458624ULL, kBt9182Dis, sizeof(kBt9182Dis), &H[H_BT_M_LEG_DISABLE], &H[H_BT_M_LEG_DISABLE_CODE]))) return rc;
  if ((rc = check_method(c, b9182, "SetChargeAttackButtons", "System.Void", 1, "Niantic.Holoholo.Battle.ChargeAttackDisplay[]", nullptr, false, false, 183459176ULL, kBt9182Btns, sizeof(kBt9182Btns), &H[H_BT_M_LEG_BUTTONS], &H[H_BT_M_LEG_BUTTONS_CODE]))) return rc;
  if ((rc = check_method(c, b9182, "bwqr", "System.Void", 2, "System.Object", "System.EventArgs", false, false, 183459244ULL, kBt9182Bwqr, sizeof(kBt9182Bwqr), &H[H_BT_M_LEG_FAST], &H[H_BT_M_LEG_FAST_CODE]))) return rc;
  MParam li_p[3] = {{"Niantic.Holoholo.Battle.BattlePokemon", nullptr}, {"Niantic.Holoholo.Battle.BattlePokemon", nullptr}, {"UnityEngine.Vector3", nullptr}};
  if ((rc = check_method_ex(c, b9308, "Init", "System.Void", nullptr, 3, li_p, false, false, 183731452ULL, kBt9308Init, sizeof(kBt9308Init), &H[H_BT_M_LEG_INIT], &H[H_BT_M_LEG_INIT_CODE]))) return rc;
  if ((rc = check_method(c, b9308, "get_IsCharged", "System.Boolean", 0, nullptr, nullptr, false, false, 183731356ULL, kBt9308Chgd, sizeof(kBt9308Chgd), &H[H_BT_M_LEG_CHARGED], &H[H_BT_M_LEG_CHARGED_CODE]))) return rc;
  if ((rc = check_method(c, b9308, "ChargeAttackButtonTapped", "System.Void", 0, nullptr, nullptr, false, false, 183752404ULL, kBt9308Tap, sizeof(kBt9308Tap), &H[H_BT_M_LEG_CHARGE], &H[H_BT_M_LEG_CHARGE_CODE]))) return rc;
  if ((rc = check_method(c, b31280, "Refresh", "System.Void", 1, "Niantic.Holoholo.Bread.BreadAbilityWidget.Data", nullptr, false, false, 153342188ULL, kBt31280Ref, sizeof(kBt31280Ref), &H[H_BT_M_MAX_REFRESH], &H[H_BT_M_MAX_REFRESH_CODE]))) return rc;
  if ((rc = check_method(c, b31280, "Cleanup", "System.Void", 0, nullptr, nullptr, false, false, 153336952ULL, kBt31280Cln, sizeof(kBt31280Cln), &H[H_BT_M_MAX_CLEANUP], &H[H_BT_M_MAX_CLEANUP_CODE]))) return rc;
  if ((rc = check_method(c, c.out->opt_classes[OC_BEHAVIOUR], "get_isActiveAndEnabled", "System.Boolean", 0, nullptr, nullptr, false, false, 0, nullptr, 0, &H[H_BT_M_ACTIVE], nullptr))) return rc;
  return check_method(c, c.out->opt_classes[OC_BUTTON], "Press", "System.Void", 0, nullptr, nullptr, false, false, 0, nullptr, 0, &H[H_BT_M_PRESS], nullptr);
}

int do_berries(Ctx &c) {
  if (c.out->optional[OPT_ITEMS] != PGR_SHARED_OPT_AVAILABLE)
    return set_err(c, PGR_MD_ERR_NOT_FOUND, "item-bag-unavailable");
  const void **H = c.out->opt_handles; int rc;
  const void *berry = c.out->opt_classes[OC_BERRY], *feedgui = c.out->opt_classes[OC_FEEDGUI],
             *beh = c.out->opt_classes[OC_BEHAVIOUR], *ball = c.out->opt_classes[OC_POKEBALL],
             *throw_cls = c.out->opt_classes[OC_POKEBALL_THROW];
  if ((rc = typed_field(c, feedgui, "confirmationDelay", 0x108, "System.Single", "berry-delay-type", nullptr))) return rc;
  if ((rc = typed_field(c, feedgui, "postConfirmationDelay", 0x10c, "System.Single", "berry-delay-type", nullptr))) return rc;

  if ((rc = ref_field(c, feedgui, "itemImage", 0xf0, &H[H_BR_F_IMAGE]))) return rc;
  if ((rc = ref_field(c, berry, "drql", 0x148, &H[H_BR_F_BAG]))) return rc;
  if ((rc = ref_field(c, berry, "drrv", 0x248, &H[H_BR_F_GUI]))) return rc;
  if ((rc = ref_field(c, berry, "<EncounterUI>k__BackingField", 0x298, &H[H_BR_F_UI]))) return rc;
  if ((rc = enum_i32_field(c, berry, "<CurrentItem>k__BackingField", 0x2a8, &H[H_BR_F_CURRENT]))) return rc;

  if ((rc = check_method(c, beh, "get_enabled", "System.Boolean", 0, nullptr, nullptr, false, false, 0, nullptr, 0, &H[H_BR_M_VISIBLE], nullptr))) return rc;
  if ((rc = check_method(c, beh, "set_enabled", "System.Void", 1, "System.Boolean", nullptr, false, false, 0, nullptr, 0, &H[H_BR_M_SET_VISIBLE], nullptr))) return rc;
  if ((rc = check_method(c, berry, "EnterState", "System.Void", 0, nullptr, nullptr, false, false, 181381832ULL, kBrEnter, sizeof(kBrEnter), &H[H_BR_M_ENTER], &H[H_BR_M_ENTER_CODE]))) return rc;
  if ((rc = check_method(c, berry, "ExitState", "System.Void", 0, nullptr, nullptr, false, false, 181396348ULL, kBrExit, sizeof(kBrExit), &H[H_BR_M_EXIT], &H[H_BR_M_EXIT_CODE]))) return rc;
  if ((rc = check_method(c, berry, "SelectBerry", "System.Void", 1, "Holoholo.Rpc.Item", nullptr, false, false, 181379392ULL, kBrSelect, sizeof(kBrSelect), &H[H_BR_M_SELECT], &H[H_BR_M_SELECT_CODE]))) return rc;
  if ((rc = check_method(c, berry, "get_CanUseBerry", "System.Boolean", 0, nullptr, nullptr, false, false, 181361316ULL, kBrCan, sizeof(kBrCan), &H[H_BR_M_CAN], &H[H_BR_M_CAN_CODE]))) return rc;
  if ((rc = check_method(c, berry, "get_PokeballInPlay", "System.Boolean", 0, nullptr, nullptr, false, false, 181361500ULL, kBrBall, sizeof(kBrBall), &H[H_BR_M_BALL], &H[H_BR_M_BALL_CODE]))) return rc;
  if ((rc = check_method(c, berry, "get_IsInPhotoMode", "System.Boolean", 0, nullptr, nullptr, false, false, 181361364ULL, kBrPhoto, sizeof(kBrPhoto), &H[H_BR_M_PHOTO], &H[H_BR_M_PHOTO_CODE]))) return rc;
  if ((rc = check_method(c, feedgui, "ClickConfirm", "System.Void", 0, nullptr, nullptr, false, false, 162887612ULL, kBrConfirm, sizeof(kBrConfirm), &H[H_BR_M_CONFIRM], &H[H_BR_M_CONFIRM_CODE]))) return rc;

  auto is_val = reinterpret_cast<bool (*)(const void *)>(c.core->class_is_valuetype);
  auto val_sz = reinterpret_cast<int (*)(const void *, int *)>(c.core->class_value_size);
  int align = 0;
  if (!throw_cls || !is_val(throw_cls) || val_sz(throw_cls, &align) != 12 || align != 4)
    return set_err(c, PGR_MD_ERR_SIZE, "berry-impact-throw-abi");
  if ((rc = typed_field(c, throw_cls, "BallType", 16, "Holoholo.Rpc.Item", "berry-impact-throw-field", nullptr))) return rc;
  if ((rc = typed_field(c, throw_cls, "ReticleSize", 20, "System.Single", "berry-impact-throw-field", nullptr))) return rc;
  if ((rc = typed_field(c, throw_cls, "HitBullseye", 24, "System.Boolean", "berry-impact-throw-field", nullptr))) return rc;
  if ((rc = typed_field(c, throw_cls, "Spinning", 25, "System.Boolean", "berry-impact-throw-field", nullptr))) return rc;
  if ((rc = typed_field(c, throw_cls, "Missed", 26, "System.Boolean", "berry-impact-throw-field", nullptr))) return rc;

  const void *cb = nullptr;
  if ((rc = nested_suffix(c, berry, "ebn", 64, true, "berry-confirm-callback", &cb))) return rc;
  H[H_BR_CALLBACK] = cb;
  if ((rc = ref_field(c, cb, "<>4__this", 0x10, &H[H_BR_F_CB_SCOPE]))) return rc;
  if ((rc = enum_i32_field(c, cb, "berryType", 0x18, &H[H_BR_F_CB_BERRY]))) return rc;
  if ((rc = check_method(c, cb, "btdx", "System.Void", 1, "System.Boolean", nullptr, false, false, 181423396ULL, kBrBtdx, sizeof(kBrBtdx), &H[H_BR_M_DIRECT], &H[H_BR_M_DIRECT_CODE]))) return rc;
  if ((rc = check_method(c, cb, "btdy", "System.Void", 1, "Holoholo.Rpc.UseItemEncounterOutProto", nullptr, false, false, 181424920ULL, kBrBtdy, sizeof(kBrBtdy), &H[H_BR_M_RESULT], &H[H_BR_M_RESULT_CODE]))) return rc;
  if ((rc = check_method(c, cb, "btdz", "System.Void", 1, "System.String", nullptr, false, false, 181425904ULL, kBrBtdz, sizeof(kBrBtdz), &H[H_BR_M_ERROR], &H[H_BR_M_ERROR_CODE]))) return rc;
  if ((rc = check_method(c, ball, "btyl", "System.Void", 1, "Niantic.Holoholo.Encounter.PokeballThrow", nullptr, false, false, 181795804ULL, kBrSubmit, sizeof(kBrSubmit), &H[H_BR_M_SUBMIT], &H[H_BR_M_SUBMIT_CODE]))) return rc;
  if ((rc = check_method(c, ball, "btxu", "Holoholo.Rpc.CatchPokemonOutProto", 0, nullptr, nullptr, false, false, 181764464ULL, kBrPeek, sizeof(kBrPeek), &H[H_BR_M_PEEK], &H[H_BR_M_PEEK_CODE]))) return rc;
  if ((rc = check_method(c, ball, "get_HoldingBreadBall", "System.Boolean", 0, nullptr, nullptr, false, false, 181757284ULL, kBrBread, sizeof(kBrBread), &H[H_BR_M_BREAD], &H[H_BR_M_BREAD_CODE]))) return rc;
  return check_method(c, ball, "get_BallType", "Holoholo.Rpc.Item", 0, nullptr, nullptr, false, false, 181765900ULL, kBrBallType, sizeof(kBrBallType), &H[H_BR_M_BALL_TYPE], &H[H_BR_M_BALL_TYPE_CODE]);
}

int do_incubator(Ctx &c) {
  const void *svc = c.out->opt_classes[OC_INC_SERVICE], *contract = c.out->opt_classes[OC_INC_CONTRACT],
             *inc = c.out->opt_classes[OC_INC_PROTO],   *ienum = c.out->opt_classes[OC_IENUMERABLE],
             *player = c.out->opt_classes[OC_VGP_PLAYER], *p_contract = c.out->opt_classes[OC_VGP_PLAYER_CONTRACT],
             *b_contract = c.out->opt_classes[OC_VGP_BAG_CONTRACT];
  if (c.out->optional[OPT_ITEMS] != PGR_SHARED_OPT_AVAILABLE ||
      c.out->optional[OPT_ITEMS_LOOKUP] != PGR_SHARED_OPT_AVAILABLE ||
      !svc || !contract || !inc || !ienum || !player || !p_contract || !b_contract)
    return set_err(c, PGR_MD_ERR_NOT_FOUND, "incubator-dependencies");
  const void **H = c.out->opt_handles; int rc;
  const void *pk = c.out->opt_classes[OC_PK], *bag = c.out->opt_classes[OC_BAG],
             *ienumerator = c.out->opt_classes[OC_IENUMERATOR], *idisp = c.out->opt_classes[OC_IDISPOSABLE],
             *item = c.out->opt_classes[OC_ITEM];

  if ((rc = typed_field(c, pk, "isEgg_", 0x50, "System.Boolean", "incubator-field", &H[H_INC_F_EGG]))) return rc;
  if ((rc = typed_field(c, pk, "id_", 0x20, "System.UInt64", "incubator-field", &H[H_INC_F_ID]))) return rc;
  if ((rc = typed_field(c, pk, "eggKmWalkedTarget_", 0x58, "System.Double", "incubator-field", &H[H_INC_F_TARGET]))) return rc;
  if ((rc = typed_field(c, pk, "eggKmWalkedStart_", 0x60, "System.Double", "incubator-field", &H[H_INC_F_START]))) return rc;
  if ((rc = typed_field(c, pk, "eggIncubatorId_", 0x98, "System.String", "incubator-field", &H[H_INC_F_ASSIGNMENT]))) return rc;
  const void *egg_type_enum = nullptr;
  if ((rc = enum_i32_field(c, pk, "eggType_", 0x154, &H[H_INC_F_EGG_TYPE], &egg_type_enum))) return rc;
  if ((rc = typed_field(c, inc, "Item", 0x18, "Holoholo.Rpc.Item", "incubator-field", &H[H_INC_F_ITEM]))) return rc;
  if ((rc = typed_field(c, inc, "ItemId", 0x10, "System.String", "incubator-field", &H[H_INC_F_INC_ID]))) return rc;
  if ((rc = typed_field(c, inc, "PokemonId", 0x28, "System.Int64", "incubator-field", &H[H_INC_F_PK_ID]))) return rc;
  if ((rc = typed_field(c, inc, "UnconvertedLocalExpirationTimeMs", 0x40, "System.Int64", "incubator-field", &H[H_INC_F_EXPIRY]))) return rc;

  MParam put_p[2] = {{"System.Int64", nullptr}, {"Holoholo.Rpc.EggIncubatorProto", nullptr}};
  if ((rc = check_method_ex(c, svc, "PutEggInIncubator", nullptr, pred_put_egg_promise, 2, put_p, false, false, 163477644ULL, kIncPut, sizeof(kIncPut), &H[H_INC_M_PUT], &H[H_INC_M_PUT_CODE]))) return rc;
  if ((rc = check_method_ex(c, player, "get_Incubators", nullptr, pred_incubators_enum, 0, nullptr, false, false, 178505260ULL, kIncList, sizeof(kIncList), &H[H_INC_M_INCUBATORS], &H[H_INC_M_INCUBATORS_CODE]))) return rc;
  if ((rc = check_method_ex(c, bag, "get_AllPokemon", nullptr, pred_all_pokemon_coll, 0, nullptr, false, false, 168081584ULL, kIncEggs, sizeof(kIncEggs), &H[H_INC_M_EGGS], &H[H_INC_M_EGGS_CODE]))) return rc;
  if ((rc = check_method(c, ienum, "GetEnumerator", "System.Collections.IEnumerator", 0, nullptr, nullptr, false, false, 0, nullptr, 0, &H[H_INC_M_ENUMERATE], nullptr))) return rc;
  if ((rc = check_method(c, ienumerator, "MoveNext", "System.Boolean", 0, nullptr, nullptr, false, false, 0, nullptr, 0, &H[H_INC_M_MOVE], nullptr))) return rc;
  if ((rc = check_method(c, ienumerator, "get_Current", "System.Object", 0, nullptr, nullptr, false, false, 0, nullptr, 0, &H[H_INC_M_CURRENT], nullptr))) return rc;
  if ((rc = check_method(c, idisp, "Dispose", "System.Void", 0, nullptr, nullptr, false, false, 0, nullptr, 0, &H[H_INC_M_DISPOSE], nullptr))) return rc;

  if ((rc = enum_literal(c, item, "IncubatorBasicUnlimited", false, "incubator-enum", &c.out->opt_ints[I_INC_UNLIMITED]))) return rc;
  return enum_literal(c, egg_type_enum, "EggTypeSpecial", false, "incubator-enum", &c.out->opt_ints[I_INC_SPECIAL]);
}

int do_buddy(Ctx &c) {
  if (c.out->optional[OPT_ITEMS] != PGR_SHARED_OPT_AVAILABLE ||
      c.out->optional[OPT_ITEMS_LOOKUP] != PGR_SHARED_OPT_AVAILABLE)
    return set_err(c, PGR_MD_ERR_NOT_FOUND, "buddy-item-lookup");
  const void **H = c.out->opt_handles; int rc;
  const void *b7757 = c.out->opt_classes[OC_BUDDY_7757], *b7768 = c.out->opt_classes[OC_BUDDY_7768],
             *b7769 = c.out->opt_classes[OC_BUDDY_7769], *b8109 = c.out->opt_classes[OC_BUDDY_8109],
             *obs = c.out->opt_classes[OC_BUDDY_OBS],    *fout = c.out->opt_classes[OC_BUDDY_FEED_OUT],
             *bpk = c.out->opt_classes[OC_BUDDY_PK_PROTO];

  const void *cb = nullptr;
  if ((rc = nested_suffix(c, b7768, "dvh", 64, true, "buddy-feed-callback", &cb))) return rc;
  H[H_BD_CALLBACK] = cb;
  H[H_BD_CONTRACT] = c.out->opt_classes[OC_IBUDDY_STATS];

  const void *res_enum = nullptr;
  if ((rc = enum_i32_field(c, fout, "Result", 0x10, &H[H_BD_F_RESULT], &res_enum))) return rc;
  if ((rc = enum_literal(c, res_enum, "Success", false, "buddy-feed-success", &c.out->opt_ints[I_BUDDY_SUCCESS]))) return rc;

  if ((rc = ref_field(c, b7769, "dpsj", 0x58, &H[H_BD_F_RPC]))) return rc;
  if ((rc = ref_field(c, b7769, "dpsi", 0x50, &H[H_BD_F_POKEMON]))) return rc;
  if ((rc = ref_field(c, b7768, "dpsh", 0x40, &H[H_BD_F_BAG]))) return rc;
  if ((rc = ref_field(c, cb, "<>4__this", 0x18, &H[H_BD_F_CB_RPC]))) return rc;
  const void *buddy_id_type = nullptr;
  if ((rc = raw_field(c, bpk, "BuddyPokemonId", 0x10, &H[H_BD_F_BUDDY_ID], &buddy_id_type))) return rc;
  if ((rc = ref_field(c, obs, "BuddyFeedStats", 0x40, &H[H_BD_F_FEED_STATS]))) return rc;

  if ((rc = check_method(c, b7769, "get_BuddyStats", "Holoholo.Rpc.BuddyObservedData", 0, nullptr, nullptr, false, false, 180193380ULL, kBdStats, sizeof(kBdStats), &H[H_BD_M_STATS], &H[H_BD_M_STATS_CODE]))) return rc;
  if ((rc = check_method(c, b8109, "get_HasBuddy", "System.Boolean", 0, nullptr, nullptr, false, false, 180846316ULL, kBdHas, sizeof(kBdHas), &H[H_BD_M_HAS], &H[H_BD_M_HAS_CODE]))) return rc;
  if ((rc = check_method(c, b8109, "get_BuddyPokemon", "Holoholo.Rpc.BuddyPokemonProto", 0, nullptr, nullptr, false, false, 180846228ULL, kBdPk, sizeof(kBdPk), &H[H_BD_M_POKEMON], &H[H_BD_M_POKEMON_CODE]))) return rc;
  if ((rc = check_method(c, b7757, "OnMapRemainingSec", "System.Single", 1, "Holoholo.Rpc.BuddyObservedData", nullptr, true, false, 180184056ULL, kBdRem, sizeof(kBdRem), &H[H_BD_M_REMAINING], &H[H_BD_M_REMAINING_CODE]))) return rc;
  if ((rc = check_method(c, b7757, "PreMapFullnessPercentage", "System.Single", 1, "Holoholo.Rpc.BuddyObservedData", nullptr, true, false, 180184200ULL, kBdFull, sizeof(kBdFull), &H[H_BD_M_FULLNESS], &H[H_BD_M_FULLNESS_CODE]))) return rc;
  if ((rc = check_method(c, b7757, "InFullnessCooldown", "System.Boolean", 1, "Holoholo.Rpc.BuddyObservedData", nullptr, true, false, 180184224ULL, kBdCool, sizeof(kBdCool), &H[H_BD_M_COOLDOWN], &H[H_BD_M_COOLDOWN_CODE]))) return rc;
  MParam fp[2] = {{"Holoholo.Rpc.Item", nullptr}, {"System.Int32", nullptr}};
  if ((rc = check_method_ex(c, b7768, "Feed", nullptr, pred_buddy_feed_promise, 2, fp, false, false, 180188396ULL, kBdFeed, sizeof(kBdFeed), &H[H_BD_M_FEED], &H[H_BD_M_FEED_CODE]))) return rc;
  if ((rc = check_method(c, cb, "brhd", "System.Void", 1, "Holoholo.Rpc.BuddyFeedingOutProto", nullptr, false, false, 180191752ULL, kBdBrhd, sizeof(kBdBrhd), &H[H_BD_M_RESULT], &H[H_BD_M_RESULT_CODE]))) return rc;

  std::string bid_tn;
  if (!get_type_name(c, buddy_id_type, &bid_tn) || bid_tn != "System.UInt64")
    return set_err(c, PGR_MD_ERR_TYPE, "buddy-id-type");
  return 0;
}

#include "shared_metadata_gifts.inc"

#include "shared_metadata_map.inc"

// ---------------------------------------------------------------------------------------------------
// Mandatory pointerHandleAbi (gchandle_new / gchandle_get_target / gchandle_free code fingerprint).
// ---------------------------------------------------------------------------------------------------
int pointer_handle_abi(const PgrMetaApi *md, const PgrSharedMetaExtraExports *ex) {
  return pgr_pointer_handles(md,ex->gchandle_new,ex->gchandle_get_target,ex->gchandle_free)==0
      ? PGR_MD_OK : PGR_SHARED_ERR_GC_ABI;
}
bool valid_binding_range(const PgrMetaBindConfig &binding) {
  const uintptr_t base=reinterpret_cast<uintptr_t>(binding.module_base);
  return base&&binding.module_size&&binding.module_size<=UINTPTR_MAX-base;
}
int resolve_class(const PgrMetaApi *md,const ClassSpec &s,const void **klass) {
  *klass=nullptr; PgrMetaClassQuery q{};q.version=PGR_MD_QUERY_VERSION;q.size=sizeof(q);
  q.name_space=s.name_space;q.name=s.name;q.canonical=s.canonical;q.canonical_match=PGR_MD_MATCH_EXACT;
  PgrMetaClassResult result{};int rc=md->resolve_class(&q,&result);
  if(!rc)*klass=result.klass;return rc;
}

int map_required_err(int rc) {
  return (rc == PGR_MD_ERR_NOT_FOUND) ? PGR_SHARED_ERR_MEMBER_NOT_FOUND : rc;
}

// ---------------------------------------------------------------------------------------------------
// Top-level API 2 resolve() entry point.
// ---------------------------------------------------------------------------------------------------
int resolve(const PgrSharedMetadataConfig *input, PgrSharedMetadataOutput *out) {
  if (input == nullptr) {
    if (out != nullptr) std::memset(out, 0, sizeof(*out));
    return PGR_SHARED_ERR_CONFIG;
  }
  if (out == nullptr) return PGR_SHARED_ERR_NULL_ARG;
  std::memset(out, 0, sizeof(*out));

  std::unique_lock<std::mutex> lock(resolving, std::try_to_lock);
  if (!lock.owns_lock()) return PGR_MD_ERR_BUSY;

  int final_rc = PGR_SHARED_ERR_CONFIG;
  struct ClearOnFailure {
    PgrSharedMetadataOutput *target;
    int *rc;
    ~ClearOnFailure() {
      if (*rc != PGR_MD_OK) std::memset(target, 0, sizeof(*target));
    }
  } guard{out, &final_rc};

  if (input->version != kApiVersion ||
      input->size != sizeof(PgrSharedMetadataConfig) ||
      input->thread_current == nullptr ||
      input->extra == nullptr ||
      input->extra->field_static_get_value == nullptr ||
      input->extra->gchandle_new == nullptr ||
      input->extra->gchandle_get_target == nullptr ||
      input->extra->gchandle_free == nullptr) {
    return (final_rc = PGR_SHARED_ERR_CONFIG);
  }
  const PgrMetaApi *md = metadata();
  if (md == nullptr || md->version != PGR_MD_API_VERSION || md->size != sizeof(PgrMetaApi)) {
    return (final_rc = PGR_SHARED_ERR_CONFIG);
  }
  const PgrMetaExports *core = input->binding.exports;
  if (core == nullptr) return (final_rc = PGR_MD_ERR_NULL_ARG);
  if (!valid_binding_range(input->binding)) return (final_rc = PGR_MD_ERR_CODE_RANGE);
  PgrMetaBindConfig bind = input->binding;
  if (!bind.seg_query) {
    const uintptr_t base=reinterpret_cast<uintptr_t>(bind.module_base);
    const uintptr_t thread=reinterpret_cast<uintptr_t>(input->thread_current);
    if(thread<base||thread-base>bind.module_size||4>bind.module_size-(thread-base))
      return (final_rc=PGR_MD_ERR_CODE_RANGE);
    const void *canonical=nullptr;uint64_t extent=0;
    const int module_status=md->resolve_module("libil2cpp.so",&canonical,&extent);
    if(module_status)return (final_rc=module_status);
    const long page=sysconf(_SC_PAGESIZE);
    if(canonical!=bind.module_base||!extent||extent>bind.module_size||page<=0||
       bind.module_size-extent>=static_cast<uint64_t>(page))return (final_rc=PGR_MD_ERR_CODE_RANGE);
    bind.module_size=extent;
  }
  if (!attached(*input)) return (final_rc = PGR_SHARED_ERR_ATTACH);
  if (bind.debug_prologue == nullptr) {
    bind.debug_prologue = kDebugPrologue;
    bind.debug_prologue_len = static_cast<uint32_t>(sizeof(kDebugPrologue));
  }
  int rc = md->bind(&bind);
  if (rc != PGR_MD_OK) return (final_rc = rc);

  out->version = kApiVersion;
  out->size = static_cast<uint32_t>(sizeof(PgrSharedMetadataOutput));

  // Only absent required classes are retryable; invalid members remain fatal.
  for (size_t i = 0; i < kClassCount; i++) {
    if (!attached(*input)) return (final_rc = PGR_SHARED_ERR_ATTACH);
    rc = resolve_class(md,kClasses[i],&out->classes[i]);
    if (rc != PGR_MD_OK) return (final_rc = rc==PGR_MD_ERR_NOT_FOUND?PGR_SHARED_NOT_READY:rc);
  }

  // 2. Resolve 6 required fields (missing member maps -9 to PGR_SHARED_ERR_MEMBER_NOT_FOUND = -33).
  for (size_t i = 0; i < kFieldCount; i++) {
    if (!attached(*input)) return (final_rc = PGR_SHARED_ERR_ATTACH);
    const FieldSpec &s = kFields[i];
    PgrMetaFieldQuery spec{};spec.version=PGR_MD_QUERY_VERSION;spec.size=sizeof(spec);
    spec.owner=out->classes[s.owner];spec.name=s.name;spec.offset=s.offset;
    spec.forbidden_mask=0x50;spec.kind=s.kind;spec.type_name=s.type_name;
    spec.type_match=s.type_name?PGR_MD_MATCH_EXACT:PGR_MD_MATCH_ANY;spec.value_size=s.value_size;
    PgrMetaFieldResult resolved{};rc=md->validate_field(&spec,&resolved);
    if (rc != PGR_MD_OK) return (final_rc = map_required_err(rc));
    out->fields[i] = resolved.field;
    out->field_types[i] = resolved.type;
    out->field_type_classes[i] = resolved.type_class;
    if (i == 0) out->caught_id_size = static_cast<uint32_t>(s.value_size);
  }

  // 3. Resolve 7 required methods.
  for (size_t i = 0; i < kMethodCount; i++) {
    if (!attached(*input)) return (final_rc = PGR_SHARED_ERR_ATTACH);
    const MethodSpec &s = kMethods[i];
    PgrMetaMethodQuery spec{};spec.version=PGR_MD_QUERY_VERSION;spec.size=sizeof(spec);
    spec.owner=out->classes[s.owner];spec.name=s.name;spec.param_count=s.param_count;
    spec.is_static=s.is_static;spec.return_type=s.return_type;spec.return_match=PGR_MD_MATCH_EXACT;
    spec.param_types[0]=s.param0;spec.param_types[1]=s.param1;
    spec.param_match[0]=spec.param_match[1]=PGR_MD_MATCH_EXACT;
    if(s.code_rva){
      const uintptr_t base=reinterpret_cast<uintptr_t>(bind.module_base);
      if(s.code_rva>UINTPTR_MAX-base)return (final_rc=PGR_MD_ERR_CODE_RANGE);
      spec.expected_code=reinterpret_cast<const void *>(base+s.code_rva);
      spec.prologue=s.prologue;spec.prologue_len=s.prologue_len;
    }
    PgrMetaMethodResult resolved{};rc=md->validate_method(&spec,&resolved);
    if (rc != PGR_MD_OK) return (final_rc = map_required_err(rc));
    out->methods[i] = resolved.method;
    out->method_code[i] = resolved.code;
  }

  if (!attached(*input)) return (final_rc = PGR_SHARED_ERR_ATTACH);

  // 4. Mandatory CatchSuccess static enum literal read.
  Ctx c{};
  c.md = md;
  c.core = core;
  c.ex = input->extra;
  c.out = out;
  c.module_base=reinterpret_cast<uintptr_t>(bind.module_base);
  for(const auto &field:kFields)
    record_field_meta(c,out->classes[field.owner],field.name,field.offset,field.kind,
                      field.kind==PGR_MD_FIELD_VALUE?field.value_size:0);
  for(size_t i=0;i<kMethodCount;++i)
    if(kMethods[i].prologue_len&&!register_code_addr(c,out->method_code[i]))
      return (final_rc=PGR_MD_ERR_CODE_RANGE);
  const void *status_enum = out->field_type_classes[kStatusFieldIndex];
  rc = enum_literal(c, status_enum, "CatchSuccess", true, "catch-success", &out->success);
  if (rc != PGR_MD_OK) return (final_rc = map_required_err(rc));

  // 5. Mandatory boxed System.Boolean value-size/align ABI check.
  const void *literal=reinterpret_cast<const void *(*)(const void*,const char*)>(core->class_get_field_from_name)(status_enum,"CatchSuccess");
  std::string literal_type;
  if(!literal||!get_type_name(c,reinterpret_cast<const void *(*)(const void*)>(core->field_get_type)(literal),&literal_type)||
     literal_type!="Holoholo.Rpc.CatchPokemonOutProto.Types.Status")return (final_rc=PGR_MD_ERR_TYPE);
  auto is_val = reinterpret_cast<bool (*)(const void *)>(core->class_is_valuetype);
  auto val_sz = reinterpret_cast<int (*)(const void *, int *)>(core->class_value_size);
  int bool_align = 0;
  const void *bool_cls = out->classes[6];
  if (!bool_cls || !is_val(bool_cls) || val_sz(bool_cls, &bool_align) != 1 ||
      bool_align != 1) {
    return (final_rc = PGR_MD_ERR_SIZE);
  }

  // 6. Mandatory pointerHandleAbi (gchandle_new / gchandle_get_target / gchandle_free code fingerprint).
  rc = pointer_handle_abi(md, input->extra);
  if (rc != PGR_MD_OK) return (final_rc = rc);

  // 7. Best-effort lookup of all 127 optional classes.
  for (size_t i = 0; i < kOptClassCount; i++) {
    const void *k = nullptr;
    int orc = resolve_class(md,kOptClasses[i],&k);
    if(orc!=PGR_MD_OK&&orc!=PGR_MD_ERR_NOT_FOUND)return (final_rc=orc);
    out->opt_classes[i] = (orc == PGR_MD_OK) ? k : nullptr;
  }

  // 8. Execute all 21 optional descriptor blocks with full failure isolation (zero DELEGATED slots).
  auto run_opt = [&](OptionalId idx, int (*fn)(Ctx &)) {
    c.reason[0] = '\0';
    int orc = fn(c);
    out->optional[idx] = (orc == PGR_MD_OK) ? PGR_SHARED_OPT_AVAILABLE : PGR_SHARED_OPT_UNAVAILABLE;
    out->optional_error[idx] = orc;
    if (orc != PGR_MD_OK) {
      std::snprintf(out->optional_reason[idx], kReasonMax, "%s",
                    c.reason[0] ? c.reason : "optional-unavailable");
    }
  };

  // Pre-initialize nested optionals in case their parent optional fails before reaching them.
  out->optional[OPT_ITEMS_LOOKUP] = PGR_SHARED_OPT_UNAVAILABLE;
  out->optional_error[OPT_ITEMS_LOOKUP] = PGR_MD_ERR_NOT_FOUND;
  std::snprintf(out->optional_reason[OPT_ITEMS_LOOKUP], kReasonMax, "items-unavailable");

  out->optional[OPT_VGP_MARKER] = PGR_SHARED_OPT_UNAVAILABLE;
  out->optional_error[OPT_VGP_MARKER] = PGR_MD_ERR_NOT_FOUND;
  std::snprintf(out->optional_reason[OPT_VGP_MARKER], kReasonMax, "vgp-unavailable");

  out->optional[OPT_MAP_POI_EXTRA] = PGR_SHARED_OPT_UNAVAILABLE;
  out->optional_error[OPT_MAP_POI_EXTRA] = PGR_MD_ERR_NOT_FOUND;
  std::snprintf(out->optional_reason[OPT_MAP_POI_EXTRA], kReasonMax, "map-poi-unavailable");

  run_opt(OPT_CATCH_CANVAS,        do_catch_canvas);
  run_opt(OPT_CATCH_DISCOVERY,     do_catch_discovery);
  run_opt(OPT_CATCH_SCENE,         do_catch_scene);
  run_opt(OPT_DETAILS,             do_details);
  run_opt(OPT_DIRECT_PRESENTATION, do_direct_presentation);
  run_opt(OPT_TRANSFER,            do_transfer);
  run_opt(OPT_SPIN,                do_spin);
  run_opt(OPT_SPIN_CAMERA,         do_spin_camera);
  run_opt(OPT_ITEMS,               do_items);       // also populates OPT_ITEMS_LOOKUP
  run_opt(OPT_ROCKET,              do_rocket);
  run_opt(OPT_BATTLES,             do_battles);
  run_opt(OPT_BERRIES,             do_berries);
  run_opt(OPT_INCUBATOR,           do_incubator);
  run_opt(OPT_BUDDY,               do_buddy);
  run_opt(OPT_GIFTS,               do_gifts);
  run_opt(OPT_SPAWN,               do_spawn);
  run_opt(OPT_VGP,                 do_vgp);         // also populates OPT_VGP_MARKER
  run_opt(OPT_MAP_POI,             do_map_poi);     // also populates OPT_MAP_POI_EXTRA

  return (final_rc = PGR_MD_OK);
}

const PgrSharedMetadataApi kApi = {
  kApiVersion,
  static_cast<uint32_t>(sizeof(PgrSharedMetadataApi)),
  static_cast<uint32_t>(sizeof(PgrSharedMetadataConfig)),
  static_cast<uint32_t>(sizeof(PgrSharedMetadataOutput)),
  resolve,
};

} // namespace
} // namespace pgr_shared_meta

extern "C" const void *pgrazor_shared_metadata_api(uint32_t version) {
  if (version != pgr_shared_meta::kApiVersion) return nullptr;
  return &pgr_shared_meta::kApi;
}
