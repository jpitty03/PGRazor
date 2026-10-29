#include "routes_tracking.h"
#include "routes_observers.h"  // feature-46 API struct + hidden getter (read-only dependency; not edited)
#include "hook_engine.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <unistd.h>

// Feature 50 native route-tracking / location-override / waypoint hooks. See routes_tracking.h and
// .artifacts/native-migration/routes-tracking-contract.txt for the ABI, the signature evidence and the
// division of labour with feature 46 (routes_observers) and runtime/features/routes.js. This module owns
// NO state: all observer state (tracking depth table, lastTrackingError, lastWaypointAt, the event ring)
// lives in feature 46, which this module reaches ONLY through its hidden getter. Three Dobby hooks are
// installed; each chains its original exactly once with its arguments and return value preserved.
//
// Thread model: the three replacements may run on any Unity thread. They touch only atomics (the published
// config + originals) and forward to feature 46's lite entries, which do their own tid==gettid()/attached/
// lease gating and own locking. No mutex is held across a chained original.

static_assert(sizeof(PgrRoutesTrackingConfig) == 64, "config 64");
static_assert(sizeof(PgrRoutesTrackingApi) == 24, "table 24");

namespace {

// --- native ABIs of the three hooked functions (see header for the derivation) ----
// CheckTrackingErrorStatus: instance, 0 managed args, returns ErrorType (int32). Trailing MethodInfo*.
using TrackingFn = int32_t (*)(void *self, const void *method);
// bjfz (waypoint add): instance, 1 managed arg (RouteWaypointProto), returns void. Trailing MethodInfo*.
using WaypointFn = void (*)(void *self, void *waypoint, const void *method);
// IsServiceEnabledByUser: static icall, no args, no MethodInfo, returns bool (1 byte).
using LocationFn = uint8_t (*)(void);
// il2cpp_object_get_class(obj) -> Il2CppClass*.
using ObjClass = void *(*)(void *obj);

// --- lifetime / publication --------------------------------------------------------
std::mutex g_install_mutex;
PgrRoutesTrackingConfig g_cfg{};
std::atomic<bool> g_installed{false};
bool g_install_failed = false;  // sticky; protected by g_install_mutex

std::atomic<TrackingFn> g_orig_tracking{nullptr};
std::atomic<WaypointFn> g_orig_waypoint{nullptr};
std::atomic<LocationFn> g_orig_location{nullptr};

// feature-46 routes_observers API, latched from its hidden getter at install(). The lite entries we call
// are pure native bookkeeping (no managed work) and fail benignly (return 0) while feature 46 is itself
// not installed/leased, so forwarding before feature 46 arms is safe.
std::atomic<const PgrRoutesObserversApi *> g_obs{nullptr};

uint64_t current_tid() { return static_cast<uint64_t>(static_cast<pid_t>(gettid())); }

ObjClass obj_class() { return reinterpret_cast<ObjClass>(g_cfg.object_get_class); }

// Publish-or-die original accessor (a live hook must have a published trampoline; never fabricate).
template <typename Fn>
Fn original_for(std::atomic<Fn> &slot) {
  Fn orig = slot.load(std::memory_order_acquire);
  if (!orig) {
    std::lock_guard<std::mutex> lock(g_install_mutex);
    orig = slot.load(std::memory_order_acquire);
  }
  if (!orig) std::abort();
  return orig;
}

// --- hook replacements -------------------------------------------------------------

// CheckTrackingErrorStatus: tracking_enter BEFORE, tracking_leave(ret) AFTER the original. Balanced even
// on reentry (each call owns its own enter/leave pair) and on an expired lease (leave is invoked iff enter
// counted, so feature 46's depth bookkeeping never underflows). self + MethodInfo forwarded verbatim.
int32_t hook_tracking(void *self, const void *method) {
  TrackingFn orig = original_for(g_orig_tracking);
  const PgrRoutesObserversApi *obs = g_installed.load(std::memory_order_acquire)?g_obs.load(std::memory_order_acquire):nullptr;
  const uint64_t tid = current_tid();
  const bool counted = obs && obs->tracking_enter(tid) == 1;
  const int32_t ret = orig(self, method);
  if (counted) obs->tracking_leave(tid, ret);
  return ret;
}

// IsServiceEnabledByUser: chain the original FIRST, then override to true ONLY inside a nested valid gate
// (route tracking depth>0 && lease live, as judged by feature 46). Outside that gate the original bool is
// returned unchanged. No args / no MethodInfo (icall).
uint8_t hook_location(void) {
  LocationFn orig = original_for(g_orig_location);
  const uint8_t ret = orig();
  const PgrRoutesObserversApi *obs = g_installed.load(std::memory_order_acquire)?g_obs.load(std::memory_order_acquire):nullptr;
  if (obs && obs->location_gate(current_tid()) == 1) return 1;
  return ret;
}

// bjfz (waypoint add): observe the raw receiver arg BEFORE the original. The LatDegrees/LngDegrees reads are
// plain, guaranteed-nonallocating loads at validated offsets, so no GC root is taken and the incoming
// receiver is never relocated (we perform no managed invoke). lat/lon go to locals and through feature 46's
// waypoint_observed (which owns the throttle + event). ALL arguments are then forwarded to the original
// unchanged.
void hook_waypoint(void *self, void *waypoint, const void *method) {
  WaypointFn orig = original_for(g_orig_waypoint);
  const PgrRoutesObserversApi *obs = g_installed.load(std::memory_order_acquire)?g_obs.load(std::memory_order_acquire):nullptr;
  if (obs && waypoint && obj_class()(waypoint) == g_cfg.class_waypoint) {
    const double lat = *reinterpret_cast<const double *>(
        reinterpret_cast<const unsigned char *>(waypoint) + g_cfg.lat_offset);
    const double lon = *reinterpret_cast<const double *>(
        reinterpret_cast<const unsigned char *>(waypoint) + g_cfg.lng_offset);
    obs->waypoint_observed(current_tid(), lat, lon);
  }
  orig(self, waypoint, method);
}

// --- install -----------------------------------------------------------------------
bool validate_config(const PgrRoutesTrackingConfig *c, int *rc) {
  if (c == nullptr) { *rc = PGR_RT_ERR_NULL_ARG; return false; }
  if (c->version != PGR_RT_CONFIG_VERSION) { *rc = PGR_RT_ERR_BAD_VERSION; return false; }
  if (c->size != sizeof(PgrRoutesTrackingConfig)) { *rc = PGR_RT_ERR_BAD_SIZE; return false; }
  if(c->reserved0||c->reserved1){*rc=PGR_RT_ERR_BAD_SIZE;return false;}
  void *const *ptrs = reinterpret_cast<void *const *>(
      reinterpret_cast<const unsigned char *>(c) + offsetof(PgrRoutesTrackingConfig, object_get_class));
  for (int i = 0; i < PGR_RT_PTR_COUNT; ++i)
    if (ptrs[i] == nullptr) { *rc = PGR_RT_ERR_MISSING_PTR; return false; }
  // Offsets: 8-aligned, within a sane ceiling, and distinct (lat != lng).
  if ((c->lat_offset & 7u) || (c->lng_offset & 7u) ||
      c->lat_offset == 0u || c->lng_offset == 0u ||
      c->lat_offset >= PGR_RT_MAX_FIELD_OFFSET || c->lng_offset >= PGR_RT_MAX_FIELD_OFFSET ||
      c->lat_offset == c->lng_offset) { *rc = PGR_RT_ERR_BAD_OFFSET; return false; }
  // Three distinct patch sites.
  if (c->addr_tracking == c->addr_waypoint || c->addr_tracking == c->addr_location_icall ||
      c->addr_waypoint == c->addr_location_icall) { *rc = PGR_RT_ERR_DUP_ADDR; return false; }
  *rc = 0;
  return true;
}

// Partial installation is permanently passive: retain every published original
// trampoline until process exit. Destroying one can invalidate an in-flight
// replacement/trampoline. g_installed gates ALL observer effects until all3 pass.
int install_hooks() {
  void *o0=nullptr;
  const int r0=pgrazor_hook(g_cfg.addr_tracking,reinterpret_cast<void *>(&hook_tracking),&o0);
  if(o0)g_orig_tracking.store(reinterpret_cast<TrackingFn>(o0),std::memory_order_release);
  if(r0||!o0)return PGR_RT_ERR_HOOK;
  void *o1=nullptr;
  const int r1=pgrazor_hook(g_cfg.addr_waypoint,reinterpret_cast<void *>(&hook_waypoint),&o1);
  if(o1)g_orig_waypoint.store(reinterpret_cast<WaypointFn>(o1),std::memory_order_release);
  if(r1||!o1)return PGR_RT_ERR_HOOK;
  void *o2=nullptr;
  const int r2=pgrazor_hook(g_cfg.addr_location_icall,reinterpret_cast<void *>(&hook_location),&o2);
  if(o2)g_orig_location.store(reinterpret_cast<LocationFn>(o2),std::memory_order_release);
  if(r2||!o2)return PGR_RT_ERR_HOOK;
  return 0;
}

int api_install(const PgrRoutesTrackingConfig *config) {
  int rc = 0;
  if (!validate_config(config, &rc)) return rc;

  // feature 46 must be present (version match). Its static table is returned regardless of its own
  // install state; the lite entries we forward to gate themselves until feature 46 is installed + leased.
  const PgrRoutesObserversApi *obs =
      static_cast<const PgrRoutesObserversApi *>(pgrazor_routes_observers_api(PGR_RO_API_VERSION));
  if (!obs || obs->version != PGR_RO_API_VERSION || obs->size != sizeof(PgrRoutesObserversApi) ||
      !obs->tracking_enter || !obs->tracking_leave || !obs->location_gate || !obs->waypoint_observed)
    return PGR_RT_ERR_NO_OBSERVERS;

  std::lock_guard<std::mutex> install_lock(g_install_mutex);
  if (g_install_failed) return PGR_RT_ERR_HOOK;
  if (g_installed.load(std::memory_order_acquire))
    return std::memcmp(&g_cfg, config, sizeof(*config)) == 0 ? PGR_RT_OK : PGR_RT_ERR_RECONFIG;

  g_cfg = *config;                               // publish before any replacement can read it (under lock)
  g_obs.store(obs, std::memory_order_release);
  const int hrc = install_hooks();
  if (hrc != 0) { g_install_failed = true; return hrc; }
  g_installed.store(true, std::memory_order_release);
  return PGR_RT_OK;
}

int api_installed() { return g_installed.load(std::memory_order_acquire) ? 1 : 0; }

const PgrRoutesTrackingApi kApiTable = {
    PGR_RT_API_VERSION, static_cast<uint32_t>(sizeof(PgrRoutesTrackingApi)),
    &api_install, &api_installed,
};

}  // namespace

extern "C" const void *pgrazor_routes_tracking_api(uint32_t version) {
  if (version != PGR_RT_API_VERSION) return nullptr;
  return &kApiTable;
}
