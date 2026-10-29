#include "vgp_results.h"
#include "pgrazor_native_api.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unistd.h>

// Fake il2cpp metadata: a class hierarchy with name-based field lookup walking parents, plus a flat
// object model (first 8 bytes = class pointer, fields at fixed offsets read with field_get_value). The
// real library under test is native/vgp_results.cpp; this exercises the production API exactly as the
// host adapter calls it. Classes are just sentinel addresses; a field carries its offset/size/type.

namespace {
// Sentinel "classes".
int promiseBase, promiseDerived, respClass, spinClass, unknownClass, boolClass, intClass, objClass;

struct Field { size_t offset; size_t size; void *type; };

// name-based field table (declared fields per class) and parent links.
std::unordered_map<void *, std::unordered_map<std::string, Field *>> declared;
std::unordered_map<void *, void *> parentOf;

Field fComplete{24, 1, &boolClass}, fError{25, 1, &boolClass}, fStatusBool{26, 1, &boolClass},
    fValue{32, 8, &objClass};
Field fCatchStatus{16, 4, &intClass}, fSpinResult{16, 4, &intClass};

// --- fake runtime exports ---------------------------------------------------------------------------
std::unordered_map<uintptr_t, void *> roots;
uintptr_t serial = 0;
int rootCalls = 0, failRoot = 0;
std::function<void(void *&)> relocateOnRoot;

uintptr_t gcNew(void *o, int) {
  ++rootCalls;
  if (!o) return 0;
  if (rootCalls == failRoot) return 0;
  if (relocateOnRoot) relocateOnRoot(o);
  roots[++serial] = o;
  return serial;
}
void *gcGet(uintptr_t h) {
  auto i = roots.find(h);
  return i == roots.end() ? nullptr : i->second;
}
void gcFree(uintptr_t h) { assert(roots.erase(h) == 1); }
void *objClassOf(void *o) { return *reinterpret_cast<void **>(o); }
void *parent(void *k) {
  auto i = parentOf.find(k);
  return i == parentOf.end() ? nullptr : i->second;
}
void *fieldByName(void *k, const char *name) {
  auto c = declared.find(k);
  if (c == declared.end()) return nullptr;
  auto f = c->second.find(name);
  return f == c->second.end() ? nullptr : static_cast<void *>(f->second);
}
size_t fieldOffset(void *f) { return static_cast<Field *>(f)->offset; }
void *fieldType(void *f) { return static_cast<Field *>(f)->type; }
void *classFromType(void *t) { return t; }  // type == class in this fake
bool isValueType(void *k) { return k == &boolClass || k == &intClass; }
void fieldGetValue(void *obj, void *f, void *out) {
  Field *fd = static_cast<Field *>(f);
  memcpy(out, reinterpret_cast<char *>(obj) + fd->offset, fd->size);
}

void setup() {
  parentOf[&promiseDerived] = &promiseBase;
  declared[&promiseBase] = {{"completeCalled", &fComplete},
                            {"errorCalled", &fError},
                            {"statusErrorCalled", &fStatusBool},
                            {"completedValue", &fValue}};
}
void reset() {
  assert(roots.empty());
  rootCalls = failRoot = 0;
  relocateOnRoot=nullptr;
}

// A fake managed object: 64-byte buffer, class at [0], fields at their declared offsets.
struct Obj {
  char buf[64]{};
  explicit Obj(void *klass) { *reinterpret_cast<void **>(buf) = klass; }
  void setBool(const Field &f, bool v) { buf[f.offset] = v ? 1 : 0; }
  void setPtr(const Field &f, void *v) { memcpy(buf + f.offset, &v, 8); }
  void setI32(const Field &f, int32_t v) { memcpy(buf + f.offset, &v, 4); }
  void *ptr() { return buf; }
};
}  // namespace

int main(int argc, char **argv) {
  assert(argc == 2);
  void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  assert(lib);
  auto load = reinterpret_cast<const void *(*)(uint32_t)>(dlsym(lib, "pgrazor_native_api"));
  assert(load);
  auto *core = static_cast<const pgrazor_native_api_v2 *>(load(2));
  assert(core);
  assert(!core->query_feature(30, 2));
  auto *api = static_cast<const PgrVgpResultsApi *>(core->query_feature(30, 1));
  assert(api && api->version == 1 && api->size == 32);

  setup();
  PgrVgpResultsConfig cfg{1,
                          sizeof(cfg),
                          reinterpret_cast<void *>(gcNew),
                          reinterpret_cast<void *>(gcGet),
                          reinterpret_cast<void *>(gcFree),
                          reinterpret_cast<void *>(objClassOf),
                          reinterpret_cast<void *>(parent),
                          reinterpret_cast<void *>(fieldByName),
                          reinterpret_cast<void *>(fieldOffset),
                          reinterpret_cast<void *>(fieldType),
                          reinterpret_cast<void *>(classFromType),
                          reinterpret_cast<void *>(isValueType),
                          reinterpret_cast<void *>(fieldGetValue),
                          &boolClass,
                          &respClass,
                          &spinClass,
                          const_cast<char *>("completeCalled"),
                          const_cast<char *>("errorCalled"),
                          const_cast<char *>("completedValue"),
                          const_cast<char *>("statusErrorCalled"),
                          &fCatchStatus,
                          &fSpinResult,
                          /*catch_success*/ 100,
                          /*spin_success*/ 7};

  // install: null / bad header rejected; good install idempotent; reconfig refused.
  assert(api->install(nullptr) < 0);
  {
    PgrVgpResultsConfig bad = cfg;
    bad.object_get_class = nullptr;
    assert(api->install(&bad) < 0);  // required pointer null
  }
  assert(api->install(&cfg) == 0);
  assert(api->install(&cfg) == 0);  // idempotent
  {
    PgrVgpResultsConfig other = cfg;
    other.catch_success = 1;
    assert(api->install(&other) < 0);  // reconfigure refused
  }

  const uint64_t tid = gettid();
  PgrVgpResultsResult out{};

  // --- phase/arg validation ----------------------------------------------------------------------
  reset();
  assert(api->poll(nullptr, PGR_VR_PHASE_CATCH, tid, nullptr) == PGR_VR_ERR_NULL_ARG);
  reset();
  Obj encPromise(&promiseDerived);
  assert(api->poll(encPromise.ptr(), 0, tid, &out) == PGR_VR_ERR_BAD_PHASE && out.state == 0);
  assert(api->poll(encPromise.ptr(), 9, tid, &out) == PGR_VR_ERR_BAD_PHASE);

  // --- owner gate: wrong tid / null target -> PENDING, no roots ----------------------------------
  reset();
  assert(api->poll(encPromise.ptr(), PGR_VR_PHASE_CATCH, tid + 1, &out) == PGR_VR_PENDING && rootCalls == 0);
  assert(api->poll(nullptr, PGR_VR_PHASE_CATCH, tid, &out) == PGR_VR_PENDING && rootCalls == 0);
  assert(api->poll(encPromise.ptr(), PGR_VR_PHASE_CATCH, 0, &out) == PGR_VR_PENDING);

  // --- pending: nothing completed yet ------------------------------------------------------------
  reset();
  assert(api->poll(encPromise.ptr(), PGR_VR_PHASE_ENCOUNTER, tid, &out) == PGR_VR_PENDING && out.state == 0);
  reset();  // zero roots after a pending poll

  // --- rejected: errorCalled, statusErrorCalled, and null resolved value -------------------------
  {
    reset();
    Obj p(&promiseDerived);
    p.setBool(fError, true);
    p.setBool(fComplete, true);  // error dominates completed
    assert(api->poll(p.ptr(), PGR_VR_PHASE_CATCH, tid, &out) == PGR_VR_REJECTED && out.state == 2 &&
           out.value_handle == 0);
    reset();
  }
  {
    reset();
    Obj p(&promiseDerived);
    p.setBool(fStatusBool, true);
    assert(api->poll(p.ptr(), PGR_VR_PHASE_CATCH, tid, &out) == PGR_VR_REJECTED);
    reset();
  }
  {
    reset();
    Obj p(&promiseDerived);
    p.setBool(fComplete, true);
    p.setPtr(fValue, nullptr);  // completed but null value -> error
    assert(api->poll(p.ptr(), PGR_VR_PHASE_CATCH, tid, &out) == PGR_VR_REJECTED && out.value_handle == 0);
    reset();
  }

  // --- catch success / escape / unknown class ----------------------------------------------------
  {
    reset();
    Obj resp(&respClass);
    resp.setI32(fCatchStatus, 100);  // == catch_success
    Obj p(&promiseDerived);
    p.setBool(fComplete, true);
    p.setPtr(fValue, resp.ptr());
    assert(api->poll(p.ptr(), PGR_VR_PHASE_CATCH, tid, &out) == PGR_VR_DONE);
    assert(out.state == 1 && out.status_known == 1 && out.status == 100 && out.success == 1);
    assert(out.value_handle != 0);
    assert(gcGet(out.value_handle) == resp.ptr());  // the rooted value is the response
    gcFree(out.value_handle);  // host owns it: free to return roots to zero
    reset();
  }
  {
    reset();
    Obj resp(&respClass);
    resp.setI32(fCatchStatus, 42);  // escape (!= catch_success)
    Obj p(&promiseDerived);
    p.setBool(fComplete, true);
    p.setPtr(fValue, resp.ptr());
    assert(api->poll(p.ptr(), PGR_VR_PHASE_CATCH, tid, &out) == PGR_VR_DONE);
    assert(out.status_known == 1 && out.status == 42 && out.success == 0);  // preserve unsuccessful result
    gcFree(out.value_handle);
    reset();
  }
  {
    reset();
    Obj other(&unknownClass);  // catch phase, value not RESP -> status unknown, success false
    Obj p(&promiseDerived);
    p.setBool(fComplete, true);
    p.setPtr(fValue, other.ptr());
    assert(api->poll(p.ptr(), PGR_VR_PHASE_CATCH, tid, &out) == PGR_VR_DONE);
    assert(out.status_known == 0 && out.status == 0 && out.success == 0 && out.value_handle != 0);
    gcFree(out.value_handle);
    reset();
  }

  // completedValue may relocate while gchandle_new acquires its root. Class and status must be read
  // from the returned handle, never from the raw value read out of the promise before that call.
  for (bool spinPhase : {false, true}) {
    for(bool poisonClass : {false,true}) {
      reset();
      Obj oldValue(spinPhase?static_cast<void*>(&spinClass):static_cast<void*>(&respClass));
      Obj movedValue(spinPhase?static_cast<void*>(&spinClass):static_cast<void*>(&respClass));
      const Field &status=spinPhase?fSpinResult:fCatchStatus;
      const int32_t success=spinPhase?7:100;
      oldValue.setI32(status,success);movedValue.setI32(status,success);
      Obj p(&promiseDerived);p.setBool(fComplete,true);p.setPtr(fValue,oldValue.ptr());
      bool moved=false;
      relocateOnRoot=[&](void *&o) {
        if(o!=oldValue.ptr())return;
        moved=true;o=movedValue.ptr();
        // All existing managed references move too; old memory remains poisoned and readable.
        for(auto &entry:roots)if(entry.second==oldValue.ptr())entry.second=movedValue.ptr();
        p.setPtr(fValue,movedValue.ptr());oldValue.setI32(status,-123456);
        if(poisonClass)*reinterpret_cast<void**>(oldValue.ptr())=&unknownClass;
      };
      const uint32_t phase=spinPhase?PGR_VR_PHASE_SPIN:PGR_VR_PHASE_CATCH;
      assert(api->poll(p.ptr(),phase,tid,&out)==PGR_VR_DONE&&moved);
      assert(out.success==1&&out.value_handle!=0&&gcGet(out.value_handle)==movedValue.ptr());
      if(!spinPhase)assert(out.status_known==1&&out.status==100);
      assert(roots.size()==1);gcFree(out.value_handle);reset();
    }
  }

  // --- spin success / failure / unknown class ----------------------------------------------------
  {
    reset();
    Obj spin(&spinClass);
    spin.setI32(fSpinResult, 7);  // == spin_success
    Obj p(&promiseDerived);
    p.setBool(fComplete, true);
    p.setPtr(fValue, spin.ptr());
    assert(api->poll(p.ptr(), PGR_VR_PHASE_SPIN, tid, &out) == PGR_VR_DONE);
    assert(out.success == 1 && out.status_known == 0);  // spin never reports a status
    gcFree(out.value_handle);
    reset();
  }
  {
    reset();
    Obj spin(&spinClass);
    spin.setI32(fSpinResult, 3);  // failure
    Obj p(&promiseDerived);
    p.setBool(fComplete, true);
    p.setPtr(fValue, spin.ptr());
    assert(api->poll(p.ptr(), PGR_VR_PHASE_SPIN, tid, &out) == PGR_VR_DONE && out.success == 0);
    gcFree(out.value_handle);
    reset();
  }
  {
    reset();
    Obj other(&unknownClass);  // spin phase, value not SPINRESULT -> success false
    Obj p(&promiseDerived);
    p.setBool(fComplete, true);
    p.setPtr(fValue, other.ptr());
    assert(api->poll(p.ptr(), PGR_VR_PHASE_SPIN, tid, &out) == PGR_VR_DONE && out.success == 0);
    gcFree(out.value_handle);
    reset();
  }

  // --- encounter: any non-null resolved value is valid; no status/success ------------------------
  {
    reset();
    Obj other(&unknownClass);
    Obj p(&promiseDerived);
    p.setBool(fComplete, true);
    p.setPtr(fValue, other.ptr());
    assert(api->poll(p.ptr(), PGR_VR_PHASE_ENCOUNTER, tid, &out) == PGR_VR_DONE);
    assert(out.state == 1 && out.success == 0 && out.status_known == 0 && out.value_handle != 0);
    gcFree(out.value_handle);
    reset();
  }

  // --- unknown shape: missing a required field, and a bad offset ---------------------------------
  {
    reset();
    int lonelyClass;  // no declared fields, no parent
    Obj p(&lonelyClass);
    assert(api->poll(p.ptr(), PGR_VR_PHASE_CATCH, tid, &out) == PGR_VR_ERR_SHAPE && rootCalls == 1);
    reset();  // only the promise root was taken and freed
  }
  {
    reset();
    int badClass;
    Field badComplete{8, 1, &boolClass};  // offset < 16 guard
    declared[&badClass] = {{"completeCalled", &badComplete},
                           {"errorCalled", &fError},
                           {"completedValue", &fValue}};
    Obj p(&badClass);
    assert(api->poll(p.ptr(), PGR_VR_PHASE_CATCH, tid, &out) == PGR_VR_ERR_SHAPE);
    declared.erase(&badClass);
    reset();
  }
  {
    reset();
    int wrongTypeClass;
    Field valueAsValueType{32, 8, &intClass};  // completedValue typed as a value type -> rejected
    declared[&wrongTypeClass] = {{"completeCalled", &fComplete},
                                 {"errorCalled", &fError},
                                 {"completedValue", &valueAsValueType}};
    Obj p(&wrongTypeClass);
    assert(api->poll(p.ptr(), PGR_VR_PHASE_CATCH, tid, &out) == PGR_VR_ERR_SHAPE);
    declared.erase(&wrongTypeClass);
    reset();
  }

  // --- optional statusErrorCalled absent is accepted (still resolves) ----------------------------
  {
    reset();
    int noStatusClass;
    declared[&noStatusClass] = {{"completeCalled", &fComplete},
                                {"errorCalled", &fError},
                                {"completedValue", &fValue}};
    Obj resp(&respClass);
    resp.setI32(fCatchStatus, 100);
    Obj p(&noStatusClass);
    p.setBool(fComplete, true);
    p.setPtr(fValue, resp.ptr());
    assert(api->poll(p.ptr(), PGR_VR_PHASE_CATCH, tid, &out) == PGR_VR_DONE && out.success == 1);
    gcFree(out.value_handle);
    declared.erase(&noStatusClass);
    reset();
  }

  // --- root failure on every path returns to zero roots, nothing returned ------------------------
  for (int n = 1; n <= 2; n++) {
    reset();
    Obj resp(&respClass);
    resp.setI32(fCatchStatus, 100);
    Obj p(&promiseDerived);
    p.setBool(fComplete, true);
    p.setPtr(fValue, resp.ptr());
    failRoot = n;  // 1 = promise root, 2 = value root
    int rc = api->poll(p.ptr(), PGR_VR_PHASE_CATCH, tid, &out);
    assert(rc == PGR_VR_ERR_GCHANDLE && out.value_handle == 0);
    failRoot = 0;
    reset();  // zero roots even when a root allocation failed mid-poll
  }

  // --- owner thread: a foreign OS thread never touches the promise -------------------------------
  {
    reset();
    Obj p(&promiseDerived);
    p.setBool(fComplete, true);
    Obj resp(&respClass);
    p.setPtr(fValue, resp.ptr());
    std::thread other([&] {
      PgrVgpResultsResult o{};
      assert(api->poll(p.ptr(), PGR_VR_PHASE_CATCH, tid, &o) == PGR_VR_PENDING);
    });
    other.join();
    assert(rootCalls == 0 && roots.empty());
  }

  // --- telemetry sample is coherent --------------------------------------------------------------
  {
    PgrVgpResultsSample s{};
    assert(api->sample(&s) == 0 && s.version == 1 && s.size == 48);
    assert(s.completed_serial > 0 && s.rejected_serial > 0 && s.error_serial > 0 && s.fault == 1);
  }

  puts("PASS VGP results production API: install, owner gate, pending/rejected/null, catch/spin/encounter, "
       "shape/type guards, relocated value class/status, zero roots, telemetry");
  dlclose(lib);
  return 0;
}
