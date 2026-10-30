#include "smart_catch_binding.h"

#include "runtime_bootstrap.h"
#include "runtime_metadata.h"
#include "smart_catch.h"
#include "smart_encounter_validation.h"

#include <dlfcn.h>
#include <mutex>
#include <time.h>
#include <unistd.h>
#include <android/log.h>
extern "C" void pgr_vgp_discovery_loaded_method(void *method);

// See .artifacts/advanced-features/smart-binding-notes.txt for the exact descriptor provenance and the
// three field/method slots whose offset could not be pinned from the research artifact (resolved by name
// with a parent/kind structural check, fail-closed). This file owns ONLY the binding glue; the helper
// contract (PgrSmartBinding layout, pgr_smart_bind) and the Java/controller plumbing are owned elsewhere.

namespace {

// Serialize ensure_bound so a reentrant managed callback / concurrent thread gets BUSY, never a race.
std::mutex g_bind_mutex;
bool g_bound = false;
void diagnostic(const char *name,int rc){
 static const char *last=nullptr;static int last_rc=0;
 if(last==name&&last_rc==rc)return;last=name;last_rc=rc;
 __android_log_print(ANDROID_LOG_WARN,"PGRazorSmartCatch","Binding %s: %d",name,rc);
}

// Module-local pointers used ONLY by pgr_smart_spawn_fresh (NOT part of PgrSmartBinding, which the helper
// worker owns). Populated by a successful ensure_bound; read-only thereafter.
struct SpawnFreshState {
  uint64_t owner = 0;
  void *class_wildmap = nullptr;       // exact WildMapPokemon class (identity gate)
  void *f_wild_proto = nullptr;        // WildMapPokemon.eluo -> WildPokemonProto (ref)
  void *f_last_modified = nullptr;     // WildPokemonProto.LastModifiedMs (long)
  void *f_time_till_hidden = nullptr;  // WildPokemonProto.TimeTillHiddenMs (int)
  void *object_get_class = nullptr;    // il2cpp_object_get_class
  void *field_get_value = nullptr;     // il2cpp_field_get_value
} g_fresh;
void *encounter_class=nullptr,*encounter_status=nullptr,*encounter_pokemon=nullptr;
void *encounter_wrapper=nullptr,*map_encounter=nullptr;
int32_t encounter_success=0;

const PgrBootApi *bootstrap_api() {
  const auto *api = static_cast<const PgrBootApi *>(pgrazor_runtime_bootstrap_api(PGR_BOOT_API_VERSION));
  if (!api || api->version != PGR_BOOT_API_VERSION || api->size != sizeof(PgrBootApi) || !api->snapshot)
    return nullptr;
  return api;
}

const PgrMetaApi *metadata_api() {
  const auto *api = static_cast<const PgrMetaApi *>(pgrazor_runtime_metadata_api(PGR_MD_API_VERSION));
  if (!api || api->version != PGR_MD_API_VERSION || api->size != sizeof(PgrMetaApi) ||
      !api->resolve_class || !api->validate_field || !api->validate_method)
    return nullptr;
  return api;
}

// Owner-thread gate: tid must match the caller-proven owner AND be the live thread (mirrors the VGP/
// quickball `attached()` check), and the il2cpp thread_current export must report an attached thread.
bool on_owner_thread(uint64_t owner, void *thread_current) {
  if (!owner || owner != static_cast<uint64_t>(gettid()) || !thread_current) return false;
  return reinterpret_cast<void *(*)()>(thread_current)() != nullptr;
}

// resolve_class(ns,name) with the ns.name EXACT canonical default. *out gets the Il2CppClass*.
int resolve_class(const PgrMetaApi *md, const char *ns, const char *name, void **out) {
  PgrMetaClassQuery q{};
  q.version = PGR_MD_QUERY_VERSION;
  q.size = sizeof(q);
  q.name_space = ns;
  q.name = name;
  q.canonical = nullptr;  // null -> ns.name EXACT
  q.canonical_match = PGR_MD_MATCH_EXACT;
  PgrMetaClassResult r{};
  int rc = md->resolve_class(&q, &r);
  if (rc) {diagnostic(name,rc);return rc;}
  *out = const_cast<void *>(r.klass);
  return 0;
}

// validate_field at an EXACT offset. type_class (the field's type class, e.g. the enum class) is optional.
int validate_field(const PgrMetaApi *md, const void *owner, const char *name, uint64_t offset,
                   uint32_t kind, uint32_t value_size, const char *type_name, void **out,
                   void **type_class) {
  PgrMetaFieldQuery q{};
  q.version = PGR_MD_QUERY_VERSION;
  q.size = sizeof(q);
  q.owner = owner;
  q.name = name;
  q.offset = offset;
  q.kind = kind;
  q.value_size = value_size;
  q.type_name = type_name;
  q.type_match = type_name ? PGR_MD_MATCH_EXACT : PGR_MD_MATCH_ANY;
  PgrMetaFieldResult r{};
  int rc = md->validate_field(&q, &r);
  if (rc) {diagnostic(name,rc);return rc;}
  *out = const_cast<void *>(r.field);
  if (type_class) *type_class = const_cast<void *>(r.type_class);
  return 0;
}

// Structural by-name field resolve for the slots whose exact offset is not pinned by the research
// artifact. This is still real runtime metadata (NOT a guessed address): the field must exist on the
// owner and its parent must equal the owner. See smart-binding-notes.txt.
int resolve_field_byname(const PgrBootRuntimeExports &, const PgrMetaExports &ex, const void *owner,
                         const char *name, void **out) {
  auto by_name = reinterpret_cast<void *(*)(const void *, const char *)>(ex.class_get_field_from_name);
  auto parent = reinterpret_cast<void *(*)(const void *)>(ex.field_get_parent);
  if (!owner || !by_name || !parent) return PGR_SMARTBIND_ERR_FIELD;
  void *f = by_name(owner, name);
  if (!f || parent(f) != owner) return PGR_SMARTBIND_ERR_FIELD;
  *out = f;
  return 0;
}

int validate_method(const PgrMetaApi *md, const void *owner, const char *name, const char *return_type,
                    uint32_t param_count, const char *param0, uint32_t is_static, void **out,const char *param1=nullptr) {
  PgrMetaMethodQuery q{};
  q.version = PGR_MD_QUERY_VERSION;
  q.size = sizeof(q);
  q.owner = owner;
  q.name = name;
  q.return_type = return_type;
  q.return_match = return_type ? PGR_MD_MATCH_EXACT : PGR_MD_MATCH_ANY;
  q.param_count = param_count;
  if (param_count >= 1) { q.param_types[0] = param0; q.param_match[0] = param0 ? PGR_MD_MATCH_EXACT : PGR_MD_MATCH_ANY; }
  if (param_count >= 2) { q.param_types[1] = param1; q.param_match[1] = param1 ? PGR_MD_MATCH_EXACT : PGR_MD_MATCH_ANY; }
  q.is_static = is_static;
  q.allow_inflated = 0;
  PgrMetaMethodResult r{};
  int rc = md->validate_method(&q, &r);
  if (rc) {diagnostic(name,rc);return rc;}
  *out = const_cast<void *>(r.method);
  return 0;
}

// Read a static enum literal value (field_static_get_value on a validated public-static-literal field),
// mirroring quickball_metadata read_enum_literal. No guessed RPC id: the value comes straight from the
// bound game image. Returns true and writes *out on success.
bool read_enum_literal(const PgrMetaExports &ex, const PgrBootRuntimeExports &rt, const void *owner,
                       const char *lit, int32_t *out) {
  auto by_name = reinterpret_cast<void *(*)(const void *, const char *)>(ex.class_get_field_from_name);
  auto parent = reinterpret_cast<void *(*)(const void *)>(ex.field_get_parent);
  auto flags = reinterpret_cast<int (*)(const void *)>(ex.field_get_flags);
  auto static_get = reinterpret_cast<void (*)(const void *, void *)>(rt.field_static_get_value);
  if (!owner || !by_name || !parent || !flags || !static_get) return false;
  void *f = by_name(owner, lit);
  // public | static | literal | has-default (0x8050), matching quick-close/quick-ball enum-literal masks.
  if (!f || parent(f) != owner || (static_cast<uint32_t>(flags(f)) & 0x8050u) != 0x8050u) return false;
  int32_t v = 0;
  static_get(f, &v);
  *out = v;
  return true;
}

int64_t now_ms() {
  struct timespec ts{};
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return -1;
  return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

}  // namespace

int pgr_smart_ensure_bound(uint64_t owner) {
  std::unique_lock<std::mutex> lock(g_bind_mutex, std::try_to_lock);
  if (!lock.owns_lock()) return PGR_SMARTBIND_ERR_BUSY;

  const PgrBootApi *boot = bootstrap_api();
  if (!boot) return PGR_SMARTBIND_ERR_NO_BOOTSTRAP;
  const PgrMetaApi *md = metadata_api();
  if (!md) return PGR_SMARTBIND_ERR_NO_METADATA;

  // Pull-only snapshot: never binds/mutates. NOT_READY is a positive, retriable state.
  PgrBootBinding snap{};
  snap.version = PGR_BOOT_BINDING_VERSION;
  snap.size = sizeof(snap);
  int brc = boot->snapshot(&snap);
  if (brc == PGR_BOOT_NOT_READY) return PGR_SMARTBIND_NOT_READY;
  if (brc != PGR_BOOT_OK) return PGR_SMARTBIND_ERR_NO_BOOTSTRAP;

  const PgrMetaExports &ex = snap.meta;
  const PgrBootRuntimeExports &rt = snap.runtime;
  if (!snap.handle) return PGR_SMARTBIND_ERR_EXPORT;
  if (!on_owner_thread(owner, rt.thread_current)) return PGR_SMARTBIND_ERR_OWNER;
  if(g_bound)return owner==g_fresh.owner?PGR_SMARTBIND_OK:PGR_SMARTBIND_ERR_OWNER;

  // Reflection exports the bootstrap table does not carry: dlsym from the SAME live handle (no fabricated
  // pointer). Every one is mandatory for the helper.
  void *field_set_value_object = ::dlsym(snap.handle, "il2cpp_field_set_value_object");
  void *method_get_object      = ::dlsym(snap.handle, "il2cpp_method_get_object");
  void *type_get_object        = ::dlsym(snap.handle, "il2cpp_type_get_object");
  void *gc_wbarrier_set_field  = ::dlsym(snap.handle, "il2cpp_gc_wbarrier_set_field");
  void *method_from_reflection = ::dlsym(snap.handle, "il2cpp_method_get_from_reflection");

  PgrSmartBinding b{};
  b.version = PGR_SMART_BINDING_VERSION;
  b.size = sizeof(b);

  // --- il2cpp runtime exports (snapshot) ---
  b.gc_new                 = rt.gchandle_new;
  b.gc_get                 = rt.gchandle_get_target;
  b.gc_free                = rt.gchandle_free;
  b.object_get_class       = rt.object_get_class;
  b.object_new             = rt.object_new;
  b.object_unbox           = rt.object_unbox;
  b.runtime_invoke         = rt.runtime_invoke;
  b.field_get_value        = rt.field_get_value;
  b.field_set_value        = rt.field_set_value;
  b.field_set_value_object = field_set_value_object;
  b.array_new              = rt.array_new;

  // --- il2cpp reflection exports (snapshot metadata + dlsym) ---
  b.class_get_method_from_name = ex.class_get_method_from_name;
  b.method_is_generic          = ex.method_is_generic;
  b.method_get_param_count     = ex.method_get_param_count;
  b.method_get_object          = method_get_object;
  b.class_get_type             = ex.class_get_type;
  b.type_get_object            = type_get_object;
  b.gc_wbarrier_set_field      = gc_wbarrier_set_field;
  b.method_get_from_reflection = method_from_reflection;
  b.method_is_inflated         = ex.method_is_inflated;

  // Fail closed on a dlsym miss. The snapshot slots are guaranteed non-null by the feature 54 contract on
  // PGR_BOOT_OK; only these dlsym'd reflection/runtime exports can come back null on an unsupported build.
  if (!field_set_value_object || !method_get_object || !type_get_object || !gc_wbarrier_set_field ||
      !method_from_reflection)
    return PGR_SMARTBIND_ERR_EXPORT;

  // --- classes (feature 47 resolve_class; ns/name from the research artifact + shared_metadata) ---
  if (resolve_class(md, "Niantic.Holoholo.Map", "WildMapPokemon", &b.class_wildmap) ||
      resolve_class(md, "Holoholo.Rpc", "PokemonProto", &b.class_pokemon_proto) ||
      resolve_class(md, "Holoholo.Rpc", "PokemonDisplayProto", &b.class_display_proto) ||
      resolve_class(md, "Niantic.Holoholo.Internal", "ItemBagImpl", &b.class_item_bag) ||
      resolve_class(md, "Niantic.Holoholo.Rpc", "RpcHandler", &b.class_rpc) ||
      resolve_class(md, "Holoholo.Rpc", "UseItemEncounterProto", &b.class_use_item_req) ||
      resolve_class(md, "Holoholo.Rpc", "UseItemEncounterOutProto", &b.class_use_item_out) ||
      resolve_class(md, "System", "Type", &b.class_type) ||
      resolve_class(md, "System", "Int32", &b.class_int) ||
      resolve_class(md, "System", "Boolean", &b.class_bool) ||
      resolve_class(md, "System", "UInt64", &b.class_ulong) ||
      resolve_class(md, "System", "String", &b.class_string))
    return PGR_SMARTBIND_ERR_CLASS;

  // Method enum (RPC id source) + WildPokemonProto (spawn-freshness source).
  void *class_method_enum = nullptr;
  void *class_wild_proto = nullptr;
  if (resolve_class(md, "Holoholo.Rpc", "Method", &class_method_enum) ||
      resolve_class(md, "Holoholo.Rpc", "WildPokemonProto", &class_wild_proto))
    return PGR_SMARTBIND_ERR_CLASS;

  // --- fields (offsets confirmed from catch-evolution-metadata.txt unless noted) ---
  void *status_enum_class = nullptr;
  if (validate_field(md, b.class_wildmap, "eluq", 0x280, PGR_MD_FIELD_REF, 0, "Holoholo.Rpc.PokemonProto", &b.f_map_pokemon, nullptr) ||
      validate_field(md, b.class_pokemon_proto, "pokemonId_", 0x28, PGR_MD_FIELD_VALUE, 4, "System.Int32", &b.f_proto_id, nullptr) ||
      validate_field(md, b.class_pokemon_proto, "cp_", 0x2c, PGR_MD_FIELD_VALUE, 4, "System.Int32", &b.f_proto_cp, nullptr) ||
      validate_field(md, b.class_pokemon_proto, "individualAttack_", 0x70, PGR_MD_FIELD_VALUE, 4, "System.Int32", &b.f_proto_atk, nullptr) ||
      validate_field(md, b.class_pokemon_proto, "individualDefense_", 0x74, PGR_MD_FIELD_VALUE, 4, "System.Int32", &b.f_proto_def, nullptr) ||
      validate_field(md, b.class_pokemon_proto, "individualStamina_", 0x78, PGR_MD_FIELD_VALUE, 4, "System.Int32", &b.f_proto_sta, nullptr) ||
      validate_field(md, b.class_pokemon_proto, "cpMultiplier_", 0x7c, PGR_MD_FIELD_VALUE, 4, "System.Single", &b.f_proto_cpmul, nullptr) ||
      validate_field(md, b.class_pokemon_proto, "additionalCpMultiplier_", 0xac, PGR_MD_FIELD_VALUE, 4, "System.Single", &b.f_proto_addcpmul, nullptr) ||
      validate_field(md, b.class_pokemon_proto, "pokemonDisplay_", 0xd8, PGR_MD_FIELD_REF, 0, "Holoholo.Rpc.PokemonDisplayProto", &b.f_proto_display, nullptr) ||
      validate_field(md, b.class_use_item_req, "Item", 0x10, PGR_MD_FIELD_ENUM_I32, 4, nullptr, &b.f_req_item, nullptr) ||
      validate_field(md, b.class_use_item_req, "EncounterId", 0x18, PGR_MD_FIELD_VALUE, 8, "System.UInt64", &b.f_req_encid, nullptr) ||
      validate_field(md, b.class_use_item_req, "SpawnPointGuid", 0x20, PGR_MD_FIELD_REF, 0, "System.String", &b.f_req_spawn, nullptr) ||
      validate_field(md, b.class_use_item_out, "Status", 0x10, PGR_MD_FIELD_ENUM_I32, 4, nullptr, &b.f_out_status, &status_enum_class))
    return PGR_SMARTBIND_ERR_FIELD;

  // PokemonDisplayProto.shiny_ and the ItemBagImpl rpc-service field (cynr): offsets NOT pinned by the
  // artifact, so resolve structurally by name (parent==owner). Fail-closed. See smart-binding-notes.txt.
  if (validate_field(md,b.class_display_proto,"Shiny",0x18,PGR_MD_FIELD_VALUE,1,"System.Boolean",&b.f_display_shiny,nullptr) ||
      validate_field(md,b.class_item_bag,"cynr",0x38,PGR_MD_FIELD_REF,0,"Niantic.Holoholo.Rpc.IRpcHandler",&b.f_bag_rpc,nullptr))
    return PGR_SMARTBIND_ERR_FIELD;

  // --- methods ---
  if (validate_method(md, b.class_wildmap, "get_EncounterId", "System.UInt64", 0, nullptr, 0, &b.m_get_encounter_id) ||
      validate_method(md, b.class_wildmap, "get_SpawnPointId", "System.String", 0, nullptr, 0, &b.m_get_spawn_id) ||
      validate_method(md, b.class_use_item_req, ".ctor", "System.Void", 0, nullptr, 0, &b.m_req_ctor))
    return PGR_SMARTBIND_ERR_METHOD;
  // IItemBag.GetItemCount(Item)->int: name/arity structurally resolved (overload/return not pinned here).
  {
    if(validate_method(md,b.class_item_bag,"GetItemCount","System.Int32",1,"Holoholo.Rpc.Item",0,&b.m_item_count))return PGR_SMARTBIND_ERR_METHOD;
  }

  // --- runtime enum literals (NO guessed RPC id) ---
  int32_t rpc_id = 0, status_success = 0;
  if (!read_enum_literal(ex, rt, class_method_enum, "UseItemEncounter", &rpc_id))
    return PGR_SMARTBIND_ERR_LITERAL;
  if (!read_enum_literal(ex, rt, status_enum_class, "Success", &status_success))
    return PGR_SMARTBIND_ERR_LITERAL;
  b.rpc_id = rpc_id;
  b.status_success = status_success;

  // --- OPTIONAL GameMaster level group: bind ONLY if every slot validates, else leave all-null so the
  //     helper never claims a level (high-level rule then simply does not fire). We do not fabricate a
  //     level. The GameMaster data class / level helpers are not pinned in the artifact, so the group is
  //     left disabled here and flagged for the coordinator (see smart-binding-notes.txt). System.Single is
  //     resolvable, but without the validated level-helper methods the group must stay off.
  //     class_gm / class_single / f_bag_gm / m_gm_level / m_gm_mult remain null.

  void *levels=nullptr;
  if(resolve_class(md,"","GameMasterData",&b.class_gm)||
     resolve_class(md,"System","Single",&b.class_single)||
     resolve_class(md,"Niantic.Holoholo","GameMasterDataExt_PlayerLevelSettings",&levels)||
     validate_field(md,b.class_item_bag,"cynq",0x30,PGR_MD_FIELD_REF,0,"IGameMasterData",&b.f_bag_gm,nullptr)||
     validate_method(md,levels,"GetPokemonLevelOfCpMultiplier","System.Int32",2,"IGameMasterData",1,&b.m_gm_level,"System.Single")||
     validate_method(md,levels,"CpMultiplierAtLevel","System.Single",2,"IGameMasterData",1,&b.m_gm_mult,"System.Int32"))
    return PGR_SMARTBIND_ERR_METHOD;

  // Latch the spawn-freshness pointers (field-validated below) for pgr_smart_spawn_fresh.
  void *f_wild_proto = nullptr, *f_last = nullptr, *f_hidden = nullptr;
  if (validate_field(md, b.class_wildmap, "eluo", 0x270, PGR_MD_FIELD_REF, 0, "Holoholo.Rpc.WildPokemonProto", &f_wild_proto, nullptr) ||
      validate_field(md, class_wild_proto, "LastModifiedMs", 0x18, PGR_MD_FIELD_VALUE, 8, "System.Int64", &f_last, nullptr) ||
      validate_field(md, class_wild_proto, "TimeTillHiddenMs", 0x40, PGR_MD_FIELD_VALUE, 4, "System.Int32", &f_hidden, nullptr))
    return PGR_SMARTBIND_ERR_FIELD;

  int rc=pgr_smart_bind(&b);
  if(rc!=PGR_SMART_OK)return PGR_SMARTBIND_ERR_BIND;

  void *unity_object=nullptr,*find_loaded=nullptr;
  if(resolve_class(md,"UnityEngine","Object",&unity_object)||
     validate_method(md,unity_object,"FindObjectsOfType","UnityEngine.Object[]",2,"System.Type",1,&find_loaded,"System.Boolean"))
    return PGR_SMARTBIND_ERR_METHOD;
  pgr_vgp_discovery_loaded_method(find_loaded);
  void *encounter_enum=nullptr;
  if(resolve_class(md,"Holoholo.Rpc","EncounterOutProto",&encounter_class)||
     resolve_class(md,"Niantic.Holoholo.Map","PokemonEncounterResponse",&encounter_wrapper)||
     validate_field(md,b.class_wildmap,"elup",0x278,PGR_MD_FIELD_REF,0,"Holoholo.Rpc.EncounterOutProto",&map_encounter,nullptr)||
     validate_field(md,encounter_class,"Status",0x1c,PGR_MD_FIELD_ENUM_I32,4,nullptr,&encounter_status,&encounter_enum)||
     validate_field(md,encounter_class,"Pokemon",0x10,PGR_MD_FIELD_REF,0,"Holoholo.Rpc.WildPokemonProto",&encounter_pokemon,nullptr)||
     !read_enum_literal(ex,rt,encounter_enum,"EncounterSuccess",&encounter_success))return PGR_SMARTBIND_ERR_FIELD;

  g_fresh.owner = owner;
  g_fresh.class_wildmap = b.class_wildmap;
  g_fresh.f_wild_proto = f_wild_proto;
  g_fresh.f_last_modified = f_last;
  g_fresh.f_time_till_hidden = f_hidden;
  g_fresh.object_get_class = rt.object_get_class;
  g_fresh.field_get_value = rt.field_get_value;
  g_bound = true;
  __android_log_print(ANDROID_LOG_INFO,"PGRazorSmartCatch","Native catch binding ready (level rules and berry RPC validated)");
  return PGR_SMARTBIND_OK;
}

int pgr_smart_spawn_fresh(void *rootedWild, uint64_t owner) {
  std::unique_lock<std::mutex> lock(g_bind_mutex, std::try_to_lock);
  if (!lock.owns_lock()) return PGR_SMARTBIND_ERR_IDENTITY;  // fail closed, never block a managed frame
  if (!g_bound || !rootedWild || owner == 0 || owner != g_fresh.owner) return PGR_SMARTBIND_ERR_IDENTITY;
  if (owner != static_cast<uint64_t>(gettid())) return PGR_SMARTBIND_ERR_IDENTITY;

  auto get_class = reinterpret_cast<void *(*)(void *)>(g_fresh.object_get_class);
  auto get_value = reinterpret_cast<void (*)(void *, void *, void *)>(g_fresh.field_get_value);
  if (!get_class || !get_value) return PGR_SMARTBIND_ERR_IDENTITY;

  // Exact-class identity: a wrong class or missing proto is a malformed identity failure.
  if (get_class(rootedWild) != g_fresh.class_wildmap) return PGR_SMARTBIND_ERR_IDENTITY;

  void *wild_proto = nullptr;
  get_value(rootedWild, g_fresh.f_wild_proto, &wild_proto);
  if (!wild_proto) return PGR_SMARTBIND_ERR_IDENTITY;

  int64_t last_modified = 0;
  int32_t time_till_hidden = 0;
  get_value(wild_proto, g_fresh.f_last_modified, &last_modified);
  get_value(wild_proto, g_fresh.f_time_till_hidden, &time_till_hidden);

  // Unknown lifetime (no hidden window) is allowed through: we never fabricate a despawn time.
  if (time_till_hidden <= 0) return PGR_SMARTBIND_FRESH_PASS;

  const int64_t now = now_ms();
  if (now < 0) return PGR_SMARTBIND_FRESH_PASS;  // clock unavailable -> do not filter (fail open to PASS)
  const int64_t hidden_at = last_modified + time_till_hidden;  // server epoch ms
  const int64_t remaining = hidden_at - now;
  return remaining < PGR_SMARTBIND_FRESH_MIN_MS ? PGR_SMARTBIND_FRESH_FILTER : PGR_SMARTBIND_FRESH_PASS;
}

int pgr_smart_encounter_success(void *target,void *response,uint64_t owner){
 if(!g_bound||owner!=g_fresh.owner||owner!=uint64_t(gettid())||!response)return PGR_SMARTBIND_ERR_IDENTITY;
 auto klass=reinterpret_cast<void*(*)(void*)>(g_fresh.object_get_class);
 auto read=reinterpret_cast<void(*)(void*,void*,void*)>(g_fresh.field_get_value);
 PgrSmartEncounterBinding b{g_fresh.class_wildmap,encounter_wrapper,encounter_class,map_encounter,encounter_status,encounter_pokemon,encounter_success,klass,read};
 return pgr_smart_validate_encounter(b,target,response);
}
