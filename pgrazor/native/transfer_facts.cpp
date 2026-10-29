#include "transfer_facts.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <time.h>
#include <unistd.h>

// Feature 35 transfer facts reader. See transfer_facts.h for the ABI and the division of labour with
// runtime/native-transfer-facts.js. Built -fno-exceptions / -fno-rtti like every sibling: the JS "throw"
// paths become fail-closed negative returns (which the caller treats as KEEP), every GC root is freed
// exactly once on the owner thread, and no managed invoke is ever issued more than once per call. This
// module is STATELESS: NO hook, NO retained GC root between calls, NO cache, NO callback into JS, and it
// NEVER issues a release action.
//
// Rooting / relocation discipline (same conservatism as vgp_aux.cpp): every managed reference held across
// a managed call (runtime_invoke) is strongly rooted and re-resolved from its handle afterwards; the
// inspected pokemon/display are re-resolved from their roots for every field read and every invoke self,
// so no raw managed pointer is reused across a potential safepoint. Each boxed scalar result is rooted,
// unboxed, read and dropped before the next managed call. A busy flag refuses a reentrant call. The gate
// is purely per-call: the controller proves the owner tid and supplies a residual lease; we clamp it to
// 2500 ms, turn it into a fresh CLOCK_BOOTTIME deadline, and re-check that deadline AFTER the managed work
// (and re-prove the id) before writing any facts. No mutex is held across a managed call.

static_assert(sizeof(PgrTransferFactsConfig) == 320, "config must be 320 bytes");
static_assert(sizeof(PgrTransferFactsRequest) == 56, "request must be 56 bytes");
static_assert(sizeof(PgrTransferFactsOut) == 40, "out must be 40 bytes");
static_assert(sizeof(PgrTransferFactsApi) == 24, "table must be 24 bytes");
static_assert(offsetof(PgrTransferFactsConfig, gc_new) == 8, "layout (first ptr)");
static_assert(offsetof(PgrTransferFactsConfig, class_pokemon) == 8 + 8 * 8, "layout (first class)");
static_assert(offsetof(PgrTransferFactsConfig, m_size) == 8 + 15 * 8, "layout (first method)");
static_assert(offsetof(PgrTransferFactsConfig, f_id) == 8 + 19 * 8, "layout (first field)");
static_assert(offsetof(PgrTransferFactsConfig, f_art_asset) == 8 + 33 * 8, "layout (last ptr)");
static_assert(offsetof(PgrTransferFactsConfig, xxs) == 280, "layout (xxs)");
static_assert(offsetof(PgrTransferFactsConfig, sizes) == 300, "layout (sizes)");
static_assert(offsetof(PgrTransferFactsRequest, pokemon) == 32, "layout (pokemon)");
static_assert(offsetof(PgrTransferFactsRequest, gm) == 40, "layout (gm)");
static_assert(offsetof(PgrTransferFactsRequest, protect) == 48, "layout (protect)");
static_assert(offsetof(PgrTransferFactsOut, caught_id) == 32, "layout (caught_id)");

namespace {

constexpr uint64_t kMaxLeaseMs = PGR_TF_MAX_LEASE_MS;

// --- il2cpp runtime ABI typedefs ---------------------------------------------------
using GcNew     = uintptr_t (*)(void *obj, int pinned);
using GcGet     = void *(*)(uintptr_t handle);
using GcFree    = void (*)(uintptr_t handle);
using ObjClass  = void *(*)(void *obj);
using Invoke    = void *(*)(const void *method, void *obj, void **params, void **exc);
using Unbox     = void *(*)(void *boxed);
using FieldGet  = void (*)(void *obj, void *field, void *out);
using StrLength = int32_t (*)(void *string);

// --- lifetime / publication --------------------------------------------------------
std::mutex g_install_mutex;
PgrTransferFactsConfig g_cfg{};
std::atomic<bool> g_installed{false};
bool g_install_attempted = false;
int g_install_error = PGR_TF_ERR_NOT_INSTALLED;

// One in-flight inspection at a time (stateless, but a managed call may re-enter the owner thread).
std::atomic<bool> g_busy{false};

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
int32_t field_i32(void *obj, void *field) {
  int32_t v = 0;
  if (obj != nullptr) field_get_fn()(obj, field, &v);
  return v;
}
uint8_t field_u8(void *obj, void *field) {
  uint8_t v = 0;
  if (obj != nullptr) field_get_fn()(obj, field, &v);
  return v;
}

void *managed_invoke(const void *method, void *obj, void **params, void **exc) {
  *exc = nullptr;
  return reinterpret_cast<Invoke>(g_cfg.runtime_invoke)(method, obj, params, exc);
}
void *unbox(void *boxed) { return reinterpret_cast<Unbox>(g_cfg.object_unbox)(boxed); }

// --- boxed-scalar invoke helpers ---------------------------------------------------
// Invoke a method returning a boxed scalar, root the box across nothing (read immediately), validate its
// exact class, unbox and drop it. `params` is nullptr for a zero-arg method. Every failure is fail-closed.
int invoke_boxed_bool(const void *method, void *self, void **params, bool *out_val) {
  void *exc = nullptr;
  void *box = managed_invoke(method, self, params, &exc);
  if (exc != nullptr) return PGR_TF_ERR_MANAGED;
  if (box == nullptr) return PGR_TF_ERR_MALFORMED;
  uintptr_t box_root = hold(box);
  if (box_root == 0) return PGR_TF_ERR_GCHANDLE;
  int rc = PGR_TF_VALID;
  void *b = gc_get(box_root);
  if (!is_exact(b, g_cfg.class_bool)) {
    rc = PGR_TF_ERR_MALFORMED;
  } else {
    void *u = unbox(b);
    if (u == nullptr) {
      rc = PGR_TF_ERR_MALFORMED;
    } else {
      uint8_t n = *static_cast<uint8_t *>(u);
      if (n > 1) rc = PGR_TF_ERR_MALFORMED;  // strict 0/1
      else *out_val = (n == 1);
    }
  }
  drop(&box_root);
  return rc;
}

int invoke_boxed_int(const void *method, void *self, void **params, void *box_class, int32_t *out_val) {
  void *exc = nullptr;
  void *box = managed_invoke(method, self, params, &exc);
  if (exc != nullptr) return PGR_TF_ERR_MANAGED;
  if (box == nullptr) return PGR_TF_ERR_MALFORMED;
  uintptr_t box_root = hold(box);
  if (box_root == 0) return PGR_TF_ERR_GCHANDLE;
  int rc = PGR_TF_VALID;
  void *b = gc_get(box_root);
  if (!is_exact(b, box_class)) {
    rc = PGR_TF_ERR_MALFORMED;
  } else {
    void *u = unbox(b);
    if (u == nullptr) rc = PGR_TF_ERR_MALFORMED;
    else *out_val = *static_cast<int32_t *>(u);
  }
  drop(&box_root);
  return rc;
}

// --- the inspection ----------------------------------------------------------------
int do_inspect(const PgrTransferFactsRequest *req, uint64_t deadline, PgrTransferFactsOut *out) {
  uintptr_t pk_root = 0, gm_root = 0, display_root = 0, events_root = 0;
  int rc = PGR_TF_OK;

  // Root the caller-provided inputs immediately. gm is required (rare/size take it); concrete class is
  // deliberately NOT asserted (legacy accepts the interface), only non-null.
  if (req->gm == nullptr) { rc = PGR_TF_ERR_IDENTITY; goto done; }
  pk_root = hold(req->pokemon);
  if (pk_root == 0) { rc = PGR_TF_ERR_GCHANDLE; goto done; }
  gm_root = hold(req->gm);
  if (gm_root == 0) { rc = PGR_TF_ERR_GCHANDLE; goto done; }

  if (!is_exact(gc_get(pk_root), g_cfg.class_pokemon)) { rc = PGR_TF_ERR_IDENTITY; goto done; }

  // Identity: the caught id must be nonzero and equal the pokemon id BEFORE any managed call.
  if (req->caught_id == 0 || field_u64(gc_get(pk_root), g_cfg.f_id) != req->caught_id) {
    rc = PGR_TF_ERR_IDENTITY;
    goto done;
  }

  {
    void *display = field_ptr(gc_get(pk_root), g_cfg.f_display);
    if (!is_exact(display, g_cfg.class_display)) { rc = PGR_TF_ERR_IDENTITY; goto done; }
    display_root = hold(display);
    if (display_root == 0) { rc = PGR_TF_ERR_GCHANDLE; goto done; }
  }

  {
    // Size: only invoked when a size protect bit is set. The box must be the exact CharacterSize class
    // and an allowed member of the 5-value enum; otherwise malformed (keep). When not requested the
    // xxs/xxl flags stay false, per legacy.
    bool xxs = false, xxl = false;
    if (req->protect & PGR_TF_PROTECT_SIZE) {
      void *exc = nullptr;
      void *params[1] = {gc_get(gm_root)};
      void *box = managed_invoke(g_cfg.m_size, gc_get(pk_root), params, &exc);
      if (exc != nullptr) { rc = PGR_TF_ERR_MANAGED; goto done; }
      if (box == nullptr) { rc = PGR_TF_ERR_MALFORMED; goto done; }
      uintptr_t box_root = hold(box);
      if (box_root == 0) { rc = PGR_TF_ERR_GCHANDLE; goto done; }
      void *b = gc_get(box_root);
      int32_t size_val = 0;
      bool ok = false;
      if (is_exact(b, g_cfg.class_character_size)) {
        void *u = unbox(b);
        if (u != nullptr) { size_val = *static_cast<int32_t *>(u); ok = true; }
      }
      drop(&box_root);
      if (!ok) { rc = PGR_TF_ERR_MALFORMED; goto done; }
      bool allowed = false;
      for (int i = 0; i < PGR_TF_SIZE_COUNT; ++i)
        if (g_cfg.sizes[i] == size_val) { allowed = true; break; }
      if (!allowed) { rc = PGR_TF_ERR_MALFORMED; goto done; }
      xxs = (size_val == g_cfg.xxs);
      xxl = (size_val == g_cfg.xxl);
    }

    // rare(self, gm) and shiny(self) -> boxed Boolean, strict 0/1.
    bool rare = false, shiny = false;
    {
      void *params[1] = {gc_get(gm_root)};
      rc = invoke_boxed_bool(g_cfg.m_rare, gc_get(pk_root), params, &rare);
      if (rc != PGR_TF_VALID) goto done;
    }
    rc = invoke_boxed_bool(g_cfg.m_shiny, gc_get(pk_root), nullptr, &shiny);
    if (rc != PGR_TF_VALID) goto done;

    // origin-events collection (exact class) -> get_Count() must be a non-negative Int32.
    int32_t count = 0;
    {
      void *events = field_ptr(gc_get(pk_root), g_cfg.f_events);
      if (!is_exact(events, g_cfg.class_event)) { rc = PGR_TF_ERR_IDENTITY; goto done; }
      events_root = hold(events);
      if (events_root == 0) { rc = PGR_TF_ERR_GCHANDLE; goto done; }
      rc = invoke_boxed_int(g_cfg.m_event_count, gc_get(events_root), nullptr, g_cfg.class_int, &count);
      if (rc != PGR_TF_VALID) goto done;
      if (count < 0) { rc = PGR_TF_ERR_MALFORMED; goto done; }
    }

    // limited identifier: null or empty is normal; any non-empty string marks an event. A non-null value
    // must be an exact System.String and report a non-negative length.
    bool limited_event = false;
    {
      void *limited = field_ptr(gc_get(pk_root), g_cfg.f_limited);
      if (limited != nullptr) {
        if (!is_exact(limited, g_cfg.class_string)) { rc = PGR_TF_ERR_MALFORMED; goto done; }
        int32_t len = reinterpret_cast<StrLength>(g_cfg.string_length)(limited);
        if (len < 0) { rc = PGR_TF_ERR_MALFORMED; goto done; }
        limited_event = (len > 0);
      }
    }

    // Scalar reads (no managed call between). Re-resolve pokemon + display from their roots.
    void *pk = gc_get(pk_root);
    int32_t species = field_i32(pk, g_cfg.f_species);
    int32_t attack = field_i32(pk, g_cfg.f_attack);
    int32_t defense = field_i32(pk, g_cfg.f_defense);
    int32_t stamina = field_i32(pk, g_cfg.f_stamina);
    uint8_t favorite_raw = field_u8(pk, g_cfg.f_favorite);
    uint8_t egg_raw = field_u8(pk, g_cfg.f_egg);
    if (favorite_raw > 1 || egg_raw > 1) { rc = PGR_TF_ERR_MALFORMED; goto done; }

    void *display = gc_get(display_root);
    int32_t costume = field_i32(display, g_cfg.f_costume);
    int32_t original_costume = field_i32(display, g_cfg.f_original_costume);
    int32_t art = field_i32(display, g_cfg.f_art);
    int32_t art_asset = field_i32(display, g_cfg.f_art_asset);
    void *background = field_ptr(display, g_cfg.f_background);

    // IV 0..15 and species 1..2000: reject out-of-range metadata (keep -- never authorize a deletion).
    if (attack < 0 || attack > PGR_TF_IV_MAX || defense < 0 || defense > PGR_TF_IV_MAX ||
        stamina < 0 || stamina > PGR_TF_IV_MAX) {
      rc = PGR_TF_ERR_MALFORMED;
      goto done;
    }
    if (species < PGR_TF_SPECIES_MIN || species > PGR_TF_SPECIES_MAX) {
      rc = PGR_TF_ERR_MALFORMED;
      goto done;
    }

    bool event = count > 0 || limited_event || costume != g_cfg.costume_none ||
                 original_costume != g_cfg.costume_none;
    bool has_background =
        background != nullptr || art != g_cfg.art_none || art_asset != g_cfg.asset_none;

    // Fresh deadline + id recheck immediately before committing. A lease spent during the managed work is
    // refused (retry later); a shifted id is an identity failure.
    {
      uint64_t end = 0;
      if (!read_boottime_ms(&end)) { rc = PGR_TF_ERR_CLOCK_FAILURE; goto done; }
      if (end >= deadline) { rc = PGR_TF_OK; goto done; }
      if (field_u64(gc_get(pk_root), g_cfg.f_id) != req->caught_id) { rc = PGR_TF_ERR_IDENTITY; goto done; }
    }

    uint32_t flags = 0;
    if (rare) flags |= PGR_TF_FLAG_RARE;
    if (shiny) flags |= PGR_TF_FLAG_SHINY;
    if (event) flags |= PGR_TF_FLAG_EVENT;
    if (has_background) flags |= PGR_TF_FLAG_BACKGROUND;
    if (xxs) flags |= PGR_TF_FLAG_XXS;
    if (xxl) flags |= PGR_TF_FLAG_XXL;
    if (favorite_raw == 1) flags |= PGR_TF_FLAG_FAVORITE;
    if (egg_raw == 1) flags |= PGR_TF_FLAG_EGG;

    out->version = PGR_TF_OUT_VERSION;
    out->size = sizeof(PgrTransferFactsOut);
    out->attack = attack;
    out->defense = defense;
    out->stamina = stamina;
    out->species = species;
    out->known = PGR_TF_KNOWN_ALL;
    out->flags = flags;
    out->caught_id = req->caught_id;
    rc = PGR_TF_VALID;
  }

done:
  drop(&pk_root);
  drop(&gm_root);
  drop(&display_root);
  drop(&events_root);
  return rc;
}

// --- gate wrapper ------------------------------------------------------------------
// Returns true (and sets *deadline) when the inspection may proceed. Returns false with *rc already
// holding the refusal/error code otherwise. A reentrant call (busy) is refused with PGR_TF_OK.
bool enter_op(uint64_t owner_tid, uint64_t lease_ms, uint64_t *deadline, int *rc) {
  if (owner_tid == 0 || current_tid() != owner_tid) { *rc = PGR_TF_OK; return false; }
  if (lease_ms == 0) { *rc = PGR_TF_OK; return false; }
  uint64_t lease = lease_ms > kMaxLeaseMs ? kMaxLeaseMs : lease_ms;
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) { *rc = PGR_TF_ERR_CLOCK_FAILURE; return false; }
  if (g_busy.exchange(true)) { *rc = PGR_TF_OK; return false; }  // reentrant -> refused
  *deadline = now + lease;
  return true;
}

// --- config validation -------------------------------------------------------------
bool validate_config(const PgrTransferFactsConfig *c, int *rc) {
  if (c == nullptr) { *rc = PGR_TF_ERR_NULL_ARG; return false; }
  if (c->version != PGR_TF_CONFIG_VERSION) { *rc = PGR_TF_ERR_BAD_VERSION; return false; }
  if (c->size != sizeof(PgrTransferFactsConfig)) { *rc = PGR_TF_ERR_BAD_SIZE; return false; }
  void *const *ptrs = reinterpret_cast<void *const *>(&c->gc_new);
  for (int i = 0; i < 34; ++i) {
    if (ptrs[i] == nullptr) { *rc = PGR_TF_ERR_MISSING_PTR; return false; }
  }
  // Size members must be pairwise distinct, and xxs/xxl must be distinct members of that set.
  for (int i = 0; i < PGR_TF_SIZE_COUNT; ++i)
    for (int j = i + 1; j < PGR_TF_SIZE_COUNT; ++j)
      if (c->sizes[i] == c->sizes[j]) { *rc = PGR_TF_ERR_BAD_CONFIG; return false; }
  bool xxs_member = false, xxl_member = false;
  for (int i = 0; i < PGR_TF_SIZE_COUNT; ++i) {
    if (c->sizes[i] == c->xxs) xxs_member = true;
    if (c->sizes[i] == c->xxl) xxl_member = true;
  }
  if (!xxs_member || !xxl_member || c->xxs == c->xxl) { *rc = PGR_TF_ERR_BAD_CONFIG; return false; }
  *rc = PGR_TF_OK;
  return true;
}

// --- public API --------------------------------------------------------------------
int api_install(const PgrTransferFactsConfig *config) {
  int rc = PGR_TF_OK;
  if (!validate_config(config, &rc)) return rc;
  uint64_t probe = 0;
  if (!read_boottime_ms(&probe)) return PGR_TF_ERR_CLOCK_FAILURE;

  std::lock_guard<std::mutex> install_lock(g_install_mutex);
  if (g_installed.load(std::memory_order_acquire)) {
    return std::memcmp(&g_cfg, config, sizeof(*config)) == 0 ? PGR_TF_OK : PGR_TF_ERR_RECONFIG;
  }
  if (g_install_attempted) return g_install_error;
  g_install_attempted = true;
  g_cfg = *config;
  g_busy.store(false);
  g_installed.store(true, std::memory_order_release);
  return g_install_error = PGR_TF_OK;
}

int api_inspect(const PgrTransferFactsRequest *request, PgrTransferFactsOut *out) {
  if (out != nullptr) {
    std::memset(out, 0, sizeof(*out));  // zeroed + header-initialised on every path
    out->version = PGR_TF_OUT_VERSION;
    out->size = sizeof(PgrTransferFactsOut);
  }
  if (!g_installed.load(std::memory_order_acquire)) return PGR_TF_ERR_NOT_INSTALLED;
  if (request == nullptr || out == nullptr) return PGR_TF_ERR_NULL_ARG;
  if (request->version != PGR_TF_REQUEST_VERSION) return PGR_TF_ERR_BAD_VERSION;
  if (request->size != sizeof(PgrTransferFactsRequest)) return PGR_TF_ERR_BAD_SIZE;
  if (request->pokemon == nullptr) return PGR_TF_ERR_NULL_ARG;

  uint64_t deadline = 0;
  int rc = PGR_TF_OK;
  if (!enter_op(request->owner_tid, request->lease_ms, &deadline, &rc)) return rc;
  rc = do_inspect(request, deadline, out);
  g_busy.store(false);
  if (rc != PGR_TF_VALID && out != nullptr) {
    // Never leak partial facts on a non-success return.
    std::memset(out, 0, sizeof(*out));
    out->version = PGR_TF_OUT_VERSION;
    out->size = sizeof(PgrTransferFactsOut);
  }
  return rc;
}

const PgrTransferFactsApi kApiTable = {
    PGR_TF_API_VERSION, static_cast<uint32_t>(sizeof(PgrTransferFactsApi)),
    &api_install, &api_inspect,
};

}  // namespace

extern "C" const void *pgrazor_transfer_facts_api(uint32_t version) {
  if (version != PGR_TF_API_VERSION) return nullptr;
  return &kApiTable;
}
