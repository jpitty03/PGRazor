#include "vgp_candidates.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <time.h>
#include <unistd.h>

// Feature 29: synchronous, read-only per-candidate inspection. No hooks, no callbacks, no retained
// managed state beyond the single fort handle transferred to the caller on an eligible stop. Every
// temporary managed value is rooted on the owner thread and freed before return.

namespace {

PgrVgpCandidateConfig cfg{};
std::mutex installMutex;
std::atomic<bool> installed{false};
std::atomic<bool> busy{false};

using New = uintptr_t (*)(void *, int);
using Get = void *(*)(uintptr_t);
using FreeH = void (*)(uintptr_t);
using Class = void *(*)(void *);
using Invoke = void *(*)(void *, void *, void **, void **);
using Unbox = void *(*)(void *);
using Virtual = void *(*)(void *, void *);
using FieldGet = void (*)(void *, void *, void *);

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
  // Relinquish ownership of the handle to the caller (used only for the transferred fort handle).
  uintptr_t release() { uintptr_t out = h; h = 0; return out; }
};

bool exact(const Root &r, void *klass) {
  void *o = r.get();
  return o && reinterpret_cast<Class>(cfg.object_get_class)(o) == klass;
}
void *unbox(const Root &r) {
  void *o = r.get();
  return o ? reinterpret_cast<Unbox>(cfg.object_unbox)(o) : nullptr;
}

// Invoke a resolved method. Roots the (non-null) return value. err is set on a managed throw
// (PGR_VC_ERR_MANAGED) or a null/unrootable return (PGR_VC_ERR_GCHANDLE). receiver, if given, must be
// live. No raw managed pointer survives across this call except through the returned Root.
bool call(void *method, const Root *receiver, void **args, Root &out, int &err) {
  void *o = receiver ? receiver->get() : nullptr;
  if (receiver && !o) { err = PGR_VC_ERR_GCHANDLE; return false; }
  void *exc = nullptr;
  void *value = reinterpret_cast<Invoke>(cfg.runtime_invoke)(method, o, args, &exc);
  if (exc) { err = PGR_VC_ERR_MANAGED; return false; }
  if (!out.set(value)) { err = PGR_VC_ERR_GCHANDLE; return false; }
  return true;
}

// Invoke a virtual getter resolved per object. Same error contract as call().
bool callVirtual(void *base, const Root &receiver, Root &out, int &err, bool allowNull = false) {
  void *o = receiver.get();
  if (!o) { err = PGR_VC_ERR_GCHANDLE; return false; }
  void *method = reinterpret_cast<Virtual>(cfg.object_get_virtual_method)(o, base);
  if (!method) { err = PGR_VC_ERR_METADATA; return false; }
  void *exc = nullptr;
  void *value = reinterpret_cast<Invoke>(cfg.runtime_invoke)(method, o, nullptr, &exc);
  if (exc) { err = PGR_VC_ERR_MANAGED; return false; }
  if (!value && allowNull) return true;
  if (!out.set(value)) { err = PGR_VC_ERR_GCHANDLE; return false; }
  return true;
}

// Read a boxed System.Boolean result; -1 on error (err set), else 0/1.
int boolValue(const Root &r, int &err) {
  if (!exact(r, cfg.class_bool)) { err = PGR_VC_ERR_METADATA; return -1; }
  void *p = unbox(r);
  if (!p) { err = PGR_VC_ERR_METADATA; return -1; }
  return *static_cast<uint8_t *>(p) != 0 ? 1 : 0;
}

// Invoke a virtual boolean getter; -1 on error (err set), else 0/1.
int virtualBool(void *base, const Root &receiver, int &err) {
  Root r;
  if (!callVirtual(base, receiver, r, err)) return -1;
  return boolValue(r, err);
}

// Great-circle metres between (plat, plon) and (clat, clon). Matches vgp.js vgpDistance exactly,
// including the lack of a lower clamp (a tiny negative under-root yields NaN).
double haversine(double plat, double plon, double clat, double clon) {
  const double d = M_PI / 180.0;
  const double a = plat * d, b = clat * d;
  const double x = (clat - plat) * d, y = (clon - plon) * d;
  const double z = std::pow(std::sin(x / 2), 2) + std::cos(a) * std::cos(b) * std::pow(std::sin(y / 2), 2);
  return 6371000.0 * 2.0 * std::asin(std::sqrt(std::fmin(1.0, z)));
}

// Append the decimal digits of v to buf (at least 20 bytes). Returns the count written.
uint32_t writeDecimal(uint64_t v, char *buf) {
  char tmp[20];
  int n = 0;
  do { tmp[n++] = char('0' + v % 10); v /= 10; } while (v);
  for (int i = 0; i < n; ++i) buf[i] = tmp[n - 1 - i];
  return uint32_t(n);
}

// Convert `units` UTF-16 code units to UTF-8 in dst (capacity `cap` bytes). Surrogate pairs combine;
// an unpaired/invalid surrogate becomes U+FFFD. Returns bytes written, or SIZE_MAX if it would
// overflow cap (never expected for units <= 256, which fit in 1024 bytes).
size_t utf16ToUtf8(const uint16_t *src, uint32_t units, char *dst, size_t cap) {
  size_t out = 0;
  auto put = [&](uint8_t byte) -> bool { if (out >= cap) return false; dst[out++] = char(byte); return true; };
  for (uint32_t i = 0; i < units; ++i) {
    uint32_t cp = src[i];
    if (cp >= 0xD800 && cp <= 0xDBFF) {
      uint32_t lo = (i + 1 < units) ? src[i + 1] : 0;
      if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); ++i; }
      else cp = 0xFFFD;
    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
      cp = 0xFFFD;
    }
    bool ok;
    if (cp < 0x80) ok = put(uint8_t(cp));
    else if (cp < 0x800) ok = put(uint8_t(0xC0 | (cp >> 6))) && put(uint8_t(0x80 | (cp & 0x3F)));
    else if (cp < 0x10000)
      ok = put(uint8_t(0xE0 | (cp >> 12))) && put(uint8_t(0x80 | ((cp >> 6) & 0x3F))) && put(uint8_t(0x80 | (cp & 0x3F)));
    else
      ok = put(uint8_t(0xF0 | (cp >> 18))) && put(uint8_t(0x80 | ((cp >> 12) & 0x3F))) &&
           put(uint8_t(0x80 | ((cp >> 6) & 0x3F))) && put(uint8_t(0x80 | (cp & 0x3F)));
    if (!ok) return SIZE_MAX;
  }
  return out;
}

int install(const PgrVgpCandidateConfig *c) {
  if (!c) return PGR_VC_ERR_NULL_ARG;
  if (c->version != PGR_VC_CONFIG_VERSION) return PGR_VC_ERR_BAD_VERSION;
  if (c->size != sizeof(*c)) return PGR_VC_ERR_BAD_SIZE;
  void *pointers[24];
  std::memcpy(pointers, reinterpret_cast<const char *>(c) + 8, sizeof(pointers));
  for (void *p : pointers) if (!p) return PGR_VC_ERR_MISSING_PTR;
  std::lock_guard<std::mutex> guard(installMutex);
  if (installed.load()) return std::memcmp(c, &cfg, sizeof(cfg)) == 0 ? 0 : PGR_VC_ERR_RECONFIG;
  cfg = *c;
  installed.store(true, std::memory_order_release);
  return 0;
}

int inspect(const PgrVgpCandidateRequest *req, PgrVgpCandidateResult *out) {
  if (!out) return PGR_VC_ERR_NULL_ARG;
  *out = {PGR_VC_RESULT_VERSION, uint32_t(sizeof(*out)), 0, PGR_VC_REASON_INVALID, 0, 0, 0, {}};
  if (!req) return PGR_VC_ERR_NULL_ARG;
  if (req->version != PGR_VC_REQUEST_VERSION) return PGR_VC_ERR_BAD_VERSION;
  if (req->size != sizeof(*req)) return PGR_VC_ERR_BAD_SIZE;
  if (!installed.load(std::memory_order_acquire)) return PGR_VC_ERR_NOT_INSTALLED;
  if (req->action != PGR_VC_ACTION_CATCH && req->action != PGR_VC_ACTION_SPIN && req->action != PGR_VC_ACTION_SMART_CATCH) return PGR_VC_ERR_BAD_ACTION;
  if (!req->target) return PGR_VC_ERR_NULL_ARG;
  if (!req->owner_tid || req->owner_tid != static_cast<uint64_t>(gettid())) return PGR_VC_ERR_THREAD;
  if (!std::isfinite(req->lat) || !std::isfinite(req->lon) ||
      std::fabs(req->lat) > 90.0 || std::fabs(req->lon) > 180.0)
    return PGR_VC_ERR_COORDS;

  const uint64_t start = now();
  if (!start) return PGR_VC_ERR_CLOCK_FAILURE;
  const uint64_t lease = req->lease_ms > PGR_VC_MAX_LEASE_MS ? PGR_VC_MAX_LEASE_MS : req->lease_ms;
  if (!lease) { out->reason = PGR_VC_REASON_INVALID; return PGR_VC_NOT; }  // closed window: benign skip
  const uint64_t deadline = start + lease;

  bool idle = false;
  if (!busy.compare_exchange_strong(idle, true)) return PGR_VC_ERR_THREAD;  // reentrant inspect
  struct BusyExit { ~BusyExit() { busy.store(false); } } exit;

  int err = 0;
  const bool isCatch = req->action != PGR_VC_ACTION_SPIN;

  Root target, manual;
  if (!target.set(req->target)) return PGR_VC_ERR_GCHANDLE;
  if (isCatch && req->manual && !manual.set(req->manual)) return PGR_VC_ERR_GCHANDLE;
  if (!exact(target, isCatch ? cfg.class_wild : cfg.class_stop)) { out->reason = PGR_VC_REASON_INVALID; return PGR_VC_NOT; }

  // Liveness: static alive(obj) -> Boolean.
  {
    Root aliveBox;
    void *self = target.get();
    if (!self) return PGR_VC_ERR_GCHANDLE;
    void *args[] = {self};
    if (!call(cfg.m_alive, nullptr, args, aliveBox, err)) return err;
    int live = boolValue(aliveBox, err);
    if (live < 0) return err;
    if (!live) { out->reason = PGR_VC_REASON_INVALID; return PGR_VC_NOT; }
  }

  if (isCatch) {
    // Manual-encounter exclusion by pointer identity (before any getter, matching vgp.js).
    if (req->manual) {
      if (manual.get() == target.get()) { out->reason = PGR_VC_REASON_MANUAL; return PGR_VC_NOT; }
    }
    // VisibleOnMap (virtual).
    int visible = virtualBool(cfg.m_visible, target, err);
    if (visible < 0) return err;
    if (!visible) { out->reason = PGR_VC_REASON_HIDDEN; return PGR_VC_NOT; }
    // Location (non-virtual) -> Niantic.LatLng (lat, lon); 40 m range.
    {
      Root loc;
      if (!call(cfg.m_location, &target, nullptr, loc, err)) return err;
      if (!exact(loc, cfg.class_latlng)) return PGR_VC_ERR_METADATA;
      void *p = unbox(loc);
      if (!p) return PGR_VC_ERR_METADATA;
      double clat, clon;
      std::memcpy(&clat, static_cast<char *>(p), sizeof(double));
      std::memcpy(&clon, static_cast<char *>(p) + 8, sizeof(double));
      if (!std::isfinite(clat) || !std::isfinite(clon) || std::fabs(clat)>90 || std::fabs(clon)>180) { out->reason=PGR_VC_REASON_OUT_OF_RANGE; return PGR_VC_NOT; }
      double dist = haversine(req->lat, req->lon, clat, clon);
      if (!std::isfinite(dist) || (req->action==PGR_VC_ACTION_CATCH && dist > 40.0)) { out->reason = PGR_VC_REASON_OUT_OF_RANGE; return PGR_VC_NOT; }
    }
    // EncounterId (non-virtual) -> UInt64; key = 'p'<decimal>.
    uint64_t id;
    {
      Root idBox;
      if (!call(cfg.m_id, &target, nullptr, idBox, err)) return err;
      if (!exact(idBox, cfg.class_uint64)) return PGR_VC_ERR_METADATA;
      void *p = unbox(idBox);
      if (!p) return PGR_VC_ERR_METADATA;
      std::memcpy(&id, p, sizeof(uint64_t));
    }
    if (now() >= deadline) { out->reason = PGR_VC_REASON_INVALID; return PGR_VC_NOT; }  // expired mid-call
    out->key[0] = 'p';
    out->key_len = 1 + writeDecimal(id, out->key + 1);
    out->key[out->key_len] = '\0';
    out->eligible = 1;
    out->reason = PGR_VC_REASON_NONE;
    return PGR_VC_ELIGIBLE;  // fort_handle stays 0 for a catch
  }

  // Spin: four virtual predicates, then the fort + its FortId string.
  int v;
  if ((v = virtualBool(cfg.m_active, target, err)) < 0) return err;
  if (!v) { out->reason = PGR_VC_REASON_INELIGIBLE; return PGR_VC_NOT; }
  if ((v = virtualBool(cfg.m_range, target, err)) < 0) return err;
  if (!v) { out->reason = PGR_VC_REASON_INELIGIBLE; return PGR_VC_NOT; }
  if ((v = virtualBool(cfg.m_closed, target, err)) < 0) return err;
  if (v) { out->reason = PGR_VC_REASON_INELIGIBLE; return PGR_VC_NOT; }
  if ((v = virtualBool(cfg.m_cooling, target, err)) < 0) return err;
  if (v) { out->reason = PGR_VC_REASON_INELIGIBLE; return PGR_VC_NOT; }

  Root fort;
  if (!callVirtual(cfg.m_fort, target, fort, err, true)) return err;
  if (!exact(fort, cfg.class_fort)) { out->reason = PGR_VC_REASON_INELIGIBLE; return PGR_VC_NOT; }

  // FortId System.String: read the reference field, root it BEFORE any further allocation, re-fetch
  // its (possibly relocated) address, then copy the chars with no intervening managed call.
  void *rawString = nullptr;
  {
    void *fortObj = fort.get();
    if (!fortObj) return PGR_VC_ERR_GCHANDLE;
    reinterpret_cast<FieldGet>(cfg.field_get_value)(fortObj, cfg.f_fort_id, &rawString);
  }
  if (!rawString) { out->reason = PGR_VC_REASON_INELIGIBLE; return PGR_VC_NOT; }
  Root fortId;
  if (!fortId.set(rawString)) return PGR_VC_ERR_GCHANDLE;
  void *str = fortId.get();
  if (!str) return PGR_VC_ERR_GCHANDLE;
  int32_t length;
  std::memcpy(&length, static_cast<char *>(str) + 16, sizeof(int32_t));
  if (length < 1 || length > PGR_VC_MAX_FORT_UNITS) { out->reason = PGR_VC_REASON_INELIGIBLE; return PGR_VC_NOT; }
  const uint16_t *chars = reinterpret_cast<const uint16_t *>(static_cast<char *>(str) + 20);
  size_t utf8 = utf16ToUtf8(chars, uint32_t(length), out->key + 1, PGR_VC_KEY_CAPACITY - 2);
  if (utf8 == SIZE_MAX) { out->reason = PGR_VC_REASON_INELIGIBLE; return PGR_VC_NOT; }

  if (now() >= deadline) { out->reason = PGR_VC_REASON_INVALID; return PGR_VC_NOT; }  // expired mid-call

  // Transfer an OWN fort handle to the caller. This allocation may relocate rawString, but the string
  // bytes were already copied above, so nothing stale is read after it.
  uintptr_t owned = reinterpret_cast<New>(cfg.gchandle_new)(fort.get(), 0);
  if (!owned) return PGR_VC_ERR_GCHANDLE;

  out->key[0] = 's';
  out->key_len = uint32_t(1 + utf8);
  out->key[out->key_len] = '\0';
  out->fort_handle = owned;
  out->eligible = 1;
  out->reason = PGR_VC_REASON_NONE;
  return PGR_VC_ELIGIBLE;
}

const PgrVgpCandidateApi api = {PGR_VC_API_VERSION, uint32_t(sizeof(PgrVgpCandidateApi)), install, inspect};

}  // namespace

extern "C" const void *pgrazor_vgp_candidates_api(uint32_t version) {
  return version == PGR_VC_API_VERSION ? &api : nullptr;
}
