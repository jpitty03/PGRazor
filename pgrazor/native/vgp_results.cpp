#include "vgp_results.h"
#include <atomic>
#include <cstring>
#include <mutex>
#include <unistd.h>

// Built with -fno-exceptions / -fno-rtti (see CMakeLists.txt): no throw/try, no managed invokes. Every
// managed interaction here is a pure metadata read (field offsets/types/classes) or a GC root call.

namespace {
PgrVgpResultsConfig cfg{};
std::mutex installMutex;
std::atomic<bool> installed{false};
std::atomic<bool> busy{false};
std::atomic<bool> fault{false};
std::atomic<uint64_t> pollSerial{0}, completedSerial{0}, rejectedSerial{0}, errorSerial{0};

using New    = uintptr_t (*)(void *, int);
using Get    = void *(*)(uintptr_t);
using Free   = void (*)(uintptr_t);
using Class  = void *(*)(void *);
using Parent = void *(*)(void *);
using FieldByName = void *(*)(void *, const char *);
using FieldOffset = size_t (*)(void *);
using FieldType   = void *(*)(void *);
using ClassFromType = void *(*)(void *);
using IsValueType = bool (*)(void *);
using FieldGetValue = void (*)(void *, void *, void *);

// A single GC root. Rooting an object immediately is the only way to touch it across another managed
// call; here there is only ever one promise root plus (on DONE) one value root which is RELEASED to the
// caller. Every other path frees its root in the destructor, so the live root count returns to zero.
struct Root {
  uintptr_t h = 0;
  Root() = default;
  Root(const Root &) = delete;
  Root &operator=(const Root &) = delete;
  ~Root() {
    if (h) reinterpret_cast<Free>(cfg.gchandle_free)(h);
  }
  bool set(void *o) {
    if (!o) return false;
    h = reinterpret_cast<New>(cfg.gchandle_new)(o, 0);
    return h != 0;
  }
  void *get() const { return h ? reinterpret_cast<Get>(cfg.gchandle_get_target)(h) : nullptr; }
  // Transfer ownership to the caller: return the handle and stop tracking it (the destructor no-ops).
  uintptr_t release() {
    uintptr_t out = h;
    h = 0;
    return out;
  }
};

void *objectClass(void *o) { return reinterpret_cast<Class>(cfg.object_get_class)(o); }

// Resolve a field by name, walking the class hierarchy up to PGR_VR_PARENT_MAX parents (existing
// vgpField semantics). Returns the FieldInfo* on the first match, or null if not present. A match with
// an offset outside the 16..512 guard is a hard shape error and sets *bad.
void *resolveField(void *klass, void *name, bool *bad) {
  *bad = false;
  void *k = klass;
  for (int i = 0; i < PGR_VR_PARENT_MAX && k; i++, k = reinterpret_cast<Parent>(cfg.class_get_parent)(k)) {
    void *f = reinterpret_cast<FieldByName>(cfg.class_get_field_from_name)(k, static_cast<const char *>(name));
    if (f) {
      size_t off = reinterpret_cast<FieldOffset>(cfg.field_get_offset)(f);
      if (off < static_cast<size_t>(PGR_VR_OFFSET_MIN) || off > static_cast<size_t>(PGR_VR_OFFSET_MAX)) {
        *bad = true;
        return nullptr;
      }
      return f;
    }
  }
  return nullptr;
}

void *fieldClass(void *f) {
  return reinterpret_cast<ClassFromType>(cfg.class_from_type)(reinterpret_cast<FieldType>(cfg.field_get_type)(f));
}

// A bool field is exactly System.Boolean -- never over-read a wider type as a byte.
bool isBool(void *f) { return fieldClass(f) == cfg.class_bool; }

// completedValue must be a reference (object) field -- never dereference a value type as a pointer.
bool isReference(void *f) {
  void *k = fieldClass(f);
  return k && !reinterpret_cast<IsValueType>(cfg.class_is_valuetype)(k);
}

bool readBool(void *obj, void *f) {
  uint8_t b = 0;
  reinterpret_cast<FieldGetValue>(cfg.field_get_value)(obj, f, &b);
  return b != 0;
}
void *readPointer(void *obj, void *f) {
  void *o = nullptr;
  reinterpret_cast<FieldGetValue>(cfg.field_get_value)(obj, f, &o);
  return o;
}
int32_t readInt32(void *obj, void *f) {
  int32_t v = 0;
  reinterpret_cast<FieldGetValue>(cfg.field_get_value)(obj, f, &v);
  return v;
}

int install(const PgrVgpResultsConfig *c) {
  if (!c || c->version != PGR_VR_CONFIG_VERSION || c->size != sizeof(*c)) return PGR_VR_ERR_NULL_ARG;
  // 20 validated pointers follow the 8-byte header; every one is required.
  void *pointers[20];
  memcpy(pointers, reinterpret_cast<const char *>(c) + 8, sizeof(pointers));
  for (void *p : pointers)
    if (!p) return PGR_VR_ERR_NULL_ARG;
  std::lock_guard<std::mutex> guard(installMutex);
  if (installed.load()) return memcmp(c, &cfg, sizeof(cfg)) == 0 ? 0 : PGR_VR_ERR_SHAPE;
  cfg = *c;
  installed.store(true, std::memory_order_release);
  return 0;
}

int poll(void *promise, uint32_t phase, uint64_t owner, PgrVgpResultsResult *out) {
  if (!out) return PGR_VR_ERR_NULL_ARG;
  *out = {PGR_VR_RESULT_VERSION, sizeof(*out), 0, 0, 0, 0, 0};
  if (!installed.load(std::memory_order_acquire)) return PGR_VR_ERR_NOT_INSTALLED;
  if (phase != PGR_VR_PHASE_ENCOUNTER && phase != PGR_VR_PHASE_CATCH && phase != PGR_VR_PHASE_SPIN)
    return PGR_VR_ERR_BAD_PHASE;
  // Owner gate: must be the proven Unity thread. A missing target or a foreign thread is PENDING, not an
  // error -- the host simply keeps waiting. VGP mask/lease is deliberately NOT consulted here.
  if (!promise || !owner || owner != static_cast<uint64_t>(gettid())) return PGR_VR_PENDING;

  bool idle = false;
  if (!busy.compare_exchange_strong(idle, true)) return PGR_VR_ERR_REENTRANT;
  struct BusyExit {
    ~BusyExit() { busy.store(false); }
  } exit;

  pollSerial.fetch_add(1);

  // Root the promise immediately before reading it.
  Root p;
  if (!p.set(promise)) {
    fault.store(true);
    errorSerial.fetch_add(1);
    return PGR_VR_ERR_GCHANDLE;
  }
  void *klass = objectClass(p.get());
  if (!klass) {
    fault.store(true);
    errorSerial.fetch_add(1);
    return PGR_VR_ERR_SHAPE;
  }

  // resolveField resets *bad on entry, so short-circuit once any lookup trips the offset guard.
  bool bad = false;
  void *fComplete = resolveField(klass, cfg.name_complete, &bad);
  void *fError = bad ? nullptr : resolveField(klass, cfg.name_error, &bad);
  void *fValue = bad ? nullptr : resolveField(klass, cfg.name_value, &bad);
  void *fStatus = bad ? nullptr : resolveField(klass, cfg.name_status, &bad);  // optional (absence OK)
  if (bad || !fComplete || !fError || !fValue) {
    fault.store(true);
    errorSerial.fetch_add(1);
    return PGR_VR_ERR_SHAPE;
  }
  // Type-validate via metadata so a wrong shape can never over-read a field's bytes.
  if (!isBool(fComplete) || !isBool(fError) || (fStatus && !isBool(fStatus)) || !isReference(fValue)) {
    fault.store(true);
    errorSerial.fetch_add(1);
    return PGR_VR_ERR_SHAPE;
  }

  // Error dominates completed: a rejected promise (errorCalled, or the optional statusErrorCalled) is
  // REJECTED regardless of completeCalled.
  if (readBool(p.get(), fError) || (fStatus && readBool(p.get(), fStatus))) {
    out->state = 2;
    rejectedSerial.fetch_add(1);
    return PGR_VR_REJECTED;
  }
  if (!readBool(p.get(), fComplete)) return PGR_VR_PENDING;

  void *value = readPointer(p.get(), fValue);
  // A null resolved value is treated as an error (host: "Request rejected"), matching value.isNull().
  if (!value) {
    out->state = 2;
    rejectedSerial.fetch_add(1);
    return PGR_VR_REJECTED;
  }

  // Root the resolved value immediately; this is the single owned root transferred to the caller.
  Root v;
  if (!v.set(value)) {
    fault.store(true);
    errorSerial.fetch_add(1);
    return PGR_VR_ERR_GCHANDLE;
  }

  void *current = v.get();
  if (!current) return PGR_VR_ERR_GCHANDLE;
  void *vk = objectClass(current);
  if (phase == PGR_VR_PHASE_CATCH) {
    // Exact RESP only -> read the catch status; otherwise status stays unknown and success stays false.
    if (vk == cfg.class_resp) {
      int32_t st = readInt32(v.get(), cfg.f_catch_status);
      out->status = st;
      out->status_known = 1;
      out->success = (st == cfg.catch_success) ? 1 : 0;
    }
  } else if (phase == PGR_VR_PHASE_SPIN) {
    // Exact SPINRESULT only -> success iff Result == spin_success; status is not reported for spins.
    if (vk == cfg.class_spinresult)
      out->success = (readInt32(v.get(), cfg.f_spin_result) == cfg.spin_success) ? 1 : 0;
  }
  // Encounter: any non-null resolved value is valid; no status/success is computed.

  out->state = 1;
  out->value_handle = v.release();  // ownership to the caller; p is freed by its destructor at scope end
  completedSerial.fetch_add(1);
  return PGR_VR_DONE;
}

int sample(PgrVgpResultsSample *out) {
  if (!out) return PGR_VR_ERR_NULL_ARG;
  out->version = PGR_VR_SAMPLE_VERSION;
  out->size = sizeof(*out);
  out->poll_serial = pollSerial.load();
  out->completed_serial = completedSerial.load();
  out->rejected_serial = rejectedSerial.load();
  out->error_serial = errorSerial.load();
  out->fault = fault.load() ? 1 : 0;
  out->reserved = 0;
  return 0;
}

const PgrVgpResultsApi api = {PGR_VR_API_VERSION, sizeof(PgrVgpResultsApi), install, poll, sample};
}  // namespace

extern "C" const void *pgrazor_vgp_results_api(uint32_t version) {
  return version == PGR_VR_API_VERSION ? &api : nullptr;
}
