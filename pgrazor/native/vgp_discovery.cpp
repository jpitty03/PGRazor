#include "vgp_discovery.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <time.h>
#include <unistd.h>

// Feature 31: synchronous, read-only VGP inventory/enumeration discovery. No hooks, no callbacks, no
// retained managed state beyond the handles explicitly transferred to the caller. Every temporary managed
// value is rooted on the owner thread and freed before return. Built with -fno-exceptions / -fno-rtti
// (see CMakeLists.txt): managed exceptions surface through runtime_invoke's exc out-param, never as C++
// throws.

namespace {

PgrVgpDiscoveryConfig cfg{};
std::mutex installMutex;
std::atomic<bool> installed{false};
std::atomic<bool> busy{false};

using New      = uintptr_t (*)(void *, int);
using Get      = void *(*)(uintptr_t);
using FreeH    = void (*)(uintptr_t);
using Class    = void *(*)(void *);
using Invoke   = void *(*)(void *, void *, void **, void **);
using Unbox    = void *(*)(void *);
using FieldGet = void (*)(void *, void *, void *);
using ClassType = void *(*)(void *);
using TypeObject = void *(*)(void *);
using ArrayLen = size_t (*)(void *);

uint64_t now() {
  timespec t{};
  if (clock_gettime(CLOCK_BOOTTIME, &t) || t.tv_sec < 0) return 0;
  return uint64_t(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
}

// Owning temporary root: allocates a GC handle for a managed object and frees it on destruction.
struct Root {
  uintptr_t h = 0;
  Root() = default;
  Root(const Root &) = delete;
  Root &operator=(const Root &) = delete;
  ~Root() { if (h) reinterpret_cast<FreeH>(cfg.gchandle_free)(h); }
  bool set(void *o) { if (!o) return false; h = reinterpret_cast<New>(cfg.gchandle_new)(o, 0); return h != 0; }
  void *get() const { return h ? reinterpret_cast<Get>(cfg.gchandle_get_target)(h) : nullptr; }
  // Transfer ownership of the handle to the caller (used for the returned bag/array/element handles).
  uintptr_t release() { uintptr_t out = h; h = 0; return out; }
};

bool exact(void *o, void *klass) {
  return o && reinterpret_cast<Class>(cfg.object_get_class)(o) == klass;
}

// Lookup methods promise an assignable result, not an exact concrete class.
bool compatible(void *o, void *expected) {
  if (!o) return false;
  void *actual = reinterpret_cast<Class>(cfg.object_get_class)(o);
  return actual && reinterpret_cast<bool (*)(void *, void *)>(cfg.class_is_assignable_from)(expected, actual);
}

// Build a managed System.Type for `klass` and root it. type_get_object is a pure metadata export (no GC
// allocation of the kind that relocates), but its result IS a managed object that must be rooted before
// any further managed call. Returns false (err set) on a root failure.
bool typeObject(void *klass, Root &out, int &err) {
  void *type = reinterpret_cast<ClassType>(cfg.class_get_type)(klass);
  if (!type) { err = PGR_VD_ERR_METADATA; return false; }
  void *obj = reinterpret_cast<TypeObject>(cfg.type_get_object)(type);
  if (!obj) { err = PGR_VD_ERR_METADATA; return false; }
  if (!out.set(obj)) { err = PGR_VD_ERR_GCHANDLE; return false; }
  return true;
}

// Invoke a method and root its (possibly null) return value. `receiver` null => static call; otherwise it
// is re-resolved from its root (relocating collector). err distinguishes a managed throw from a root
// failure. `allowNull` lets a legitimately-null managed result (not-ready / null element) through without
// rooting; the caller treats !out.get() as the null case.
enum CallResult { CALL_OK, CALL_THROW, CALL_GCHANDLE, CALL_DEAD };

CallResult call(void *method, const Root *receiver, void **args, Root &out, bool allowNull) {
  void *self = nullptr;
  if (receiver) {
    self = receiver->get();
    if (!self) return CALL_DEAD;  // receiver collected despite its root (never expected)
  }
  void *exc = nullptr;
  void *value = reinterpret_cast<Invoke>(cfg.runtime_invoke)(method, self, args, &exc);
  if (exc) return CALL_THROW;
  if (!value) return allowNull ? CALL_OK : CALL_GCHANDLE;
  return out.set(value) ? CALL_OK : CALL_GCHANDLE;
}

// Invoke an Int32-returning count method on `receiver` and read the unboxed value. Returns PGR_VD_OK and
// sets *value, or a negative error: managed throw, root failure, wrong box class, or (fail closed) a
// negative count.
int countValue(void *method, const Root &receiver, void **args, int32_t *value) {
  Root box;
  switch (call(method, &receiver, args, box, /*allowNull=*/false)) {
    case CALL_THROW:    return PGR_VD_ERR_MANAGED;
    case CALL_GCHANDLE: return PGR_VD_ERR_GCHANDLE;
    case CALL_DEAD:     return PGR_VD_ERR_GCHANDLE;
    case CALL_OK:       break;
  }
  void *obj = box.get();
  if (!exact(obj, cfg.class_int32)) return PGR_VD_ERR_METADATA;
  void *p = reinterpret_cast<Unbox>(cfg.object_unbox)(obj);
  if (!p) return PGR_VD_ERR_METADATA;
  int32_t v;
  std::memcpy(&v, p, sizeof(int32_t));
  if (v < 0) return PGR_VD_ERR_NEG_COUNT;  // valid counts are non-negative; negatives fail closed
  *value = v;
  return PGR_VD_OK;
}

// Shared owner/lease/clock preamble. Returns PGR_VD_OK and fills *start/*deadline, or a non-OK code the
// caller returns verbatim (PGR_VD_SKIP for a closed lease / owner mismatch, negative for a clock failure).
int gate(uint64_t owner, uint64_t lease_ms, uint64_t *start, uint64_t *deadline) {
  if (!installed.load(std::memory_order_acquire)) return PGR_VD_ERR_NOT_INSTALLED;
  if (!owner || owner != static_cast<uint64_t>(gettid())) return PGR_VD_SKIP;  // not the proven owner tid
  const uint64_t s = now();
  if (!s) return PGR_VD_ERR_CLOCK_FAILURE;
  const uint64_t lease = lease_ms > PGR_VD_MAX_LEASE_MS ? PGR_VD_MAX_LEASE_MS : lease_ms;
  if (!lease) return PGR_VD_SKIP;  // closed window: benign skip
  *start = s;
  *deadline = s + lease;
  return PGR_VD_OK;
}

// Scoped reentrancy guard across all three entry points.
struct BusyGuard {
  bool held = false;
  BusyGuard() { bool idle = false; held = busy.compare_exchange_strong(idle, true); }
  ~BusyGuard() { if (held) busy.store(false); }
};

int install(const PgrVgpDiscoveryConfig *c) {
  if (!c) return PGR_VD_ERR_NULL_ARG;
  if (c->version != PGR_VD_CONFIG_VERSION) return PGR_VD_ERR_BAD_VERSION;
  if (c->size != sizeof(*c)) return PGR_VD_ERR_BAD_SIZE;
  void *pointers[33];
  std::memcpy(pointers, reinterpret_cast<const char *>(c) + 8, sizeof(pointers));
  for (void *p : pointers) if (!p) return PGR_VD_ERR_MISSING_PTR;
  std::lock_guard<std::mutex> guard(installMutex);
  if (installed.load()) return std::memcmp(c, &cfg, sizeof(cfg)) == 0 ? 0 : PGR_VD_ERR_RECONFIG;
  cfg = *c;
  installed.store(true, std::memory_order_release);
  return 0;
}

int snapshot(void *svc, uint64_t owner, uint64_t lease_ms, PgrVgpDiscoverySnapshot *out) {
  if (!out) return PGR_VD_ERR_NULL_ARG;
  *out = {PGR_VD_SNAPSHOT_VERSION, uint32_t(sizeof(*out)), 0, 0, 0, 0, 0, 0, 0, 0};
  if (!svc) return PGR_VD_ERR_NULL_ARG;

  uint64_t start, deadline;
  int g = gate(owner, lease_ms, &start, &deadline);
  if (g != PGR_VD_OK) return g;

  BusyGuard bg;
  if (!bg.held) return PGR_VD_ERR_BUSY;

  int err = 0;

  // Root the service and confirm its exact class before touching its field (identity check).
  out->reserved = 1;
  Root service;
  if (!service.set(svc)) return PGR_VD_ERR_GCHANDLE;
  if (!exact(service.get(), cfg.class_item_service)) return PGR_VD_ERR_METADATA;

  // FindObjectOfType(typeof MapSceneContext) -> map context (static call).
  out->reserved = 2;
  Root mapType;
  if (!typeObject(cfg.class_map, mapType, err)) return err;
  Root mapObj;
  {
    void *self = mapType.get();
    if (!self) return PGR_VD_ERR_GCHANDLE;
    void *args[] = {self};
    switch (call(cfg.m_find, nullptr, args, mapObj, /*allowNull=*/true)) {
      case CALL_THROW:    return PGR_VD_ERR_MANAGED;
      case CALL_GCHANDLE: return PGR_VD_ERR_GCHANDLE;
      case CALL_DEAD:     return PGR_VD_ERR_GCHANDLE;
      case CALL_OK:       break;
    }
    if (!mapObj.get()) return PGR_VD_SKIP;  // no map yet: not ready
    if (!compatible(mapObj.get(), cfg.class_map)) return PGR_VD_ERR_METADATA;
  }

  // get_Container() -> DiContainer.
  out->reserved = 3;
  Root container;
  switch (call(cfg.m_container, &mapObj, nullptr, container, /*allowNull=*/true)) {
    case CALL_THROW:    return PGR_VD_ERR_MANAGED;
    case CALL_GCHANDLE: return PGR_VD_ERR_GCHANDLE;
    case CALL_DEAD:     return PGR_VD_ERR_GCHANDLE;
    case CALL_OK:       break;
  }
  if (!container.get()) return PGR_VD_SKIP;  // no container yet: not ready
  if (!compatible(container.get(), cfg.class_container)) return PGR_VD_ERR_METADATA;

  // Resolve(playerContract) -> player, Resolve(bagContract) -> pokemon bag.
  Root playerType, bagType;
  if (!typeObject(cfg.class_player_contract, playerType, err)) return err;
  if (!typeObject(cfg.class_bag_contract, bagType, err)) return err;

  out->reserved = 4;
  Root player;
  {
    void *t = playerType.get();
    if (!t) return PGR_VD_ERR_GCHANDLE;
    void *args[] = {t};
    switch (call(cfg.m_resolve, &container, args, player, /*allowNull=*/true)) {
      case CALL_THROW:    return PGR_VD_ERR_MANAGED;
      case CALL_GCHANDLE: return PGR_VD_ERR_GCHANDLE;
      case CALL_DEAD:     return PGR_VD_ERR_GCHANDLE;
      case CALL_OK:       break;
    }
    if (!player.get()) return PGR_VD_SKIP;  // service not ready
    if (!exact(player.get(), cfg.class_player)) return PGR_VD_ERR_METADATA;
  }

  out->reserved = 5;
  Root pokemonBag;
  {
    void *t = bagType.get();
    if (!t) return PGR_VD_ERR_GCHANDLE;
    void *args[] = {t};
    switch (call(cfg.m_resolve, &container, args, pokemonBag, /*allowNull=*/true)) {
      case CALL_THROW:    return PGR_VD_ERR_MANAGED;
      case CALL_GCHANDLE: return PGR_VD_ERR_GCHANDLE;
      case CALL_DEAD:     return PGR_VD_ERR_GCHANDLE;
      case CALL_OK:       break;
    }
    if (!pokemonBag.get()) return PGR_VD_SKIP;  // service not ready
    if (!exact(pokemonBag.get(), cfg.class_pokemon_bag)) return PGR_VD_ERR_METADATA;
  }

  // Item bag from the service field (reference read; root immediately before any further allocation).
  out->reserved = 6;
  Root itemBag;
  {
    void *self = service.get();
    if (!self) return PGR_VD_ERR_GCHANDLE;
    void *rawBag = nullptr;
    reinterpret_cast<FieldGet>(cfg.field_get_value)(self, cfg.f_item_bag, &rawBag);
    if (!rawBag) return PGR_VD_SKIP;  // service not ready
    if (!itemBag.set(rawBag)) return PGR_VD_ERR_GCHANDLE;
    if (!exact(itemBag.get(), cfg.class_item_bag)) return PGR_VD_ERR_METADATA;
  }

  // Five Int32 counts via non-virtual getters (legacy itemInt semantics: invoke, exact Int32, unbox).
  int32_t balls = 0, stored = 0, capacity = 0, totalItems = 0, maxItems = 0, rc;
  int32_t itemId = 1;  // GetItemCount(item id 1): red Poke Ball
  void *itemArgs[] = {&itemId};
  out->reserved = 7;
  if ((rc = countValue(cfg.m_item_count, itemBag, itemArgs, &balls)) != PGR_VD_OK) return rc;
  out->reserved = 8;
  if ((rc = countValue(cfg.m_pokemon_count, pokemonBag, nullptr, &stored)) != PGR_VD_OK) return rc;
  out->reserved = 9;
  if ((rc = countValue(cfg.m_max_pokemon, player, nullptr, &capacity)) != PGR_VD_OK) return rc;
  out->reserved = 10;
  if ((rc = countValue(cfg.m_total_items, itemBag, nullptr, &totalItems)) != PGR_VD_OK) return rc;
  out->reserved = 11;
  if ((rc = countValue(cfg.m_max_items, player, nullptr, &maxItems)) != PGR_VD_OK) return rc;

  // Fresh clock AFTER the lookups: only commit a positive result if the lease is still open.
  const uint64_t end = now();
  if (!end || end < start || end >= deadline) return PGR_VD_SKIP;

  // Commit: transfer exactly the two owned handles (no extra allocation; release the live roots). The
  // other temporary roots free on scope exit, leaving precisely two retained handles.
  out->reserved = 0;
  out->balls = balls;
  out->stored = stored;
  out->capacity = capacity;
  out->total_items = totalItems;
  out->max_items = maxItems;
  out->pokemon_bag_handle = pokemonBag.release();
  out->item_bag_handle = itemBag.release();
  return PGR_VD_OK;
}

int enumerate(uint32_t kind, uint64_t owner, uint64_t lease_ms, PgrVgpDiscoveryEnumeration *out) {
  if (!out) return PGR_VD_ERR_NULL_ARG;
  *out = {PGR_VD_ENUM_VERSION, uint32_t(sizeof(*out)), 0, 0, 0};
  if (kind != PGR_VD_KIND_CATCH && kind != PGR_VD_KIND_SPIN) return PGR_VD_ERR_BAD_KIND;

  uint64_t start, deadline;
  int g = gate(owner, lease_ms, &start, &deadline);
  if (g != PGR_VD_OK) return g;

  BusyGuard bg;
  if (!bg.held) return PGR_VD_ERR_BUSY;

  int err = 0;
  void *klass = (kind == PGR_VD_KIND_CATCH) ? cfg.class_wild : cfg.class_stop;

  Root elementType;
  if (!typeObject(klass, elementType, err)) return err;

  Root arrayObj;
  {
    void *t = elementType.get();
    if (!t) return PGR_VD_ERR_GCHANDLE;
    void *args[] = {t};
    switch (call(cfg.m_find_all, nullptr, args, arrayObj, /*allowNull=*/true)) {
      case CALL_THROW:    return PGR_VD_ERR_MANAGED;
      case CALL_GCHANDLE: return PGR_VD_ERR_GCHANDLE;
      case CALL_DEAD:     return PGR_VD_ERR_GCHANDLE;
      case CALL_OK:       break;
    }
    if (!arrayObj.get()) return PGR_VD_SKIP;  // no array: treat as not ready
  }
  if (!compatible(arrayObj.get(), cfg.class_object_array)) return PGR_VD_ERR_METADATA;

  size_t count = reinterpret_cast<ArrayLen>(cfg.array_length)(arrayObj.get());
  if (count > PGR_VD_MAX_ARRAY) return PGR_VD_ERR_BAD_COUNT;  // strict ceiling (count 0 is valid)

  const uint64_t end = now();
  if (!end || end < start || end >= deadline) return PGR_VD_SKIP;  // fresh clock before output

  out->count = uint32_t(count);
  out->array_handle = arrayObj.release();
  return PGR_VD_OK;
}

int at(void *array, uint32_t index, uint64_t owner, uint64_t lease_ms, uintptr_t *out_handle) {
  if (!out_handle) return PGR_VD_ERR_NULL_ARG;
  *out_handle = 0;
  if (!array) return PGR_VD_ERR_NULL_ARG;

  uint64_t start, deadline;
  int g = gate(owner, lease_ms, &start, &deadline);
  if (g != PGR_VD_OK) return g;

  BusyGuard bg;
  if (!bg.held) return PGR_VD_ERR_BUSY;

  // Root the array and revalidate its class and element count before indexing.
  Root arrayObj;
  if (!arrayObj.set(array)) return PGR_VD_ERR_GCHANDLE;
  if (!compatible(arrayObj.get(), cfg.class_object_array)) return PGR_VD_ERR_METADATA;

  size_t count = reinterpret_cast<ArrayLen>(cfg.array_length)(arrayObj.get());
  if (count > PGR_VD_MAX_ARRAY) return PGR_VD_ERR_BAD_COUNT;
  if (index >= count) return PGR_VD_ERR_BAD_INDEX;

  // Raw LP64 element read at data offset 32 (element[i] @ base + 32 + i*8). Re-fetch the (possibly
  // relocated) array base from its root right before the read.
  void *base = arrayObj.get();
  if (!base) return PGR_VD_ERR_GCHANDLE;
  void *element = nullptr;
  std::memcpy(&element,
              static_cast<char *>(base) + PGR_VD_ARRAY_DATA_OFF + size_t(index) * sizeof(void *),
              sizeof(void *));
  if (!element) return PGR_VD_SKIP;  // null element at index: host skips it

  Root result;
  if (!result.set(element)) return PGR_VD_ERR_GCHANDLE;
  const uint64_t end = now();
  if (!end || end < start || end >= deadline) return PGR_VD_SKIP;
  *out_handle = result.release();
  return PGR_VD_OK;
}

const PgrVgpDiscoveryApi api = {
    PGR_VD_API_VERSION, uint32_t(sizeof(PgrVgpDiscoveryApi)), install, snapshot, enumerate, at};

}  // namespace

extern "C" const void *pgrazor_vgp_discovery_api(uint32_t version) {
  return version == PGR_VD_API_VERSION ? &api : nullptr;
}
