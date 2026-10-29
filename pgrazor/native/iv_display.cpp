#include "iv_display.h"
#include "hook_engine.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <time.h>
#include <unistd.h>

// Feature 15 IV display. See iv_display.h for the ABI and the division of labour with
// runtime/native-iv-display.js. Built -fno-exceptions / -fno-rtti like every sibling:
// the JS "throw" paths become fail-closed error flags, and every managed root is freed
// exactly once on the owner thread. No mutex is ever held across a managed call, a GC
// handle call, or an original trampoline.

static_assert(sizeof(PgrIvDisplayConfig) == 640, "config must be 640 bytes");
static_assert(sizeof(PgrIvDisplaySample) == 656, "sample must be 656 bytes");
static_assert(sizeof(PgrIvDisplayApi) == 64, "table must be 64 bytes");
static_assert(offsetof(PgrIvDisplayConfig, gchandle_new) == 8, "layout");
static_assert(offsetof(PgrIvDisplayConfig, m_get_level) == 8 + 13 * 8, "layout");
static_assert(offsetof(PgrIvDisplayConfig, setup_address) == 8 + 37 * 8, "layout");
static_assert(offsetof(PgrIvDisplayConfig, class_plate) == 8 + 39 * 8, "layout");
static_assert(offsetof(PgrIvDisplayConfig, f_plate_text) == 8 + 57 * 8, "layout");
static_assert(offsetof(PgrIvDisplayConfig, f_panel_gm) == 8 + 76 * 8, "layout");
static_assert(offsetof(PgrIvDisplayConfig, overflow_value) == 8 + 77 * 8, "layout");
static_assert(offsetof(PgrIvDisplayConfig, external_catch_observer) == 8 + 77 * 8 + 8, "layout");
static_assert(offsetof(PgrIvDisplayConfig, reserved) == 8 + 77 * 8 + 12, "layout");

namespace {

constexpr uint64_t kMaxLeaseMs = PGR_IV_MAX_LEASE_MS;
constexpr int kLabelCap = PGR_IV_LABEL_CAP;

// --- il2cpp runtime ABI typedefs ---------------------------------------------------
using GcNew = uintptr_t (*)(void *obj, int pinned);
using GcGet = void *(*)(uintptr_t handle);
using GcFree = void (*)(uintptr_t handle);
using ObjClass = void *(*)(void *obj);
using Unbox = void *(*)(void *obj);
using Invoke = void *(*)(const void *method, void *obj, void **params, void **exc);
using FieldGet = void (*)(void *obj, void *field, void *out);
using Assignable = uint8_t (*)(void *klass, void *oklass);
using ClassType = void *(*)(void *klass);
using TypeObject = void *(*)(void *type);
using StringNew = void *(*)(const char *utf8);
using StringLen = int (*)(void *str);
using StringChars = uint16_t *(*)(void *str);

// EncounterNameplate.SetPokemonUI(self, IMapPokemon, WeatherCondition enum, MethodInfo*).
using SetupFn = void (*)(void *self, void *map_pokemon, int32_t weather, const void *method);
// PokemonInfoPanel.DoUpdate(self, PokemonProto, MethodInfo*).
using OwnedFn = void (*)(void *self, void *proto, const void *method);

// --- lifetime / publication --------------------------------------------------------
std::mutex g_install_mutex;
std::mutex g_state_mutex;
PgrIvDisplayConfig g_cfg{};
bool g_outline_available = false;
std::atomic<bool> g_installed{false};
bool g_install_attempted = false;
int g_install_error = PGR_IV_ERR_NOT_INSTALLED;
std::atomic<SetupFn> g_orig_setup{nullptr};
std::atomic<OwnedFn> g_orig_owned{nullptr};

// Permanent Unity owner, latched once from a genuine on-thread hook observation.
std::atomic<uint64_t> g_owner_tid{0};

// Conservative finish/root accounting (atomic so sample is lock-free for these).
std::atomic<uint32_t> g_durable_roots{0};  // text + outline handles held by live records
std::atomic<uint32_t> g_temp_roots{0};     // ScopedRoot handles in flight
std::atomic<uint32_t> g_active_depth{0};   // owner-thread hook/tick reentrancy depth

// --- gate / telemetry (guarded by g_state_mutex) -----------------------------------
uint64_t g_proven_tid = 0;
bool g_catch_enabled = false;
bool g_owned_enabled = false;
uint64_t g_lease_until_ms = 0;
// Per-channel revocation generation. Bumped under g_state_mutex whenever a gate transition
// (disable, or an update that turned a channel off/refreshed it from a dead lease) could
// invalidate an in-flight apply. An apply pins the generation when it begins and
// check_apply_gate() cancels the apply if it no longer matches -- so a disable+reenable that
// straddles a getter revokes the stale apply even though the channel is enabled again.
uint64_t g_catch_apply_gen = 1;
uint64_t g_owned_apply_gen = 1;
int g_catch_fault = 0;
int g_owned_fault = 0;

uint64_t g_catch_serial = 0, g_owned_serial = 0, g_position_serial = 0, g_encounter_serial = 0;
uint64_t g_encounter_id = 0, g_last_tid = 0;
double g_latitude = 0.0, g_longitude = 0.0;
uint32_t g_catch_notice = PGR_IV_NOTICE_NONE, g_owned_notice = PGR_IV_NOTICE_NONE;
int32_t g_catch_level_half = PGR_IV_LEVEL_UNKNOWN, g_owned_level_half = PGR_IV_LEVEL_UNKNOWN;
int32_t g_catch_atk = -1, g_catch_def = -1, g_catch_sta = -1, g_catch_cp = -1;
int32_t g_owned_atk = -1, g_owned_def = -1, g_owned_sta = -1, g_owned_cp = -1;
uint32_t g_catch_reason = PGR_IV_REASON_NONE, g_owned_reason = PGR_IV_REASON_NONE;
char g_catch_label[kLabelCap] = {0};
char g_owned_label[kLabelCap] = {0};

// --- owner-thread-only overlay records ---------------------------------------------
struct Record {
  uintptr_t text;    // durable GC root to the UI.Text
  uintptr_t outline; // durable GC root to the UI.Outline (0 when none)
  bool applied;
  bool outline_created;
  bool outline_enabled;  // snapshotted enabled state (restore target)
  int32_t font_size, rich, fit, overflow, alignment;
  float outline_color[4];
  float outline_distance[2];
  char original[kLabelCap];  // the game's authentic label (restore target)
  char label[kLabelCap];     // the overlay label we write (ownership proof on restore)
};
Record g_catch_rec{};
std::atomic<bool> g_catch_present{false};
Record g_owned_recs[PGR_IV_MAX_OWNED]{};
std::atomic<uint32_t> g_owned_count{0};

// Owner-thread-only context for the apply currently in flight: the channel it targets and
// the revocation generation pinned when it began. Reentrant disable()/update() bumps the
// generation; the next check_apply_gate() then sees the mismatch and revokes. `active` is
// false on the restore path (restore is always permitted, even after the lease expired).
// Never read or written off the owner thread.
struct ApplyScope {
  bool active = false;
  bool is_catch = false;
  uint64_t gen = 0;
  bool revoked = false;
};
ApplyScope g_apply_scope{};
// Generation pinned at hook entry and consumed by capture_overlay. The explicit valid bit
// preserves a disabled (zero) generation across the original and all capture getters.
uint64_t g_pending_apply_gen = 0;
bool g_pending_apply_valid = false;
std::atomic<uint64_t> g_stop_serial{0};

// --- single owner-thread catch frame (begin/end) -----------------------------------
// Both the standalone Dobby hook (hook_setup) and the external-observer begin_catch/end_catch
// claim a catch pass through this one frame. It holds the two temporary GC roots that must stay
// alive across the original call plus the token that pairs the end to its begin. `active` is
// only ever written on the owner thread; a foreign end_catch reads it solely to reject itself.
// active_depth is incremented when a begin claims and decremented only by the matching end.
struct CatchFrame {
  bool active = false;
  bool do_capture = false;
  uint64_t token = 0;
  uint64_t tid = 0;
  uint64_t apply_gen = 0;
  uint64_t stop_serial = 0;
  uintptr_t plate_root = 0;
  uintptr_t map_root = 0;
};
CatchFrame g_catch_frame{};
// Monotonic catch-pass token source (owner-thread only: begin runs with active_depth == 0, so
// there is never a concurrent writer). 0 is reserved for "not claimed"; overflow is rejected.
uint64_t g_token_counter = 0;

// --- clock / tid -------------------------------------------------------------------
uint64_t current_tid() { return static_cast<uint64_t>(static_cast<pid_t>(gettid())); }

bool read_boottime_ms(uint64_t *out_ms) {
  struct timespec ts {};
  if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0 || ts.tv_sec < 0 || ts.tv_nsec < 0) return false;
  *out_ms = static_cast<uint64_t>(ts.tv_sec) * 1000ULL +
            static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
  return true;
}

template <typename Fn>
Fn load_orig(std::atomic<Fn> &slot) {
  Fn fn = slot.load(std::memory_order_acquire);
  if (fn != nullptr) return fn;
  std::lock_guard<std::mutex> lock(g_install_mutex);
  return slot.load(std::memory_order_acquire);
}

// Typed accessors onto the immutable published config.
GcNew gc_new() { return reinterpret_cast<GcNew>(g_cfg.gchandle_new); }
GcGet gc_get() { return reinterpret_cast<GcGet>(g_cfg.gchandle_get_target); }
GcFree gc_free() { return reinterpret_cast<GcFree>(g_cfg.gchandle_free); }
ObjClass obj_class() { return reinterpret_cast<ObjClass>(g_cfg.object_get_class); }
Unbox unbox() { return reinterpret_cast<Unbox>(g_cfg.object_unbox); }
Invoke invoke_fn() { return reinterpret_cast<Invoke>(g_cfg.runtime_invoke); }
FieldGet field_get() { return reinterpret_cast<FieldGet>(g_cfg.field_get_value); }
Assignable assignable() { return reinterpret_cast<Assignable>(g_cfg.class_is_assignable_from); }

// Runtime error context for a single capture/render operation. `ok` latches false on the
// first failure; `reason` carries the PgrIvReason the adapter surfaces as iv-*-unavailable.
struct Ctx {
  bool ok = true;
  uint32_t reason = PGR_IV_REASON_NONE;
  void fail(uint32_t r) {
    if (ok) {
      ok = false;
      reason = r;
    }
  }
};

// Temporary GC root (owner thread). Created/freed on the owner thread only.
class ScopedRoot {
 public:
  ScopedRoot(const ScopedRoot &) = delete;
  ScopedRoot &operator=(const ScopedRoot &) = delete;
  ScopedRoot() : handle_(0) {}
  ~ScopedRoot() { reset(); }
  void acquire(void *obj) {
    reset();
    if (obj == nullptr) return;
    handle_ = gc_new()(obj, 0);
    if (handle_ != 0) g_temp_roots.fetch_add(1, std::memory_order_relaxed);
  }
  bool ok() const { return handle_ != 0; }
  void *target() const { return handle_ ? gc_get()(handle_) : nullptr; }
  void reset() {
    if (handle_ == 0) return;
    uintptr_t h = handle_;
    handle_ = 0;
    gc_free()(h);
    g_temp_roots.fetch_sub(1, std::memory_order_relaxed);
  }

 private:
  uintptr_t handle_;
};

uintptr_t hold_durable(void *obj) {
  if (obj == nullptr) return 0;
  uintptr_t h = gc_new()(obj, 0);
  if (h != 0) g_durable_roots.fetch_add(1, std::memory_order_relaxed);
  return h;
}
void free_durable(uintptr_t h) {
  if (h == 0) return;
  gc_free()(h);
  g_durable_roots.fetch_sub(1, std::memory_order_relaxed);
}

// Raw (non-RAII) temporary GC root for the catch frame: the handle outlives the stack frame
// that created it (it is held across the original call and freed by the matching end). It
// counts against g_temp_roots exactly like ScopedRoot, so the finish guard still sees it.
uintptr_t hold_temp(void *obj) {
  if (obj == nullptr) return 0;
  uintptr_t h = gc_new()(obj, 0);
  if (h != 0) g_temp_roots.fetch_add(1, std::memory_order_relaxed);
  return h;
}
void free_temp(uintptr_t h) {
  if (h == 0) return;
  gc_free()(h);
  g_temp_roots.fetch_sub(1, std::memory_order_relaxed);
}

bool is_exact(void *obj, void *klass) {
  return obj != nullptr && obj_class()(obj) == klass;
}

// --- field readers (object must be rooted by the caller) ---------------------------
void *field_ptr(void *obj, void *field) {
  void *out = nullptr;
  field_get()(obj, field, &out);
  return out;
}
int32_t field_i32(void *obj, void *field) {
  int32_t v = 0;
  field_get()(obj, field, &v);
  return v;
}
float field_f32(void *obj, void *field) {
  float v = 0.0f;
  field_get()(obj, field, &v);
  return v;
}
double field_f64(void *obj, void *field) {
  double v = 0.0;
  field_get()(obj, field, &v);
  return v;
}
uint64_t field_u64(void *obj, void *field) {
  uint64_t v = 0;
  field_get()(obj, field, &v);
  return v;
}

// --- managed invoke helpers (no mutex held) ----------------------------------------
void *invoke(void *method, void *obj, void **params, Ctx &c) {
  if (!c.ok) return nullptr;
  void *exc = nullptr;
  void *r = invoke_fn()(method, obj, params, &exc);
  if (exc != nullptr) {
    c.fail(PGR_IV_REASON_INVOKE_EXCEPTION);
    return nullptr;
  }
  return r;
}

// Invoke, root the boxed return, check its class exactly, unbox and copy `size` bytes.
bool boxed_read(void *method, void *obj, void **params, void *klass, void *out, size_t size,
                Ctx &c) {
  void *r = invoke(method, obj, params, c);
  if (!c.ok) return false;
  if (r == nullptr) {
    c.fail(PGR_IV_REASON_BOX_MISMATCH);
    return false;
  }
  ScopedRoot root;
  root.acquire(r);
  if (!root.ok()) {
    c.fail(PGR_IV_REASON_GCHANDLE);
    return false;
  }
  void *bt = root.target();
  if (bt == nullptr || obj_class()(bt) != klass) {
    c.fail(PGR_IV_REASON_BOX_MISMATCH);
    return false;
  }
  void *data = unbox()(bt);
  if (data == nullptr) {
    c.fail(PGR_IV_REASON_BOX_MISMATCH);
    return false;
  }
  std::memcpy(out, data, size);
  return true;
}

int32_t boxed_int(void *method, void *obj, void **params, Ctx &c) {
  int32_t v = 0;
  boxed_read(method, obj, params, g_cfg.class_int, &v, sizeof(v), c);
  return v;
}
float boxed_single(void *method, void *obj, void **params, Ctx &c) {
  float v = 0.0f;
  boxed_read(method, obj, params, g_cfg.class_single, &v, sizeof(v), c);
  return v;
}
bool boxed_bool(void *method, void *obj, void **params, Ctx &c) {
  uint8_t v = 0;
  if (!boxed_read(method, obj, params, g_cfg.class_bool, &v, sizeof(v), c)) return false;
  if (v > 1) {
    c.fail(PGR_IV_REASON_BOX_MISMATCH);
    return false;
  }
  return v == 1;
}
int32_t boxed_enum(void *method, void *obj, void *klass, Ctx &c) {
  int32_t v = 0;
  boxed_read(method, obj, nullptr, klass, &v, sizeof(v), c);
  return v;
}

// --- string helpers ----------------------------------------------------------------
size_t utf16_to_utf8(const uint16_t *src, int len, char *out, size_t cap) {
  size_t o = 0;
  for (int i = 0; i < len && o + 4 < cap; ++i) {
    uint32_t cp = src[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len) {
      uint32_t lo = src[i + 1];
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        ++i;
      }
    }
    if (cp < 0x80) {
      out[o++] = static_cast<char>(cp);
    } else if (cp < 0x800) {
      out[o++] = static_cast<char>(0xC0 | (cp >> 6));
      out[o++] = static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out[o++] = static_cast<char>(0xE0 | (cp >> 12));
      out[o++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out[o++] = static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out[o++] = static_cast<char>(0xF0 | (cp >> 18));
      out[o++] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out[o++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out[o++] = static_cast<char>(0x80 | (cp & 0x3F));
    }
  }
  out[o] = '\0';
  return o;
}

// UI.Text.get_text(): reference return (never boxed). Null string -> "". Rooted before read.
bool get_text(void *text_obj, char *out, size_t cap, Ctx &c) {
  out[0] = '\0';
  void *r = invoke(g_cfg.m_get_text, text_obj, nullptr, c);
  if (!c.ok) return false;
  if (r == nullptr) return true;  // empty string
  ScopedRoot root;
  root.acquire(r);
  if (!root.ok()) {
    c.fail(PGR_IV_REASON_GCHANDLE);
    return false;
  }
  void *s = root.target();
  if (s == nullptr || obj_class()(s) != g_cfg.class_string) {
    c.fail(PGR_IV_REASON_STRING);
    return false;
  }
  int n = reinterpret_cast<StringLen>(g_cfg.string_length)(s);
  if (n < 0 || n > 512) {
    c.fail(PGR_IV_REASON_STRING);
    return false;
  }
  if (n == 0) return true;
  // Re-resolve the string from its root before string_chars: the string_length invoke above
  // may have moved the managed object (the raw pointer we invoked on is only valid up to it).
  s = root.target();
  uint16_t *chars = s != nullptr ? reinterpret_cast<StringChars>(g_cfg.string_chars)(s) : nullptr;
  if (chars == nullptr) {
    c.fail(PGR_IV_REASON_STRING);
    return false;
  }
  utf16_to_utf8(chars, n, out, cap);
  return true;
}

// Set a value-type scalar argument (int / enum / bool); the boolean low byte of an int32
// buffer matches the JS Memory.alloc(4) pattern.
void set_i32(void *method, void *obj, int32_t value, Ctx &c) {
  if (!c.ok) return;
  int32_t v = value;
  void *params[1] = {&v};
  invoke(method, obj, params, c);
}

// --- IV / level formatting (exact parity with encounterLabel) ----------------------
int js_round(double x) { return static_cast<int>(std::floor(x + 0.5)); }  // Math.round, positives

void shade(const int a[3], const int b[3], double t, char out[8]) {
  if (t < 0.0) t = 0.0;
  if (t > 1.0) t = 1.0;
  int ch[3];
  for (int i = 0; i < 3; ++i) ch[i] = js_round(a[i] + (b[i] - a[i]) * t);
  std::snprintf(out, 8, "#%02x%02x%02x", ch[0] & 0xff, ch[1] & 0xff, ch[2] & 0xff);
}

void iv_color(int attack, int defense, int stamina, bool valid, char iv[16], char color[8]) {
  if (!valid) {
    std::snprintf(iv, 16, "--");
    std::snprintf(color, 8, "#dedede");
    return;
  }
  int sum = attack + defense + stamina;
  long tenths = js_round(sum / 45.0 * 1000.0);
  if (tenths % 10 == 0)
    std::snprintf(iv, 16, "%ld%%", tenths / 10);
  else
    std::snprintf(iv, 16, "%ld.%ld%%", tenths / 10, tenths % 10);
  double percent = sum / 45.0 * 100.0;
  static const int g90a[3] = {190, 145, 255}, g90b[3] = {235, 190, 255};
  static const int g80a[3] = {105, 215, 135}, g80b[3] = {160, 245, 175};
  if (percent == 100.0)
    std::snprintf(color, 8, "#ff5252");
  else if (percent >= 90.0)
    shade(g90a, g90b, (percent - 90.0) / 10.0, color);
  else if (percent >= 80.0)
    shade(g80a, g80b, (percent - 80.0) / 10.0, color);
  else
    std::snprintf(color, 8, "#dedede");
}

void fmt_level(int32_t level_half, char out[8]) {
  if (level_half == PGR_IV_LEVEL_UNKNOWN) {
    std::snprintf(out, 8, "--");
    return;
  }
  int whole = level_half / 2;
  if (level_half % 2 == 0)
    std::snprintf(out, 8, "%d", whole);
  else
    std::snprintf(out, 8, "%d.5", whole);
}

// Build the " Â· Lv .. Â· <color>IV ..%" suffix (the part after the CP prefix), the
// full telemetry label (CP + suffix) and the on-screen overlay (original + suffix, bold for
// owned). The CP prefix on screen is the game's own cpText (`original`), never our computed
// CP. The Unicode middle dot is the literal 0xC2 0xB7.
void build_labels(bool owned, const char *original, int32_t cp, int32_t level_half, int attack,
                  int defense, int stamina, char full_out[kLabelCap], char overlay_out[kLabelCap]) {
  bool valid_iv = attack >= 0 && attack <= 15 && defense >= 0 && defense <= 15 && stamina >= 0 &&
                  stamina <= 15;
  char iv[16], color[8], lv[8];
  iv_color(attack, defense, stamina, valid_iv, iv, color);
  fmt_level(level_half, lv);
  char suffix[128];
  std::snprintf(suffix, sizeof(suffix), " \xc2\xb7 Lv %s \xc2\xb7 <color=\"%s\">IV %s</color>", lv,
                color, iv);
  char cp_part[16];
  if (cp > 0)
    std::snprintf(cp_part, sizeof(cp_part), "%d", cp);
  else
    std::snprintf(cp_part, sizeof(cp_part), "--");
  std::snprintf(full_out, kLabelCap, "%s%s", cp_part, suffix);
  if (owned)
    std::snprintf(overlay_out, kLabelCap, "<b>%s%s</b>", original, suffix);
  else
    std::snprintf(overlay_out, kLabelCap, "%s%s", original, suffix);
}

// --- gate helpers (brief lock) ------------------------------------------------------
uint32_t owned_root_count() {
  return g_durable_roots.load(std::memory_order_relaxed) +
         g_temp_roots.load(std::memory_order_relaxed) +
         g_active_depth.load(std::memory_order_relaxed);
}

// --- telemetry publishers (brief lock) ---------------------------------------------
void publish_catch_label(const char *full, int32_t level_half, int a, int d, int s, int cp,
                         uint64_t tid) {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  ++g_catch_serial;
  g_catch_notice = PGR_IV_NOTICE_LABEL;
  g_catch_level_half = level_half;
  g_catch_atk = a;
  g_catch_def = d;
  g_catch_sta = s;
  g_catch_cp = cp;
  g_catch_reason = PGR_IV_REASON_NONE;
  std::snprintf(g_catch_label, kLabelCap, "%s", full);
  g_last_tid = tid;
}
void publish_catch_pending(uint32_t reason, uint64_t tid) {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  ++g_catch_serial;
  g_catch_notice = PGR_IV_NOTICE_PENDING;
  g_catch_reason = reason;
  g_last_tid = tid;
}
void publish_catch_unavailable(uint32_t reason, uint64_t tid) {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  g_catch_fault = 1;
  ++g_catch_serial;
  g_catch_notice = PGR_IV_NOTICE_UNAVAILABLE;
  g_catch_reason = reason;
  g_last_tid = tid;
}
void publish_owned_label(const char *full, int32_t level_half, int a, int d, int s, int cp,
                         uint64_t tid) {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  ++g_owned_serial;
  g_owned_notice = PGR_IV_NOTICE_LABEL;
  g_owned_level_half = level_half;
  g_owned_atk = a;
  g_owned_def = d;
  g_owned_sta = s;
  g_owned_cp = cp;
  g_owned_reason = PGR_IV_REASON_NONE;
  std::snprintf(g_owned_label, kLabelCap, "%s", full);
  g_last_tid = tid;
}
void publish_owned_pending(uint32_t reason, uint64_t tid) {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  ++g_owned_serial;
  g_owned_notice = PGR_IV_NOTICE_PENDING;
  g_owned_reason = reason;
  g_last_tid = tid;
}
void publish_owned_unavailable(uint32_t reason, uint64_t tid) {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  g_owned_fault = 1;
  ++g_owned_serial;
  g_owned_notice = PGR_IV_NOTICE_UNAVAILABLE;
  g_owned_reason = reason;
  g_last_tid = tid;
}
void publish_position(double lat, double lon, uint64_t id) {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  ++g_position_serial;
  g_latitude = lat;
  g_longitude = lon;
  g_encounter_id = id;
}
void publish_encounter(uint64_t tid) {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  ++g_encounter_serial;
  g_last_tid = tid;
}
void publish_unavailable(bool is_catch, uint32_t reason, uint64_t tid) {
  if (is_catch)
    publish_catch_unavailable(reason, tid);
  else
    publish_owned_unavailable(reason, tid);
}

// --- per-apply revocation gate & rooted-receiver helpers (owner thread) ------------
// Pin the generation an apply must run under. Returns 0 (meaning "do not apply") when the
// channel is disabled or the lease has already elapsed.
uint64_t snapshot_apply_gen(bool is_catch) {
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) return 0;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  if (is_catch) {
    if (g_catch_fault != 0 || !g_catch_enabled || now >= g_lease_until_ms) return 0;
    return g_catch_apply_gen;
  }
  if (g_owned_fault != 0 || !g_owned_enabled || now >= g_lease_until_ms) return 0;
  return g_owned_apply_gen;
}

// True while an apply is still permitted. On the restore path (scope inactive) it is always
// true -- restore is allowed even after the lease expired. On the apply path it re-reads a
// fresh CLOCK_BOOTTIME and re-checks the gate + pinned generation immediately before each UI
// mutation, so a reentrant disable (or disable+reenable) latches `revoked` and cancels the
// rest of the apply. A cancellation is NOT a fault (c is only failed on a clock read error).
bool check_apply_gate(Ctx &c) {
  if (!c.ok) return false;
  if (!g_apply_scope.active) return true;
  if (g_apply_scope.revoked || g_apply_scope.gen == 0) {
    g_apply_scope.revoked = true;
    return false;
  }
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) {
    c.fail(PGR_IV_REASON_CLOCK);
    return false;
  }
  std::lock_guard<std::mutex> lock(g_state_mutex);
  const bool en = g_apply_scope.is_catch ? g_catch_enabled : g_owned_enabled;
  const int fault = g_apply_scope.is_catch ? g_catch_fault : g_owned_fault;
  const uint64_t cur_gen = g_apply_scope.is_catch ? g_catch_apply_gen : g_owned_apply_gen;
  if (fault != 0 || !en || now >= g_lease_until_ms || cur_gen != g_apply_scope.gen) {
    g_apply_scope.revoked = true;
    return false;
  }
  return true;
}

// Re-resolve a durable GC root's current target immediately before an instance call: a prior
// managed call may have moved the object, leaving any cached raw pointer stale.
void *refresh_durable(uintptr_t root, Ctx &c, uint32_t reason = PGR_IV_REASON_GCHANDLE) {
  if (!c.ok) return nullptr;
  void *obj = root != 0 ? gc_get()(root) : nullptr;
  if (obj == nullptr) c.fail(reason);
  return obj;
}

// Gated int/enum/bool setter: re-check the apply gate, then re-resolve the receiver.
void set_i32_rooted(void *method, uintptr_t root, int32_t value, Ctx &c) {
  if (!check_apply_gate(c)) return;
  void *obj = refresh_durable(root, c, PGR_IV_REASON_RENDER);
  if (obj == nullptr || !check_apply_gate(c)) return;
  set_i32(method, obj, value, c);
}

// Gated set_text: allocate+root the System.String FIRST, then re-check the gate and re-resolve
// the receiver (the StringNew/GC may have moved the Text), and only then invoke set_text.
void set_text_rooted(uintptr_t text_root, const char *utf8, Ctx &c) {
  if (!c.ok) return;
  void *s = reinterpret_cast<StringNew>(g_cfg.string_new)(utf8);
  if (s == nullptr) {
    c.fail(PGR_IV_REASON_STRING);
    return;
  }
  ScopedRoot root;
  root.acquire(s);
  if (!root.ok()) {
    c.fail(PGR_IV_REASON_GCHANDLE);
    return;
  }
  if (!check_apply_gate(c)) return;
  void *text_obj = refresh_durable(text_root, c, PGR_IV_REASON_RENDER);
  void *str_obj = root.target();
  if (text_obj == nullptr || str_obj == nullptr) {
    if (str_obj == nullptr) c.fail(PGR_IV_REASON_STRING);
    return;
  }
  if (!check_apply_gate(c)) return;
  void *params[1] = {str_obj};
  invoke(g_cfg.m_set_text, text_obj, params, c);
}

// --- outline (owned only, optional, fails locally) ---------------------------------
void render_outline(Record &r, bool enabled) {
  if (r.outline == 0 || !g_outline_available) return;
  if (gc_get()(r.outline) == nullptr) return;
  Ctx c;  // outline errors are best-effort; never fault the record
  float on_color[4] = {0.0f, 0.0f, 0.0f, 0.95f};
  float on_dist[2] = {1.5f, -1.5f};
  const float *color = enabled ? on_color : r.outline_color;
  const float *dist = enabled ? on_dist : r.outline_distance;

  // The apply direction is gated/revocable; the restore direction (enabled == false) is not.
  if (enabled && !check_apply_gate(c)) return;
  void *obj = gc_get()(r.outline);
  if (obj == nullptr || (enabled && !check_apply_gate(c))) return;
  void *pc[1] = {const_cast<float *>(color)};
  invoke(g_cfg.m_outline_colorset, obj, pc, c);

  if (enabled && !check_apply_gate(c)) return;
  obj = gc_get()(r.outline);
  if (obj == nullptr || (enabled && !check_apply_gate(c))) return;
  void *pd[1] = {const_cast<float *>(dist)};
  invoke(g_cfg.m_outline_distanceset, obj, pd, c);

  if (enabled && !check_apply_gate(c)) return;
  obj = gc_get()(r.outline);
  if (obj == nullptr || (enabled && !check_apply_gate(c))) return;
  int32_t want = enabled ? 1 : (r.outline_enabled ? 1 : 0);
  set_i32(g_cfg.m_outline_setenabled, obj, want, c);
}

void prepare_outline(Record &r) {
  if (!g_outline_available) return;
  Ctx c;
  ScopedRoot go_root, type_root;
  void *t_obj = refresh_durable(r.text, c);
  if (t_obj == nullptr) return;
  void *go = invoke(g_cfg.m_outline_gameobject, t_obj, nullptr, c);
  go_root.acquire(go);
  void *type = reinterpret_cast<TypeObject>(g_cfg.type_get_object)(
      reinterpret_cast<ClassType>(g_cfg.class_get_type)(g_cfg.class_outline));
  type_root.acquire(type);
  if (!c.ok || go == nullptr || type == nullptr || !go_root.ok() || !type_root.ok())
    c.fail(PGR_IV_REASON_OUTLINE_UNAVAILABLE);
  // Resolve or (if absent AND the apply gate is still open) add the component. AddComponent is
  // a mutation: never add an Outline while the channel is disabled/revoked.
  bool created = false;
  if (c.ok && go_root.ok() && type_root.ok()) {
    void *params[1] = {type_root.target()};
    void *effect = invoke(g_cfg.m_outline_getcomponent, go_root.target(), params, c);
    if (c.ok && effect == nullptr) {
      Ctx gate_c;
      if (!check_apply_gate(gate_c)) {
        c.fail(PGR_IV_REASON_OUTLINE_UNAVAILABLE);
      } else {
        params[0] = type_root.target();
        void *go_obj = go_root.target();
        if (go_obj != nullptr && params[0] != nullptr && check_apply_gate(gate_c)) {
          effect = invoke(g_cfg.m_outline_addcomponent, go_obj, params, c);
          created = true;
        } else c.fail(PGR_IV_REASON_OUTLINE_UNAVAILABLE);
      }
    }
    if (c.ok && effect != nullptr && obj_class()(effect) == g_cfg.class_outline) {
      r.outline = hold_durable(effect);
      if (r.outline != 0) {
        r.outline_created = created;
        void *eff = refresh_durable(r.outline, c, PGR_IV_REASON_OUTLINE_UNAVAILABLE);
        r.outline_enabled =
            created ? false
                    : (eff != nullptr ? boxed_bool(g_cfg.m_outline_getenabled, eff, nullptr, c)
                                      : false);
        eff = refresh_durable(r.outline, c, PGR_IV_REASON_OUTLINE_UNAVAILABLE);
        if (eff != nullptr)
          boxed_read(g_cfg.m_outline_colorget, eff, nullptr, g_cfg.class_color, r.outline_color,
                     sizeof(r.outline_color), c);
        eff = refresh_durable(r.outline, c, PGR_IV_REASON_OUTLINE_UNAVAILABLE);
        if (eff != nullptr)
          boxed_read(g_cfg.m_outline_distanceget, eff, nullptr, g_cfg.class_vector2,
                     r.outline_distance, sizeof(r.outline_distance), c);
        if (c.ok && created) {
          eff = refresh_durable(r.outline, c, PGR_IV_REASON_OUTLINE_UNAVAILABLE);
          if (eff != nullptr) set_i32(g_cfg.m_outline_setenabled, eff, 0, c);
        }
      }
    } else if (c.ok) {
      c.fail(PGR_IV_REASON_OUTLINE_UNAVAILABLE);
    }
  }
  if (!c.ok) {
    // Local failure: drop the (partial) outline and keep the record working without it.
    // The outline is purely cosmetic, so its absence never faults the owned channel and
    // never blocks the label overlay (mirrors the JS iv-outline-unavailable fall-through).
    if (r.outline != 0) {
      const uintptr_t h = r.outline;
      r.outline = 0;
      free_durable(h);
    }
  }
}

// --- overlay apply / restore (owner thread) ----------------------------------------
void render(Record &r, bool enabled, Ctx &c) {
  if (r.applied == enabled) return;
  if (!c.ok) return;
  if ((r.text ? gc_get()(r.text) : nullptr) == nullptr) {
    // A dead target is not a channel fault; the caller drops the record on the next pass.
    r.applied = false;
    return;
  }
  if (enabled) {
    // Gate once up front; if already revoked nothing is mutated and applied stays false.
    if (!check_apply_gate(c)) return;
    r.applied = true;
    render_outline(r, true);
    set_i32_rooted(g_cfg.m_set_rich, r.text, 1, c);
    set_i32_rooted(g_cfg.m_set_fit, r.text, 0, c);
    int32_t fs = r.font_size;
    if (fs < 12) fs = 12;
    if (fs > 24) fs = 24;
    set_i32_rooted(g_cfg.m_set_font, r.text, fs, c);
    set_i32_rooted(g_cfg.m_set_overflow, r.text, g_cfg.overflow_value, c);
    set_i32_rooted(g_cfg.m_set_alignment, r.text, g_cfg.alignment_value, c);
    set_text_rooted(r.text, r.label, c);
    // A revocation part-way through leaves a half-styled label: restore the authentic style
    // (as an ungated restore) without faulting the channel -- cancellation is not an error.
    if (c.ok && g_apply_scope.active && g_apply_scope.revoked) {
      ApplyScope saved = g_apply_scope;
      g_apply_scope = ApplyScope{};
      render(r, false, c);
      g_apply_scope = saved;
    }
  } else {
    // Restore path: never gated (always allowed, even past the lease).
    ApplyScope saved = g_apply_scope;
    g_apply_scope = ApplyScope{};
    void *obj = gc_get()(r.text);
    char cur[kLabelCap];
    Ctx read;
    bool got = obj != nullptr && get_text(obj, cur, sizeof(cur), read);
    render_outline(r, false);
    set_i32_rooted(g_cfg.m_set_rich, r.text, r.rich, c);
    set_i32_rooted(g_cfg.m_set_fit, r.text, r.fit, c);
    set_i32_rooted(g_cfg.m_set_font, r.text, r.font_size, c);
    set_i32_rooted(g_cfg.m_set_overflow, r.text, r.overflow, c);
    set_i32_rooted(g_cfg.m_set_alignment, r.text, r.alignment, c);
    // Only restore our label if the game has not independently refreshed it.
    if (got && std::strcmp(cur, r.label) == 0) set_text_rooted(r.text, r.original, c);
    r.applied = false;
    g_apply_scope = saved;
  }
}

bool is_alive(void *obj, Ctx &c) {
  // op_Implicit(UnityEngine.Object) is static; the Object arg is a reference type, so the
  // params slot carries the object pointer itself (not its address).
  void *params[1] = {obj};
  return boxed_bool(g_cfg.m_alive, nullptr, params, c);
}

void free_record(Record &r) {
  if (r.outline != 0) {
    const uintptr_t h = r.outline;
    r.outline = 0;
    free_durable(h);
  }
  if (r.text != 0) {
    const uintptr_t h = r.text;
    r.text = 0;
    free_durable(h);
  }
}

// Restore (if alive) and free a record. Errors never leave the roots forgotten.
void drop_record(Record &r) {
  Ctx c;
  void *obj = r.text ? gc_get()(r.text) : nullptr;
  if (obj != nullptr && is_alive(obj, c) && c.ok) {
    Ctx rc;
    render(r, false, rc);
  }
  free_record(r);
}

void drop_catch() {
  if (!g_catch_present) return;
  g_catch_present = false;
  Record r = g_catch_rec;
  g_catch_rec = Record{};
  drop_record(r);
}

// Remove the owned record whose text target equals `text_obj` (live comparison; never a
// stored raw address as durable identity).
void drop_owned_matching(void *text_obj) {
  for (uint32_t i = 0; i < g_owned_count;) {
    void *obj = g_owned_recs[i].text ? gc_get()(g_owned_recs[i].text) : nullptr;
    if (text_obj == nullptr || obj == text_obj) {
      Record r = g_owned_recs[i];
      for (uint32_t j = i; j + 1 < g_owned_count; ++j) g_owned_recs[j] = g_owned_recs[j + 1];
      --g_owned_count;
      g_owned_recs[g_owned_count] = Record{};
      drop_record(r);
      if (text_obj != nullptr) return;  // dedup: at most one
    } else {
      ++i;
    }
  }
}

void evict_oldest_owned() {
  if (g_owned_count == 0) return;
  Record r = g_owned_recs[0];
  for (uint32_t j = 0; j + 1 < g_owned_count; ++j) g_owned_recs[j] = g_owned_recs[j + 1];
  --g_owned_count;
  g_owned_recs[g_owned_count] = Record{};
  drop_record(r);
}

// Snapshot the authentic UI state, build the overlay and apply/restore per the gate. On
// any error the partial record's roots are freed and the channel faults.
void capture_overlay(bool is_catch, void *text_obj, int32_t cp, int32_t level_half, int atk,
                     int def, int sta, uint64_t tid) {
  // Use the generation pinned at hook entry, including an explicitly disabled zero value.
  // Only callers without a pinned generation take a fresh snapshot. A zero result
  // means the channel was disabled/expired: the record is still captured, just left un-applied
  // so that toggling the channel ON mid-encounter (without a new hook pass) applies it.
  const uint64_t apply_gen =
      g_pending_apply_valid ? g_pending_apply_gen : snapshot_apply_gen(is_catch);
  g_pending_apply_gen = 0;
  g_pending_apply_valid = false;

  // Root the incoming Text before the dropIv teardown (which does managed restore calls that
  // may move it), then hold the durable root from the refreshed target.
  ScopedRoot incoming_root;
  incoming_root.acquire(text_obj);
  if (!incoming_root.ok()) {
    publish_unavailable(is_catch, PGR_IV_REASON_GCHANDLE, tid);
    return;
  }

  // Restore+free any existing overlay for this slot before snapshotting the refreshed label
  // (mirrors the dropIv at the head of the JS captureIv): a pooled panel/nameplate can never
  // carry styling or stats from the previously displayed Pokemon, and the catch record is
  // never overwritten without freeing its roots.
  if (is_catch)
    drop_catch();
  else
    drop_owned_matching(incoming_root.target());

  Ctx c;
  Record r{};
  r.text = hold_durable(incoming_root.target());
  incoming_root.reset();
  if (r.text == 0) {
    publish_unavailable(is_catch, PGR_IV_REASON_GCHANDLE, tid);
    return;
  }
  // Every managed read below re-resolves the Text from r.text first: the prior managed call
  // may have moved it.
  char original[kLabelCap];
  void *cur = refresh_durable(r.text, c, PGR_IV_REASON_STRING);
  if (cur == nullptr || !get_text(cur, original, sizeof(original), c) || !c.ok) {
    free_record(r);
    publish_unavailable(is_catch, c.reason ? c.reason : PGR_IV_REASON_STRING, tid);
    return;
  }
  std::snprintf(r.original, kLabelCap, "%s", original);
  char full[kLabelCap];
  build_labels(!is_catch, original, cp, level_half, atk, def, sta, full, r.label);

  cur = refresh_durable(r.text, c);
  if (cur != nullptr) r.font_size = boxed_int(g_cfg.m_get_font, cur, nullptr, c);
  cur = refresh_durable(r.text, c);
  if (cur != nullptr) r.rich = boxed_bool(g_cfg.m_get_rich, cur, nullptr, c) ? 1 : 0;
  cur = refresh_durable(r.text, c);
  if (cur != nullptr) r.fit = boxed_bool(g_cfg.m_get_fit, cur, nullptr, c) ? 1 : 0;
  cur = refresh_durable(r.text, c);
  if (cur != nullptr) r.overflow = boxed_enum(g_cfg.m_get_overflow, cur, g_cfg.class_hwm, c);
  cur = refresh_durable(r.text, c);
  if (cur != nullptr) r.alignment = boxed_enum(g_cfg.m_get_alignment, cur, g_cfg.class_ta, c);
  if (!c.ok) {
    free_record(r);
    publish_unavailable(is_catch, c.reason, tid);
    return;
  }

  // Open the apply scope (AddComponent in prepare_outline and every setter in render are now
  // gated/revocable). apply_gen == 0 => born-revoked (captured while disabled/expired).
  g_apply_scope = ApplyScope{true, is_catch, apply_gen, apply_gen == 0};
  if (!is_catch) prepare_outline(r);

  // Re-check the residual lease / revocation generation AFTER the managed getters and root
  // allocations, immediately before the UI mutation (a disable during a getter revokes here).
  bool enabled = check_apply_gate(c);
  if (!c.ok) {
    g_apply_scope = ApplyScope{};
    drop_record(r);
    publish_unavailable(is_catch, c.reason ? c.reason : PGR_IV_REASON_CLOCK, tid);
    return;
  }
  render(r, enabled, c);
  g_apply_scope = ApplyScope{};
  if (!c.ok) {
    drop_record(r);
    publish_unavailable(is_catch, c.reason ? c.reason : PGR_IV_REASON_RENDER, tid);
    return;
  }
  // Commit the record (kept even when un-applied, so a later enable can apply it).
  if (is_catch) {
    g_catch_rec = r;
    g_catch_present = true;
  } else {
    if (g_owned_count >= PGR_IV_MAX_OWNED) evict_oldest_owned();
    g_owned_recs[g_owned_count++] = r;
  }
  if (is_catch)
    publish_catch_label(full, level_half, atk, def, sta, cp, tid);
  else
    publish_owned_label(full, level_half, atk, def, sta, cp, tid);
}

// GameMaster half-level resolution; identical search and tolerances to the JS.
int32_t resolve_level(void *gm, float cpmul, float addcpmul, Ctx &c) {
  int32_t level_half = PGR_IV_LEVEL_UNKNOWN;
  float mult = cpmul + addcpmul;  // float32 add == Math.fround(double sum)
  if (!(std::isfinite(mult) && mult > 0.0f)) return level_half;
  if (gm == nullptr) {
    c.fail(PGR_IV_REASON_STATS_NOT_POPULATED);
    return level_half;
  }
  // Root gm locally and re-resolve its target before each invoke: these are two or three
  // consecutive managed calls and the first may move the GameMaster object.
  ScopedRoot gm_root;
  gm_root.acquire(gm);
  if (!gm_root.ok()) {
    c.fail(PGR_IV_REASON_GCHANDLE);
    return level_half;
  }
  float mul_arg = mult;
  void *lp[2] = {gm_root.target(), &mul_arg};
  int32_t lower = boxed_int(g_cfg.m_get_level, nullptr, lp, c);
  if (!c.ok) return level_half;
  if (lower >= 1 && lower <= 100) {
    int32_t lv = lower;
    void *mp[2] = {gm_root.target(), &lv};
    float lo = boxed_single(g_cfg.m_get_multiplier, nullptr, mp, c);
    if (!c.ok) return level_half;
    double md = (double)mult;
    if (std::fabs((double)lo - md) < 0.00002) {
      level_half = lower * 2;
    } else {
      int32_t lv2 = lower + 1;
      void *hp[2] = {gm_root.target(), &lv2};
      float hi = boxed_single(g_cfg.m_get_multiplier, nullptr, hp, c);
      if (!c.ok) return level_half;
      double qm = std::sqrt(((double)lo * lo + (double)hi * hi) / 2.0);
      if (std::fabs(qm - md) < 0.00002) level_half = lower * 2 + 1;
    }
  }
  return level_half;
}

// --- capture bodies (onLeave halves) -----------------------------------------------
void capture_catch(void *plate, void *map, uint64_t tid) {
  Ctx c;
  // The displayed map object must be exactly WildMapPokemon.
  if (!is_exact(map, g_cfg.class_wildmap)) return;

  ScopedRoot response_root, wh_root, mh_root, th_root, pokemon_root;
  void *pokemon = nullptr;

  void *response = field_ptr(map, g_cfg.f_map_response);
  if (response != nullptr) {
    response_root.acquire(response);
    if (!response_root.ok()) {
      publish_catch_unavailable(PGR_IV_REASON_GCHANDLE, tid);
      return;
    }
    if (obj_class()(response) != g_cfg.class_encounter_out) {
      publish_catch_unavailable(PGR_IV_REASON_RESPONSE_CLASS, tid);
      return;
    }
    void *wh = field_ptr(response, g_cfg.f_response_wild);
    void *mh = field_ptr(map, g_cfg.f_map_wild);
    if (wh == nullptr || mh == nullptr) return;
    wh_root.acquire(wh);
    mh_root.acquire(mh);
    if (!wh_root.ok() || !mh_root.ok()) {
      publish_catch_unavailable(PGR_IV_REASON_GCHANDLE, tid);
      return;
    }
    if (obj_class()(wh) != g_cfg.class_wild_proto || obj_class()(mh) != g_cfg.class_wild_proto) {
      publish_catch_unavailable(PGR_IV_REASON_RESPONSE_CLASS, tid);
      return;
    }
    if (field_u64(wh, g_cfg.f_wild_id) != field_u64(mh, g_cfg.f_wild_id)) {
      publish_catch_pending(PGR_IV_REASON_ENCOUNTER_ID_MISMATCH, tid);
      return;
    }
    pokemon = field_ptr(wh, g_cfg.f_wild_pokemon);
    publish_position(field_f64(wh, g_cfg.f_wild_lat), field_f64(wh, g_cfg.f_wild_lon),
                     field_u64(wh, g_cfg.f_wild_id));
  } else {
    void *tappable = field_ptr(map, g_cfg.f_map_tappable);
    if (tappable != nullptr) {
      th_root.acquire(tappable);
      if (!th_root.ok()) {
        publish_catch_unavailable(PGR_IV_REASON_GCHANDLE, tid);
        return;
      }
      if (obj_class()(tappable) != g_cfg.class_tappable) {
        publish_catch_unavailable(PGR_IV_REASON_RESPONSE_CLASS, tid);
        return;
      }
      pokemon = field_ptr(tappable, g_cfg.f_tappable_pokemon);
    }
  }
  if (pokemon == nullptr) return;
  pokemon_root.acquire(pokemon);
  if (!pokemon_root.ok()) {
    publish_catch_unavailable(PGR_IV_REASON_GCHANDLE, tid);
    return;
  }
  if (obj_class()(pokemon) != g_cfg.class_pokemon) {
    publish_catch_unavailable(PGR_IV_REASON_POKEMON_CLASS, tid);
    return;
  }

  int32_t cp = field_i32(pokemon, g_cfg.f_cp);
  int32_t atk = field_i32(pokemon, g_cfg.f_atk);
  int32_t def = field_i32(pokemon, g_cfg.f_def);
  int32_t sta = field_i32(pokemon, g_cfg.f_sta);
  float cpmul = field_f32(pokemon, g_cfg.f_cpmul);
  float addcpmul = field_f32(pokemon, g_cfg.f_addcpmul);
  if (cp <= 0 || !(std::isfinite(cpmul) && cpmul > 0.0f)) {
    publish_catch_pending(PGR_IV_REASON_STATS_NOT_POPULATED, tid);
    return;
  }

  void *text_obj = field_ptr(plate, g_cfg.f_plate_text);
  ScopedRoot text_root;
  text_root.acquire(text_obj);
  if (text_obj == nullptr || !text_root.ok()) {
    publish_catch_unavailable(PGR_IV_REASON_GCHANDLE, tid);
    return;
  }
  if (assignable()(g_cfg.class_text, obj_class()(text_obj)) == 0) {
    publish_catch_unavailable(PGR_IV_REASON_TEXT_CLASS, tid);
    return;
  }

  void *gm = field_ptr(plate, g_cfg.f_plate_gm);
  ScopedRoot gm_root;
  gm_root.acquire(gm);
  int32_t level_half = resolve_level(gm, cpmul, addcpmul, c);
  if (!c.ok) {
    publish_catch_unavailable(c.reason, tid);
    return;
  }
  capture_overlay(true, text_obj, cp, level_half, atk, def, sta, tid);
}

void capture_owned(void *panel, void *proto, uint64_t tid) {
  Ctx c;
  if (!is_exact(proto, g_cfg.class_pokemon)) return;
  if (field_ptr(panel, g_cfg.f_panel_displayed) != proto) {
    publish_owned_unavailable(PGR_IV_REASON_OWNED_IDENTITY, tid);
    return;
  }

  int32_t cp = field_i32(proto, g_cfg.f_cp);
  int32_t atk = field_i32(proto, g_cfg.f_atk);
  int32_t def = field_i32(proto, g_cfg.f_def);
  int32_t sta = field_i32(proto, g_cfg.f_sta);
  float cpmul = field_f32(proto, g_cfg.f_cpmul);
  float addcpmul = field_f32(proto, g_cfg.f_addcpmul);
  if (cp <= 0 || !(std::isfinite(cpmul) && cpmul > 0.0f)) {
    publish_owned_pending(PGR_IV_REASON_STATS_NOT_POPULATED, tid);
    return;
  }

  void *text_obj = field_ptr(panel, g_cfg.f_panel_cptext);
  ScopedRoot text_root;
  text_root.acquire(text_obj);
  if (text_obj == nullptr || !text_root.ok()) {
    publish_owned_unavailable(PGR_IV_REASON_GCHANDLE, tid);
    return;
  }
  if (assignable()(g_cfg.class_text, obj_class()(text_obj)) == 0) {
    publish_owned_unavailable(PGR_IV_REASON_TEXT_CLASS, tid);
    return;
  }

  void *gm = field_ptr(panel, g_cfg.f_panel_gm);
  ScopedRoot gm_root;
  gm_root.acquire(gm);
  int32_t level_half = resolve_level(gm, cpmul, addcpmul, c);
  if (!c.ok) {
    publish_owned_unavailable(c.reason, tid);
    return;
  }
  capture_overlay(false, text_obj, cp, level_half, atk, def, sta, tid);
}

// --- hooks -------------------------------------------------------------------------
bool owner_ready(uint64_t tid) {
  uint64_t owner = g_owner_tid.load(std::memory_order_acquire);
  uint64_t proven;
  {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    proven = g_proven_tid;
  }
  if (proven == 0 || tid != proven || !(owner == 0 || owner == tid)) return false;
  uint64_t expected = 0;
  g_owner_tid.compare_exchange_strong(expected, tid, std::memory_order_acq_rel);
  return true;
}

// Issue the next monotonic catch-pass token (owner thread only). Returns 0 when the counter is
// exhausted so an overflowed token is never reused or handed out.
uint64_t next_token() {
  if (g_token_counter == UINT64_MAX) return 0;
  return ++g_token_counter;
}

// onEnter half, shared by the Dobby hook and the external observer. Claims the single catch
// frame on the proven owner thread: validates owner/non-reentrancy/fault/classes, roots self +
// map (BEFORE the dropIv teardown, which makes managed restore calls that may move them),
// restores+frees the previous overlay, announces the encounter, and returns a nonzero token.
// Returns 0 without touching active_depth when the pass is not eligible. Never calls the
// original. A root-allocation failure still claims (so the matching end releases active_depth)
// but faults the catch channel and skips the capture -- no root can escape.
uint64_t catch_begin(void *self, void *map_pokemon, uint64_t tid) {
  bool eligible = false;
  if (g_active_depth.load(std::memory_order_relaxed) == 0 && owner_ready(tid)) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    eligible = g_catch_fault == 0;
  }
  if (!(eligible && self != nullptr && map_pokemon != nullptr &&
        is_exact(self, g_cfg.class_plate) &&
        assignable()(g_cfg.class_map_pokemon, obj_class()(map_pokemon)) != 0))
    return 0;

  const uint64_t token = next_token();
  if (token == 0) return 0;  // token space exhausted: do not claim

  g_active_depth.fetch_add(1, std::memory_order_relaxed);
  CatchFrame f{};
  f.active = true;
  f.token = token;
  f.tid = tid;
  f.apply_gen = snapshot_apply_gen(true);
  f.stop_serial = g_stop_serial.load(std::memory_order_acquire);
  f.plate_root = hold_temp(self);
  f.map_root = hold_temp(map_pokemon);
  f.do_capture = f.plate_root != 0 && f.map_root != 0;
  g_catch_frame = f;

  if (f.do_capture)
    drop_catch();
  else
    publish_catch_unavailable(PGR_IV_REASON_GCHANDLE, tid);
  publish_encounter(tid);
  return token;
}

// onLeave half, shared by both paths. Completes the pass identified by (token, tid) AFTER the
// original ran: uses the entry-pinned generation so cancellation straddling the original cannot
// re-apply a stale label, captures from refreshed roots, then frees roots BEFORE releasing
// active_depth. A mismatched/duplicate token, a foreign thread, or a non-owner caller is rejected
// without stealing, clearing or freeing the live frame.
int catch_end(uint64_t token, uint64_t tid) {
  if (token == 0) return PGR_IV_ERR_INVALID_ARG;
  // Only the latched owner may complete a frame; a foreign thread only reads to reject itself.
  if (g_owner_tid.load(std::memory_order_acquire) != tid) return PGR_IV_ERR_INVALID_ARG;
  if (!g_catch_frame.active || g_catch_frame.token != token || g_catch_frame.tid != tid) return PGR_IV_ERR_INVALID_ARG;
  CatchFrame f = g_catch_frame;
  g_catch_frame = CatchFrame{}; // retire token before any reentrant managed call or free

  if (f.do_capture) {
    bool fault;
    {
      std::lock_guard<std::mutex> lock(g_state_mutex);
      fault = g_catch_fault != 0;
    }
    if (!fault && f.stop_serial == g_stop_serial.load(std::memory_order_acquire)) {
      g_pending_apply_gen = f.apply_gen;
      g_pending_apply_valid = true;
      void *plate = f.plate_root != 0 ? gc_get()(f.plate_root) : nullptr;
      void *mp = f.map_root != 0 ? gc_get()(f.map_root) : nullptr;
      if (plate != nullptr && mp != nullptr) capture_catch(plate, mp, tid);
      g_pending_apply_gen = 0;
      g_pending_apply_valid = false;
    }
  }
  free_temp(f.map_root);
  free_temp(f.plate_root);
  g_catch_frame = CatchFrame{};
  g_active_depth.fetch_sub(1, std::memory_order_relaxed);
  return PGR_IV_OK;
}

void hook_setup(void *self, void *map_pokemon, int32_t weather, const void *method) {
  SetupFn orig = load_orig(g_orig_setup);
  const uint64_t tid = current_tid();

  const uint64_t token = catch_begin(self, map_pokemon, tid);
  if (token != 0 && g_catch_frame.do_capture) {
    // Re-resolve the rooted args for the original call: drop_catch() may have moved them.
    void *rp = gc_get()(g_catch_frame.plate_root);
    void *rm = gc_get()(g_catch_frame.map_root);
    if (rp != nullptr) self = rp;
    if (rm != nullptr) map_pokemon = rm;
  }

  orig(self, map_pokemon, weather, method);

  if (token != 0) catch_end(token, tid);
}

void hook_owned(void *self, void *proto, const void *method) {
  const uint64_t entry_apply_gen = snapshot_apply_gen(false);
  const uint64_t entry_stop_serial = g_stop_serial.load(std::memory_order_acquire);
  OwnedFn orig = load_orig(g_orig_owned);
  const uint64_t tid = current_tid();

  bool claimed = false;
  bool eligible = false;
  if (g_active_depth.load(std::memory_order_relaxed) == 0 && owner_ready(tid)) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    eligible = g_owned_fault == 0;
  }

  ScopedRoot panel_root, proto_root;
  bool do_capture = false;
  if (eligible && self != nullptr && proto != nullptr && is_exact(self, g_cfg.class_panel) &&
      is_exact(proto, g_cfg.class_pokemon)) {
    g_active_depth.fetch_add(1, std::memory_order_relaxed);
    claimed = true;
    // Root the arguments BEFORE the dropIv teardown (managed restore calls may move them).
    panel_root.acquire(self);
    proto_root.acquire(proto);
    do_capture = panel_root.ok() && proto_root.ok();
    // onEnter: drop any existing overlay for this panel's label before the native refresh.
    if (!do_capture) publish_owned_unavailable(PGR_IV_REASON_GCHANDLE, tid);
    void *panel_obj = do_capture ? panel_root.target() : nullptr;
    void *existing = panel_obj != nullptr ? field_ptr(panel_obj, g_cfg.f_panel_cptext) : nullptr;
    if (existing != nullptr) drop_owned_matching(existing);
    if (panel_root.ok()) self = panel_root.target();
    if (proto_root.ok()) proto = proto_root.target();
  }

  orig(self, proto, method);

  if (claimed) {
    if (do_capture) {
      bool fault;
      {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        fault = g_owned_fault != 0;
      }
      if (!fault && g_owner_tid.load(std::memory_order_acquire) == tid &&
          entry_stop_serial == g_stop_serial.load(std::memory_order_acquire)) {
        g_pending_apply_gen = entry_apply_gen;
        g_pending_apply_valid = true;
        capture_owned(panel_root.target(), proto_root.target(), tid);
        g_pending_apply_gen = 0;
        g_pending_apply_valid = false;
      }
    }
    proto_root.reset();
    panel_root.reset();
    g_active_depth.fetch_sub(1, std::memory_order_relaxed);
  }
}

// --- install -----------------------------------------------------------------------
bool required_index(int i) {
  // Optional (outline) pointer indices in the adapter's flat array; everything else is
  // required non-null. Indices: class_get_type(8), type_get_object(9), m_outline_* (28..36),
  // class_outline(54), class_color(55), class_vector2(56).
  if (i == 8 || i == 9) return false;
  if (i >= 28 && i <= 36) return false;
  if (i >= 54 && i <= 56) return false;
  return true;
}

bool validate_config(const PgrIvDisplayConfig *c, int *out_rc) {
  if (c == nullptr) {
    *out_rc = PGR_IV_ERR_INVALID_ARG;
    return false;
  }
  if (c->version != PGR_IV_CONFIG_VERSION) {
    *out_rc = PGR_IV_ERR_BAD_VERSION;
    return false;
  }
  if (c->size != sizeof(PgrIvDisplayConfig)) {
    *out_rc = PGR_IV_ERR_BAD_SIZE;
    return false;
  }
  void *const *ptrs = reinterpret_cast<void *const *>(&c->gchandle_new);
  bool outline_all = true;
  for (int i = 0; i < 77; ++i) {
    if (ptrs[i] == nullptr) {
      if (required_index(i)) {
        *out_rc = PGR_IV_ERR_MISSING_PTR;
        return false;
      }
      outline_all = false;
    }
  }
  if (c->setup_address == c->owned_update_address) {
    *out_rc = PGR_IV_ERR_DUPLICATE_TARGET;
    return false;
  }
  if (c->external_catch_observer > 1 || c->reserved != 0) {
    *out_rc = PGR_IV_ERR_INVALID_ARG;
    return false;
  }
  g_outline_available = outline_all;
  *out_rc = PGR_IV_OK;
  return true;
}

int api_install(const PgrIvDisplayConfig *config) {
  int rc = PGR_IV_OK;
  if (!validate_config(config, &rc)) return rc;

  uint64_t probe = 0;
  if (!read_boottime_ms(&probe)) return PGR_IV_ERR_CLOCK_FAILURE;

  std::lock_guard<std::mutex> install_lock(g_install_mutex);

  if (g_installed.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    if (std::memcmp(&g_cfg, config, sizeof(*config)) != 0) return PGR_IV_ERR_RECONFIG_MISMATCH;
    return PGR_IV_OK;
  }
  if (g_install_attempted) return g_install_error;
  g_install_attempted = true;

  {
    std::lock_guard<std::mutex> state_lock(g_state_mutex);
    g_cfg = *config;
    g_catch_enabled = false;
    g_owned_enabled = false;
    g_lease_until_ms = 0;
    g_proven_tid = 0;
    g_catch_fault = 0;
    g_owned_fault = 0;
  }

  // Shared mode (external_catch_observer == 1): the adapter's Gum observer owns SetPokemonUI
  // (it coexists with vgp.js's observer at the same address), so the native layer must NOT
  // place a Dobby trampoline there. The catch pass runs through begin_catch/end_catch instead.
  // g_orig_setup stays null (hook_setup never runs). The owned panel hook is always a Dobby hook.
  if (config->external_catch_observer == 0) {
    void *orig_setup = nullptr;
    if (pgrazor_hook(config->setup_address, reinterpret_cast<void *>(&hook_setup), &orig_setup) !=
            0 ||
        orig_setup == nullptr) {
      return g_install_error = PGR_IV_ERR_HOOK_SETUP;
    }
    g_orig_setup.store(reinterpret_cast<SetupFn>(orig_setup), std::memory_order_release);
  }

  void *orig_owned = nullptr;
  if (pgrazor_hook(config->owned_update_address, reinterpret_cast<void *>(&hook_owned),
                   &orig_owned) != 0 ||
      orig_owned == nullptr) {
    return g_install_error = PGR_IV_ERR_HOOK_OWNED;
  }
  g_orig_owned.store(reinterpret_cast<OwnedFn>(orig_owned), std::memory_order_release);

  g_installed.store(true, std::memory_order_release);
  return g_install_error = PGR_IV_OK;
}

// --- update / disable --------------------------------------------------------------
int api_update(int catch_enabled, int owned_enabled, uint64_t remaining_lease_ms,
               uint64_t proven_tid) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_IV_ERR_NOT_INSTALLED;
  uint64_t now = 0;
  const bool clock_ok = read_boottime_ms(&now);

  std::lock_guard<std::mutex> lock(g_state_mutex);
  const uint64_t owner = g_owner_tid.load(std::memory_order_acquire);
  const bool wrong_thread = owner != 0 && proven_tid != 0 && proven_tid != owner;
  if (wrong_thread) {
    catch_enabled = 0;
    owned_enabled = 0;  // a wrong-thread update can only disable; never reassigns the owner
  } else if (proven_tid != 0) {
    g_proven_tid = proven_tid;
  }

  if (!clock_ok) {
    g_catch_enabled = false;
    g_owned_enabled = false;
    g_lease_until_ms = 0;
    ++g_catch_apply_gen;  // a forced disable revokes any in-flight apply
    ++g_owned_apply_gen;
    return PGR_IV_ERR_CLOCK_FAILURE;
  }
  const uint64_t clamped = remaining_lease_ms > kMaxLeaseMs ? kMaxLeaseMs : remaining_lease_ms;
  const bool allow = clamped > 0 && proven_tid != 0 && !wrong_thread;
  const bool next_catch = allow && catch_enabled != 0;
  const bool next_owned = allow && owned_enabled != 0;
  const bool lease_was_live = g_lease_until_ms != 0 && now < g_lease_until_ms;
  // Bump the generation unless this is a pure lease refresh of an already-live channel. Any
  // disable, any enable-from-off, and any refresh after the lease had died revokes any apply
  // that a getter/allocation might still be straddling (so disable+reenable cancels it).
  if (!(g_catch_enabled && lease_was_live && next_catch)) ++g_catch_apply_gen;
  if (!(g_owned_enabled && lease_was_live && next_owned)) ++g_owned_apply_gen;
  g_catch_enabled = next_catch;
  g_owned_enabled = next_owned;
  g_lease_until_ms = allow ? now + clamped : 0;
  return wrong_thread ? PGR_IV_ERR_INVALID_ARG : PGR_IV_OK;
}

int api_disable(void) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_IV_ERR_NOT_INSTALLED;
  g_stop_serial.fetch_add(1, std::memory_order_acq_rel);
  std::lock_guard<std::mutex> lock(g_state_mutex);
  g_catch_enabled = false;
  g_owned_enabled = false;
  g_lease_until_ms = 0;
  ++g_catch_apply_gen;  // revoke any in-flight apply; the next owner-thread tick restores
  ++g_owned_apply_gen;
  return PGR_IV_OK;  // never frees here; the next owner-thread tick restores
}

// --- tick --------------------------------------------------------------------------
int api_tick(uint64_t proven_tid, int stopping) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_IV_ERR_NOT_INSTALLED;
  const uint64_t tid = current_tid();
  const uint64_t owner = g_owner_tid.load(std::memory_order_acquire);
  if (owner == 0 || tid != owner || proven_tid == 0 || tid != proven_tid) return PGR_IV_OK;
  if (stopping) g_stop_serial.fetch_add(1, std::memory_order_acq_rel);
  if (g_active_depth.load(std::memory_order_relaxed) != 0) return PGR_IV_OK;  // nested tick

  g_active_depth.fetch_add(1, std::memory_order_relaxed);
  uint64_t now = 0;
  const bool clock_ok = read_boottime_ms(&now);
  const bool stop = stopping != 0;

  // Catch record. The apply direction runs under a revocation scope (so a disable that
  // reenters during a setter cancels it); restore is always permitted.
  if (g_catch_present) {
    const uint64_t apply_gen = clock_ok ? snapshot_apply_gen(true) : 0;
    Ctx c;
    void *obj = g_catch_rec.text ? gc_get()(g_catch_rec.text) : nullptr;
    bool alive = obj != nullptr && is_alive(obj, c) && c.ok;
    if (stop || !alive || !clock_ok) {
      drop_catch();
    } else {
      Ctx rc;
      g_apply_scope = ApplyScope{true, true, apply_gen, apply_gen == 0};
      bool en = check_apply_gate(rc);
      if (rc.ok) render(g_catch_rec, en, rc);
      g_apply_scope = ApplyScope{};
      if (!rc.ok) {
        publish_catch_unavailable(rc.reason ? rc.reason : PGR_IV_REASON_RENDER, tid);
        drop_catch();
      }
    }
  }

  // Owned records (iterate by index; drops shift the array).
  for (uint32_t i = 0; i < g_owned_count;) {
    const uint64_t apply_gen = clock_ok ? snapshot_apply_gen(false) : 0;
    Ctx c;
    void *obj = g_owned_recs[i].text ? gc_get()(g_owned_recs[i].text) : nullptr;
    bool alive = obj != nullptr && is_alive(obj, c) && c.ok;
    if (stop || !alive || !clock_ok) {
      Record r = g_owned_recs[i];
      for (uint32_t j = i; j + 1 < g_owned_count; ++j) g_owned_recs[j] = g_owned_recs[j + 1];
      --g_owned_count;
      g_owned_recs[g_owned_count] = Record{};
      drop_record(r);
      continue;
    }
    Ctx rc;
    g_apply_scope = ApplyScope{true, false, apply_gen, apply_gen == 0};
    bool en = check_apply_gate(rc);
    if (rc.ok) render(g_owned_recs[i], en, rc);
    g_apply_scope = ApplyScope{};
    if (!rc.ok) {
      publish_owned_unavailable(rc.reason ? rc.reason : PGR_IV_REASON_RENDER, tid);
      Record r = g_owned_recs[i];
      for (uint32_t j = i; j + 1 < g_owned_count; ++j) g_owned_recs[j] = g_owned_recs[j + 1];
      --g_owned_count;
      g_owned_recs[g_owned_count] = Record{};
      drop_record(r);
      continue;
    }
    ++i;
  }

  g_active_depth.fetch_sub(1, std::memory_order_relaxed);
  return PGR_IV_OK;
}

// --- sample ------------------------------------------------------------------------
int api_sample(PgrIvDisplaySample *out) {
  if (out == nullptr) return PGR_IV_ERR_INVALID_ARG;
  std::lock_guard<std::mutex> lock(g_state_mutex);
  out->version = PGR_IV_SAMPLE_VERSION;
  out->size = sizeof(PgrIvDisplaySample);
  out->catch_serial = g_catch_serial;
  out->owned_serial = g_owned_serial;
  out->position_serial = g_position_serial;
  out->encounter_serial = g_encounter_serial;
  out->encounter_id = g_encounter_id;
  out->last_tid = g_last_tid;
  out->latitude = g_latitude;
  out->longitude = g_longitude;
  out->owned_root_count = owned_root_count();
  out->catch_active = g_catch_present ? 1u : 0u;
  out->owned_count = g_owned_count;
  out->fault = (g_catch_fault ? PGR_IV_FAULT_CATCH : 0) | (g_owned_fault ? PGR_IV_FAULT_OWNED : 0);
  out->catch_notice = g_catch_notice;
  out->catch_level_half = g_catch_level_half;
  out->catch_attack = g_catch_atk;
  out->catch_defense = g_catch_def;
  out->catch_stamina = g_catch_sta;
  out->catch_cp = g_catch_cp;
  out->catch_reason = g_catch_reason;
  out->owned_notice = g_owned_notice;
  out->owned_level_half = g_owned_level_half;
  out->owned_attack = g_owned_atk;
  out->owned_defense = g_owned_def;
  out->owned_stamina = g_owned_sta;
  out->owned_cp = g_owned_cp;
  out->owned_reason = g_owned_reason;
  std::memcpy(out->catch_label, g_catch_label, kLabelCap);
  std::memcpy(out->owned_label, g_owned_label, kLabelCap);
  return PGR_IV_OK;
}

// --- external-observer catch pass --------------------------------------------------
uint64_t api_begin_catch(void *self, void *map_pokemon, uint64_t proven_tid, void **refreshed_args) {
  if (refreshed_args == nullptr) return 0;
  if (!g_installed.load(std::memory_order_acquire)) return 0;
  // Only valid in shared mode; in standalone the Dobby hook_setup owns the pass.
  if (g_cfg.external_catch_observer == 0) return 0;
  const uint64_t tid = current_tid();
  if (proven_tid == 0 || tid != proven_tid) return 0;  // native re-validates the real caller tid
  const uint64_t token = catch_begin(self, map_pokemon, tid);
  if (token != 0) {
    // begin can restore the prior label through managed calls. Forward refreshed
    // rooted arguments to Gum before it executes the game original.
    void *plate = g_catch_frame.plate_root ? gc_get()(g_catch_frame.plate_root) : nullptr;
    void *map = g_catch_frame.map_root ? gc_get()(g_catch_frame.map_root) : nullptr;
    refreshed_args[0] = plate ? plate : self;
    refreshed_args[1] = map ? map : map_pokemon;
  }
  return token;
}

int api_end_catch(uint64_t token, uint64_t proven_tid) {
  if (!g_installed.load(std::memory_order_acquire)) return PGR_IV_ERR_NOT_INSTALLED;
  if (g_cfg.external_catch_observer == 0) return PGR_IV_ERR_INVALID_ARG;
  const uint64_t tid = current_tid();
  if (proven_tid == 0 || tid != proven_tid) return PGR_IV_ERR_INVALID_ARG;
  return catch_end(token, tid);
}

const PgrIvDisplayApi kApiTable = {
    PGR_IV_API_VERSION, static_cast<uint32_t>(sizeof(PgrIvDisplayApi)),
    &api_install,       &api_update,
    &api_tick,          &api_sample,
    &api_disable,       &api_begin_catch,
    &api_end_catch,
};

}  // namespace

extern "C" const void *pgrazor_iv_display_api(uint32_t version) {
  if (version != PGR_IV_API_VERSION) return nullptr;
  return &kApiTable;
}
