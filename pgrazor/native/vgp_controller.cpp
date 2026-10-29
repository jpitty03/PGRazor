#include "vgp_controller.h"

#include "vgp_policy.h"
#include "vgp_actions.h"
#include "vgp_candidates.h"
#include "vgp_results.h"
#include "vgp_discovery.h"
#include "vgp_maintenance.h"
#include "vgp_marker.h"
#include "items.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <time.h>
#include <unistd.h>

// Feature 33 implementation. The controller OWNS the orchestration and REUSES the already-installed sibling
// tables through the config. It installs no hook. The shared auto-transfer classifier/executor stays in JS;
// the controller only hands off OWNED record handles via Out120 and consumes transfer_feedback().

namespace {

// ---- decoupled view of the feature-34 aux table (owned by the aux worker) --------------------------
// The aux header is published by the aux worker; the controller only receives the table pointer as a
// void* in its config and reinterpret-casts it to this fixed-ABI view (table32 / Spin32 / Transfer32 per
// the aux consolidation contract). Distinct names avoid any ODR/symbol collision with vgp_aux.h.
struct AuxSpinOut {
  uint32_t version, size;
  uint64_t rpc_handle;         // @8  OWNED rpc service root
  uint64_t reflection_handle;  // @16 OWNED closed reflection root (kept until the pending completes)
  void    *closed_method;      // @24 inflated SendRpc MethodInfo* (validated non-null by aux)
};
static_assert(sizeof(AuxSpinOut) == 32, "aux Spin32 ABI");
struct AuxTransferOut {
  uint32_t version, size;
  uint64_t caught_id;       // @8  full 64-bit caught id
  uint64_t pokemon_handle;  // @16 OWNED resolved PK root
  uint64_t gm_handle;       // @24 OWNED GameMaster/full-id helper root
};
static_assert(sizeof(AuxTransferOut) == 32, "aux Transfer32 ABI");
struct AuxApi {
  uint32_t version, size;
  int (*install)(const void *config);
  int (*spin)(void *item_bag, uint64_t owner, uint64_t lease_ms, AuxSpinOut *out);
  int (*transfer)(void *response, void *pokemon_bag, uint64_t owner, uint64_t lease_ms, AuxTransferOut *out);
};
static_assert(sizeof(AuxApi) == 32, "aux table32 ABI");

// ---- il2cpp primitive signatures ------------------------------------------------------------------
using New = uintptr_t (*)(void *, int);
using Get = void *(*)(uintptr_t);
using FreeH = void (*)(uintptr_t);
using Class = void *(*)(void *);
using Invoke = void *(*)(void *, void *, void **, void **);
using Unbox = void *(*)(void *);

PgrVgpControllerConfig cfg{};
std::mutex installMutex;
std::atomic<bool> installed{false};

// Sibling tables (validated at install).
const PgrVgpPolicyApi      *POL = nullptr;
const PgrVgpActionsApi      *ACT = nullptr;
const PgrVgpCandidateApi    *CND = nullptr;
const PgrVgpResultsApi      *RES = nullptr;
const PgrVgpDiscoveryApi    *DSC = nullptr;
const PgrVgpMaintenanceApi  *MNT = nullptr;
const PgrVgpMarkerApi       *MRK = nullptr;
const pgrazor_items_api_v1  *ITM = nullptr;
const AuxApi                *AUX = nullptr;

// ---- controller state -----------------------------------------------------------------------------
std::mutex gate;        // small pure mutex: generation bump + actions.stop; arm-actions pre-invoke window
std::mutex manualMutex; // brief guard for the manual-exclusion root (never held across a managed invoke)
std::atomic<uint64_t> ownerTid{0};
std::atomic<uint64_t> generationCounter{0};
std::atomic<bool> busy{false};
std::atomic<bool> stopped{false};
std::atomic<uint32_t> rootCount{0};

uint64_t manualTarget = 0;
uint64_t manualAt = 0;

struct Pending {
  bool active = false;
  uint32_t phase = 0;  // PGR_VGC_PH_ENCOUNTER / CATCH / SPIN (== PGR_VR phase)
  uint64_t target = 0, bag = 0, promise = 0, reflection = 0;
  uint64_t deadline = 0;
  uint32_t key_len = 0;
  char key[PGR_VC_KEY_CAPACITY]{};
} pend;

struct Transfer {
  bool active = false;
  bool handed = false;       // phase 5: with the shared JS executor
  uint64_t response = 0, bag = 0;  // internal roots (dropped after handoff)
  uint64_t epoch = 0;
  uint64_t token = 0;
  uint64_t deadline = 0;     // inventory-wait (now+10000) then handoff (now+20000)
  uint32_t feedback = PGR_VGC_FB_PENDING;
  bool feedback_set = false;
} xfer;

uint64_t tokenCounter = 0;
uint64_t markerAt = 0;
std::atomic<uint32_t> lastStatus{PGR_VGC_ST_OFF}, publishedPhase{PGR_VGC_PH_NONE};
std::atomic<int32_t> lastError{0};
uint64_t tickStarted = 0; // owner-only, fixed for each outer tick
uint64_t eventSerial = 0;

// ---- primitives -----------------------------------------------------------------------------------
uint64_t nowMs() {
  timespec t{};
  if (clock_gettime(CLOCK_BOOTTIME, &t) || t.tv_sec < 0) return 0;
  return uint64_t(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
}
uint64_t rootNew(void *obj) {
  if (!obj) return 0;
  uint64_t h = reinterpret_cast<New>(cfg.gc_new)(obj, 0);
  if (h) rootCount.fetch_add(1);
  return h;
}
void rootFree(uint64_t &slot) {
  if (slot) {
    reinterpret_cast<FreeH>(cfg.gc_free)(slot);
    slot = 0;
    rootCount.fetch_sub(1);
  }
}
void *rootGet(uint64_t h) { return h ? reinterpret_cast<Get>(cfg.gc_get)(h) : nullptr; }
// Raw gc handle for the handoff exports -- NOT counted in rootCount (ownership leaves the controller).
uint64_t exportNew(void *obj) { return obj ? reinterpret_cast<New>(cfg.gc_new)(obj, 0) : 0; }
void exportFree(uint64_t h) { if (h) reinterpret_cast<FreeH>(cfg.gc_free)(h); }

bool exactClass(void *obj, void *klass) {
  return obj && reinterpret_cast<Class>(cfg.object_class)(obj) == klass;
}
// Static alive(obj) -> boxed Boolean; exact bool class, read first byte.
bool alive(void *obj) {
  if (!obj) return false;
  void *args[] = {obj}, *exc = nullptr;
  void *boxed = reinterpret_cast<Invoke>(cfg.runtime_invoke)(cfg.m_alive, nullptr, args, &exc);
  if (exc || !exactClass(boxed, cfg.class_bool)) return false;
  void *v = reinterpret_cast<Unbox>(cfg.object_unbox)(boxed);
  return v && *static_cast<uint8_t *>(v) == 1;
}
uint64_t clampLease(uint64_t ms) { return ms > PGR_VGC_MAX_LEASE_MS ? PGR_VGC_MAX_LEASE_MS : ms; }
uint64_t remaining(uint64_t ms) {
  const uint64_t now = nowMs(), bounded = clampLease(ms);
  return now && now >= tickStarted && now - tickStarted < bounded ? bounded - (now - tickStarted) : 0;
}
bool manualMatches(uint64_t target) {
  std::lock_guard<std::mutex> g(manualMutex);
  return manualTarget && rootGet(manualTarget) == rootGet(target);
}


// ---- out helpers ----------------------------------------------------------------------------------
void initOut(PgrVgpControllerOut *o) {
  memset(o, 0, sizeof(*o));
  o->version = PGR_VGC_OUT_VERSION;
  o->size = sizeof(*o);
}
uint32_t currentPhase() {
  if (xfer.active) return xfer.handed ? PGR_VGC_PH_HANDED_OFF : PGR_VGC_PH_INVENTORY_WAIT;
  if (pend.active) return pend.phase;
  return PGR_VGC_PH_NONE;
}
void finalize(PgrVgpControllerOut *o) {
  if (o->flags) o->event_serial = ++eventSerial;
  o->phase = currentPhase();
  publishedPhase.store(o->phase);
  lastStatus = o->status;
  lastError = o->error;
}

// ---- root lifecycle -------------------------------------------------------------------------------
void dropPending(PgrVgpControllerOut *o) {
  if (!pend.active) return;
  rootFree(pend.promise);
  rootFree(pend.target);
  rootFree(pend.bag);
  rootFree(pend.reflection);
  pend = Pending{};
  if (o) o->flags |= PGR_VGC_EV_MARKER_HIDE;  // vgpDrop() hides the marker
}
void clearTransfer(PgrVgpControllerOut *o, bool cancel) {
  if (!xfer.active) return;
  rootFree(xfer.response);
  rootFree(xfer.bag);
  if (cancel && xfer.handed && o) {
    o->flags |= PGR_VGC_EV_TRANSFER_CANCEL;
    o->transfer_token = xfer.token;
  }
  xfer = Transfer{};
}
void freeAllRoots(PgrVgpControllerOut *o) {
  dropPending(o);
  clearTransfer(o, true);
  {
    std::lock_guard<std::mutex> g(manualMutex);
    rootFree(manualTarget);
  }
}
void setStatus(PgrVgpControllerOut *o, uint32_t s) { o->status = s; }
int pauseError(PgrVgpControllerOut *o, int32_t code) {
  POL->block(ownerTid.load());
  dropPending(o);
  clearTransfer(o, true);
  setStatus(o, PGR_VGC_ST_ERROR);
  o->error = code;
  return PGR_VGC_OK;
}
// vgpRetry: suppress a non-spin pending target for the retry window (the seen policy).
void retrySeen(uint64_t owner, uint64_t ms) {
  if (pend.phase != PGR_VGC_PH_SPIN && pend.key_len)
    POL->touch(owner, pend.key, pend.key_len, ms);
}

// ---- actions gate ---------------------------------------------------------------------------------
// Arm the actions sibling under the gate iff not stopped and the tick's generation is still live, then
// unlock BEFORE the managed action so no lock is ever held across a managed invoke.
bool armActions(const PgrVgpControllerTick *in, uint64_t lease) {
  std::lock_guard<std::mutex> g(gate);
  const uint64_t fresh = remaining(lease);
  if (!fresh || stopped.load() || !(in->flags & PGR_VGC_TF_MANUAL_READY) || in->generation != generationCounter.load()) return false;
  return ACT->update(in->mask, fresh, in->epoch, in->owner) == PGR_VA_OK;
}
// Translate an action return into orchestration. 1 issued -> store promise. 0 not-issued -> quarantine +
// arm + WAIT_READY. negative -> block + ERROR (no retry: an issued-but-uncertain action must not re-fire).
enum IssueResult { ISSUE_OK, ISSUE_REFUSED, ISSUE_ERROR };
IssueResult classifyIssue(int rc) {
  if (rc == PGR_VA_ISSUED) return ISSUE_OK;
  if (rc == PGR_VA_OK) return ISSUE_REFUSED;  // 0: proven NOT issued
  return ISSUE_ERROR;
}
void onRefused(PgrVgpControllerOut *o, uint64_t owner, uint64_t now) {
  if (pend.key_len) POL->touch(owner, pend.key, pend.key_len, 5000);  // 5 s admission quarantine
  dropPending(o);
  POL->arm(owner, 2500);
  setStatus(o, PGR_VGC_ST_WAIT_READY);
}

// ---- marker ---------------------------------------------------------------------------------------
void tickMarker(const PgrVgpControllerTick *in, PgrVgpControllerOut *o, uint64_t now) {
  if (!pend.active || now - markerAt < 100) return;
  markerAt = now;
  if (remaining(in->lease_ms) == 0 || stopped.load()) {
    o->flags |= PGR_VGC_EV_MARKER_HIDE;
    return;
  }
  PgrVgpMarkerResult r{};
  int rc = MRK->project(rootGet(pend.target), remaining(in->lease_ms), in->owner, &r);
  if (rc < 0) {  // a marker error never blocks the tick
    o->flags |= PGR_VGC_EV_MARKER_HIDE;
    return;
  }
  o->flags |= PGR_VGC_EV_MARKER_UPDATE;
  o->marker_x = r.x;
  o->marker_y = r.y;
  o->marker_visible = (rc == 1) ? r.visible : 0;
}

// ---- transfer ------------------------------------------------------------------------------------
// Returns true if the transfer machine consumed this tick (skip the scan / pending logic).
bool tickTransfer(const PgrVgpControllerTick *in, PgrVgpControllerOut *o, uint64_t now) {
  if (!xfer.active) return false;
  const bool transferEnabled = in->flags & PGR_VGC_TF_TRANSFER_ENABLED;
  // Clear conditions (apply to any transfer record; a handed-off record also cancels the JS record).
  if (stopped.load() || !transferEnabled || xfer.epoch != in->epoch || in->allowed_ms == 0 ||
      in->lease_ms == 0) {
    clearTransfer(o, true);
    return false;
  }
  if (xfer.handed) {
    // A confirmed terminal result wins over a deadline reached on this same tick.
    if (xfer.feedback_set && xfer.feedback >= PGR_VGC_FB_SUCCESS) {
      setStatus(o, xfer.feedback == PGR_VGC_FB_SUCCESS ? PGR_VGC_ST_TRANSFERRED
                   : xfer.feedback == PGR_VGC_FB_KEPT ? PGR_VGC_ST_KEPT : PGR_VGC_ST_TRANSFER_ERROR);
      clearTransfer(o, true);
      return true;
    }
    if (now >= xfer.deadline) {
      setStatus(o, PGR_VGC_ST_TRANSFER_ERROR);
      clearTransfer(o, true);
      return true;
    }
    if (xfer.feedback_set) {
      if (xfer.feedback == PGR_VGC_FB_PENDING) {
        xfer.feedback_set = false;
        setStatus(o, PGR_VGC_ST_CHECK_TRANSFER);
        return true;
      }
      if (xfer.feedback == PGR_VGC_FB_REQUESTED) {
        setStatus(o, PGR_VGC_ST_TRANSFERRING);
        xfer.feedback_set = false;  // consumed; wait for a terminal state
        return true;
      }
    }
    setStatus(o, xfer.feedback == PGR_VGC_FB_REQUESTED ? PGR_VGC_ST_TRANSFERRING : PGR_VGC_ST_CHECK_TRANSFER);
    return true;
  }
  if (now >= xfer.deadline) {  // 10 s inventory-update wait elapsed
    setStatus(o, PGR_VGC_ST_TRANSFER_INV_TIMEOUT);
    clearTransfer(o, false);
    return true;
  }
  uint64_t lease = remaining(in->allowed_ms < in->lease_ms ? in->allowed_ms : in->lease_ms);
  if (!lease) { clearTransfer(o, true); return true; }
  if (in->generation != generationCounter.load()) { setStatus(o, PGR_VGC_ST_CHECK_TRANSFER); return true; }
  AuxTransferOut t{};
  int rc = AUX->transfer(rootGet(xfer.response), rootGet(xfer.bag), in->owner, lease, &t);
  if (rc == 0) {  // inventory not ready yet
    setStatus(o, PGR_VGC_ST_CHECK_TRANSFER);
    return true;
  }
  if (rc == 2) {  // missing caught id or identity mismatch
    setStatus(o, PGR_VGC_ST_KEPT);
    clearTransfer(o, false);
    return true;
  }
  if (rc < 0) {
    pauseError(o, rc);
    clearTransfer(o, false);
    return true;
  }
  if (!remaining(in->allowed_ms) || !remaining(in->lease_ms) || stopped.load() || in->generation != generationCounter.load()) {
    exportFree(t.pokemon_handle); exportFree(t.gm_handle);
    // A mask restriction only defers this caught-record handoff until a fresh tick. A revoked
    // transfer setting changes epoch; the next tick applies that cancellation before any handoff.
    if (!remaining(in->allowed_ms) || !remaining(in->lease_ms) || stopped.load()) clearTransfer(o, true);
    setStatus(o, PGR_VGC_ST_CHECK_TRANSFER);
    return true;
  }
  // rc == 1: resolved. Export four OWNED handles (response + bag reconstructed independently, PK + GM
  // from aux). ALL FOUR or none -- a partial root failure frees every handle and pauses.
  uint64_t resp = exportNew(rootGet(xfer.response));
  uint64_t bag = exportNew(rootGet(xfer.bag));
  if (!resp || !bag || !t.pokemon_handle || !t.gm_handle) {
    exportFree(resp);
    exportFree(bag);
    exportFree(t.pokemon_handle);
    exportFree(t.gm_handle);
    pauseError(o, -20);  // GC export allocation failure
    clearTransfer(o, false);
    return true;
  }
  o->flags |= PGR_VGC_EV_TRANSFER_READY;
  o->transfer_token = xfer.token;
  o->caught_id = t.caught_id;
  o->response_handle = resp;
  o->bag_handle = bag;
  o->pokemon_handle = t.pokemon_handle;
  o->gm_handle = t.gm_handle;
  // Drop the internal roots; phase 5 keeps only token / epoch / a fresh 20 s deadline.
  rootFree(xfer.response);
  rootFree(xfer.bag);
  xfer.handed = true;
  xfer.feedback_set = false;
  xfer.deadline = now + 20000;
  setStatus(o, PGR_VGC_ST_CHECK_TRANSFER);
  return true;
}

// ---- pending result ------------------------------------------------------------------------------
void tickPending(const PgrVgpControllerTick *in, PgrVgpControllerOut *o, uint64_t now, bool stale) {
  const uint64_t owner = in->owner;
  const bool allowed = in->mask && remaining(in->lease_ms) > 0;
  const bool actionsBlocked = stale;  // a stale generation blocks a NEW action, not the completion itself

  PgrVgpResultsResult pr{};
  int rc = RES->poll(rootGet(pend.promise), pend.phase, owner, &pr);
  if (rc < 0) { pauseError(o, rc); return; }
  if (rc == PGR_VR_PENDING) {
    if (now > pend.deadline) {
      dropPending(o);
      POL->block(owner);
      setStatus(o, PGR_VGC_ST_TIMEOUT);
    }
    return;
  }
  if (rc == PGR_VR_REJECTED) {
    retrySeen(owner, 30000);
    dropPending(o);
    POL->arm(owner, 3000);
    setStatus(o, PGR_VGC_ST_REJECTED);
    return;
  }
  // DONE: adopt the resolved value root.
  uint64_t value = pr.value_handle;
  if (value) rootCount.fetch_add(1);  // adopted as a counted root until freed/handed off

  if (pend.phase == PGR_VGC_PH_ENCOUNTER) {
    void *targetObj = rootGet(pend.target);
    bool manualHit;
    {
      std::lock_guard<std::mutex> g(manualMutex);
      manualHit = manualTarget && rootGet(manualTarget) == targetObj;
    }
    if (!allowed || !(in->flags & PGR_VGC_TF_MANUAL_READY) || !(in->mask & 1) || !alive(targetObj) || manualHit) {
      retrySeen(owner, 5000);
      dropPending(o);
      if (value) rootFree(value);
      return;
    }
    uint64_t svcRoot = reinterpret_cast<uint64_t>(ITM->service_acquire());  // OWNED gc handle
    if (!svcRoot) {
      retrySeen(owner, 5000);
      dropPending(o);
      if (value) rootFree(value);
      setStatus(o, PGR_VGC_ST_WAIT_INV);
      return;
    }
    rootCount.fetch_add(1);
    int32_t balls = 0;
    int br = MNT->balls(rootGet(svcRoot), owner, remaining(in->lease_ms), &balls);
    if (br < 0) { rootFree(svcRoot); if (value) rootFree(value); pauseError(o, br); return; }
    if (br == 0) {
      rootFree(svcRoot);
      retrySeen(owner, 5000);
      dropPending(o);
      if (value) rootFree(value);
      setStatus(o, PGR_VGC_ST_WAIT_READY);
      return;
    }
    if (balls < 1) {
      rootFree(svcRoot);
      retrySeen(owner, 5000);
      dropPending(o);
      if (value) rootFree(value);
      setStatus(o, PGR_VGC_ST_OUT_BALLS);
      return;
    }
    if (actionsBlocked || manualMatches(pend.target) || !remaining(in->lease_ms)) {
      rootFree(svcRoot);
      dropPending(o);
      if (value) rootFree(value);
      setStatus(o, PGR_VGC_ST_NATIVE_UNAVAILABLE);
      return;
    }
    // Transition encounter -> catch and issue the straight ball throw.
    rootFree(pend.promise);
    pend.phase = PGR_VGC_PH_CATCH;
    pend.deadline = now + 20000;
    PgrVgpActionsTarget req{PGR_VA_TARGET_VERSION, sizeof(req), in->epoch, owner, rootGet(pend.target)};
    uintptr_t handle = 0;
    int ir = armActions(in, in->lease_ms) ? ACT->capture(&req, &handle) : PGR_VA_OK;
    rootFree(svcRoot);
    if (value) rootFree(value);
    IssueResult res = classifyIssue(ir);
    if (res == ISSUE_OK) {
      pend.promise = handle;
      rootCount.fetch_add(1);
      o->flags |= PGR_VGC_EV_CATCH_ISSUED;
      setStatus(o, PGR_VGC_ST_CATCHING);
    } else if (res == ISSUE_REFUSED) {
      onRefused(o, owner, now);
    } else {
      POL->block(owner);
      dropPending(o);
      setStatus(o, PGR_VGC_ST_ERROR);
      o->error = ir;
    }
    return;
  }

  // catch or spin result.
  const bool isCatch = pend.phase == PGR_VGC_PH_CATCH;
  int32_t catchStatus = (isCatch && pr.status_known) ? pr.status : 0;
  if (isCatch) {
    uint64_t delay = 30000;
    POL->catch_delay(pr.status_known ? pr.status : 0, &delay);
    retrySeen(owner, delay);
  }
  const bool success = pr.success != 0;
  if (!isCatch && success) {
    int cr = MNT->cooldown(rootGet(pend.target), rootGet(value), owner, PGR_VGC_MAX_LEASE_MS);
    if (cr < 0) { if (value) rootFree(value); pauseError(o, cr); return; }
  }
  const bool transferEnabled = in->flags & PGR_VGC_TF_TRANSFER_ENABLED;
  bool queued = false;
  if (success && isCatch && transferEnabled && value) {
    // Retain the response + pokemon bag for the inventory wait; a fresh token / the current epoch.
    xfer = Transfer{};
    xfer.active = true;
    xfer.response = value;        // adopt the resolved RESP root (already counted)
    xfer.bag = pend.bag;          // move the pokemon-bag root out of the pending record
    pend.bag = 0;
    xfer.epoch = in->epoch;
    xfer.token = ++tokenCounter;
    xfer.deadline = now + 10000;
    queued = true;
    value = 0;
  }
  dropPending(o);
  POL->arm(owner, 2500);
  o->flags |= PGR_VGC_EV_ITEM_CLEANUP;  // reset the item poll; the adapter arms vgpCleanupUntil if enabled
  if (isCatch) {
    o->flags |= PGR_VGC_EV_CATCH_RESULT;
    o->status_known = pr.status_known;
    o->catch_status = catchStatus;
    o->success = success;
    if (!success) { uint64_t d = 30000; POL->catch_delay(pr.status_known ? pr.status : 0, &d); o->retry_ms = d; }
    setStatus(o, success ? (queued ? PGR_VGC_ST_CHECK_TRANSFER : PGR_VGC_ST_CAUGHT) : PGR_VGC_ST_NOT_CAUGHT);
  } else {
    o->flags |= PGR_VGC_EV_SPIN_RESULT;
    o->success = success;
    setStatus(o, success ? PGR_VGC_ST_SPUN : PGR_VGC_ST_SPIN_FAIL);
  }
  if (value) rootFree(value);
}

// ---- scan ----------------------------------------------------------------------------------------
// Issue at most one new action. Frees svc + the discovery handles on every exit.
void tickScan(const PgrVgpControllerTick *in, PgrVgpControllerOut *o, uint64_t now, bool stale) {
  const uint64_t owner = in->owner;
  const bool allowed = in->mask && remaining(in->lease_ms) > 0;

  if (in->mask == 0) { POL->clear_block(owner); setStatus(o, PGR_VGC_ST_OFF); return; }
  if (!allowed) { setStatus(o, PGR_VGC_ST_WAIT_READY); return; }
  if (stale) { setStatus(o, PGR_VGC_ST_NATIVE_UNAVAILABLE); return; }
  if (!(in->flags & PGR_VGC_TF_MANUAL_READY)) { setStatus(o, PGR_VGC_ST_INIT_SUPPORT); return; }
  const int due = POL->due(owner);
  if (due < 0) { pauseError(o, due); return; }
  if (due != PGR_VP_YES) return;  // not due yet: keep the last status
  if ((in->flags & PGR_VGC_TF_ITEM_ENABLED) && in->cleanup_ms > 0) {
    setStatus(o, PGR_VGC_ST_CLEAN_INV);
    return;
  }
  const int armed = POL->arm(owner, 2500);
  if (armed < 0) { pauseError(o, armed); return; }

  uint64_t svcRoot = reinterpret_cast<uint64_t>(ITM->service_acquire());  // OWNED gc handle
  if (!svcRoot) { setStatus(o, PGR_VGC_ST_WAIT_INV); return; }
  rootCount.fetch_add(1);

  PgrVgpDiscoverySnapshot snap{};
  int sr = DSC->snapshot(rootGet(svcRoot), owner, remaining(in->lease_ms), &snap);
  if (sr < 0) { rootFree(svcRoot); pauseError(o, sr); return; }
  if (sr == 0) { rootFree(svcRoot); setStatus(o, PGR_VGC_ST_WAIT_INV); return; }

  uint64_t pokeBag = snap.pokemon_bag_handle;  // OWNED
  uint64_t itemBag = snap.item_bag_handle;     // OWNED
  rootCount.fetch_add(2);
  const int32_t balls = snap.balls, stored = snap.stored, capacity = snap.capacity;
  const int32_t totalItems = snap.total_items, maxItems = snap.max_items;

  uint64_t arrHandle = 0;  // the enumeration array handle currently held (freed by cleanup on every exit)
  auto cleanup = [&]() {
    rootFree(arrHandle);
    rootFree(pokeBag);
    rootFree(itemBag);
    rootFree(svcRoot);
  };

  uint32_t catchBlock = !(in->mask & 1)  ? PGR_VGC_ST_CATCH_DISABLED
                        : balls < 1      ? PGR_VGC_ST_OUT_BALLS
                        : stored >= capacity ? PGR_VGC_ST_STORAGE_FULL
                                             : 0;
  const bool canCatch = catchBlock == 0;
  const bool canSpin = (in->mask & 2) && totalItems < maxItems;
  if (!canCatch && !canSpin) { setStatus(o, catchBlock ? catchBlock : PGR_VGC_ST_ITEM_FULL); cleanup(); return; }

  uint32_t seenSize = 0;
  const int swept = POL->sweep(owner, &seenSize);
  if (swept < 0) { cleanup(); pauseError(o, swept); return; }
  if (swept == PGR_VP_YES) {
    setStatus(o, PGR_VGC_ST_FULL_RECENT);
    cleanup();
    return;
  }
  uint32_t ord = 0;
  const int ordered = POL->order(owner, &ord);
  if (ordered < 0) { cleanup(); pauseError(o, ordered); return; }
  const uint32_t actions[2] = {ord == 0 ? PGR_VGC_PH_CATCH : PGR_VGC_PH_SPIN,
                               ord == 0 ? PGR_VGC_PH_SPIN : PGR_VGC_PH_CATCH};
  uint32_t recent = 0, outRange = 0;

  for (uint32_t a = 0; a < 2; a++) {
    const bool isCatch = actions[a] == PGR_VGC_PH_CATCH;
    if (isCatch ? !canCatch : !canSpin) continue;
    const uint32_t kind = isCatch ? PGR_VD_KIND_CATCH : PGR_VD_KIND_SPIN;

    PgrVgpDiscoveryEnumeration en{};
    int er = DSC->enumerate(kind, owner, remaining(in->lease_ms), &en);
    if (er < 0) { cleanup(); pauseError(o, er); return; }
    if (er == 0) continue;
    arrHandle = en.array_handle;  // OWNED
    rootCount.fetch_add(1);

    for (uint32_t i = 0; i < en.count; i++) {
      uintptr_t candH = 0;
      int ar = DSC->at(rootGet(arrHandle), i, owner, remaining(in->lease_ms), &candH);
      if (ar < 0) { cleanup(); pauseError(o, ar); return; }  // at() leaves *out_handle 0 on failure
      if (ar == 0 || !candH) continue;
      rootCount.fetch_add(1);
      uint64_t cand = candH;

      void *manualObj = nullptr;
      if (isCatch) {
        std::lock_guard<std::mutex> g(manualMutex);
        manualObj = manualTarget ? rootGet(manualTarget) : nullptr;
      }
      PgrVgpCandidateRequest creq{};
      creq.version = PGR_VC_REQUEST_VERSION;
      creq.size = sizeof(creq);
      creq.action = isCatch ? PGR_VC_ACTION_CATCH : PGR_VC_ACTION_SPIN;
      creq.owner_tid = owner;
      creq.lease_ms = remaining(in->lease_ms);
      creq.lat = in->lat;
      creq.lon = in->lon;
      creq.target = rootGet(cand);
      creq.manual = manualObj;
      PgrVgpCandidateResult cres{};
      int ir = CND->inspect(&creq, &cres);
      if (ir < 0) { rootFree(cand); cleanup(); pauseError(o, ir); return; }
      uint64_t fort = cres.fort_handle;  // OWNED on an eligible spin
      if (fort) rootCount.fetch_add(1);
      if (ir != PGR_VC_ELIGIBLE) {
        if (isCatch && cres.reason == PGR_VC_REASON_OUT_OF_RANGE) outRange++;
        if (fort) rootFree(fort);
        rootFree(cand);
        continue;
      }
      const int seen = POL->seen(owner, cres.key, cres.key_len);
      if (seen < 0) { if (fort) rootFree(fort); rootFree(cand); cleanup(); pauseError(o, seen); return; }
      if (seen == PGR_VP_YES) {
        if (isCatch) recent++;
        if (fort) rootFree(fort);
        rootFree(cand);
        continue;
      }
      const int touched = POL->touch(owner, cres.key, cres.key_len, isCatch ? 30000 : 300000);
      if (touched < 0) { if (fort) rootFree(fort); rootFree(cand); cleanup(); pauseError(o, touched); return; }
      if (touched == PGR_VP_FULL) {
        if (fort) rootFree(fort);
        rootFree(cand);
        continue;
      }
      // Reserve the pending record (exception-safe: the candidate root becomes the pending target).
      pend = Pending{};
      pend.active = true;
      pend.phase = isCatch ? PGR_VGC_PH_ENCOUNTER : PGR_VGC_PH_SPIN;
      pend.target = cand;          // adopt (already counted)
      pend.bag = rootNew(rootGet(pokeBag));
      if (!pend.bag) { if (fort) rootFree(fort); dropPending(o); cleanup(); pauseError(o, -20); return; }
      pend.deadline = now + 20000;
      pend.key_len = cres.key_len;
      memcpy(pend.key, cres.key, cres.key_len < PGR_VC_KEY_CAPACITY ? cres.key_len : PGR_VC_KEY_CAPACITY);

      uintptr_t handle = 0;
      int issue;
      if (isCatch) {
        if (manualMatches(pend.target)) { onRefused(o, owner, now); cleanup(); return; }
        PgrVgpActionsTarget req{PGR_VA_TARGET_VERSION, sizeof(req), in->epoch, owner, rootGet(pend.target)};
        issue = armActions(in, in->lease_ms) ? ACT->encounter(&req, &handle) : PGR_VA_OK;
      } else {
        AuxSpinOut sp{};
        int spr = AUX->spin(rootGet(itemBag), owner, remaining(in->lease_ms), &sp);
        if (spr < 0) { if (fort) rootFree(fort); dropPending(o); cleanup(); pauseError(o, spr); return; }
        if (spr == 0) {  // aux not ready / gated: treat as a not-issued admission refusal
          if (fort) rootFree(fort);
          onRefused(o, owner, now);
          cleanup();
          return;
        }
        pend.reflection = sp.reflection_handle;  // kept until the pending completes
        if (sp.reflection_handle) rootCount.fetch_add(1);
        uint64_t rpcRoot = sp.rpc_handle;
        if (rpcRoot) rootCount.fetch_add(1);
        PgrVgpActionsSpin req{};
        req.version = PGR_VA_SPIN_VERSION;
        req.size = sizeof(req);
        req.epoch = in->epoch;
        req.tid = owner;
        req.rpc_id = cfg.rpc_id;
        req.lat = in->lat;
        req.lon = in->lon;
        req.target = rootGet(pend.target);
        req.fort = rootGet(fort);
        req.rpc = rootGet(rpcRoot);
        req.closed_method = sp.closed_method;
        issue = armActions(in, in->lease_ms) ? ACT->spin(&req, &handle) : PGR_VA_OK;
        rootFree(rpcRoot);  // actions roots the rpc internally
      }
      if (fort) rootFree(fort);

      IssueResult res = classifyIssue(issue);
      if (res == ISSUE_OK) {
        pend.promise = handle;
        rootCount.fetch_add(1);
        if (!isCatch) o->flags |= PGR_VGC_EV_SPIN_ISSUED;
        setStatus(o, isCatch ? PGR_VGC_ST_CATCHING : PGR_VGC_ST_SPINNING);
      } else if (res == ISSUE_REFUSED) {
        onRefused(o, owner, now);
      } else {
        POL->block(owner);
        dropPending(o);
        setStatus(o, PGR_VGC_ST_ERROR);
        o->error = issue;
      }
      cleanup();
      return;
    }
    rootFree(arrHandle);
  }
  // Nothing issued this scan.
  setStatus(o, catchBlock ? catchBlock
               : recent    ? PGR_VGC_ST_RECENT
               : outRange   ? PGR_VGC_ST_OUT_RANGE
                            : PGR_VGC_ST_WATCHING);
  cleanup();
}

// ---- table functions -----------------------------------------------------------------------------
template <typename T> const T *checkTable(void *p, uint32_t ver, uint32_t sz) {
  auto *t = static_cast<const T *>(p);
  if (!t || t->version != ver || t->size != sz) return nullptr;
  return t;
}
bool allFns(const void *const *fns, size_t n) {
  for (size_t i = 0; i < n; i++) if (!fns[i]) return false;
  return true;
}

int install(const PgrVgpControllerConfig *c) {
  if (!c) return PGR_VGC_ERR_NULL_ARG;
  if (c->version != PGR_VGC_CONFIG_VERSION) return PGR_VGC_ERR_BAD_VERSION;
  if (c->size != sizeof(*c)) return PGR_VGC_ERR_BAD_SIZE;
  // Every table pointer + shared primitive must be non-null.
  void *const *ptrs = reinterpret_cast<void *const *>(reinterpret_cast<const char *>(c) + 8);
  for (int i = 0; i < 19; i++) if (!ptrs[i]) return PGR_VGC_ERR_MISSING_PTR;

  auto pol = checkTable<PgrVgpPolicyApi>(c->policy_table, PGR_VP_API_VERSION, sizeof(PgrVgpPolicyApi));
  auto act = checkTable<PgrVgpActionsApi>(c->actions_table, PGR_VA_API_VERSION, sizeof(PgrVgpActionsApi));
  auto cnd = checkTable<PgrVgpCandidateApi>(c->candidates_table, PGR_VC_API_VERSION, sizeof(PgrVgpCandidateApi));
  auto res = checkTable<PgrVgpResultsApi>(c->results_table, PGR_VR_API_VERSION, sizeof(PgrVgpResultsApi));
  auto dsc = checkTable<PgrVgpDiscoveryApi>(c->discovery_table, PGR_VD_API_VERSION, sizeof(PgrVgpDiscoveryApi));
  auto mnt = checkTable<PgrVgpMaintenanceApi>(c->maintenance_table, PGR_VM_API_VERSION, sizeof(PgrVgpMaintenanceApi));
  auto mrk = checkTable<PgrVgpMarkerApi>(c->marker_table, 1, sizeof(PgrVgpMarkerApi));
  auto itm = checkTable<pgrazor_items_api_v1>(c->items_table, PGRAZOR_ITEMS_API_VERSION, sizeof(pgrazor_items_api_v1));
  auto aux = checkTable<AuxApi>(c->aux_table, 1, sizeof(AuxApi));
  if (!pol || !act || !cnd || !res || !dsc || !mnt || !mrk || !itm || !aux) return PGR_VGC_ERR_BAD_TABLE;
  if (!allFns(reinterpret_cast<const void *const *>(&pol->install), 13) ||
      !allFns(reinterpret_cast<const void *const *>(&act->install), 7) ||
      !allFns(reinterpret_cast<const void *const *>(&cnd->install), 2) ||
      !allFns(reinterpret_cast<const void *const *>(&res->install), 3) ||
      !allFns(reinterpret_cast<const void *const *>(&dsc->install), 4) ||
      !allFns(reinterpret_cast<const void *const *>(&mnt->install), 3) ||
      !allFns(reinterpret_cast<const void *const *>(&mrk->install), 2) ||
      !allFns(reinterpret_cast<const void *const *>(&itm->install), 8) ||
      !allFns(reinterpret_cast<const void *const *>(&aux->install), 3))
    return PGR_VGC_ERR_BAD_TABLE;

  std::lock_guard<std::mutex> g(installMutex);
  if (installed.load()) return memcmp(c, &cfg, sizeof(cfg)) == 0 ? PGR_VGC_OK : PGR_VGC_ERR_RECONFIG;
  cfg = *c;
  POL = pol; ACT = act; CND = cnd; RES = res; DSC = dsc; MNT = mnt; MRK = mrk; ITM = itm; AUX = aux;
  installed.store(true, std::memory_order_release);
  return PGR_VGC_OK;
}

int tick(const PgrVgpControllerTick *in, PgrVgpControllerOut *out) {
  if (!out) return PGR_VGC_ERR_NULL_ARG;
  initOut(out);
  out->status = lastStatus.load();
  out->phase = publishedPhase.load();
  if (!installed.load(std::memory_order_acquire)) return PGR_VGC_ERR_NOT_INSTALLED;
  if (!in || in->version != PGR_VGC_TICK_VERSION) return PGR_VGC_ERR_BAD_VERSION;
  if (in->size != sizeof(*in)) return PGR_VGC_ERR_BAD_SIZE;

  const uint64_t tid = static_cast<uint64_t>(gettid());
  if (in->owner == 0 || in->owner != tid) return PGR_VGC_ERR_OFF_OWNER;
  uint64_t expected = 0;
  ownerTid.compare_exchange_strong(expected, in->owner);
  if (ownerTid.load() != in->owner) return PGR_VGC_ERR_OFF_OWNER;

  bool idle = false;
  if (!busy.compare_exchange_strong(idle, true)) return PGR_VGC_OK;  // reentrant tick: no-op
  struct BusyExit { ~BusyExit() { busy.store(false); } } exit;

  const uint64_t now = nowMs();
  if (!now) return PGR_VGC_ERR_CLOCK_FAILURE;
  tickStarted = now;

  if (stopped.load()) {
    freeAllRoots(out);
    POL->reset(in->owner);
    setStatus(out, PGR_VGC_ST_OFF);
    finalize(out);
    return PGR_VGC_OK;
  }

  const bool stale = in->generation != generationCounter.load();
  const int policyRc = POL->update(in->epoch, in->owner);
  if (policyRc < 0) { pauseError(out, policyRc); finalize(out); return PGR_VGC_OK; }

  // Age-out the manual exclusion while the map is visible.
  {
    std::lock_guard<std::mutex> g(manualMutex);
    if (manualTarget && (in->flags & PGR_VGC_TF_MAP_VISIBLE) && now - manualAt > 2000) rootFree(manualTarget);
  }

  tickMarker(in, out, now);
  if (tickTransfer(in, out, now)) { finalize(out); return PGR_VGC_OK; }
  if (pend.active) { tickPending(in, out, now, stale); finalize(out); return PGR_VGC_OK; }
  tickScan(in, out, now, stale);
  finalize(out);
  return PGR_VGC_OK;
}

int observe(void *gui, void *target, uint64_t owner) {
  if (!installed.load(std::memory_order_acquire)) return PGR_VGC_ERR_NOT_INSTALLED;
  if (owner == 0 || owner != static_cast<uint64_t>(gettid())) return PGR_VGC_ERR_OFF_OWNER;
  uint64_t unset = 0; ownerTid.compare_exchange_strong(unset, owner);
  if (ownerTid.load() != owner) return PGR_VGC_ERR_OFF_OWNER;
  if (stopped.load()) return PGR_VGC_OK;
  if (!exactClass(gui, cfg.class_nameplate)) return PGR_VGC_OK;  // not the nameplate: ignore
  const bool wild = target && exactClass(target, cfg.class_wild);
  uint64_t fresh = wild ? rootNew(target) : 0;
  // Preserve the previous rooted exclusion on allocation failure. The adapter disables VGP.
  if (wild && !fresh) return PGR_VGC_ERR_MISSING_PTR;
  std::lock_guard<std::mutex> g(manualMutex);
  rootFree(manualTarget);  // root-replacement already taken before freeing the old root
  manualTarget = fresh;
  manualAt = nowMs();
  return PGR_VGC_OK;
}

int transfer_feedback(uint64_t token, uint32_t state, uint64_t owner) {
  if (!installed.load(std::memory_order_acquire)) return PGR_VGC_ERR_NOT_INSTALLED;
  if (owner == 0 || owner != static_cast<uint64_t>(gettid()) || owner != ownerTid.load()) return PGR_VGC_ERR_OFF_OWNER;
  if (state > PGR_VGC_FB_KEPT) return PGR_VGC_ERR_BAD_VERSION;
  if (!xfer.active || !xfer.handed || xfer.token != token) return PGR_VGC_OK;  // stale: ignored
  xfer.feedback = state;
  xfer.feedback_set = true;
  return PGR_VGC_OK;
}

int revoke(void) {
  if (!installed.load(std::memory_order_acquire)) return PGR_VGC_ERR_NOT_INSTALLED;
  std::lock_guard<std::mutex> g(gate);
  generationCounter.fetch_add(1);
  ACT->stop(ownerTid.load());
  return PGR_VGC_OK;
}

int stop(uint64_t owner) {
  if (!installed.load(std::memory_order_acquire)) return PGR_VGC_ERR_NOT_INSTALLED;
  stopped.store(true);
  {
    std::lock_guard<std::mutex> g(gate);
    generationCounter.fetch_add(1);
    ACT->stop(ownerTid.load());
  }
  // Root cleanup happens only on the latched owner thread; a foreign stop defers it to the owner's tick().
  if (owner != 0 && owner == static_cast<uint64_t>(gettid()) && ownerTid.load() == owner) {
    bool idle = false;
    if (busy.compare_exchange_strong(idle, true)) {
      struct BusyExit { ~BusyExit() { busy.store(false); } } exit;
      PgrVgpControllerOut scratch{};
      initOut(&scratch);
      freeAllRoots(&scratch);
      POL->reset(owner);
      publishedPhase.store(PGR_VGC_PH_NONE);
    }
  }
  return PGR_VGC_OK;
}

int sample(PgrVgpControllerSample *out) {
  if (!out) return PGR_VGC_ERR_NULL_ARG;
  out->version = PGR_VGC_SAMPLE_VERSION;
  out->size = sizeof(*out);
  out->roots = rootCount.load();
  out->phase = publishedPhase.load();
  out->status = lastStatus.load();
  out->error = lastError.load();
  out->generation = generationCounter.load();
  return PGR_VGC_OK;
}

uint64_t generation(void) { return generationCounter.load(); }

const PgrVgpControllerApi api = {PGR_VGC_API_VERSION, sizeof(PgrVgpControllerApi),
                                 install, tick, observe, transfer_feedback,
                                 revoke, stop, sample, generation};

}  // namespace

extern "C" const void *pgrazor_vgp_controller_api(uint32_t version) {
  return version == PGR_VGC_API_VERSION ? &api : nullptr;
}
