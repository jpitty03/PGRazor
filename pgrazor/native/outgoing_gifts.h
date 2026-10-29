#pragma once
#include <stdint.h>

// Feature 22 (API 1). Native OUTGOING bundled-gift + sticker controller: a focused C++ port of the
// bundled-send machinery in runtime/features/gifts.js -- bundledGift (~81), bundledClear (~85),
// bundledQueue (~93), bundledTick (~105-155), the sendReplacement eligibility (~273-291) and the
// ownedCallback result/finally match (~293-297). It owns the single pending send transaction, its
// GC roots, the first-sendable-gift enumeration, the optional sticker reserve and the SendGift RPC.
//
// SCOPE / NOT: ONLY the bundled outgoing send. The incoming gift-open path (feature 17), the gift
// list / profile walk UI and the guest-open path stay in JS. This module installs NO Dobby/Gum
// hook and NEVER calls back into JS. The host keeps its existing OnGiftClick (choose) Interceptor
// replacement -- which forwards into queue() -- and its rpcResult/rpcFinally Gum observers, which
// forward into observe(). The adapter maps config pointers straight off the validated p.gifts
// metadata (no offset discovery here) and drains sample()/next_event().
//
// Single pending transaction. queue() captures+roots panel/profile/friend and the bounded recipient
// id; tick() builds and ISSUES it. "issued" is latched BEFORE the SendGift invoke, so an uncertain
// send is never retried; a 30 s timeout is telemetry/quarantine only (the request is retained until
// a matching finally or a stop). An issued transaction survives profile-close / off / lease expiry
// (result bookkeeping still runs); an unissued one cancels on those. A reserved sticker is returned
// exactly once on cleanup. See outgoing_gifts.cpp for the reentrancy / rooting / epoch discipline.

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "OutgoingGifts native ABI requires LP64");

enum PgrOutgoingGiftsInfo {
  PGR_OG_FEATURE_ID      = 22,
  PGR_OG_API_VERSION     = 1,
  PGR_OG_CONFIG_VERSION  = 1,
  PGR_OG_SAMPLE_VERSION  = 1,
  PGR_OG_EVENT_VERSION   = 1,
  PGR_OG_MAX_LEASE_MS    = 2500,   // residual foreground lease ceiling (queue/reserve/send gate)
  PGR_OG_TXN_TIMEOUT_MS  = 30000,  // issued-transaction quarantine deadline (JS r.deadline = now+30000)
  PGR_OG_EVENT_CAP       = 32,     // bounded FIFO depth; overflow fails closed
  PGR_OG_ID_MAX          = 256,    // recipient/callback id length window (JS giftText 1..256)
  PGR_OG_STICKER_SCAN_MAX = 4096,  // sticker inventory scan bound (JS count>4096 throws)
  PGR_OG_RESERVE_COUNT   = 1,      // ReserveStickers / ReturnReservedStickers count
  PGR_OG_MASK_SEND       = 2,      // giftMask bit: bundled send enabled
  PGR_OG_MASK_STICKER    = 4       // giftMask bit: attach a sticker
};

// install()/update()/observe()/tick()/sample()/next_event() status codes (queue() returns 0/1).
enum PgrOutgoingGiftsStatus {
  PGR_OG_OK                 = 0,
  PGR_OG_ERR_INVALID_ARG    = -1,
  PGR_OG_ERR_BAD_VERSION    = -2,
  PGR_OG_ERR_BAD_SIZE       = -3,
  PGR_OG_ERR_MISSING_PTR    = -4,
  PGR_OG_ERR_NOT_INSTALLED  = -6,
  PGR_OG_ERR_RECONFIG       = -9,
  PGR_OG_ERR_CLOCK_FAILURE  = -10
};

// observe() kinds. RPC_RESULT carries (callback, response); RPC_FINALLY carries (callback, null);
// PROFILE_CLOSE carries (profile) and cancels an unissued transaction.
enum PgrOutgoingGiftsObserve {
  PGR_OG_OBSERVE_PROFILE_CLOSE = 0,
  PGR_OG_OBSERVE_RPC_RESULT    = 1,
  PGR_OG_OBSERVE_RPC_FINALLY   = 2
};

// queue() result.
enum PgrOutgoingGiftsQueue {
  PGR_OG_QUEUE_ORIGINAL = 0,  // not eligible: the host runs the original OnGiftClick
  PGR_OG_QUEUE_HANDLED  = 1    // handled (queued, blocked, already-pending, or error)
};

// next_event() record kinds. Codes/ints only -- never a player/gift/sticker id string.
enum PgrOutgoingGiftsEventKind {
  PGR_OG_EVENT_STATUS  = 1,  // id=PgrOutgoingGiftsMsg, aux=result int (FAILED_RESULT only) -> gift-status
  PGR_OG_EVENT_REQUEST = 2,  // aux=sticker(1/0)                                             -> gift-bundled-request
  PGR_OG_EVENT_RESULT  = 3,  // id=result, aux bit0=success, bit1=result known               -> gift-bundled-result
  PGR_OG_EVENT_ERROR   = 4   // id=PgrOutgoingGiftsError                                     -> gift-error
};

// Status message codes (PGR_OG_EVENT_STATUS.id). The adapter reproduces the exact JS strings.
enum PgrOutgoingGiftsMsg {
  PGR_OG_MSG_PREPARING      = 1,  // 'Preparing gift'
  PGR_OG_MSG_NOT_AVAILABLE  = 2,  // 'Gift sending is not available for this friend yet'
  PGR_OG_MSG_NO_GIFTS       = 3,  // 'No gifts available'
  PGR_OG_MSG_SENDING        = 4,  // 'Sending gift'
  PGR_OG_MSG_SENT           = 5,  // 'Gift sent'
  PGR_OG_MSG_FAILED_NULL    = 6,  // 'Gift send failed; no automatic retry'   (result == null)
  PGR_OG_MSG_FAILED_RESULT  = 7,  // 'Gift send failed (result N)'            (aux = N)
  PGR_OG_MSG_PENDING        = 8,  // 'Gift result pending; no automatic retry'
  PGR_OG_MSG_UNCERTAIN      = 9,  // 'Gift result uncertain; no automatic retry'
  PGR_OG_MSG_ALREADY        = 10  // 'Gift send already pending'
};

// Error reason codes (PGR_OG_EVENT_ERROR.id). Map onto JS 'gift-error' reasons; the coordinator
// adapter reconstructs the bundled-send / bundled-trigger / sticker-release prefixes.
enum PgrOutgoingGiftsError {
  PGR_OG_ERR_NONE            = 0,
  PGR_OG_ERR_RECIPIENT       = 1,   // gift-recipient-missing (trigger)
  PGR_OG_ERR_SERVICE_TYPE    = 2,   // gift-service-type
  PGR_OG_ERR_INVENTORY       = 3,   // gift-inventory-missing / -result / -entry
  PGR_OG_ERR_GIFT_ID_ZERO    = 4,   // gift-id-zero
  PGR_OG_ERR_NIA             = 5,   // gift-nia-service / gift-nia-id
  PGR_OG_ERR_STICKER_SVC     = 6,   // gift-sticker-service / sticker-inventory-type/-size
  PGR_OG_ERR_STICKER_ID      = 7,   // gift-sticker-id
  PGR_OG_ERR_GCHANDLE        = 8,   // a GC root could not be allocated
  PGR_OG_ERR_INVOKE          = 9,   // a managed invoke threw (send path)
  PGR_OG_ERR_STICKER_RELEASE = 10,  // ReturnReservedStickers threw during cleanup
  PGR_OG_ERR_OVERFLOW        = 11   // FIFO overflow (fail-closed)
};

// Fixed LP64 config (488 bytes): 8-byte header + 59 validated il2cpp pointers + 2 int32 literals.
// Every pointer is an already-validated export / MethodInfo* / Il2CppClass* / FieldInfo* packed by
// the JS adapter straight off p.gifts; the native layer re-checks the header and rejects any null.
// No hook address is carried. See the static_asserts in outgoing_gifts.cpp for the authoritative
// offsets.
struct PgrOutgoingGiftsConfig {
  uint32_t version;  // == PGR_OG_CONFIG_VERSION
  uint32_t size;     // == sizeof(PgrOutgoingGiftsConfig)

  // il2cpp runtime exports.
  void *gchandle_new;              // uintptr_t (void* obj, int pinned)
  void *gchandle_get_target;       // void*     (uintptr_t handle)
  void *gchandle_free;             // void      (uintptr_t handle)
  void *object_get_class;          // void*     (void* obj)
  void *object_unbox;              // void*     (void* boxed)
  void *object_get_virtual_method; // void*     (void* obj, MethodInfo*) -> concrete MethodInfo*
  void *runtime_invoke;            // void*     (MethodInfo*, void* obj, void** params, void** exc)
  void *field_get_value;           // void      (void* obj, FieldInfo*, void* out)
  void *field_set_value;           // void      (void* obj, FieldInfo*, void* value) [unused; parity]
  void *string_length;             // int       (void* str)
  void *string_chars;              // uint16_t* (void* str)

  // MethodInfo* (via runtime_invoke; the *_virtual ones are dispatched via object_get_virtual_method).
  void *m_panel;          // GiftProfileGui.get_CurrentlyOpenFriendProfileInfoPanel -> panel
  void *m_disabled;       // GiftProfileGui.get_ActionButtonsDisabled -> bool
  void *m_active;         // UnityEngine.Behaviour.get_isActiveAndEnabled -> bool
  void *m_interactable;   // UnityEngine.UI.Selectable.IsInteractable -> bool
  void *m_sendable;       // service.get_SendableGiftsInInventory -> IEnumerable<GiftBoxProto>
  void *m_get_enumerator; // iterable.GetEnumerator (virtual) -> IEnumerator<GiftBoxProto>
  void *m_move_gift;      // IEnumerator.MoveNext (virtual) -> bool
  void *m_current_gift;   // enumerator.get_Current (virtual) -> GiftBoxProto
  void *m_dispose_gift;   // IDisposable.Dispose (virtual) -> void
  void *m_nia_id;         // niaUtil.GetPlayerIdOrNiaAccountId (virtual) (PlayerSummaryProto) -> string
  void *m_inventory;      // stickerSvc.GetStickerInventory(bool) -> List<StickerProto>
  void *m_sticker_count;  // list.get_Count -> int
  void *m_sticker_item;   // list.get_Item(int) -> StickerProto
  void *m_quantity;       // stickerSvc.StickerCountExcludingReserved(StickerProto) -> int
  void *m_reserve;        // stickerSvc.ReserveStickers(string, int) -> void
  void *m_return_sticker; // stickerSvc.ReturnReservedStickers(string, int) -> void
  void *m_rpc_send;       // rpc.SendGift(UInt64, string, string, StickerProto, bool) -> IPromise
  void *m_alive;          // UnityEngine.Object.op_Implicit(Object) -> bool [static]

  // Exact Il2CppClass* identities.
  void *class_panel;        // GIFT_TYPES[5936] FriendProfileInfoPanel (queue panel exact)
  void *class_friend;       // Niantic.Platform.Internal.Protos.FriendDetailsProto
  void *class_player;       // Niantic.Platform.Internal.Protos.PlayerSummaryProto
  void *class_service;      // GIFT_TYPES[6154] gifting service
  void *class_rpc;          // GIFT_TYPES[6149] rpc service
  void *class_sticker_svc;  // GIFT_TYPES[6164] sticker service
  void *class_gift_box;     // Holoholo.Rpc.GiftBoxProto
  void *class_sticker;      // Holoholo.Rpc.StickerProto
  void *class_sticker_list; // List<StickerProto> (stickerListClass)
  void *class_send_out;     // Holoholo.Rpc.SendGiftOutProto
  void *class_callback;     // the SendGift callback nested type (callbackClass)
  void *class_bool;         // System.Boolean (boxed-bool returns)
  void *class_int32;        // System.Int32   (boxed-int returns: count/quantity)
  void *class_string;       // System.String

  // FieldInfo* (read via field_get_value).
  void *f_data;             // panel.thisData
  void *f_panel_friend;     // data.FriendProto -> FriendDetailsProto
  void *f_player;           // FriendDetailsProto.player_ -> PlayerSummaryProto
  void *f_player_id;        // PlayerSummaryProto.playerId_ -> String (recipient id)
  void *f_choose_button;    // panel.sendGiftButton -> Selectable
  void *f_check_pending;    // panel.dkto (bool)
  void *f_check_result;     // panel.dktn (Int32 enum)
  void *f_panel_gifting;    // panel.dkrv -> gifting service
  void *f_panel_rpc;        // panel.dkrw -> rpc service
  void *f_nia_util;         // panel.dksq -> nia util service
  void *f_rpc_stickers;     // rpc.dlil -> sticker service
  void *f_gift_id;          // GiftBoxProto.GiftboxId (UInt64)
  void *f_sticker_id;       // StickerProto.StickerId (String)
  void *f_send_result;      // SendGiftOutProto.Result (Int32 enum)
  void *f_callback_service; // callback.<>4__this -> rpc service
  void *f_callback_id;      // callback.playerId -> String (recipient id)

  // Validated enum literals.
  int32_t check_success;    // gifts.checkSuccess (panel.dktn Success)
  int32_t send_success;     // gifts.sendSuccess  (SendGiftOutProto.Result Success)
};

static_assert(sizeof(PgrOutgoingGiftsConfig) == 488,
              "PgrOutgoingGiftsConfig must remain 488 bytes on LP64");

// Fixed LP64 telemetry gauge (88 bytes). Pull-only; the native layer never calls JS.
struct PgrOutgoingGiftsSample {
  uint32_t version;         // == PGR_OG_SAMPLE_VERSION
  uint32_t size;            // == sizeof(PgrOutgoingGiftsSample)
  uint64_t request_serial;  // bumps on each issued send (gift-bundled-request)
  uint64_t result_serial;   // bumps on each completed result (gift-bundled-result)
  uint64_t error_serial;    // bumps on each error
  uint64_t status_serial;   // bumps on each status message
  uint64_t last_tid;        // owner tid of the last event
  uint32_t roots;           // durable GC roots + active frames (coordinator finish guard)
  uint32_t active;          // a transaction exists / is pending (1/0)
  uint32_t issued;          // the SendGift was issued (1/0)
  uint32_t result_known;    // a result was recorded (1/0)
  int32_t  result;          // last recorded SendGiftOutProto.Result (valid iff result_known)
  uint32_t timed_out;       // the issued transaction passed its quarantine deadline (1/0)
  uint32_t fault;           // 1 once latched (FIFO overflow)
  uint32_t overflow;        // 1 if the FIFO ever overflowed
  uint32_t pending_events;  // records currently queued in the FIFO
  uint32_t reserved;        // 0
};

static_assert(sizeof(PgrOutgoingGiftsSample) == 88,
              "PgrOutgoingGiftsSample must remain 88 bytes on LP64");

// Fixed LP64 drain record (16 bytes). id/aux are codes/ints only, never an id string.
struct PgrOutgoingGiftsEvent {
  uint32_t kind;      // PgrOutgoingGiftsEventKind
  int32_t  id;        // status msg / error reason / result value
  int32_t  aux;       // sticker / success / result int
  int32_t  reserved;  // 0
};

static_assert(sizeof(PgrOutgoingGiftsEvent) == 16,
              "PgrOutgoingGiftsEvent must remain 16 bytes on LP64");

// Fixed LP64 table: 8-byte header + 7 function pointers = 64 bytes.
struct PgrOutgoingGiftsApi {
  uint32_t version;  // == PGR_OG_API_VERSION
  uint32_t size;     // == sizeof(PgrOutgoingGiftsApi)
  int (*install)(const PgrOutgoingGiftsConfig *config);
  // Refresh the policy gate: mask bits 2(send)/4(sticker) and a residual lease clamped to 2500 ms.
  // Carries proven_tid; a foreign thread (once the owner is latched) can only disarm. Any thread.
  int (*update)(uint32_t mask, uint64_t remaining_lease_ms, uint64_t proven_tid);
  // sendReplacement eligibility (OnGiftClick). Returns PGR_OG_QUEUE_ORIGINAL (0) to let the host run
  // the original, or PGR_OG_QUEUE_HANDLED (1) when queued / blocked / already-pending / errored.
  // Owner thread only; off the owner thread or during cleanup it returns 0 (original runs).
  int (*queue)(void *panel, void *profile, uint64_t proven_tid);
  // Forward one synchronous observation (see PgrOutgoingGiftsObserve) on the proven Unity owner.
  int (*observe)(uint32_t kind, void *obj, void *arg, uint64_t proven_tid);
  // One owner-thread bundledTick pass: complete / quarantine an issued txn, else build+issue the
  // pending one. stopping != 0 cancels the transaction (returning any reserved sticker). No-op off
  // the owner thread (never frees).
  int (*tick)(uint64_t proven_tid, int stopping);
  int (*sample)(PgrOutgoingGiftsSample *out_sample);
  // Pop one FIFO record (returns 1), report empty (0), or a negative status. Any thread.
  int (*next_event)(PgrOutgoingGiftsEvent *out);
};

static_assert(sizeof(PgrOutgoingGiftsApi) == 64,
              "PgrOutgoingGiftsApi must remain a 64-byte version-1 table");

// Hidden getter. NOT exported by the version script; the coordinator registers it in query_feature
// under feature id 22 and answers only on an exact version match.
const void *pgrazor_outgoing_gifts_api(uint32_t version);

#ifdef __cplusplus
}
#endif
