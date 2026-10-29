#pragma once
#include <stdint.h>

// Feature 17. Native INCOMING gift-open controller: a focused C++ port of the incoming-gift
// machinery in runtime/features/gifts.js -- the `incomingGift` slot (~line 16), incomingClear
// (~18), incomingWait (~20), incomingTick (~22-42) and the OPEN-sequence animation-skip policy
// (the giftObserve(m.sequence,...) block at ~248-256), together with the identity bookkeeping
// of the openReady/open/openClose observers (~290-294) for the single owned dialog.
//
// SCOPE. This is ONLY the incoming gift-open controller. Bundled sending, stickers and the
// legacy gift-list / profile UI stay in JS for a later increment; nothing here touches them.
//
// NO HOOKS. This module installs NO Dobby or Gum hooks. The host's existing single Gum
// observation of the gift methods forwards its four events synchronously into the ABI as
// observe(kind,obj,proven_tid) calls on the Unity thread; the shared catch canvas, the RET-only
// guest-visibility path and every other gift observer remain in JS. There are no duplicate
// entry patches and no JS callbacks out of C++ -- the adapter pulls via sample()/drain().
//
// Observer kinds (observe()):
//   READY     (6240 HandleOpenComplete)  the incoming dialog finished opening -> arm the slot.
//   OPEN      (6240 OnOpenClick)          the Open button was pressed -- by the user OR by our
//                                         own automation. It only marks `attempted`; it never
//                                         sets `automated`, preserving the manual-open vs
//                                         automation distinction (a user open skips no animation).
//   CLOSE     (6240 HandleDismiss)        the owned dialog dismissed -> clear the slot. A close
//                                         for a GUI we do not own is ignored.
//   SEQUENCE  (OPEN_SEQUENCE MoveNext)    the gift-open presentation iterator. The state-3 skip is
//                                         written with field_set_value on the validated sequence
//                                         state FieldInfo* (never an arbitrary offset) and ONLY
//                                         when: our slot is owned AND automated, auto-open is on,
//                                         the lease is live, the object is exactly the sequence
//                                         class, its GUI equals our owned GUI, its response is
//                                         exactly OpenGiftOutProto with the Success result, and its
//                                         state is still 0. A manually opened dialog never skips.
//
// The open attempt lives in tick() (the JS incomingTick), runs ONLY on the proven Unity owner,
// and issues OnOpenClick exactly once per owned-GUI open lifecycle (`attempted` latches it). A
// repeated READY on the SAME still-live GUI does NOT reset `attempted`; only a CLOSE (or deadline
// / death / stop) followed by a fresh READY permits a new gift. `attempted` + `automated` are set
// BEFORE the Open invoke, so an uncertain invoke is never retried. Every mutation (the Open invoke
// and the sequence state-3 write) re-checks a fresh CLOCK_BOOTTIME lease, the revocation
// generation, the fault latch and the owner thread immediately after its final managed lookup.
//
// Telemetry is pull-only. sample() is a gauge snapshot (owned/root counts for teardown gating,
// fault, FIFO overflow, last wait reason). drain() pops one bounded-FIFO event record at a time;
// records carry reason CODES only -- never sender strings or friend ids. A FIFO overflow fails
// closed (fault latched; no further issue) rather than dropping silently.
//
// Loader contract (matches the other native features):
//   root.PGRazorNativeFeature(17,1,64) -> query_feature(17,1) -> pgrazor_incoming_gifts_api(1).
// The adapter factory returns null ONLY when the native pilot is absent; inert/missing metadata
// throws (fails local); an install failure throws (no second-engine fallback).
//
// Ownership / concurrency (enforced in incoming_gifts.cpp; same guarantees as iv_display.cpp):
//   * LP64 only; GC handles are pointer-sized.
//   * Immutable config published once under the install mutex; a mismatched re-install is
//     rejected. install() installs no hook.
//   * The permanent Unity owner is latched once from a genuine on-thread observe()/tick() whose
//     tid matches proven_tid. update() carries no tid and can only widen/clear the gate.
//   * No mutex is held across a managed call, a GC-handle call, or an original. The single durable
//     GUI root is created/refreshed/freed only on the owner thread; the root field is detached
//     before the handle is freed so a reentrant close/stop never double-frees.
//   * CLOCK_BOOTTIME residual lease <= 2500 ms; the open lifecycle deadline is 30 s; the attempt
//     is throttled to 100 ms, all mirroring the JS.
//   * A stopping tick clears the root even if the lease expired; roots are never freed off the
//     owner thread. root_count (durable root + active observe/tick depth) lets the poll loop gate
//     finish on it reaching 0.

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "IncomingGifts native ABI requires LP64");

enum PgrIncomingGiftsInfo {
  PGR_GIFT_FEATURE_ID = 17,
  PGR_GIFT_API_VERSION = 1,
  PGR_GIFT_CONFIG_VERSION = 1,
  PGR_GIFT_SAMPLE_VERSION = 1,
  PGR_GIFT_EVENT_VERSION = 1,
  PGR_GIFT_MAX_LEASE_MS = 2500,  // residual foreground lease ceiling
  PGR_GIFT_DEADLINE_MS = 30000,  // per-open-lifecycle deadline (JS incomingGift.deadline)
  PGR_GIFT_THROTTLE_MS = 100,    // attempt throttle (JS incomingGift.next)
  PGR_GIFT_EVENT_CAP = 32,       // bounded FIFO depth; overflow fails closed
  PGR_GIFT_SENDER_MIN = 1,       // valid sender string length window (JS giftText)
  PGR_GIFT_SENDER_MAX = 256,
  PGR_GIFT_STATE_SKIP = 3        // value written to the sequence state to skip the animation
};

// install()/update()/observe()/tick()/sample()/drain()/is_open() status codes.
enum PgrIncomingGiftsStatus {
  PGR_GIFT_OK = 0,
  PGR_GIFT_ERR_INVALID_ARG = -1,
  PGR_GIFT_ERR_BAD_VERSION = -2,
  PGR_GIFT_ERR_BAD_SIZE = -3,
  PGR_GIFT_ERR_MISSING_PTR = -4,
  PGR_GIFT_ERR_NOT_INSTALLED = -6,
  PGR_GIFT_ERR_RECONFIG_MISMATCH = -9,
  PGR_GIFT_ERR_CLOCK_FAILURE = -10
};

// observe() kinds.
enum PgrIncomingGiftsObserve {
  PGR_GIFT_OBSERVE_READY = 1,
  PGR_GIFT_OBSERVE_OPEN = 2,
  PGR_GIFT_OBSERVE_CLOSE = 3,
  PGR_GIFT_OBSERVE_SEQUENCE = 4
};

// drain() event record kind. DIALOG (the READY arrival) is surfaced only as a serial in the
// sample, because the JS gift-dialog event also carries bundled profile state; the adapter emits
// it directly from ready(). The FIFO carries the reason-coded events the inline block emitted.
enum PgrIncomingGiftsEventKind {
  PGR_GIFT_EVENT_WAIT = 1,    // -> gift-wait {reason, mask}
  PGR_GIFT_EVENT_ISSUED = 2,  // -> onIssued() + gift-status 'Opening gift'
  PGR_GIFT_EVENT_ERROR = 3,   // -> gift-error {reason}
  PGR_GIFT_EVENT_SKIP = 4     // -> gift-presentation-skipped {kindOfGift:'received'}
};

// Wait reasons (PGR_GIFT_EVENT_WAIT.reason), mapped back to the exact JS strings by the adapter.
enum PgrIncomingGiftsWait {
  PGR_GIFT_WAIT_NONE = 0,
  PGR_GIFT_WAIT_AUTO_OFF = 1,     // 'Auto open is off'
  PGR_GIFT_WAIT_FOREGROUND = 2,   // 'Waiting for foreground readiness'
  PGR_GIFT_WAIT_OPENING = 3,      // 'Waiting for dialog to finish opening'
  PGR_GIFT_WAIT_DETAILS = 4,      // 'Waiting for incoming gift details'
  PGR_GIFT_WAIT_BUTTON = 5        // 'Waiting for Open button'
};

// Error reasons (PGR_GIFT_EVENT_ERROR.reason).
enum PgrIncomingGiftsError {
  PGR_GIFT_ERR_REASON_NONE = 0,
  PGR_GIFT_ERR_REASON_INVOKE = 1,    // the OnOpenClick invoke threw
  PGR_GIFT_ERR_REASON_GCHANDLE = 2,  // a GC root could not be allocated
  PGR_GIFT_ERR_REASON_OVERFLOW = 3   // the FIFO overflowed (fail-closed)
};

// Fixed LP64 config: 8-byte header + 28 pointers (224) + two int32 enum literals (8) = 240.
// Packed by the JS adapter after its metadata validation. Every pointer is an already validated
// il2cpp export, MethodInfo*, Il2CppClass* or FieldInfo*. The native layer re-checks the header
// and rejects any null pointer (all 28 are required). No hook address is carried -- this module
// installs nothing.
struct PgrIncomingGiftsConfig {
  uint32_t version;  // == PGR_GIFT_CONFIG_VERSION
  uint32_t size;     // == sizeof(PgrIncomingGiftsConfig)

  // il2cpp runtime exports (indices 0..9 in the adapter's flat array).
  void *gchandle_new;        // void* (void* obj, int pinned)
  void *gchandle_get_target; // void* (void* handle)
  void *gchandle_free;       // void  (void* handle)
  void *object_get_class;    // void* (void* obj)
  void *object_unbox;        // void* (void* boxed)
  void *runtime_invoke;      // void* (MethodInfo*, void* obj, void** params, void** exc)
  void *field_get_value;     // void  (void* obj, FieldInfo*, void* out)
  void *field_set_value;     // void  (void* obj, FieldInfo*, void* value)
  void *string_length;       // int   (void* str)
  void *string_chars;        // uint16_t* (void* str)

  // Managed MethodInfo* invoked via runtime_invoke (never hooked).
  void *m_open;         // GiftOpenGui.OnOpenClick()                 [instance, void]
  void *m_active;       // UnityEngine.Behaviour.get_isActiveAndEnabled() -> bool
  void *m_interactable; // UnityEngine.UI.Selectable.IsInteractable()     -> bool
  void *m_alive;        // UnityEngine.Object.op_Implicit(Object)         -> bool [static]

  // Exact Il2CppClass* identities.
  void *class_open_gui;  // GIFT_TYPES[6240]            (READY/OPEN/CLOSE self exact)
  void *class_details;   // Holoholo.Rpc.GiftBoxDetailsProto (openDetails exact)
  void *class_open_out;  // Holoholo.Rpc.OpenGiftOutProto    (sequence response exact)
  void *class_sequence;  // GIFT_OPEN_SEQUENCE iterator      (sequence object exact)
  void *class_bool;      // System.Boolean                  (boxed-bool check)
  void *class_string;    // System.String                   (sender exact)

  // Il2CppFieldInfo* read via field_get_value (and the sequence state written via field_set_value).
  void *f_current_state; // GUI.currentState (Int32 enum)
  void *f_open_details;  // GiftOpenGui.dlst -> GiftBoxDetailsProto
  void *f_sender;        // GiftBoxDetailsProto.SenderId -> System.String
  void *f_open_button;   // GiftOpenGui.openButton -> UnityEngine.UI.Selectable
  void *f_seq_gui;       // sequence.<>4__this -> GiftOpenGui
  void *f_seq_response;  // sequence.response -> OpenGiftOutProto
  void *f_seq_state;     // sequence.dlrm (Int32) -- read for ==0, written to 3
  void *f_open_result;   // OpenGiftOutProto.Result (Int32 enum)

  // Enum literal values (after validation in JS).
  int32_t state_open;    // STATE_OPEN (currentState == this means the dialog finished opening)
  int32_t open_success;  // gifts.openSuccess (OpenGiftOutProto.Result Success literal)
};

static_assert(sizeof(PgrIncomingGiftsConfig) == 240,
              "PgrIncomingGiftsConfig must remain 240 bytes on LP64");

// Fixed LP64 telemetry gauge (88 bytes). Pull-only; the native layer never calls JS.
struct PgrIncomingGiftsSample {
  uint32_t version;          // == PGR_GIFT_SAMPLE_VERSION
  uint32_t size;             // == sizeof(PgrIncomingGiftsSample)
  uint64_t issued_serial;    // bumps on each auto-issued open
  uint64_t wait_serial;      // bumps on each wait-reason change
  uint64_t error_serial;     // bumps on each error
  uint64_t skip_serial;      // bumps on each applied sequence skip
  uint64_t dialog_serial;    // bumps on each READY that armed a new slot
  uint64_t last_tid;         // owner tid of the last event
  uint32_t root_count;       // durable GUI root + active observe/tick depth (finish guard)
  uint32_t owned;            // 1 while a GUI slot is owned
  uint32_t attempted;        // 1 once the open was attempted for the owned GUI
  uint32_t automated;        // 1 when WE issued the open (sequence skip eligible)
  uint32_t fault;            // 1 once latched (FIFO overflow)
  uint32_t overflow;         // 1 if the FIFO ever overflowed
  uint32_t last_wait_reason; // PgrIncomingGiftsWait of the latest wait
  uint32_t pending_events;   // records currently queued in the FIFO
};

static_assert(sizeof(PgrIncomingGiftsSample) == 88,
              "PgrIncomingGiftsSample must remain 88 bytes on LP64");

// Fixed LP64 drain record (16 bytes). reason is a code (PgrIncomingGiftsWait /
// PgrIncomingGiftsError); it is NEVER a sender string or friend id.
struct PgrIncomingGiftsEvent {
  uint32_t kind;    // PgrIncomingGiftsEventKind
  uint32_t reason;  // wait/error reason code (0 for ISSUED/SKIP)
  uint64_t tid;     // owner tid that produced the record
};

static_assert(sizeof(PgrIncomingGiftsEvent) == 16,
              "PgrIncomingGiftsEvent must remain 16 bytes on LP64");

// Fixed LP64 table: 8-byte header + 7 function pointers = 64 bytes.
struct PgrIncomingGiftsApi {
  uint32_t version;  // == PGR_GIFT_API_VERSION
  uint32_t size;     // == sizeof(PgrIncomingGiftsApi)
  int (*install)(const PgrIncomingGiftsConfig *config);
  // Arm/clear the auto-open gate and the residual lease. Carries no tid (the owner is latched by
  // observe()/tick()); a bit-1 mask enables auto-open. remaining_lease_ms is clamped to 2500.
  int (*update)(uint32_t mask, uint64_t remaining_lease_ms);
  // Forward one synchronous Gum observation on the proven Unity owner. For SEQUENCE it may apply
  // the state-3 skip. Off the owner thread (or before install) it is a no-op.
  int (*observe)(uint32_t kind, void *obj, uint64_t proven_tid);
  // One owner-thread attempt pass (incomingTick). stopping != 0 clears the owned root regardless
  // of the lease. Off the owner thread it is a no-op (never frees).
  int (*tick)(uint64_t proven_tid, int stopping);
  int (*sample)(PgrIncomingGiftsSample *out_sample);
  // Pop one FIFO record into *out. Returns 1 when a record was written, 0 when empty, or a
  // negative status on error. Any thread.
  int (*drain)(PgrIncomingGiftsEvent *out);
  // 1 if the owned GUI is alive and currentState == STATE_OPEN, else 0. Owner thread only (does
  // managed work); off the owner thread or with no slot it returns 0.
  int (*is_open)(uint64_t proven_tid);
};

static_assert(sizeof(PgrIncomingGiftsApi) == 64,
              "PgrIncomingGiftsApi must remain a 64-byte version-1 table");

// Standard feature getter. Returns the static const 64-byte table when `version` == 1, else NULL.
// The coordinator registers it in query_feature as case 17.
const void *pgrazor_incoming_gifts_api(uint32_t version);

#ifdef __cplusplus
}
#endif
