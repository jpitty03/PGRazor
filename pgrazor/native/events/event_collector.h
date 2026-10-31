#pragma once
#include <stdint.h>
struct PgrEventCollectorState {
  int ready,error,enabled,inflight,timed_out,pending_encounters,stale_encounters;
  uint64_t update_calls,owner_updates,last_update_tid,scan_calls;
  int session_active,pump_reason,list_count,candidate_count,matched_box_count,in_range_count,visible_count,expired_count,zero_expiry_count;
  double nearest_meters;
  uint64_t attempted,completed,failed,skipped_encounters,candy,xl_candy,rare_candy,xl_rare_candy;
};
// Owner pulse only. Initialization/capture remains active when enabled=false.
int pgr_event_collector_tick(bool enabled,uint64_t proven_tid);
// Fresh BOOTTIME location; must be called from owner pulse before tick.
void pgr_event_collector_location(double lat,double lng,uint64_t boottime_ms);
// Thread-safe stop request. Drain on owner pulse; dispatched ownership survives OFF
// until response, to prevent an already-requested encounter screen from appearing.
void pgr_event_collector_stop();
PgrEventCollectorState pgr_event_collector_state();

// pump_reason: 1 disabled,2 stopped,3 enable lease expired,4 cooldown,5 no session,
// 6 stale/missing location,7 inactive session,8 invalid list,9 array root failed,
// 10 no eligible candidate,11 final revalidation,12 dispatch,13 in flight,
// 14 session class mismatch,15 inactive capture,16 session root failed.

// in_range_count is retained for integration compatibility: counts candidates
// with valid finite location; there is no collector-imposed distance limit.

// visible_count counts candidates passing the visibility getter among the
// nearest-candidate scan subset; it is not a census of every visible map box.
// expired_count is diagnostic only: an elapsed local timestamp does not override
// the game's active-list/visibility eligibility or the server's tap response.

// Encounter dispatch may occur after animation, outside bfvy. One rooted
// exact ownership record survives OFF until consumption/manual override. A stale
// record quarantines new collection but remains attributable for a late callback.
// reason17: delayed owned encounter presentation pending (new taps blocked).
