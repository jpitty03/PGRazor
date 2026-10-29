#include "vgp_aux.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <time.h>
#include <unistd.h>

// Feature 34 VGP consolidation auxiliary. See vgp_aux.h for the ABI and the division of labour with
// runtime/native-vgp-aux.js. Built -fno-exceptions / -fno-rtti like every sibling: the JS "throw" paths
// become fail-closed negative returns, every GC root is freed exactly once on the owner thread, and no
// managed invoke is ever issued more than once per call. This module is STATELESS: it installs NO hook,
// retains NO GC root between calls, keeps NO cache, and NEVER calls back into JS.
//
// Rooting / relocation discipline (same conservatism as vgp_actions.cpp / transfer_execution.cpp): every
// managed reference held across a managed call (runtime_invoke, method_get_object, array_new,
// type_get_object, gc_new) is strongly rooted and re-resolved from its handle afterwards; no raw managed
// pointer is reused across a potential safepoint. A single busy flag refuses a reentrant op (e.g. one
// fired from inside MakeGenericMethod or GetPokemon). The gate is purely per-call: the controller proves
// the owner tid and supplies a residual lease; we clamp it to 2500 ms, turn it into a fresh CLOCK_BOOTTIME
// deadline, and re-check that deadline AFTER the managed work -- so an op whose work outran the foreground
// lease is refused without transferring any handle. No mutex is ever held across a managed call.

static_assert(sizeof(PgrVgpAuxConfig) == 232, "config must be 232 bytes");
static_assert(sizeof(PgrVgpAuxSpin) == 32, "spin result must be 32 bytes");
static_assert(sizeof(PgrVgpAuxTransfer) == 32, "transfer result must be 32 bytes");
static_assert(sizeof(PgrVgpAuxApi) == 32, "table must be 32 bytes");
static_assert(offsetof(PgrVgpAuxConfig, gc_new) == 8, "layout (first ptr)");
static_assert(offsetof(PgrVgpAuxConfig, class_rpc) == 8 + 16 * 8, "layout (first class)");
static_assert(offsetof(PgrVgpAuxConfig, f_rpc) == 8 + 23 * 8, "layout (f_rpc)");
static_assert(offsetof(PgrVgpAuxConfig, m_get_pokemon) == 8 + 27 * 8, "layout (last ptr)");
static_assert(offsetof(PgrVgpAuxSpin, rpc_handle) == 8, "layout");
static_assert(offsetof(PgrVgpAuxSpin, reflection_handle) == 16, "layout");
static_assert(offsetof(PgrVgpAuxSpin, closed_method) == 24, "layout");
static_assert(offsetof(PgrVgpAuxTransfer, caught_id) == 8, "layout");
static_assert(offsetof(PgrVgpAuxTransfer, pokemon_handle) == 16, "layout");
static_assert(offsetof(PgrVgpAuxTransfer, gm_handle) == 24, "layout");

namespace {

constexpr uint64_t kMaxLeaseMs = PGR_VX_MAX_LEASE_MS;

// --- il2cpp runtime ABI typedefs ---------------------------------------------------
using GcNew          = uintptr_t (*)(void *obj, int pinned);
using GcGet          = void *(*)(uintptr_t handle);
using GcFree         = void (*)(uintptr_t handle);
using ObjClass       = void *(*)(void *obj);
using Invoke         = void *(*)(const void *method, void *obj, void **params, void **exc);
using FieldGet       = void (*)(void *obj, void *field, void *out);
using ClassGetMethod = const void *(*)(void *klass, const char *name, int argc);
using MethodBool     = uint8_t (*)(const void *method);
using MethodParamCnt = uint32_t (*)(const void *method);
using MethodGetObj   = void *(*)(const void *method, void *klass);
using ArrayNew       = void *(*)(void *elem_class, uintptr_t length);
using ClassGetType   = void *(*)(void *klass);
using TypeGetObj     = void *(*)(void *type);
using WBarrierSet    = void (*)(void *obj, void **field, void *value);
using MethodFromRefl = const void *(*)(void *reflection);

// --- lifetime / publication --------------------------------------------------------
std::mutex g_install_mutex;
PgrVgpAuxConfig g_cfg{};
std::atomic<bool> g_installed{false};
bool g_install_attempted = false;
int g_install_error = PGR_VX_ERR_NOT_INSTALLED;

// One in-flight op at a time (stateless, but a managed call may re-enter the owner thread).
std::atomic<bool> g_busy{false};

static const char kSendRpc[] = "SendRpc";
static const char kMakeGenericMethod[] = "MakeGenericMethod";

// --- clock / tid -------------------------------------------------------------------
uint64_t current_tid() { return static_cast<uint64_t>(static_cast<pid_t>(gettid())); }

bool read_boottime_ms(uint64_t *out_ms) {
  struct timespec ts {};
  if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0 || ts.tv_sec < 0 || ts.tv_nsec < 0) return false;
  *out_ms = static_cast<uint64_t>(ts.tv_sec) * 1000ULL + static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
  return true;
}

// --- typed accessors onto the immutable published config ---------------------------
GcNew gc_new_fn() { return reinterpret_cast<GcNew>(g_cfg.gc_new); }
GcGet gc_get_fn() { return reinterpret_cast<GcGet>(g_cfg.gc_get); }
GcFree gc_free_fn() { return reinterpret_cast<GcFree>(g_cfg.gc_free); }
ObjClass obj_class_fn() { return reinterpret_cast<ObjClass>(g_cfg.object_get_class); }
FieldGet field_get_fn() { return reinterpret_cast<FieldGet>(g_cfg.field_get_value); }

void *gc_get(uintptr_t h) { return h ? gc_get_fn()(h) : nullptr; }

uintptr_t hold(void *obj) {
  if (obj == nullptr) return 0;
  return gc_new_fn()(obj, 0);
}
void drop(uintptr_t *h) {
  if (*h == 0) return;
  uintptr_t v = *h;
  *h = 0;  // detach before the (possibly reentrant) free
  gc_free_fn()(v);
}

bool is_exact(void *obj, void *klass) { return obj != nullptr && obj_class_fn()(obj) == klass; }

void *field_ptr(void *obj, void *field) {
  void *out = nullptr;
  if (obj != nullptr) field_get_fn()(obj, field, &out);
  return out;
}
uint64_t field_u64(void *obj, void *field) {
  uint64_t v = 0;
  if (obj != nullptr) field_get_fn()(obj, field, &v);
  return v;
}

void *managed_invoke(const void *method, void *obj, void **params, void **exc) {
  *exc = nullptr;
  return reinterpret_cast<Invoke>(g_cfg.runtime_invoke)(method, obj, params, exc);
}

// --- reflection helpers ------------------------------------------------------------
const void *class_method(void *klass, const char *name, int argc) {
  return reinterpret_cast<ClassGetMethod>(g_cfg.class_get_method_from_name)(klass, name, argc);
}
bool method_is_generic(const void *m) {
  return m != nullptr && reinterpret_cast<MethodBool>(g_cfg.method_is_generic)(m) != 0;
}
bool method_is_inflated(const void *m) {
  return m != nullptr && reinterpret_cast<MethodBool>(g_cfg.method_is_inflated)(m) != 0;
}
uint32_t method_param_count(const void *m) {
  return reinterpret_cast<MethodParamCnt>(g_cfg.method_get_param_count)(m);
}
void *reflect_method(const void *m, void *klass) {
  return reinterpret_cast<MethodGetObj>(g_cfg.method_get_object)(m, klass);
}
void *array_new(void *elem_class, uintptr_t len) {
  return reinterpret_cast<ArrayNew>(g_cfg.array_new)(elem_class, len);
}
void *type_object(void *klass) {
  void *type = reinterpret_cast<ClassGetType>(g_cfg.class_get_type)(klass);
  if (type == nullptr) return nullptr;
  return reinterpret_cast<TypeGetObj>(g_cfg.type_get_object)(type);
}
void wbarrier(void *array_obj, uint32_t element, void *value) {
  void **slot = reinterpret_cast<void **>(static_cast<char *>(array_obj) +
                                          PGR_VX_ARRAY_DATA_OFF + element * sizeof(void *));
  reinterpret_cast<WBarrierSet>(g_cfg.gc_wbarrier_set_field)(array_obj, slot, value);
}
const void *unreflect(void *reflection) {
  return reinterpret_cast<MethodFromRefl>(g_cfg.method_get_from_reflection)(reflection);
}

// --- spin: resolve the closed SendRpc off the item bag -----------------------------
int do_spin(void *item_bag, uint64_t deadline, PgrVgpAuxSpin *out) {
  uintptr_t bag_root = 0, rpc_root = 0, refl_root = 0, types_root = 0, closed_root = 0;
  const void *closed_method = nullptr;
  int rc = PGR_VX_OK;

  bag_root = hold(item_bag);
  if (bag_root == 0) { rc = PGR_VX_ERR_GCHANDLE; goto done; }
  if (!is_exact(gc_get(bag_root), g_cfg.class_item_bag)) { rc = PGR_VX_ERR_IDENTITY; goto done; }

  {
    // Read the rpc service off the item bag's validated cynr field; root it, require the exact rpc class.
    void *rpc = field_ptr(gc_get(bag_root), g_cfg.f_rpc);
    if (!is_exact(rpc, g_cfg.class_rpc)) { rc = PGR_VX_ERR_IDENTITY; goto done; }
    rpc_root = hold(rpc);
    if (rpc_root == 0) { rc = PGR_VX_ERR_GCHANDLE; goto done; }
  }

  {
    // Resolve the open generic SendRpc on the rpc class and reflect it. Validate generic + arity 5.
    const void *generic = class_method(g_cfg.class_rpc, kSendRpc, PGR_VX_SENDRPC_ARITY);
    if (!method_is_generic(generic) || method_param_count(generic) != PGR_VX_SENDRPC_ARITY) {
      rc = PGR_VX_ERR_REFLECT;
      goto done;
    }
    void *reflection = reflect_method(generic, g_cfg.class_rpc);
    if (reflection == nullptr) { rc = PGR_VX_ERR_REFLECT; goto done; }
    refl_root = hold(reflection);
    if (refl_root == 0) { rc = PGR_VX_ERR_GCHANDLE; goto done; }
  }

  {
    // Build the 2-element System.Type[] {request, spin-result}. Root the array, then write each Type via
    // the write barrier, re-resolving the array from its handle after each (allocating) type_object().
    void *types = array_new(g_cfg.class_type, PGR_VX_TYPE_ARGS);
    types_root = hold(types);
    if (types_root == 0) { rc = PGR_VX_ERR_GCHANDLE; goto done; }

    void *const generic_args[PGR_VX_TYPE_ARGS] = {g_cfg.class_request, g_cfg.class_spin};
    for (uint32_t i = 0; i < PGR_VX_TYPE_ARGS; ++i) {
      void *type_obj = type_object(generic_args[i]);
      uintptr_t type_root = hold(type_obj);  // root before the next managed call can move it
      if (type_root == 0) { rc = PGR_VX_ERR_REFLECT; goto done; }
      wbarrier(gc_get(types_root), i, gc_get(type_root));
      drop(&type_root);
    }
  }

  {
    // Invoke MakeGenericMethod(Type[]) on the reflected method; unreflect to the inflated MethodInfo*.
    const void *make = class_method(obj_class_fn()(gc_get(refl_root)), kMakeGenericMethod, 1);
    if (make == nullptr) { rc = PGR_VX_ERR_REFLECT; goto done; }
    void *params[1] = {gc_get(types_root)};  // reference-type array argument passed directly
    void *exc = nullptr;
    void *closed = managed_invoke(make, gc_get(refl_root), params, &exc);
    if (exc != nullptr) { rc = PGR_VX_ERR_MANAGED; goto done; }
    if (closed == nullptr) { rc = PGR_VX_ERR_REFLECT; goto done; }
    closed_root = hold(closed);
    if (closed_root == 0) { rc = PGR_VX_ERR_GCHANDLE; goto done; }

    closed_method = unreflect(gc_get(closed_root));
    if (!method_is_inflated(closed_method) || method_param_count(closed_method) != PGR_VX_SENDRPC_ARITY) {
      rc = PGR_VX_ERR_REFLECT;
      goto done;
    }
  }

  {
    // Fresh deadline: refuse (retry later) if the foreground lease was spent during the managed work.
    uint64_t end = 0;
    if (!read_boottime_ms(&end)) { rc = PGR_VX_ERR_CLOCK_FAILURE; goto done; }
    if (end >= deadline) { rc = PGR_VX_OK; goto done; }

    // Transfer OWN rpc + closed reflection to the caller; closed_method lives with reflection_handle.
    out->version = PGR_VX_OUT_VERSION;
    out->size = sizeof(PgrVgpAuxSpin);
    out->rpc_handle = static_cast<uint64_t>(rpc_root);
    out->reflection_handle = static_cast<uint64_t>(closed_root);
    out->closed_method = const_cast<void *>(closed_method);
    rpc_root = 0;
    closed_root = 0;
    rc = PGR_VX_SUCCESS;
  }

done:
  drop(&bag_root);
  drop(&rpc_root);
  drop(&refl_root);
  drop(&types_root);
  drop(&closed_root);
  return rc;
}

// --- transfer: look up the caught pokemon + bag GameMaster -------------------------
int do_transfer(void *response, void *pokemon_bag, uint64_t deadline, PgrVgpAuxTransfer *out) {
  uintptr_t resp_root = 0, bag_root = 0, pk_root = 0, gm_root = 0;
  uint64_t id = 0;
  int rc = PGR_VX_OK;

  resp_root = hold(response);
  if (resp_root == 0) { rc = PGR_VX_ERR_GCHANDLE; goto done; }
  bag_root = hold(pokemon_bag);
  if (bag_root == 0) { rc = PGR_VX_ERR_GCHANDLE; goto done; }

  if (!is_exact(gc_get(resp_root), g_cfg.class_response)) { rc = PGR_VX_ERR_IDENTITY; goto done; }
  id = field_u64(gc_get(resp_root), g_cfg.f_caught_id);
  if (id == 0) { rc = PGR_VX_SKIP; goto done; }  // missing captured id -> drop

  {
    // bag.GetPokemon(id). A null result means the inventory has not caught up yet (not ready: retry).
    uint64_t id_arg = id;
    void *params[1] = {&id_arg};
    void *exc = nullptr;
    void *pokemon = managed_invoke(g_cfg.m_get_pokemon, gc_get(bag_root), params, &exc);
    if (exc != nullptr) { rc = PGR_VX_ERR_MANAGED; goto done; }
    if (pokemon == nullptr) { rc = PGR_VX_OK; goto done; }  // inventory not ready
    pk_root = hold(pokemon);
    if (pk_root == 0) { rc = PGR_VX_ERR_GCHANDLE; goto done; }
  }

  // Prove the resolved pokemon is the exact Pokemon class AND carries the captured id; else skip.
  if (!is_exact(gc_get(pk_root), g_cfg.class_pokemon) ||
      field_u64(gc_get(pk_root), g_cfg.f_pokemon_id) != id) {
    rc = PGR_VX_SKIP;
    goto done;
  }

  {
    // Read + root the bag's GameMaster handle; it must be present for the caught transfer check.
    void *gm = field_ptr(gc_get(bag_root), g_cfg.f_bag_gm);
    if (gm == nullptr) { rc = PGR_VX_ERR_IDENTITY; goto done; }
    gm_root = hold(gm);
    if (gm_root == 0) { rc = PGR_VX_ERR_GCHANDLE; goto done; }
  }

  {
    uint64_t end = 0;
    if (!read_boottime_ms(&end)) { rc = PGR_VX_ERR_CLOCK_FAILURE; goto done; }
    if (end >= deadline) { rc = PGR_VX_OK; goto done; }

    out->version = PGR_VX_OUT_VERSION;
    out->size = sizeof(PgrVgpAuxTransfer);
    out->caught_id = id;
    out->pokemon_handle = static_cast<uint64_t>(pk_root);
    out->gm_handle = static_cast<uint64_t>(gm_root);
    pk_root = 0;
    gm_root = 0;
    rc = PGR_VX_SUCCESS;
  }

done:
  drop(&resp_root);
  drop(&bag_root);
  drop(&pk_root);
  drop(&gm_root);
  return rc;
}

// --- gate wrapper ------------------------------------------------------------------
// Returns true (and sets *deadline) when the op may proceed. Returns false with *rc already holding the
// refusal/error code otherwise. A reentrant op (busy) is refused with PGR_VX_OK.
bool enter_op(uint64_t owner_tid, uint64_t lease_ms, uint64_t *deadline, int *rc) {
  if (owner_tid == 0 || current_tid() != owner_tid) { *rc = PGR_VX_OK; return false; }
  if (lease_ms == 0) { *rc = PGR_VX_OK; return false; }
  uint64_t lease = lease_ms > kMaxLeaseMs ? kMaxLeaseMs : lease_ms;
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) { *rc = PGR_VX_ERR_CLOCK_FAILURE; return false; }
  if (g_busy.exchange(true)) { *rc = PGR_VX_OK; return false; }  // reentrant -> refused
  *deadline = now + lease;
  return true;
}

// --- public API --------------------------------------------------------------------
bool validate_config(const PgrVgpAuxConfig *c, int *rc) {
  if (c == nullptr) { *rc = PGR_VX_ERR_NULL_ARG; return false; }
  if (c->version != PGR_VX_CONFIG_VERSION) { *rc = PGR_VX_ERR_BAD_VERSION; return false; }
  if (c->size != sizeof(PgrVgpAuxConfig)) { *rc = PGR_VX_ERR_BAD_SIZE; return false; }
  void *const *ptrs = reinterpret_cast<void *const *>(&c->gc_new);
  for (int i = 0; i < 28; ++i) {
    if (ptrs[i] == nullptr) { *rc = PGR_VX_ERR_MISSING_PTR; return false; }
  }
  *rc = PGR_VX_OK;
  return true;
}

int api_install(const PgrVgpAuxConfig *config) {
  int rc = PGR_VX_OK;
  if (!validate_config(config, &rc)) return rc;
  uint64_t probe = 0;
  if (!read_boottime_ms(&probe)) return PGR_VX_ERR_CLOCK_FAILURE;

  std::lock_guard<std::mutex> install_lock(g_install_mutex);
  if (g_installed.load(std::memory_order_acquire)) {
    return std::memcmp(&g_cfg, config, sizeof(*config)) == 0 ? PGR_VX_OK : PGR_VX_ERR_RECONFIG;
  }
  if (g_install_attempted) return g_install_error;
  g_install_attempted = true;
  g_cfg = *config;
  g_busy.store(false);
  g_installed.store(true, std::memory_order_release);
  return g_install_error = PGR_VX_OK;
}

int api_spin(void *item_bag, uint64_t owner_tid, uint64_t lease_ms, PgrVgpAuxSpin *out) {
  if (out != nullptr) std::memset(out, 0, sizeof(*out));  // always initialize the output
  if (!g_installed.load(std::memory_order_acquire)) return PGR_VX_ERR_NOT_INSTALLED;
  if (item_bag == nullptr || out == nullptr) return PGR_VX_ERR_NULL_ARG;

  uint64_t deadline = 0;
  int rc = PGR_VX_OK;
  if (!enter_op(owner_tid, lease_ms, &deadline, &rc)) return rc;
  rc = do_spin(item_bag, deadline, out);
  g_busy.store(false);
  return rc;
}

int api_transfer(void *response, void *pokemon_bag, uint64_t owner_tid, uint64_t lease_ms,
                 PgrVgpAuxTransfer *out) {
  if (out != nullptr) std::memset(out, 0, sizeof(*out));  // always initialize the output
  if (!g_installed.load(std::memory_order_acquire)) return PGR_VX_ERR_NOT_INSTALLED;
  if (response == nullptr || pokemon_bag == nullptr || out == nullptr) return PGR_VX_ERR_NULL_ARG;

  uint64_t deadline = 0;
  int rc = PGR_VX_OK;
  if (!enter_op(owner_tid, lease_ms, &deadline, &rc)) return rc;
  rc = do_transfer(response, pokemon_bag, deadline, out);
  g_busy.store(false);
  return rc;
}

const PgrVgpAuxApi kApiTable = {
    PGR_VX_API_VERSION, static_cast<uint32_t>(sizeof(PgrVgpAuxApi)),
    &api_install, &api_spin, &api_transfer,
};

}  // namespace

extern "C" const void *pgrazor_vgp_aux_api(uint32_t version) {
  if (version != PGR_VX_API_VERSION) return nullptr;
  return &kApiTable;
}
