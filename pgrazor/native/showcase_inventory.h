#pragma once
#include "showcase_score.h"
#include "showcase_policy.h"
namespace pgr_showcase {
struct InventoryRequest {
  ScoreRequest gate;
  uintptr_t bag=0, game_master=0;
  std::array<uintptr_t,kTypes> contests{}; // borrowed strong handles
  // Optional caller-owned, locally constructed ContestProto with both scalar
  // reference fields zero. Uses the game's normal species/form scoring for
  // predicted points where no actual contest is supplied. Never a guarantee
  // about a future contest with a different scalar reference.
  uintptr_t prediction_reference=0;
  bool (*authorized)(void*)=nullptr;
  void* authorization_context=nullptr;
};
// Synchronous, read-only. Every handle remains owned by the caller. No RPC,
// transfer authorization or retained managed state. Failure clears rows.
bool scan_inventory(const InventoryRequest&,std::vector<Row>& rows,const char*& error);
}
