#pragma once
#include <cstdint>

// Native Showcase tag RPC builder + promise poller.
//
// Starts EXACTLY ONE managed Tagging operation per call on the proven Unity owner,
// then (separately) polls the resulting promise. No hooks, no transfer operations,
// no cache reconciliation and NO retained global managed state. Every managed step
// is fenced by an absolute CLOCK_BOOTTIME lease (<=2500 ms remaining), the exact
// active scheduler generation, and an attached IL2CPP thread; nothing is dispatched
// once the lease is lost. Reentry is refused.
//
// The two operations are:
//   * create: AddTag(System.String, PokemonTagColor.Yellow) -> IPromise<CreatePokemonTagOutProto>
//             The display name is derived from the canonical type index 0..17
//             (e.g. index 9 -> "Showcase \xC2\xB7 Fire"). The builder never adopts an
//             existing tag by display name; AddTag creates a fresh owned tag.
//   * add/remove: SetPokemonTags(List<PokemonTagChanges>) -> IPromise<Boolean> for ONE
//             Pokemon and ONE caller-owned tag. Only that single membership is touched;
//             unrelated memberships are never altered.
//
// The caller (the Showcase keeper host) is responsible for having already selected the
// winners and verified account / owned-tag ownership / ranking generation and lease via
// showcase_tags.h before invoking this builder. See showcase_bindings.cpp / showcase_score.cpp
// for the sibling lease model and batch_evolution.cpp for the SetPokemonTags field contract.

namespace pgr_showcase {

enum class TagRpcOp {
  create,          // AddTag(name, Yellow); type_index selects the name. pokemon/tag ignored.
  add,             // SetPokemonTags: put `tag` in this Pokemon's TagsToAdd.
  remove           // SetPokemonTags: put `tag` in this Pokemon's TagsToRemove.
};

struct TagRpcRequest {
  uint64_t owner = 0;          // proven Unity owner tid (must equal gettid()).
  uint64_t generation = 0;     // exact active scheduler generation authorizing this request.
  uint64_t deadline_ms = 0;    // absolute CLOCK_BOOTTIME deadline; never extended here.
  TagRpcOp op = TagRpcOp::create;
  // Borrowed STRONG GC handles held by the caller throughout the synchronous call.
  // Raw pointers are deliberately not accepted across rooting safepoints.
  uintptr_t service = 0;       // ITaggingRpcService instance (required, all ops).
  uintptr_t pokemon = 0;       // Holoholo.Rpc.PokemonProto (required for add/remove).
  uintptr_t tag = 0;           // Holoholo.Rpc.PokemonTagProto membership (required for add/remove).
  uint64_t pokemon_id = 0;     // expected captured inventory ID for add/remove.
  uint64_t tag_id = 0;         // persisted feature-owned tag ID for add/remove.
  unsigned type_index = 0;     // canonical 0..17 (required for create; names the new tag).
  // Optional native-only caller authorization, checked at every managed gate.
  // Borrowed only for this synchronous call; never retained with the promise.
  bool (*authorized)(void*) = nullptr;
  void* authorization_context = nullptr;
};

enum class TagStartStatus {
  dispatched,       // RPC invoked; *out_promise is an OWNED handle the caller polls then frees.
  refused,          // lease/owner/deadline/thread/reentry or a pre-dispatch alloc fault; nothing sent.
  bad_binding,      // runtime/metadata signatures not resolvable/validated; nothing sent.
  bad_identity,     // a borrowed handle's class/args were wrong; nothing sent.
  outcome_unknown,  // dispatch_started, then error/expiry/exception. The RPC MAY have reached the
                    // server. The caller MUST reconcile via cache and MUST NEVER auto-retry.
};

enum class TagPollStatus {
  pending,          // promise not settled; keep polling before the deadline.
  created,          // create settled: Success + nonzero created tag Id; *out_tag_id is set.
  confirmed,        // add/remove settled: promise resolved true.
  refused,          // lease lost/reentry while polling; promise retained, poll again under a fresh lease.
  rejected,         // settled but failed (error, non-Success, zero Id, or false). Terminal.
  bad_binding,      // runtime/metadata signatures not resolvable/validated.
  outcome_unknown,  // managed exception/expiry mid-read. Terminal; do not auto-retry.
};

// Start exactly one RPC. On TagStartStatus::dispatched, *out_promise is an OWNED GC handle
// (the pending promise); the caller must poll it with poll_tag_rpc and free it on the Unity
// owner. On every other status *out_promise is 0. The module frees all of its own temporary
// roots before returning and retains no managed state.
TagStartStatus start_tag_rpc(const TagRpcRequest& request, uintptr_t& out_promise);

// Poll a promise previously returned by start_tag_rpc for the SAME op. Does NOT free the
// handle (the caller frees it on the owner). Polling only reports that the promise settled;
// it does NOT claim cache reconciliation, so the caller must separately observe the tag cache
// before advancing to the next phase.
TagPollStatus poll_tag_rpc(const TagRpcRequest& request, uintptr_t promise, uint64_t& out_tag_id);

}  // namespace pgr_showcase
