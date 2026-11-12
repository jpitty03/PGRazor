#include "showcase_tag_runtime.h"
#include "runtime_bootstrap.h"
#include "runtime_metadata.h"
#include "unity_scheduler.h"

#include <dlfcn.h>
#include <unistd.h>
#include <time.h>
#include <cstring>
#include <string>

// Local exceptions unwind temporary roots on every failed gate; public APIs catch
// all exceptions. Compile this source with -fexceptions, like batch_evolution.cpp.

namespace pgr_showcase {
namespace {

// --------------------------------------------------------------------------------------
// Exact managed type identity (DATA, never a guessed raw offset/address/array layout).
//
// CONFIRMED: corroborated by existing usage in batch_evolution.cpp
// (SetPokemonTags / PokemonTagChanges / PokemonTagProto / the IPromise<bool> fields).
//
// Create-tag class, method and CreatedTag field identities were checked against
// the local 0.435.2 metadata extract by the coordinator. Runtime checks remain
// required; this draft is not yet integrated or device-verified.
// --------------------------------------------------------------------------------------

// Canonical type order (index 0..17) the Showcase keeper tags by. DATA, per the task spec.
const char* const kTypeNames[18] = {
    "Normal", "Fighting", "Flying", "Poison", "Ground", "Rock", "Bug", "Ghost",
    "Steel", "Fire", "Water", "Grass", "Electric", "Psychic", "Ice", "Dragon", "Dark", "Fairy"};

// CONFIRMED (batch_evolution.cpp).
const char* const kSetPokemonTags = "SetPokemonTags";
const char* const kSetReturn = "Niantic.Promises.IPromise<System.Boolean>";
const char* const kSetParam = "System.Collections.Generic.List<Niantic.Holoholo.Tagging.PokemonTagChanges>";
const char* const kChangesNs = "Niantic.Holoholo.Tagging";
const char* const kChangesName = "PokemonTagChanges";
const char* const kPokemonProtoType = "Holoholo.Rpc.PokemonProto";
const char* const kTagListType = "System.Collections.Generic.List<Holoholo.Rpc.PokemonTagProto>";
const char* const kPokemonNs = "Holoholo.Rpc";
const char* const kPokemonName = "PokemonProto";
const char* const kTagProtoNs = "Holoholo.Rpc";
const char* const kTagProtoName = "PokemonTagProto";
const char* const kTagProtoType = "Holoholo.Rpc.PokemonTagProto";
const char* const kStringType = "System.String";

// Create path metadata checked against the local 0.435.2 extract.
const char* const kAddTag = "AddTag";
const char* const kAddTagReturn = "Niantic.Promises.IPromise<Holoholo.Rpc.CreatePokemonTagOutProto>";
const char* const kColorNs = "Holoholo.Rpc";
const char* const kColorName = "PokemonTagColor";
const char* const kColorType = "Holoholo.Rpc.PokemonTagColor";
const char* const kColorYellow = "Yellow";
const char* const kCreateOutNs = "Holoholo.Rpc";
const char* const kCreateOutName = "CreatePokemonTagOutProto";
const char* const kCreateOutType = "Holoholo.Rpc.CreatePokemonTagOutProto";
const char* const kCreateTagField = "CreatedTag";   // created PokemonTagProto on the out-proto.
const char* const kResultSuccess = "Success";

// IL2CPP field attribute bits (metadata.h/quick-close): STATIC=0x10, and a static literal
// const carries STATIC|LITERAL|HAS_DEFAULT = 0x8050.
constexpr int kFieldStatic = 0x10;
constexpr int kFieldLiteralConst = 0x8050;

// Test seam: the reference-field setter (il2cpp_field_set_value_object). In production it is
// resolved from the bootstrap dlopen handle; the standalone fixture sets it directly before use.
void* g_reference_setter = nullptr;

// Non-blocking reentry guard shared by start and poll. Never a mutex across managed/gchandle
// calls: a reentrant managed callback or concurrent caller is refused, not deadlocked.
thread_local bool g_busy = false;
struct ReentryGuard {
  bool entered;
  explicit ReentryGuard() : entered(!g_busy) { if (entered) g_busy = true; }
  ~ReentryGuard() { if (entered) g_busy = false; }
};

// --------------------------------------------------------------------------------------
// Native-only lease. CLOCK_BOOTTIME, exact scheduler owner/generation, valid()==1 (a return,
// not a status), attached thread. Nonallocating; makes no managed calls. No lease extension.
// --------------------------------------------------------------------------------------
uint64_t boottime_ms() {
  timespec t{};
  if (clock_gettime(CLOCK_BOOTTIME, &t) != 0 || t.tv_sec < 0 || t.tv_nsec < 0) return 0;
  return uint64_t(t.tv_sec) * 1000 + uint64_t(t.tv_nsec) / 1000000;
}
bool owner_valid(uint64_t owner, uint64_t generation) {
  if (!owner || owner != uint64_t(gettid()) || !generation) return false;
  const auto* api = static_cast<const PgrUnitySchedulerApi*>(pgrazor_unity_scheduler_api(2));
  PgrUnitySchedulerSnapshot s{};
  return api && !api->snapshot(&s) && s.installed && !s.poisoned && s.owner_tid > 0 &&
         uint64_t(s.owner_tid) == owner && s.generation == generation && api->valid(generation) == 1;
}
bool live(const TagRpcRequest& r) {
  const auto now = boottime_ms();
  return now && r.deadline_ms > now && r.deadline_ms - now <= 2500 && owner_valid(r.owner, r.generation)&&
         (!r.authorized||r.authorized(r.authorization_context));
}

// --------------------------------------------------------------------------------------
// Resolved runtime binding (copied per call; no global managed state is retained).
// --------------------------------------------------------------------------------------
struct GateFailure {};
struct Rt {
  const TagRpcRequest* request = nullptr;
  mutable bool dispatched = false;
  PgrBootBinding boot{};
  const PgrMetaApi* meta_api = nullptr;
  void* ref_setter = nullptr;
};

template <class F> F fn(void* p) { return reinterpret_cast<F>(p); }

bool thread_attached(const Rt& rt);
void checked(const Rt& rt) {
  if(!rt.request || !live(*rt.request) || !thread_attached(rt))throw GateFailure{};
}


bool bind_runtime(Rt& rt) {
  const auto* bootstrap = static_cast<const PgrBootApi*>(pgrazor_runtime_bootstrap_api(1));
  if (!bootstrap || bootstrap->snapshot(&rt.boot) != 0) return false;
  rt.meta_api = static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));
  if (!rt.meta_api) return false;
  const PgrBootRuntimeExports& r = rt.boot.runtime;
  if (!r.gchandle_new || !r.gchandle_get_target || !r.gchandle_free || !r.object_get_class ||
      !r.object_new || !r.runtime_invoke || !r.field_get_value || !r.field_static_get_value ||
      !r.string_new || !r.thread_current)
    return false;
  const PgrMetaExports& m = rt.boot.meta;
  if (!m.class_get_field_from_name || !m.field_get_type || !m.class_from_type || !m.type_get_name ||
      !m.mem_free || !m.class_get_method_from_name || !m.method_get_param_count || !m.method_get_param ||
      !m.method_get_return_type || !m.field_get_flags || !m.class_is_enum || !m.class_enum_basetype ||
      !m.method_get_class || !m.class_is_assignable_from)
    return false;
  rt.ref_setter = g_reference_setter;
  if (!rt.ref_setter && rt.boot.handle) rt.ref_setter = dlsym(rt.boot.handle, "il2cpp_field_set_value_object");
  if (!rt.ref_setter) return false;
  return true;
}

void* target(const Rt& rt, uintptr_t h) {
  checked(rt);
  return h ? fn<void* (*)(uintptr_t)>(rt.boot.runtime.gchandle_get_target)(h) : nullptr;
}
uintptr_t gc_new(const Rt& rt, void* o) {
  checked(rt);
  return o ? fn<uintptr_t (*)(void*, int)>(rt.boot.runtime.gchandle_new)(o, 0) : 0;
}
void gc_free(const Rt& rt, uintptr_t h) {
  if (h) fn<void (*)(uintptr_t)>(rt.boot.runtime.gchandle_free)(h);
}
void* klass_of(const Rt& rt, void* o) {
  checked(rt);
  return o ? fn<void* (*)(void*)>(rt.boot.runtime.object_get_class)(o) : nullptr;
}
bool thread_attached(const Rt& rt) {
  return fn<void* (*)()>(rt.boot.runtime.thread_current)() != nullptr;
}

// Owned GC root with relocating-GC-safe re-resolution and explicit ownership transfer.
struct Root {
  const Rt* rt = nullptr;
  uintptr_t h = 0;
  Root() = default;
  Root(const Rt& r, void* obj) : rt(&r) { if (obj) h = gc_new(r, obj); }
  Root(const Root&) = delete;
  Root& operator=(const Root&) = delete;
  Root(Root&& o) noexcept : rt(o.rt), h(o.h) { o.h = 0; }
  Root& operator=(Root&& o) noexcept {
    if (this != &o) { reset(); rt = o.rt; h = o.h; o.h = 0; }
    return *this;
  }
  ~Root() { reset(); }
  void reset() { if (h && rt) gc_free(*rt, h); h = 0; }
  void* get() const { return (h && rt) ? target(*rt, h) : nullptr; }
  explicit operator bool() const { return get() != nullptr; }
  uintptr_t release() { uintptr_t t = h; h = 0; return t; }
};

void* resolve_class(const Rt& rt, const char* ns, const char* name) {
  checked(rt);
  PgrMetaClassQuery q{};
  q.version = 1; q.size = sizeof(q); q.name_space = ns; q.name = name;
  q.canonical_match = PGR_MD_MATCH_EXACT;
  PgrMetaClassResult out{};
  return rt.meta_api->resolve_class(&q, &out) == 0 ? const_cast<void*>(out.klass) : nullptr;
}

// type_get_name, with the IL2CPP "`1" generic-arity marker stripped (quick-close typeName
// semantics, matching batch_evolution.cpp). Returns false if the runtime yields no name.
bool type_name(const Rt& rt, void* type, std::string& out) {
  out.clear();
  if (!type) return false;
  char* p = fn<char* (*)(void*)>(rt.boot.meta.type_get_name)(type);
  if (!p) return false;
  out = p;
  fn<void (*)(void*)>(rt.boot.meta.mem_free)(p);
  for (size_t pos = 0; (pos = out.find("`1<", pos)) != std::string::npos;) out.erase(pos, 2);
  return true;
}

void* field_type(const Rt& rt, void* f) {
  return fn<void* (*)(void*)>(rt.boot.meta.field_get_type)(f);
}
void* class_from_type(const Rt& rt, void* type) {
  return type ? fn<void* (*)(void*)>(rt.boot.meta.class_from_type)(type) : nullptr;
}
int field_flags(const Rt& rt, void* f) {
  return fn<int (*)(void*)>(rt.boot.meta.field_get_flags)(f);
}

// Instance field by name. If `type` is non-null the exact (arity-stripped) type name must match.
void* instance_field(const Rt& rt, void* k, const char* name, const char* type) {
  if (!k) return nullptr;
  void* f = fn<void* (*)(void*, const char*)>(rt.boot.meta.class_get_field_from_name)(k, name);
  if (!f) return nullptr;
  if (field_flags(rt, f) & kFieldStatic) return nullptr;
  if (type) {
    std::string tn;
    if (!type_name(rt, field_type(rt, f), tn) || tn != type) return nullptr;
  }
  return f;
}

// Method by name/arity. Returned so the caller can further validate the signature.
void* method_of(const Rt& rt, void* k, const char* name, int argc) {
  if (!k) return nullptr;
  void* m = fn<void* (*)(void*, const char*, int)>(rt.boot.meta.class_get_method_from_name)(k, name, argc);
  if (!m) return nullptr;
  if (fn<unsigned (*)(void*)>(rt.boot.meta.method_get_param_count)(m) != unsigned(argc)) return nullptr;
  void* declaring=fn<void*(*)(void*)>(rt.boot.meta.method_get_class)(m);
  if(!declaring||!fn<bool(*)(void*,void*)>(rt.boot.meta.class_is_assignable_from)(declaring,k))return nullptr;
  PgrMetaMethodQuery q{};q.version=1;q.size=sizeof(q);q.owner=declaring;q.name=name;
  q.param_count=unsigned(argc);q.allow_inflated=1;
  PgrMetaMethodResult result{};
  return rt.meta_api->validate_method(&q,&result)==0?const_cast<void*>(result.method):nullptr;
}
void* return_type(const Rt& rt, void* m) {
  return fn<void* (*)(void*)>(rt.boot.meta.method_get_return_type)(m);
}
void* param_type(const Rt& rt, void* m, unsigned i) {
  return fn<void* (*)(void*, unsigned)>(rt.boot.meta.method_get_param)(m, i);
}

// runtime_invoke wrapper. Sets threw when the managed call raised an IL2CPP exception.
void* invoke(const Rt& rt, void* m, void* self, void** args, bool& threw) {
  checked(rt);
  void* ex = nullptr;
  void* v = fn<void* (*)(void*, void*, void**, void**)>(rt.boot.runtime.runtime_invoke)(m, self, args, &ex);
  threw = ex != nullptr;
  return v;
}

// Invoke an instance method resolved by name/arity on obj's live class. false => managed fault.
bool call(const Rt& rt, void* obj, const char* name, int argc, void** args, void*& result) {
  result = nullptr;
  void* m = method_of(rt, klass_of(rt, obj), name, argc);
  if (!m) return false;
  std::string returned;
  if(!type_name(rt,return_type(rt,m),returned)||returned!="System.Void")return false;
  if(argc==1){
    if(!args||!args[0])return false;
    void* expected=class_from_type(rt,param_type(rt,m,0));
    if(!expected||!fn<bool(*)(void*,void*)>(rt.boot.meta.class_is_assignable_from)(expected,klass_of(rt,args[0])))return false;
  }
  bool threw = false;
  result = invoke(rt, m, obj, args, threw);
  return !threw;
}
bool ctor(const Rt& rt, void* obj) {
  void* r = nullptr;
  return call(rt, obj, ".ctor", 0, nullptr, r);
}

template <class T> void read_field(const Rt& rt, void* o, void* f, T& out) {
  checked(rt);
  fn<void (*)(void*, void*, void*)>(rt.boot.runtime.field_get_value)(o, f, &out);
}
template <> void read_field<bool>(const Rt& rt,void* o,void* f,bool& out) {
  checked(rt);uint8_t value=0;
  fn<void (*)(void*,void*,void*)>(rt.boot.runtime.field_get_value)(o,f,&value);
  if(value>1)throw GateFailure{};
  out=value!=0;
}
void set_ref(const Rt& rt, void* o, void* f, void* val) {
  checked(rt);
  fn<void (*)(void*, void*, void*)>(rt.ref_setter)(o, f, val);
}

// Value of a static literal const field (an enum literal). false if it is not a literal const.
bool enum_literal(const Rt& rt, void* enum_class, const char* name, int& out) {
  checked(rt);
  if (!enum_class) return false;
  if(!fn<bool(*)(void*)>(rt.boot.meta.class_is_enum)(enum_class))return false;
  std::string underlying;
  if(!type_name(rt,fn<void*(*)(void*)>(rt.boot.meta.class_enum_basetype)(enum_class),underlying)||underlying!="System.Int32")return false;
  void* f = fn<void* (*)(void*, const char*)>(rt.boot.meta.class_get_field_from_name)(enum_class, name);
  if (!f) return false;
  if ((field_flags(rt, f) & kFieldLiteralConst) != kFieldLiteralConst) return false;
  int v = 0;
  checked(rt);
  fn<void (*)(void*, void*)>(rt.boot.runtime.field_static_get_value)(f, &v);
  out = v;
  return true;
}

void* new_object(const Rt& rt, void* k) {
  checked(rt);
  return k ? fn<void* (*)(void*)>(rt.boot.runtime.object_new)(k) : nullptr;
}
void* new_string(const Rt& rt, const char* utf8) {
  checked(rt);
  return fn<void* (*)(const char*)>(rt.boot.runtime.string_new)(utf8);
}

// Build a validated generic List<T> from an actual method param / field type. The exact
// (arity-stripped) type name is checked and a default-constructed instance returned.
Root new_list(const Rt& rt, void* list_type, const char* expected) {
  std::string tn;
  if (!type_name(rt, list_type, tn) || tn != expected) return {};
  Root list(rt, new_object(rt, class_from_type(rt, list_type)));
  if (!list || !ctor(rt, list.get())) return {};
  return list;
}

// --------------------------------------------------------------------------------------
// create: AddTag(String, PokemonTagColor.Yellow)
// --------------------------------------------------------------------------------------
TagStartStatus start_create(const Rt& rt, const TagRpcRequest& req, uintptr_t& out_promise) {
  if (req.type_index >= 18) return TagStartStatus::bad_identity;
  void* service = target(rt, req.service);
  if (!service) return TagStartStatus::bad_identity;

  // Resolve + validate AddTag on the live service class. Fail closed on any mismatch.
  void* svc_class = klass_of(rt, service);
  if(svc_class!=resolve_class(rt,"Niantic.Holoholo.Tagging","TaggingRpcService"))return TagStartStatus::bad_identity;
  void* add = method_of(rt, svc_class, kAddTag, 2);
  if (!add) return TagStartStatus::bad_binding;
  std::string tn;
  if (!type_name(rt, return_type(rt, add), tn) || tn != kAddTagReturn) return TagStartStatus::bad_binding;
  if (!type_name(rt, param_type(rt, add, 0), tn) || tn != kStringType) return TagStartStatus::bad_binding;
  if (!type_name(rt, param_type(rt, add, 1), tn) || tn != kColorType) return TagStartStatus::bad_binding;

  void* color_class = resolve_class(rt, kColorNs, kColorName);
  int yellow = 0;
  if (!enum_literal(rt, color_class, kColorYellow, yellow)) return TagStartStatus::bad_binding;

  std::string name = "Showcase \xC2\xB7 ";  // "Showcase <U+00B7> "
  name += kTypeNames[req.type_index];

  if (!live(req) || !thread_attached(rt)) return TagStartStatus::refused;
  Root str(rt, new_string(rt, name.c_str()));
  if (!str) return TagStartStatus::refused;
  if (!live(req)) return TagStartStatus::refused;

  // Re-resolve the borrowed service after the allocation safepoint; never cache it across allocs.
  service = target(rt, req.service);
  if (!service || klass_of(rt, service) != svc_class) return TagStartStatus::refused;
  void* args[2] = {str.get(), &yellow};

  // ---- dispatch boundary: anything from here on is outcome_unknown, never auto-retried ----
  checked(rt);
  rt.dispatched = true;
  bool threw = false;
  void* promise = invoke(rt, add, service, args, threw);
  if (threw || !promise) return TagStartStatus::outcome_unknown;
  uintptr_t h = gc_new(rt, promise);
  if (!h) return TagStartStatus::outcome_unknown;  // RPC sent but the owned handle was lost.
  out_promise = h;
  return TagStartStatus::dispatched;
}

// --------------------------------------------------------------------------------------
// add/remove: SetPokemonTags([{Pokemon, TagsToAdd/TagsToRemove:[tag]}])
// --------------------------------------------------------------------------------------
TagStartStatus start_membership(const Rt& rt, const TagRpcRequest& req, uintptr_t& out_promise) {
  void* service = target(rt, req.service);
  void* pokemon = target(rt, req.pokemon);
  void* tag = target(rt, req.tag);
  if (!service || !pokemon || !tag) return TagStartStatus::bad_identity;

  void* pokemon_class = resolve_class(rt, kPokemonNs, kPokemonName);
  void* tagproto_class = resolve_class(rt, kTagProtoNs, kTagProtoName);
  if (!pokemon_class || !tagproto_class) return TagStartStatus::bad_binding;
  if (klass_of(rt, pokemon) != pokemon_class) return TagStartStatus::bad_identity;
  if (klass_of(rt, tag) != tagproto_class) return TagStartStatus::bad_identity;

  // Resolve + validate SetPokemonTags on the live service class.
  void* svc_class = klass_of(rt, service);
  if(svc_class!=resolve_class(rt,"Niantic.Holoholo.Tagging","TaggingRpcService"))return TagStartStatus::bad_identity;
  void* set = method_of(rt, svc_class, kSetPokemonTags, 1);
  if (!set) return TagStartStatus::bad_binding;
  std::string tn;
  if (!type_name(rt, return_type(rt, set), tn) || tn != kSetReturn) return TagStartStatus::bad_binding;
  void* set_param_type = param_type(rt, set, 0);
  if (!type_name(rt, set_param_type, tn) || tn != kSetParam) return TagStartStatus::bad_binding;

  // PokemonTagChanges structural contract (exact field names + types).
  void* change_class = resolve_class(rt, kChangesNs, kChangesName);
  void* f_pokemon = instance_field(rt, change_class, "PokemonProto", kPokemonProtoType);
  void* f_add = instance_field(rt, change_class, "TagsToAdd", kTagListType);
  void* f_remove = instance_field(rt, change_class, "TagsToRemove", kTagListType);
  if (!change_class || !f_pokemon || !f_add || !f_remove) return TagStartStatus::bad_binding;
  void* pokemon_id_field=instance_field(rt,pokemon_class,"id_","System.UInt64");
  void* tag_id_field=instance_field(rt,tagproto_class,"Id","System.UInt64");
  if(!pokemon_id_field||!tag_id_field)return TagStartStatus::bad_binding;
  auto identity=[&](){
    void* pk=target(rt,req.pokemon);void* tg=target(rt,req.tag);
    if(!pk||!tg||klass_of(rt,pk)!=pokemon_class||klass_of(rt,tg)!=tagproto_class)return false;
    uint64_t pkid=0,tagid=0;read_field(rt,pk,pokemon_id_field,pkid);read_field(rt,tg,tag_id_field,tagid);
    return req.pokemon_id&&req.tag_id&&pkid==req.pokemon_id&&tagid==req.tag_id;
  };
  if(!identity())return TagStartStatus::bad_identity;

  if (!live(req) || !thread_attached(rt)) return TagStartStatus::refused;

  // Build the request graph. Lists come from the actual validated param/field types.
  Root changes_list = new_list(rt, set_param_type, kSetParam);
  if (!changes_list || !live(req)) return TagStartStatus::refused;
  Root change(rt, new_object(rt, change_class));
  if (!change || !ctor(rt, change.get()) || !live(req)) return TagStartStatus::refused;
  Root add_list = new_list(rt, field_type(rt, f_add), kTagListType);
  if (!add_list || !live(req)) return TagStartStatus::refused;
  Root remove_list = new_list(rt, field_type(rt, f_remove), kTagListType);
  if (!remove_list || !live(req)) return TagStartStatus::refused;

  // Put the single caller-owned tag into exactly one list; the other stays empty so no
  // unrelated membership is touched. Re-resolve the borrowed tag after the allocations.
  tag = target(rt, req.tag);
  if (!tag) return TagStartStatus::refused;
  Root& target_list = req.op == TagRpcOp::add ? add_list : remove_list;
  {
    void* a[1] = {tag};
    void* r = nullptr;
    if (!call(rt, target_list.get(), "Add", 1, a, r)) return TagStartStatus::refused;
  }
  if (!live(req)) return TagStartStatus::refused;

  // Wire the change via reference writes (GC write barrier). Re-resolve pokemon after safepoints.
  pokemon = target(rt, req.pokemon);
  if (!pokemon) return TagStartStatus::refused;
  set_ref(rt, change.get(), f_pokemon, pokemon);
  set_ref(rt, change.get(), f_add, add_list.get());
  set_ref(rt, change.get(), f_remove, remove_list.get());
  {
    void* a[1] = {change.get()};
    void* r = nullptr;
    if (!call(rt, changes_list.get(), "Add", 1, a, r)) return TagStartStatus::refused;
  }
  if (!live(req)) return TagStartStatus::refused;

  // Final guard, then dispatch. Re-resolve the service after the build safepoints.
  if(!identity())return TagStartStatus::bad_identity;
  service = target(rt, req.service);
  if (!service || klass_of(rt, service) != svc_class) return TagStartStatus::refused;
  void* args[1] = {changes_list.get()};

  // ---- dispatch boundary ----
  checked(rt);
  rt.dispatched = true;
  bool threw = false;
  void* promise = invoke(rt, set, service, args, threw);
  if (threw || !promise) return TagStartStatus::outcome_unknown;
  uintptr_t h = gc_new(rt, promise);
  if (!h) return TagStartStatus::outcome_unknown;
  out_promise = h;
  return TagStartStatus::dispatched;
}

// Read completeCalled/errorCalled on a promise. Returns false on a structural fault.
bool promise_flags(const Rt& rt, void* promise, bool& done, bool& errored) {
  void* pcls = klass_of(rt, promise);
  void* f_err = instance_field(rt, pcls, "errorCalled", "System.Boolean");
  void* f_done = instance_field(rt, pcls, "completeCalled", "System.Boolean");
  if (!f_err || !f_done) return false;
  errored = false;
  done = false;
  read_field(rt, promise, f_err, errored);
  read_field(rt, promise, f_done, done);
  return true;
}

TagPollStatus poll_create(const Rt& rt, const TagRpcRequest& req, void* promise, uint64_t& out_tag_id) {
  void* pcls = klass_of(rt, promise);
  void* f_val = instance_field(rt, pcls, "completedValue", kCreateOutType);
  if (!f_val) return TagPollStatus::bad_binding;
  void* outp = nullptr;
  read_field(rt, promise, f_val, outp);
  Root out_proto(rt, outp);  // root the reference result across the following safepoints
  if (!out_proto) return TagPollStatus::rejected;

  void* out_class = resolve_class(rt, kCreateOutNs, kCreateOutName);
  if (!out_class) return TagPollStatus::bad_binding;
  if (klass_of(rt, out_proto.get()) != out_class) return TagPollStatus::rejected;

  void* f_result = instance_field(rt, out_class, "Result", nullptr);
  if (!f_result) return TagPollStatus::bad_binding;
  void* result_enum = class_from_type(rt, field_type(rt, f_result));
  int success = 0;
  if (!enum_literal(rt, result_enum, kResultSuccess, success)) return TagPollStatus::bad_binding;
  int result = 0;
  read_field(rt, out_proto.get(), f_result, result);
  if (result != success) return TagPollStatus::rejected;

  void* f_tag = instance_field(rt, out_class, kCreateTagField, kTagProtoType);
  if (!f_tag) return TagPollStatus::bad_binding;
  void* tagp = nullptr;
  read_field(rt, out_proto.get(), f_tag, tagp);
  Root created(rt, tagp);
  if (!created) return TagPollStatus::rejected;
  void* tagproto_class = resolve_class(rt, kTagProtoNs, kTagProtoName);
  if (!tagproto_class) return TagPollStatus::bad_binding;
  if (klass_of(rt, created.get()) != tagproto_class) return TagPollStatus::rejected;

  void* f_id = instance_field(rt, tagproto_class, "Id", "System.UInt64");
  if (!f_id) return TagPollStatus::bad_binding;
  uint64_t id = 0;
  read_field(rt, created.get(), f_id, id);
  if (!id) return TagPollStatus::rejected;

  // Publish only after the roots are cleaned up on return and a final lease guard.
  if (!live(req)) return TagPollStatus::refused;
  out_tag_id = id;
  return TagPollStatus::created;
}

TagPollStatus poll_membership(const Rt& rt, const TagRpcRequest& req, void* promise) {
  void* pcls = klass_of(rt, promise);
  void* f_val = instance_field(rt, pcls, "completedValue", "System.Boolean");
  if (!f_val) return TagPollStatus::bad_binding;
  bool accepted = false;
  read_field(rt, promise, f_val, accepted);
  if (!live(req)) return TagPollStatus::refused;
  return accepted ? TagPollStatus::confirmed : TagPollStatus::rejected;
}

}  // namespace

TagStartStatus start_tag_rpc(const TagRpcRequest& req, uintptr_t& out_promise) {
  out_promise=0;ReentryGuard guard;
  if(!guard.entered || !live(req))return TagStartStatus::refused;
  Rt rt;rt.request=&req;uintptr_t pending=0;
  try {
    if(!bind_runtime(rt))return TagStartStatus::bad_binding;
    checked(rt);
    TagStartStatus status=TagStartStatus::bad_identity;
    switch(req.op) {
      case TagRpcOp::create: status=start_create(rt,req,pending);break;
      case TagRpcOp::add: case TagRpcOp::remove: status=start_membership(rt,req,pending);break;
    }
    // All builder temporaries have been released. Lease loss during cleanup is
    // uncertain after dispatch; no successful result is published prematurely.
    checked(rt);
    if(status==TagStartStatus::dispatched){out_promise=pending;pending=0;}
    else if(pending){gc_free(rt,pending);pending=0;}
    return status;
  } catch(...) {
    if(pending)gc_free(rt,pending);
    return rt.dispatched?TagStartStatus::outcome_unknown:TagStartStatus::refused;
  }
}

TagPollStatus poll_tag_rpc(const TagRpcRequest& req, uintptr_t promise, uint64_t& out_tag_id) {
  out_tag_id=0;ReentryGuard guard;
  if(!guard.entered || !promise || !live(req))return TagPollStatus::refused;
  Rt rt;rt.request=&req;
  try {
    if(!bind_runtime(rt))return TagPollStatus::bad_binding;
    checked(rt);void* p=target(rt,promise);if(!p)return TagPollStatus::refused;
    bool done=false,errored=false;
    if(!promise_flags(rt,p,done,errored))return TagPollStatus::bad_binding;
    checked(rt);
    if(errored)return TagPollStatus::rejected;
    if(!done)return TagPollStatus::pending;
    uint64_t id=0;TagPollStatus status=TagPollStatus::bad_binding;
    switch(req.op) {
      case TagRpcOp::create:status=poll_create(rt,req,p,id);break;
      case TagRpcOp::add:case TagRpcOp::remove:status=poll_membership(rt,req,p);break;
    }
    checked(rt); // poll temporaries have been released before publishing.
    if(status==TagPollStatus::created)out_tag_id=id;
    return status;
  } catch(...) {return TagPollStatus::outcome_unknown;}
}
} // namespace pgr_showcase
