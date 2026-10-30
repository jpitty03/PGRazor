#include "vgp_discovery.h"
#include "pgrazor_native_api.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <map>
#include <unistd.h>

// Real-API fake-IL2CPP harness for feature 31 (native VGP inventory/enumeration discovery). It resolves
// the table through the exported pgrazor_native_api v2 registry (query_feature(31,1)) and drives
// snapshot()/enumerate()/at() against a fake managed heap: a map context, a DI container, player /
// pokemon-bag / item-bag objects reachable through FindObjectOfType + get_Container + Resolve, Int32
// count boxes, and LP64 Object[] arrays whose element data starts at offset 32. gchandle_new/get/free
// back a handle table so every temporary root and every transferred handle is accounted exactly
// (roots == 0 between calls, except the handles the caller adopts). runtime_invoke dispatches by method
// token and can throw / return a wrong-class box. The module installs NO hook and NEVER calls back to JS.

namespace {

using GetApiFn = const void *(*)(uint32_t);

// ----- class tokens (distinct addresses) ------------------------------------------
int cls_map, cls_container, cls_playerContract, cls_bagContract, cls_player, cls_pokemonBag,
    cls_itemBag, cls_itemService, cls_int32, cls_wild, cls_stop, cls_objectArray, cls_type, cls_other;
int cls_derivedMap,cls_derivedContainer,cls_wildArray,cls_stopArray,cls_derivedExact,cls_derivedInt32;
// ----- method / field tokens ------------------------------------------------------
int m_find, m_container, m_resolve, m_itemCount, m_pokemonCount, m_maxPokemon, m_totalItems, m_maxItems,
    m_findAll, f_itemBag;

// ----- fake managed objects -------------------------------------------------------
struct Int32Box { void *klass; int32_t v; };
struct FakeType { void *klass; void *represents; };  // type_get_object result (identity type == class)
struct FakeObj { void *klass; };
struct FakeService { void *klass; void *itemBag; };
// LP64 Object[]: klass @0, header padding, element data @ offset 32 (8-byte slots).
struct FakeArray { void *klass; uint64_t pad0; uint64_t pad1; uint64_t length; void *data[8]; };

FakeObj g_map{&cls_map};
FakeObj g_container{&cls_container};
FakeObj g_player{&cls_player};
FakeObj g_pokemonBag{&cls_pokemonBag};
FakeObj g_itemBag{&cls_itemBag};
FakeService g_service{&cls_itemService, &g_itemBag};
FakeService g_moved_service{};
FakeObj g_badService{&cls_other};
Int32Box g_int{&cls_int32, 0};
Int32Box g_wrongBox{&cls_other, 0};

FakeObj g_wild0{&cls_wild}, g_wild1{&cls_wild}, g_wild2{&cls_wild};
FakeObj g_stop0{&cls_stop};
FakeArray g_wildArray{&cls_objectArray, 0, 0, 3, {&g_wild0, &g_wild1, &g_wild2}};
FakeArray g_stopArray{&cls_objectArray, 0, 0, 1, {&g_stop0}};
FakeArray g_moved_array{};

// Configurable returns / anomalies.
void *g_ret_map = &g_map;
void *g_ret_container = &g_container;
void *g_ret_player = &g_player;
void *g_ret_pokemonBag = &g_pokemonBag;
void *g_throw_method = nullptr;
void *g_count_box = nullptr;  // override box returned by every count method (wrong-class test)
int32_t g_balls = 5, g_stored = 10, g_capacity = 100, g_totalItems = 20, g_maxItems = 50;
int g_invokes = 0;
std::function<void()> g_oneshot;        // fired inside the method named by g_oneshot_on
void *g_oneshot_on = nullptr;
std::function<void()> g_oneshot_arrlen; // fired inside array_length

// ----- gc handle table ------------------------------------------------------------
std::map<uintptr_t, void *> g_handles;
uintptr_t g_seq = 0x900;
int g_gcnew_calls = 0, g_gcnew_fail = 0;  // fail the Nth gchandle_new (1-based); 0 = never
std::function<void()> g_oneshot_gcnew;   // fired while acquiring the selected object's root
void *g_gcnew_on = nullptr;

uintptr_t fake_gc_new(void *obj, int) {
  ++g_gcnew_calls;
  if (!obj) return 0;
  if (g_gcnew_fail && g_gcnew_calls == g_gcnew_fail) return 0;
  uintptr_t h = ++g_seq;
  g_handles[h] = obj;
  if(g_oneshot_gcnew&&obj==g_gcnew_on){auto fn=g_oneshot_gcnew;g_oneshot_gcnew=nullptr;fn();}
  return h;
}
void *fake_gc_get(uintptr_t h) { auto it = g_handles.find(h); return it == g_handles.end() ? nullptr : it->second; }
void fake_gc_free(uintptr_t h) { g_handles.erase(h); }
void *fake_obj_class(void *obj) { return obj ? *static_cast<void **>(obj) : nullptr; }
bool fake_assignable(void *expected,void *actual){
  if(expected==actual)return true;
  if(expected==&cls_map&&actual==&cls_derivedMap)return true;
  if(expected==&cls_container&&actual==&cls_derivedContainer)return true;
  if(expected==&cls_objectArray&&(actual==&cls_wildArray||actual==&cls_stopArray))return true;
  // These relationships are intentional traps: production must keep exact checks for inventory types.
  if(expected==&cls_int32&&actual==&cls_derivedInt32)return true;
  return actual==&cls_derivedExact&&(expected==&cls_itemService||expected==&cls_player||expected==&cls_pokemonBag||expected==&cls_itemBag);
}
void *fake_unbox(void *obj) {
  const void *klass=fake_obj_class(obj);
  return klass==&cls_int32||klass==&cls_derivedInt32 ? &static_cast<Int32Box *>(obj)->v : nullptr;
}
void fake_field_get(void *obj, void *field, void *out) {
  assert(field == &f_itemBag);
  *static_cast<void **>(out) = static_cast<FakeService *>(obj)->itemBag;
}

// class_get_type is identity (type token == class token); type_get_object caches one managed Type/class.
std::map<void *, FakeType *> g_types;
void *fake_class_get_type(void *klass) { return klass; }
void *fake_type_get_object(void *type) {
  auto it = g_types.find(type);
  if (it != g_types.end()) return it->second;
  FakeType *t = new FakeType{&cls_type, type};
  g_types[type] = t;
  return t;
}
size_t fake_array_length(void *a) {
  size_t len = static_cast<FakeArray *>(a)->length;
  if (g_oneshot_arrlen) { auto fn = g_oneshot_arrlen; g_oneshot_arrlen = nullptr; fn(); }
  return len;
}

void *fake_invoke(void *method, void *obj, void **args, void **exc) {
  ++g_invokes;
  if (exc) *exc = nullptr;
  if (method == g_throw_method) { if (exc) *exc = reinterpret_cast<void *>(0x1); return nullptr; }
  if (g_oneshot && method == g_oneshot_on) { auto fn = g_oneshot; g_oneshot = nullptr; fn(); }
  if (method == &m_find) return g_ret_map;
  if (method == &m_container) return g_ret_container;
  if (method == &m_resolve) {
    void *rep = static_cast<FakeType *>(args[0])->represents;
    if (rep == &cls_playerContract) return g_ret_player;
    if (rep == &cls_bagContract) return g_ret_pokemonBag;
    assert(false && "unexpected resolve contract");
    return nullptr;
  }
  if (method == &m_findAll) {
    void *rep = static_cast<FakeType *>(args[0])->represents;
    if (rep == &cls_wild) return &g_wildArray;
    if (rep == &cls_stop) return &g_stopArray;
    assert(false && "unexpected findAll type");
    return nullptr;
  }
  if (method == &m_itemCount) {
    assert(args && *static_cast<int32_t *>(args[0]) == 1);  // GetItemCount(item id 1)
    g_int.v = g_balls; return g_count_box ? g_count_box : &g_int;
  }
  if (method == &m_pokemonCount) { g_int.v = g_stored; return g_count_box ? g_count_box : &g_int; }
  if (method == &m_maxPokemon) { g_int.v = g_capacity; return g_count_box ? g_count_box : &g_int; }
  if (method == &m_totalItems) { g_int.v = g_totalItems; return g_count_box ? g_count_box : &g_int; }
  if (method == &m_maxItems) { g_int.v = g_maxItems; return g_count_box ? g_count_box : &g_int; }
  (void)obj;
  assert(false && "unknown method token");
  return nullptr;
}

PgrVgpDiscoveryConfig make_config() {
  PgrVgpDiscoveryConfig c{};
  c.version = PGR_VD_CONFIG_VERSION;
  c.size = sizeof(c);
  c.gchandle_new = reinterpret_cast<void *>(&fake_gc_new);
  c.gchandle_get_target = reinterpret_cast<void *>(&fake_gc_get);
  c.gchandle_free = reinterpret_cast<void *>(&fake_gc_free);
  c.object_get_class = reinterpret_cast<void *>(&fake_obj_class);
  c.runtime_invoke = reinterpret_cast<void *>(&fake_invoke);
  c.object_unbox = reinterpret_cast<void *>(&fake_unbox);
  c.field_get_value = reinterpret_cast<void *>(&fake_field_get);
  c.class_get_type = reinterpret_cast<void *>(&fake_class_get_type);
  c.type_get_object = reinterpret_cast<void *>(&fake_type_get_object);
  c.array_length = reinterpret_cast<void *>(&fake_array_length);
  c.m_find = &m_find;
  c.m_container = &m_container;
  c.m_resolve = &m_resolve;
  c.m_item_count = &m_itemCount;
  c.m_pokemon_count = &m_pokemonCount;
  c.m_max_pokemon = &m_maxPokemon;
  c.m_total_items = &m_totalItems;
  c.m_max_items = &m_maxItems;
  c.m_find_all = &m_findAll;
  c.class_map = &cls_map;
  c.class_player_contract = &cls_playerContract;
  c.class_bag_contract = &cls_bagContract;
  c.class_player = &cls_player;
  c.class_pokemon_bag = &cls_pokemonBag;
  c.class_item_bag = &cls_itemBag;
  c.class_item_service = &cls_itemService;
  c.class_int32 = &cls_int32;
  c.class_wild = &cls_wild;
  c.class_stop = &cls_stop;
  c.class_object_array = &cls_objectArray;
  c.class_container = &cls_container;
  c.f_item_bag = &f_itemBag;
  c.class_is_assignable_from=reinterpret_cast<void*>(&fake_assignable);
  return c;
}

uint64_t this_tid() { return static_cast<uint64_t>(static_cast<pid_t>(::gettid())); }

void reset() {
  assert(g_handles.empty());
  g_map = {&cls_map};
  g_container = {&cls_container};
  g_player = {&cls_player};
  g_pokemonBag = {&cls_pokemonBag};
  g_itemBag = {&cls_itemBag};
  g_service = {&cls_itemService, &g_itemBag};
  g_wildArray = {&cls_objectArray, 0, 0, 3, {&g_wild0, &g_wild1, &g_wild2}};
  g_stopArray = {&cls_objectArray, 0, 0, 1, {&g_stop0}};
  g_ret_map = &g_map;
  g_ret_container = &g_container;
  g_ret_player = &g_player;
  g_ret_pokemonBag = &g_pokemonBag;
  g_throw_method = nullptr;
  g_count_box = nullptr;
  g_wrongBox.klass=&cls_other;
  g_balls = 5; g_stored = 10; g_capacity = 100; g_totalItems = 20; g_maxItems = 50;
  g_invokes = 0;
  g_gcnew_calls = 0;
  g_gcnew_fail = 0;
  g_oneshot_gcnew = nullptr;
  g_gcnew_on = nullptr;
  g_oneshot = nullptr;
  g_oneshot_on = nullptr;
  g_oneshot_arrlen = nullptr;
}

// Repoint every live handle that targets `from` to `to` (fake relocating collection).
void relocate(void *from, void *to) {
  for (auto &e : g_handles) if (e.second == from) e.second = to;
}

const PgrVgpDiscoveryApi *g_api = nullptr;

}  // namespace

int main(int argc, char **argv) {
  const char *so_path = argc > 1 ? argv[1] : "./libpgrazor.so";
  void *lib = ::dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
  if (!lib) { std::fprintf(stderr, "dlopen(%s) failed: %s\n", so_path, ::dlerror()); return 1; }
  auto getter = reinterpret_cast<GetApiFn>(::dlsym(lib, "pgrazor_native_api"));
  assert(getter);
  const auto *core = static_cast<const pgrazor_native_api_v2 *>(getter(2));
  assert(core && core->size == 48);
  assert(core->query_feature(31, 99) == nullptr);
  const auto *api = static_cast<const PgrVgpDiscoveryApi *>(core->query_feature(31, 1));
  assert(api && api->version == 1 && api->size == sizeof(PgrVgpDiscoveryApi));
  g_api = api;

  // 0. ABI layout: fixed config/output/table sizes and field offsets.
  std::printf("ABI sizes: config=%zu snapshot=%zu enumeration=%zu table=%zu\n",
              sizeof(PgrVgpDiscoveryConfig), sizeof(PgrVgpDiscoverySnapshot),
              sizeof(PgrVgpDiscoveryEnumeration), sizeof(PgrVgpDiscoveryApi));
  assert(sizeof(PgrVgpDiscoveryConfig) == 272 && PGR_VD_CONFIG_VERSION==2 && sizeof(PgrVgpDiscoverySnapshot) == 48 &&
         sizeof(PgrVgpDiscoveryEnumeration) == 24 && sizeof(PgrVgpDiscoveryApi) == 40);
  assert(offsetof(PgrVgpDiscoveryConfig,class_is_assignable_from)==264);
  assert(offsetof(PgrVgpDiscoverySnapshot, pokemon_bag_handle) == 32 &&
         offsetof(PgrVgpDiscoverySnapshot, item_bag_handle) == 40);
  assert(offsetof(PgrVgpDiscoveryEnumeration, array_handle) == 16);

  const uint64_t tid = this_tid();
  PgrVgpDiscoveryConfig cfg = make_config();
  PgrVgpDiscoverySnapshot snap{};
  PgrVgpDiscoveryEnumeration en{};
  uintptr_t handle = 0;

  // 1. install validation + not-installed + idempotence + reconfig.
  assert(api->install(nullptr) == PGR_VD_ERR_NULL_ARG);
  { PgrVgpDiscoveryConfig b = cfg; b.version = 1; assert(api->install(&b) == PGR_VD_ERR_BAD_VERSION); }
  { PgrVgpDiscoveryConfig b = cfg; b.size = sizeof(cfg) - 1; assert(api->install(&b) == PGR_VD_ERR_BAD_SIZE); }
  { PgrVgpDiscoveryConfig b = cfg; b.f_item_bag = nullptr; assert(api->install(&b) == PGR_VD_ERR_MISSING_PTR); }
  { PgrVgpDiscoveryConfig b = cfg; b.gchandle_new = nullptr; assert(api->install(&b) == PGR_VD_ERR_MISSING_PTR); }
  { PgrVgpDiscoveryConfig b=cfg;b.class_is_assignable_from=nullptr;assert(api->install(&b)==PGR_VD_ERR_MISSING_PTR); }
  assert(api->snapshot(&g_service, tid, 2000, &snap) == PGR_VD_ERR_NOT_INSTALLED);
  assert(snap.version == 1 && snap.size == sizeof(snap) && snap.pokemon_bag_handle == 0);
  assert(api->install(&cfg) == 0);
  assert(api->install(&cfg) == 0);  // idempotent
  { PgrVgpDiscoveryConfig b = cfg; b.class_wild = &cls_stop; assert(api->install(&b) == PGR_VD_ERR_RECONFIG); }

  // 2. Owner / lease gate: no managed work, output initialised, no handles.
  reset();
  assert(api->snapshot(&g_service, tid + 7, 2000, &snap) == PGR_VD_SKIP && g_invokes == 0);
  assert(api->snapshot(&g_service, 0, 2000, &snap) == PGR_VD_SKIP && g_invokes == 0);
  assert(api->snapshot(&g_service, tid, 0, &snap) == PGR_VD_SKIP && g_invokes == 0);  // closed lease
  assert(api->snapshot(nullptr, tid, 2000, &snap) == PGR_VD_ERR_NULL_ARG && g_invokes == 0);
  assert(api->snapshot(&g_service, tid, 2000, nullptr) == PGR_VD_ERR_NULL_ARG);
  assert(g_handles.empty());

  // 3. Snapshot success: five counts + two owned handles; roots zero once the caller frees them.
  reset();
  assert(api->snapshot(&g_service, tid, 2000, &snap) == PGR_VD_OK);
  assert(snap.balls == 5 && snap.stored == 10 && snap.capacity == 100 &&
         snap.total_items == 20 && snap.max_items == 50);
  assert(snap.version == 1 && snap.size == sizeof(snap) && snap.reserved == 0);
  assert(snap.pokemon_bag_handle != 0 && snap.item_bag_handle != 0 &&
         snap.pokemon_bag_handle != snap.item_bag_handle);
  assert(fake_gc_get(snap.pokemon_bag_handle) == &g_pokemonBag);  // handle identity
  assert(fake_gc_get(snap.item_bag_handle) == &g_itemBag);
  assert(g_handles.size() == 2);  // exactly the two transferred handles, nothing else retained
  fake_gc_free(snap.pokemon_bag_handle);
  fake_gc_free(snap.item_bag_handle);
  assert(g_handles.empty());

  // Read-only scene receivers may be derived implementations; this fixture proves assignability
  // semantics without claiming which concrete runtime classes a Unity session actually returns.
  reset();g_map.klass=&cls_derivedMap;g_container.klass=&cls_derivedContainer;
  assert(api->snapshot(&g_service,tid,2000,&snap)==PGR_VD_OK);
  fake_gc_free(snap.pokemon_bag_handle);fake_gc_free(snap.item_bag_handle);assert(g_handles.empty());

  // 4. map/container/service not-ready -> benign SKIP, no handles, output initialised.
  for (int which = 0; which < 5; ++which) {
    reset();
    if (which == 0) g_ret_map = nullptr;
    else if (which == 1) g_ret_container = nullptr;
    else if (which == 2) g_ret_player = nullptr;
    else if (which == 3) g_ret_pokemonBag = nullptr;
    else g_service.itemBag = nullptr;
    assert(api->snapshot(&g_service, tid, 2000, &snap) == PGR_VD_SKIP);
    assert(snap.pokemon_bag_handle == 0 && snap.item_bag_handle == 0 && snap.balls == 0);
    assert(g_handles.empty());
  }

  // 5. Service identity + resolved-class validation: a wrong class is an invalid-class error (negative).
  reset();
  assert(api->snapshot(&g_badService, tid, 2000, &snap) == PGR_VD_ERR_METADATA && g_invokes == 0);
  assert(g_handles.empty());
  for(int which=0;which<4;++which){
    reset();
    if(which==0)g_service.klass=&cls_derivedExact;
    else if(which==1)g_player.klass=&cls_derivedExact;
    else if(which==2)g_pokemonBag.klass=&cls_derivedExact;
    else g_itemBag.klass=&cls_derivedExact;
    assert(api->snapshot(&g_service,tid,2000,&snap)==PGR_VD_ERR_METADATA);
    const uint32_t expectedStage[]={1,4,5,6};
    assert(snap.reserved==expectedStage[which]&&snap.pokemon_bag_handle==0&&snap.item_bag_handle==0&&g_handles.empty());
  }
  reset();
  {
    g_map.klass=&cls_other;
    bool containerReached=false;
    g_oneshot_on=&m_container;g_oneshot=[&](){containerReached=true;};
    assert(api->snapshot(&g_service,tid,2000,&snap)==PGR_VD_ERR_METADATA);
    assert(!containerReached&&g_invokes==1);  // FindObjectOfType only; never dispatch on wrong map
    assert(snap.reserved==2);
    assert(snap.pokemon_bag_handle==0&&snap.item_bag_handle==0&&g_handles.empty());
  }
  for (int which = 0; which < 4; ++which) {
    reset();
    if (which == 0) g_container.klass = &cls_other;
    else if (which == 1) g_player.klass = &cls_other;
    else if (which == 2) g_pokemonBag.klass = &cls_other;
    else g_itemBag.klass = &cls_other;
    assert(api->snapshot(&g_service, tid, 2000, &snap) == PGR_VD_ERR_METADATA);
    assert(snap.pokemon_bag_handle == 0 && snap.item_bag_handle == 0);
    assert(g_handles.empty());
  }

  // 6. Counts: zero is valid; a negative count fails closed; a non-Int32 box is a metadata error.
  reset();
  g_balls = 0; g_stored = 0; g_capacity = 0; g_totalItems = 0; g_maxItems = 0;
  assert(api->snapshot(&g_service, tid, 2000, &snap) == PGR_VD_OK);
  assert(snap.balls == 0 && snap.stored == 0 && snap.capacity == 0 && snap.max_items == 0);
  fake_gc_free(snap.pokemon_bag_handle); fake_gc_free(snap.item_bag_handle);
  for (int field = 0; field < 5; ++field) {
    reset();
    int32_t *target = field == 0 ? &g_balls : field == 1 ? &g_stored : field == 2 ? &g_capacity
                    : field == 3 ? &g_totalItems : &g_maxItems;
    *target = -1;
    assert(api->snapshot(&g_service, tid, 2000, &snap) == PGR_VD_ERR_NEG_COUNT);
    assert(snap.pokemon_bag_handle == 0 && snap.item_bag_handle == 0);
    assert(g_handles.empty());
  }
  reset();
  g_count_box = &g_wrongBox;  // every count method returns a non-Int32 box
  assert(api->snapshot(&g_service, tid, 2000, &snap) == PGR_VD_ERR_METADATA);
  assert(g_handles.empty());
  reset();g_wrongBox.klass=&cls_derivedInt32;g_count_box=&g_wrongBox;
  assert(api->snapshot(&g_service,tid,2000,&snap)==PGR_VD_ERR_METADATA);
  assert(snap.reserved==7&&snap.pokemon_bag_handle==0&&snap.item_bag_handle==0&&g_handles.empty());

  // 7. Managed throw from any step -> ERR_MANAGED, roots freed, no partial handles.
  for (void *m : {static_cast<void *>(&m_find), static_cast<void *>(&m_container),
                  static_cast<void *>(&m_resolve), static_cast<void *>(&m_itemCount),
                  static_cast<void *>(&m_pokemonCount), static_cast<void *>(&m_maxPokemon),
                  static_cast<void *>(&m_totalItems), static_cast<void *>(&m_maxItems)}) {
    reset();
    g_throw_method = m;
    assert(api->snapshot(&g_service, tid, 2000, &snap) == PGR_VD_ERR_MANAGED);
    assert(snap.pokemon_bag_handle == 0 && snap.item_bag_handle == 0 && g_handles.empty());
  }

  // 8. gchandle_new failure at each allocation -> negative, never a false OK, roots freed, no partials.
  for (int n = 1; n <= 12; ++n) {
    reset();
    g_gcnew_fail = n;
    int rc = api->snapshot(&g_service, tid, 2000, &snap);
    if (n <= g_gcnew_calls) {
      assert(rc < 0);
      assert(snap.pokemon_bag_handle == 0 && snap.item_bag_handle == 0);
    }
    assert(g_handles.empty());
  }

  // 9. Relocation during a getter: the service root is re-resolved for the later item-bag field read.
  reset();
  g_oneshot_on = &m_resolve;  // fires on the first Resolve (player), before the item-bag field read
  g_oneshot = []() {
    g_moved_service = g_service;           // live copy keeps the correct item-bag pointer
    relocate(&g_service, &g_moved_service);
    g_service.klass = &cls_other;          // poison the old storage so a stale read fails loudly
    g_service.itemBag = nullptr;
  };
  assert(api->snapshot(&g_service, tid, 2000, &snap) == PGR_VD_OK);
  assert(fake_gc_get(snap.item_bag_handle) == &g_itemBag);  // read came from the relocated service
  fake_gc_free(snap.pokemon_bag_handle); fake_gc_free(snap.item_bag_handle);
  assert(g_handles.empty());

  // 10. Typeref rooting: inside FindObjectOfType, the map's managed Type is already rooted.
  reset();
  g_oneshot_on = &m_find;
  bool typeRooted = false;
  g_oneshot = [&]() {
    for (const auto &e : g_handles) if (fake_obj_class(e.second) == &cls_type) typeRooted = true;
  };
  assert(api->snapshot(&g_service, tid, 2000, &snap) == PGR_VD_OK && typeRooted);
  fake_gc_free(snap.pokemon_bag_handle); fake_gc_free(snap.item_bag_handle);
  assert(g_handles.empty());

  // 11. Lease expiry discovered after the lookups -> benign SKIP, fresh clock gate, no handles.
  reset();
  g_oneshot_on = &m_find;
  g_oneshot = []() { ::usleep(4000); };  // blow past the 1 ms lease before the counts finish
  assert(api->snapshot(&g_service, tid, 1, &snap) == PGR_VD_SKIP);
  assert(snap.pokemon_bag_handle == 0 && snap.item_bag_handle == 0 && g_handles.empty());

  // 12. Reentrancy: a nested discovery call fired inside a getter is refused busy; outer still succeeds.
  reset();
  int nested = 99;
  g_oneshot_on = &m_find;
  g_oneshot = [&]() {
    PgrVgpDiscoveryEnumeration inner{};
    nested = g_api->enumerate(PGR_VD_KIND_CATCH, this_tid(), 2000, &inner);
  };
  assert(api->snapshot(&g_service, tid, 2000, &snap) == PGR_VD_OK && nested == PGR_VD_ERR_BUSY);
  fake_gc_free(snap.pokemon_bag_handle); fake_gc_free(snap.item_bag_handle);
  assert(g_handles.empty());

  // 13. enumerate: bad kind, catch/spin success (owned array handle + count), owner/lease gate.
  reset();
  assert(api->enumerate(0, tid, 2000, &en) == PGR_VD_ERR_BAD_KIND);
  assert(api->enumerate(4, tid, 2000, &en) == PGR_VD_ERR_BAD_KIND && en.array_handle == 0);
  assert(api->enumerate(PGR_VD_KIND_CATCH, tid + 1, 2000, &en) == PGR_VD_SKIP);
  assert(api->enumerate(PGR_VD_KIND_CATCH, tid, 0, &en) == PGR_VD_SKIP);
  assert(g_handles.empty());
  reset();
  assert(api->enumerate(PGR_VD_KIND_CATCH, tid, 2000, &en) == PGR_VD_OK);
  assert(en.count == 3 && en.array_handle != 0 && en.version == 1 && en.size == sizeof(en));
  assert(fake_gc_get(en.array_handle) == &g_wildArray && g_handles.size() == 1);
  void *catchArray = &g_wildArray;
  fake_gc_free(en.array_handle);
  reset();
  assert(api->enumerate(PGR_VD_KIND_SPIN, tid, 2000, &en) == PGR_VD_OK && en.count == 1);
  assert(fake_gc_get(en.array_handle) == &g_stopArray);
  fake_gc_free(en.array_handle);
  assert(g_handles.empty());

  // count 0 is a valid enumeration; an oversize array (> MAX) is a hard count error.
  reset();
  g_wildArray.length = 0;
  assert(api->enumerate(PGR_VD_KIND_CATCH, tid, 2000, &en) == PGR_VD_OK && en.count == 0 &&
         en.array_handle != 0);
  fake_gc_free(en.array_handle);
  reset();
  g_wildArray.length = PGR_VD_MAX_ARRAY + 1;
  assert(api->enumerate(PGR_VD_KIND_CATCH, tid, 2000, &en) == PGR_VD_ERR_BAD_COUNT &&
         en.array_handle == 0);
  assert(g_handles.empty());
  // wrong array class -> metadata error.
  reset();
  g_wildArray.klass = &cls_other;
  assert(api->enumerate(PGR_VD_KIND_CATCH, tid, 2000, &en) == PGR_VD_ERR_METADATA && en.array_handle == 0);
  assert(g_handles.empty());

  // Covariant arrays are compatible with Object[]; exact array-class equality would reject these.
  for(bool spin:{false,true}){
    reset();FakeArray &array=spin?g_stopArray:g_wildArray;
    array.klass=spin?static_cast<void*>(&cls_stopArray):static_cast<void*>(&cls_wildArray);
    assert(api->enumerate(spin?PGR_VD_KIND_SPIN:PGR_VD_KIND_CATCH,tid,2000,&en)==PGR_VD_OK);
    assert(fake_gc_get(en.array_handle)==&array);fake_gc_free(en.array_handle);
    assert(api->at(&array,0,tid,2000,&handle)==PGR_VD_OK);
    assert(fake_gc_get(handle)==array.data[0]);fake_gc_free(handle);assert(g_handles.empty());
  }

  // 14. at: valid element, null element (skip), out-of-bounds, oversize, wrong class, null args.
  reset();
  assert(api->at(catchArray, 0, tid, 2000, &handle) == PGR_VD_OK && handle != 0);
  assert(fake_gc_get(handle) == &g_wild0 && g_handles.size() == 1);
  fake_gc_free(handle);
  assert(api->at(catchArray, 2, tid, 2000, &handle) == PGR_VD_OK && fake_gc_get(handle) == &g_wild2);
  fake_gc_free(handle);
  assert(g_handles.empty());
  reset();
  g_wildArray.data[1] = nullptr;  // null element at a valid index
  assert(api->at(&g_wildArray, 1, tid, 2000, &handle) == PGR_VD_SKIP && handle == 0 && g_handles.empty());
  reset();
  assert(api->at(&g_wildArray, 3, tid, 2000, &handle) == PGR_VD_ERR_BAD_INDEX && handle == 0);  // == count
  assert(api->at(&g_wildArray, 99, tid, 2000, &handle) == PGR_VD_ERR_BAD_INDEX && handle == 0);
  assert(g_handles.empty());
  reset();
  g_wildArray.length = PGR_VD_MAX_ARRAY + 1;
  assert(api->at(&g_wildArray, 0, tid, 2000, &handle) == PGR_VD_ERR_BAD_COUNT && handle == 0);
  assert(g_handles.empty());
  reset();
  g_wildArray.klass = &cls_other;
  assert(api->at(&g_wildArray, 0, tid, 2000, &handle) == PGR_VD_ERR_METADATA && handle == 0);
  assert(g_handles.empty());
  reset();
  assert(api->at(nullptr, 0, tid, 2000, &handle) == PGR_VD_ERR_NULL_ARG && handle == 0);
  assert(api->at(&g_wildArray, 0, tid, 2000, nullptr) == PGR_VD_ERR_NULL_ARG);
  assert(api->at(&g_wildArray, 0, tid + 1, 2000, &handle) == PGR_VD_SKIP && handle == 0);  // owner gate
  assert(api->at(&g_wildArray, 0, tid, 0, &handle) == PGR_VD_SKIP && handle == 0);         // closed lease
  assert(g_handles.empty());

  // 15. at: relocation inside array_length -> element read uses the re-resolved array base.
  reset();
  g_oneshot_arrlen = []() {
    g_moved_array = g_wildArray;
    relocate(&g_wildArray, &g_moved_array);
    g_wildArray.klass = &cls_other;
    g_wildArray.data[0] = reinterpret_cast<void *>(0xdeadbeef);  // poison the stale element slot
  };
  assert(api->at(&g_wildArray, 0, tid, 2000, &handle) == PGR_VD_OK);
  assert(fake_gc_get(handle) == &g_wild0);  // read from the relocated array, not the poisoned original
  fake_gc_free(handle);
  assert(g_handles.empty());

  // Expiry can occur in the final gchandle_new, after array indexing succeeded. The freshly rooted
  // element must be released rather than transferred using a timestamp captured before that call.
  reset();
  bool rootedElement=false;
  g_gcnew_on=&g_wild0;
  g_oneshot_gcnew=[&](){
    rootedElement=true;
    assert(g_gcnew_calls==2&&g_handles.size()==2);  // array plus the final element root
    ::usleep(4000);
  };
  handle=uintptr_t(0xfeed);
  assert(api->at(&g_wildArray,0,tid,1,&handle)==PGR_VD_SKIP);
  assert(rootedElement&&handle==0&&g_handles.empty());

  puts("PASS VGP discovery: ABI (config v2 272 / snapshot 48 / enumeration 24 / table 40) + offsets, "
       "install/validation/idempotence/reconfig, owner/lease gate, snapshot counts + two owned handles, "
       "map/container/service not-ready skips, service + resolved-class identity, zero/negative/non-Int32 "
       "counts, managed throws, gchandle failures (no partial handles), service relocation + typeref "
       "rooting, lease expiry fresh-clock, reentrancy busy, enumerate kind/count/oversize/class, "
       "at element/null/out-of-bounds/oversize/class/null-arg/gate + array relocation, roots zero "
       "except returned handles; wrong map rejected before container, expiry during final element rooting");
  dlclose(lib);
  return 0;
}
