#include "details_recovery.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <time.h>
#include <unistd.h>

// Feature 38 native post-catch DETAILS recovery. See details_recovery.h for the ABI and the division of
// labour with the host JS. This is the C++ port of quick-close.js recoverDetails(gui): from a borrowed
// GUI + the expected response it re-derives and roots the full details record, proves the identity, and
// hands the six native-owned handles back. The actual dismiss click stays in feature 37. Built
// -fno-exceptions / -fno-rtti like every sibling: JS "throw/return null" paths become fail-closed
// refusals. No mutex is held across a managed call; no raw managed pointer survives one.

static_assert(sizeof(PgrDetailsRecoveryConfig) == 272, "config 272");
static_assert(sizeof(PgrDetailsRecoveryRequest) == 56, "request 56");
static_assert(sizeof(PgrDetailsRecoveryOutput) == 64, "output 64");
static_assert(sizeof(PgrDetailsRecoveryApi) == 32, "table 32");
static_assert(offsetof(PgrDetailsRecoveryRequest, expected_key) == 8, "layout");
static_assert(offsetof(PgrDetailsRecoveryRequest, remaining_ms) == 32, "layout");
static_assert(offsetof(PgrDetailsRecoveryRequest, gui) == 40, "layout");
static_assert(offsetof(PgrDetailsRecoveryRequest, response) == 48, "layout");
static_assert(offsetof(PgrDetailsRecoveryOutput, gui) == 16, "layout");
static_assert(offsetof(PgrDetailsRecoveryOutput, dismissed) == 56, "layout");

namespace {

constexpr uint64_t kMaxLeaseMs = PGR_DR_MAX_LEASE_MS;
constexpr int kConfigPtrCount = 32;  // validated pointers between the header and the two int32s

using ObjClass = void *(*)(void *obj);
using FieldGet = void (*)(void *obj, void *field, void *out);
using Invoke = void *(*)(const void *method, void *obj, void **params, void **exc);
using Unbox = void *(*)(void *boxed);

// --- lifetime / publication --------------------------------------------------------
std::mutex g_install_mutex;
std::mutex g_state_mutex;
PgrDetailsRecoveryConfig g_cfg{};
std::atomic<bool> g_installed{false};
bool g_install_attempted = false;
int g_install_error = PGR_DR_ERR_NOT_INSTALLED;
const PgrDetailsCompletionApi *g_validator = nullptr;  // feature 37 table (the identity validator)

// Permanent Unity owner, latched once from a genuine on-thread call.
std::atomic<uint64_t> g_owner_tid{0};
bool g_busy = false;  // recovery, including root cleanup, is in flight (guarded by g_state_mutex)

// --- clock / tid -------------------------------------------------------------------
uint64_t current_tid() { return static_cast<uint64_t>(static_cast<pid_t>(gettid())); }

bool read_boottime_ms(uint64_t *out_ms) {
  struct timespec ts {};
  if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0 || ts.tv_sec < 0 || ts.tv_nsec < 0) return false;
  *out_ms = static_cast<uint64_t>(ts.tv_sec) * 1000ULL + static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
  return true;
}

// Typed accessors onto the immutable published config.
ObjClass obj_class() { return reinterpret_cast<ObjClass>(g_cfg.object_get_class); }
FieldGet field_get() { return reinterpret_cast<FieldGet>(g_cfg.field_get_value); }
Unbox unbox() { return reinterpret_cast<Unbox>(g_cfg.object_unbox); }
void *managed_invoke(const void *method, void *obj, void **params, void **exc) {
  *exc = nullptr;
  return reinterpret_cast<Invoke>(g_cfg.runtime_invoke)(method, obj, params, exc);
}
bool is_exact(void *obj, void *klass) { return obj != nullptr && obj_class()(obj) == klass; }

// Native-owned GC root. set() roots a raw object; duplicate() roots a borrowed handle's current target;
// release() hands the handle to the caller WITHOUT freeing (ownership transfer). Destructor frees.
struct Root {
  uintptr_t handle = 0;
  Root() = default;
  Root(const Root &) = delete;
  Root &operator=(const Root &) = delete;
  ~Root() { if (handle) reinterpret_cast<void (*)(uintptr_t)>(g_cfg.gc_free)(handle); }
  void *get() const {
    return handle ? reinterpret_cast<void *(*)(uintptr_t)>(g_cfg.gc_get)(handle) : nullptr;
  }
  bool set(void *object) {
    if (!object) return false;
    handle = reinterpret_cast<uintptr_t (*)(void *, int)>(g_cfg.gc_new)(object, 0);
    return handle && get();
  }
  bool duplicate(void *borrowed) {
    return borrowed &&
           set(reinterpret_cast<void *(*)(uintptr_t)>(g_cfg.gc_get)(reinterpret_cast<uintptr_t>(borrowed)));
  }
  uintptr_t release() { uintptr_t h = handle; handle = 0; return h; }
};

// The recovered record. On success all six are released into the output in record order.
struct Record {
  Root gui, view, response, data, pokemon, dismissed;
};

// Declared before Record cleanup so reentrant root cleanup still observes busy=true.
struct BusyExit { ~BusyExit() { std::lock_guard<std::mutex> lock(g_state_mutex); g_busy = false; } };

// --- field readers (fresh target from an owned temporary root) ---------------------
void *field_ptr(void *obj, void *field) {
  void *out = nullptr;
  if (obj != nullptr) field_get()(obj, field, &out);
  return out;
}
int32_t field_i32(void *obj, void *field) {
  int32_t v = 0;
  if (obj != nullptr) field_get()(obj, field, &v);
  return v;
}
uint8_t field_u8(void *obj, void *field) {
  uint8_t v = 0;
  if (obj != nullptr) field_get()(obj, field, &v);
  return v;
}
void field_bytes8(void *obj, void *field, uint8_t out[8]) {
  std::memset(out, 0, 8);
  if (obj != nullptr) field_get()(obj, field, out);
}

// --- owner latching ----------------------------------------------------------------
bool owner_ready(uint64_t tid) {
  if (tid == 0) return false;
  uint64_t owner = g_owner_tid.load(std::memory_order_acquire);
  if (!(owner == 0 || owner == tid)) return false;
  uint64_t expected = 0;
  g_owner_tid.compare_exchange_strong(expected, tid, std::memory_order_acq_rel);
  return current_tid() == tid && g_owner_tid.load(std::memory_order_acquire) == tid;
}

// The scalar / pointer relationships that must hold of the derived record (no managed calls). Re-run
// after each heap-relocating managed call. Every read resolves its object freshly through its root.
bool exact_record(const Record &r, bool withPokemon = true) {
  return is_exact(r.gui.get(), g_cfg.class_gui) && is_exact(r.view.get(), g_cfg.class_view) &&
         is_exact(r.response.get(), g_cfg.class_response) && is_exact(r.data.get(), g_cfg.class_data) &&
         (!withPokemon || is_exact(r.pokemon.get(), g_cfg.class_pokemon)) && is_exact(r.dismissed.get(), g_cfg.class_action);
}
bool check_relationships(const PgrDetailsRecoveryRequest *req, const Record &r, bool withPokemon = true) {
  if (!exact_record(r,withPokemon)) return false;
  if (field_ptr(r.gui.get(), g_cfg.f_gui_data) != r.data.get()) return false;
  if (field_ptr(r.gui.get(), g_cfg.f_gui_callback) != r.dismissed.get()) return false;
  if (field_ptr(r.data.get(), g_cfg.f_data_callback) != r.dismissed.get()) return false;
  if (field_ptr(r.dismissed.get(), g_cfg.f_delegate_target) != r.view.get()) return false;
  if (field_ptr(r.view.get(), g_cfg.f_view_response) != r.response.get()) return false;
  if (field_i32(r.response.get(), g_cfg.f_status) != g_cfg.success_literal) return false;
  if (field_u8(r.data.get(), g_cfg.f_post) != 1) return false;  // rejects ordinary inventory details
  if (field_i32(r.data.get(), g_cfg.f_index) != 0) return false;
  uint8_t key[8];
  field_bytes8(r.response.get(), g_cfg.f_caught_id, key);
  if (std::memcmp(key, req->expected_key, 8) != 0) return false;
  if (withPokemon) {
    field_bytes8(r.pokemon.get(), g_cfg.f_pokemon_id, key);
    if (std::memcmp(key, req->expected_key, 8) != 0) return false;
  }
  return true;
}

// Re-derive + root the whole record from the borrowed gui + response, proving every relationship. On
// success the record is fully rooted and validated by feature 37. Returns 1 / 0. Mirrors
// quick-close.js recoverDetails plus the one-element backing-list proof.
int rebuild(const PgrDetailsRecoveryRequest *req, Record &r) {
  bool any = false;
  for (int i = 0; i < 8; ++i) any = any || (req->expected_key[i] != 0);
  if (!any) return 0;  // zero caught key never recovers

  if (!r.gui.duplicate(req->gui) || !r.response.duplicate(req->response)) return 0;
  if (!is_exact(r.gui.get(), g_cfg.class_gui) || !is_exact(r.response.get(), g_cfg.class_response)) return 0;
  if (field_i32(r.response.get(), g_cfg.f_status) != g_cfg.success_literal) return 0;
  uint8_t key[8];
  field_bytes8(r.response.get(), g_cfg.f_caught_id, key);
  if (std::memcmp(key, req->expected_key, 8) != 0) return 0;

  if (!r.data.set(field_ptr(r.gui.get(), g_cfg.f_gui_data)) || !is_exact(r.data.get(), g_cfg.class_data))
    return 0;
  if (!r.dismissed.set(field_ptr(r.data.get(), g_cfg.f_data_callback)) ||
      !is_exact(r.dismissed.get(), g_cfg.class_action))
    return 0;
  if (field_ptr(r.gui.get(), g_cfg.f_gui_callback) != r.dismissed.get()) return 0;
  if (!r.view.set(field_ptr(r.dismissed.get(), g_cfg.f_delegate_target)) ||
      !is_exact(r.view.get(), g_cfg.class_view))
    return 0;
  if (field_ptr(r.view.get(), g_cfg.f_view_response) != r.response.get()) return 0;
  if (field_u8(r.data.get(), g_cfg.f_post) != 1 || field_i32(r.data.get(), g_cfg.f_index) != 0) return 0;

  // Root the backing list, prove count()==1, then item(0)==the caught pokemon. Both are managed calls
  // that can relocate; the list, the boxed count and the item result are rooted immediately and every
  // relationship is re-proven afterwards.
  Root list;
  if (!list.set(field_ptr(r.data.get(), g_cfg.f_list)) || !is_exact(list.get(), g_cfg.class_list)) return 0;
  {
    void *exc = nullptr;
    void *boxed = managed_invoke(g_cfg.m_count, list.get(), nullptr, &exc);
    Root count_box;
    if (exc != nullptr || !count_box.set(boxed)) return 0;
    if (!is_exact(count_box.get(), g_cfg.class_int)) return 0;
    void *raw = unbox()(count_box.get());
    if (raw == nullptr || *static_cast<int32_t *>(raw) != 1) return 0;
  }
  if (!check_relationships(req,r,false) || !is_exact(list.get(), g_cfg.class_list) ||
      field_ptr(r.data.get(), g_cfg.f_list) != list.get())
    return 0;
  {
    int32_t index = 0;
    void *params[1] = {&index};  // value-type index passed BY ADDRESS
    void *exc = nullptr;
    void *obj = managed_invoke(g_cfg.m_item, list.get(), params, &exc);
    if (exc != nullptr || !r.pokemon.set(obj)) return 0;
  }
  // Re-prove classes + scalar relationships (incl. pokemon.id) after the relocating managed calls.
  if (!exact_record(r) || !is_exact(list.get(), g_cfg.class_list) || field_ptr(r.data.get(), g_cfg.f_list) != list.get())
    return 0;
  if (!check_relationships(req, r)) return 0;
  return 1;
}

void init_output(PgrDetailsRecoveryOutput *out) {
  std::memset(out, 0, sizeof(*out));
  out->version = PGR_DR_OUTPUT_VERSION;
  out->size = sizeof(PgrDetailsRecoveryOutput);
  out->status = PGR_DR_OK;
}

// --- config validation / install ---------------------------------------------------
bool validate_config(const PgrDetailsRecoveryConfig *c, int *rc) {
  if (c == nullptr) { *rc = PGR_DR_ERR_NULL_ARG; return false; }
  if (c->version != PGR_DC_CONFIG_VERSION) { *rc = PGR_DR_ERR_BAD_VERSION; return false; }
  if (c->size != sizeof(PgrDetailsRecoveryConfig)) { *rc = PGR_DR_ERR_BAD_SIZE; return false; }
  void *ptrs[kConfigPtrCount];
  std::memcpy(ptrs,reinterpret_cast<const unsigned char*>(c)+offsetof(PgrDetailsRecoveryConfig,object_get_class),sizeof(ptrs));
  for (int i = 0; i < kConfigPtrCount; ++i)
    if (ptrs[i] == nullptr) { *rc = PGR_DR_ERR_MISSING_PTR; return false; }
  *rc = PGR_DR_OK;
  return true;
}

int api_install(const PgrDetailsRecoveryConfig *config) {
  int rc = PGR_DR_OK;
  if (!validate_config(config, &rc)) return rc;
  uint64_t probe = 0;
  if (!read_boottime_ms(&probe)) return PGR_DR_ERR_CLOCK_FAILURE;

  std::lock_guard<std::mutex> install_lock(g_install_mutex);
  if (g_installed.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    if (std::memcmp(&g_cfg, config, sizeof(*config)) != 0) return PGR_DR_ERR_RECONFIG;
    return PGR_DR_OK;
  }
  if (g_install_attempted) return g_install_error;
  g_install_attempted = true;

  // Guarantee the prerequisite validator: resolve feature 37's table and install it with the SAME
  // config (idempotent). Without it recover() has nothing to prove the final snapshot against.
  const auto *validator =
      static_cast<const PgrDetailsCompletionApi *>(pgrazor_details_completion_api(PGR_DC_API_VERSION));
  if (validator == nullptr || validator->version != PGR_DC_API_VERSION ||
      validator->size != sizeof(PgrDetailsCompletionApi) || validator->install == nullptr ||
      validator->validate == nullptr)
    return g_install_error = PGR_DR_ERR_PREREQUISITE;
  const int vrc = validator->install(config);
  if (vrc != PGR_DC_OK) return g_install_error = PGR_DR_ERR_PREREQUISITE;

  {
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    g_cfg = *config;
    g_validator = validator;
    g_busy = false;
  }
  g_installed.store(true, std::memory_order_release);
  return g_install_error = PGR_DR_OK;
}

int api_recover(const PgrDetailsRecoveryRequest *request, PgrDetailsRecoveryOutput *out) {
  if (out == nullptr) return PGR_DR_ERR_NULL_ARG;  // cannot report
  init_output(out);
  if (!g_installed.load(std::memory_order_acquire)) return out->status = PGR_DR_ERR_NOT_INSTALLED;
  if (request == nullptr) return out->status = PGR_DR_ERR_NULL_ARG;
  if (request->version != PGR_DR_REQUEST_VERSION) return out->status = PGR_DR_ERR_BAD_VERSION;
  if (request->size != sizeof(PgrDetailsRecoveryRequest)) return out->status = PGR_DR_ERR_BAD_SIZE;

  const uint64_t tid = current_tid();
  if (request->tid == 0 || tid != request->tid || !owner_ready(tid))
    return out->status = PGR_DR_OK;  // foreign / unproven owner: refuse

  const uint64_t remaining = request->remaining_ms > kMaxLeaseMs ? kMaxLeaseMs : request->remaining_ms;
  if (remaining == 0) return out->status = PGR_DR_OK;  // no live lease: refuse
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) return out->status = PGR_DR_ERR_CLOCK_FAILURE;
  const uint64_t deadline = now + remaining;

  { std::lock_guard<std::mutex> lock(g_state_mutex); if (g_busy) return out->status = PGR_DR_OK; g_busy = true; }
  BusyExit busy_exit;

  Record r;
  if (rebuild(request, r) != 1) return out->status = PGR_DR_OK;  // identity mismatch: refuse

  // Final validated snapshot: feature 37 proves alive/callback/scalars/list(count/post) on the rooted
  // handles. It re-runs the heap-relocating managed calls, so re-prove the relationships afterwards.
  PgrDetailsCompletionRequest v{};
  v.version = PGR_DC_REQUEST_VERSION;
  v.size = sizeof(v);
  std::memcpy(v.expected_key, request->expected_key, 8);
  v.epoch = request->epoch;
  v.tid = request->tid;
  v.gui = reinterpret_cast<void *>(r.gui.handle);
  v.view = reinterpret_cast<void *>(r.view.handle);
  v.response = reinterpret_cast<void *>(r.response.handle);
  v.data = reinterpret_cast<void *>(r.data.handle);
  v.pokemon = reinterpret_cast<void *>(r.pokemon.handle);
  v.dismissed = reinterpret_cast<void *>(r.dismissed.handle);
  if (g_validator->validate(&v) != PGR_DC_DONE) return out->status = PGR_DR_OK;
  if (!check_relationships(request, r)) return out->status = PGR_DR_OK;

  // Re-check the deadline after all managed work and right before publishing.
  uint64_t final_now = 0;
  if (!read_boottime_ms(&final_now)) return out->status = PGR_DR_ERR_CLOCK_FAILURE;
  if (final_now >= deadline) return out->status = PGR_DR_OK;

  // Publish: transfer the six roots to the caller in record order (gui, state/view, response, data,
  // pokemon, dismissed). The Record destructors now free nothing; release() is the caller's job.
  out->gui = reinterpret_cast<void *>(r.gui.release());
  out->state = reinterpret_cast<void *>(r.view.release());
  out->response = reinterpret_cast<void *>(r.response.release());
  out->data = reinterpret_cast<void *>(r.data.release());
  out->pokemon = reinterpret_cast<void *>(r.pokemon.release());
  out->dismissed = reinterpret_cast<void *>(r.dismissed.release());
  return out->status = PGR_DR_RECOVERED;
}

int api_release(PgrDetailsRecoveryOutput *out) {
  if (out == nullptr) return PGR_DR_ERR_NULL_ARG;
  if (!g_installed.load(std::memory_order_acquire)) return PGR_DR_ERR_NOT_INSTALLED;
  void **slots[] = {&out->gui,&out->state,&out->response,&out->data,&out->pokemon,&out->dismissed};
  auto gc_free = reinterpret_cast<void (*)(uintptr_t)>(g_cfg.gc_free);
  for (auto slot : slots) {
    const auto handle=reinterpret_cast<uintptr_t>(*slot);
    *slot=nullptr;
    if (handle) gc_free(handle);
  }
  return PGR_DR_OK;
}

const PgrDetailsRecoveryApi kApiTable = {
    PGR_DR_API_VERSION, static_cast<uint32_t>(sizeof(PgrDetailsRecoveryApi)),
    &api_install, &api_recover, &api_release,
};

}  // namespace

extern "C" const void *pgrazor_details_recovery_api(uint32_t version) {
  if (version != PGR_DR_API_VERSION) return nullptr;
  return &kApiTable;
}
