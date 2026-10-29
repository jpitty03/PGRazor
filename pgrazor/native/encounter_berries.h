#pragma once
#include <stdint.h>

// PGRazor native migration: encounter berry priority / request / lifetime controller
// (feature id 18). A native port of the state machine in runtime/features/berries.js
// (berryState / berryTick, with berryResume / berryClear), PLUS the submit/peek decision
// logic and the result/error done-latch that the inline Gum replacement and observers host.
//
// WHAT THIS MODULE IS NOT:
//   * It installs NO Dobby and NO Gum hooks. The game's Pokeball submit address already
//     carries runtime/features/throws.js's Gum observer (see native-throws.js -- it attaches
//     an Interceptor there and delegates the transform into pgrazor_throws_api.rewrite_submit);
//     stacking a Dobby inline trampoline on that same prologue would corrupt the throw site.
//     So the berry layer, exactly like the IV-display catch channel (iv_display.h external
//     observer mode), keeps the EXISTING JS Gum replacements (submit + peek) and the four
//     Interceptor observers (enter / exit / result / error) and forwards their decisions here
//     through a thin adapter. The C++ layer owns the state, the GC rooting, the priority /
//     request flow and the resume bookkeeping; it NEVER calls the original and NEVER calls JS.
//   * It NEVER auto-catches. The only original-submit call is the single held throw being
//     resumed, and that call is made by JS (never by this module -- see the resume protocol).
//
// THE RESUME PROTOCOL (the one subtle part). The original Pokeball submit can only be called
// by JS -- the native layer must not call JS and must not call the submit address directly
// (calling the raw original from C++ could re-enter the Gum replacement; it is not a valid
// original trampoline). So resuming the held throw is a three-step dance driven by the JS tick:
//   1. tick() runs the state machine and, when the held throw should be released (feed done,
//      8 s timeout, lease expired before start, or stop), QUEUES a resume and DETACHES the
//      pending frame (so a stale peek can no longer suppress and submit can no longer dedup).
//   2. begin_resume(proven_tid, out) atomically CLAIMS the queued resume IFF the scope and the
//      rooted ball are still alive and the scope epoch is unchanged, writes the refreshed rooted
//      ball + the exact packed throw words + MethodInfo* into `out`, and returns a nonzero
//      claim ticket (0 == nothing to resume / the encounter is gone, in which case the frame is
//      cancelled and freed WITHOUT a submit). JS then calls h.originalSubmit(ball, first, second,
//      method) -- the real original trampoline it (the coordinator) supplied -- EXACTLY once.
//   3. finish_resume(ticket, proven_tid) releases ONLY that claimed frame, in the JS `finally`.
// Once claimed, a reentrant stop/exit defers the frame's cleanup to finish_resume (it never
// frees a claimed frame and never replays or frees a newer record). Exit (or a replaced
// encounter) cancels an UNCLAIMED pending WITHOUT submit; stop queues the resume so the held
// throw still fires once -- but ONLY if the SAME encounter scope and the rooted ball are still
// alive (stop is gated on the scope exactly like every other resume cause; there is no
// ignore-scope path). The instant a resume is queued the held attempt is reset, so submit() and
// peek() stop treating that ball as held and the JS-driven original-submit of the resumed ball
// passes straight through (PASS) instead of being re-swallowed as a duplicate.
//
// Division of labour with runtime/native-encounter-berries.js:
//   * JS validates and packs every il2cpp pointer (exports, MethodInfo*, class identities,
//     FieldInfo*, the MasterBall/STATE_OPEN literals) into the config, keeps the Gum submit +
//     peek replacements and the four observers, forwards submit/peek/enter/exit/result into the
//     table, drives update()/tick() from the verified Unity heartbeat, pumps begin/finish_resume,
//     drains the bounded event FIFO into the exact berry-* / cooldown-* events, and -- on a feed
//     that completed (resume cause DONE) -- sets getItemState().next = 0. The original submit
//     NativeFunction is supplied by JS; ONLY the adapter ever calls it.
//   * This module owns all GC rooting (RAII, bounded, freed once, refreshed after every managed/
//     GC call), the scope/pending/resume lifetime, the priority snapshot + first-stocked pick,
//     the callback allocation + field write/readback, the single feed request, the 8 s deadline
//     (CLOCK_BOOTTIME), the residual lease, the Unity-thread guard, the scope epoch, and the
//     bounded event FIFO. No mutex is ever held across a managed call or a GC-handle call.

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "EncounterBerries native ABI requires LP64");

enum PgrBerryFeatureInfo {
  PGR_BERRY_FEATURE_ID   = 18,
  PGR_BERRY_API_VERSION  = 1,
  PGR_BERRY_MAX_PRIORITY = 3,     // priority list cap (berryIds, <= 3 distinct ids)
  PGR_BERRY_MAX_LEASE_MS = 2500,  // residual authorization-lease cap
  PGR_BERRY_DEADLINE_MS  = 8000,  // feed-response deadline (then latch failed, resume once)
  PGR_BERRY_PULSE_MS     = 500,   // overlay UI-state poll cadence
  PGR_BERRY_EVENT_CAP    = 64     // bounded telemetry FIFO depth (fail-closed on overflow)
};

// submit() decision (also the peek_held() return domain: PASS=0 / HOLD=1).
enum PgrBerryDecision {
  PGR_BERRY_PASS = 0,  // JS must call the original submit/peek
  PGR_BERRY_HOLD = 1   // JS must NOT call the original (throw held / peek suppressed)
};

// Table status codes. 0 == success.
enum PgrBerryStatus {
  PGR_BERRY_OK              = 0,
  PGR_BERRY_ERR_NULL_ARG    = 1,
  PGR_BERRY_ERR_BAD_CONFIG  = 2,
  PGR_BERRY_ERR_ALREADY_BOUND = 3,
  PGR_BERRY_ERR_NOT_INSTALLED = 4,
  PGR_BERRY_ERR_NO_CLOCK    = 5,
  PGR_BERRY_ERR_BAD_TOKEN   = 6
};

// Resume cause, written into the begin_resume POD so JS can route the follow-up events and the
// getItemState().next reset (DONE only). Mirrors the berryTick branch that called berryResume.
enum PgrBerryResumeCause {
  PGR_BERRY_RESUME_MISC    = 0,  // preflight bypass (bread / !can / photo / master / no stock / ...)
  PGR_BERRY_RESUME_DONE    = 1,  // server responded (result/error): JS sets getItemState().next=0
  PGR_BERRY_RESUME_TIMEOUT = 2,  // 8 s deadline: failed latched, resume once, no retry
  PGR_BERRY_RESUME_LEASE   = 3,  // lease expired before the feed started
  PGR_BERRY_RESUME_STOP    = 4   // stop(): resume once if the encounter is still valid
};

// Bounded-FIFO event codes drained by next_event(). Carry only berry ids / reason codes --
// never an account or friend id.
enum PgrBerryEventCode {
  PGR_BERRY_EVT_NONE            = 0,
  PGR_BERRY_EVT_READY           = 1,  // feature armed (aux: 1 available)
  PGR_BERRY_EVT_ENCOUNTER       = 2,  // encounter-berry (aux bit0 active, bit1 busy)
  PGR_BERRY_EVT_COOLDOWN_ENC    = 3,  // cooldown-encounter (scope exit)
  PGR_BERRY_EVT_ACTION          = 4,  // cooldown-action (aux: 0 throw, 1 drop, 2 feeding)
  PGR_BERRY_EVT_IMPACT_QUEUED   = 5,  // berry-impact-queued (id)
  PGR_BERRY_EVT_FEED_REQUESTED  = 6,  // berry-feed-requested (id)
  PGR_BERRY_EVT_ERROR           = 7   // berry-error (id, aux reason: see PgrBerryErrorReason)
};

// Reason codes carried in PgrBerryEvent.aux for PGR_BERRY_EVT_ERROR.
enum PgrBerryErrorReason {
  PGR_BERRY_REASON_TIMEOUT        = 1,  // "Berry response timed out; submitting catch without retry"
  PGR_BERRY_REASON_CALLBACK_READBACK = 2,  // callback field readback mismatch
  PGR_BERRY_REASON_REQUEST_FAULT  = 3,  // managed exception issuing the feed request
  PGR_BERRY_REASON_BALLTYPE_FAULT = 4,  // managed exception / box mismatch reading BallType
  PGR_BERRY_REASON_INVENTORY      = 5,  // bag class / quantity invoke fault
  PGR_BERRY_REASON_GCHANDLE       = 6   // a GC root allocation failed
};

// Install-time configuration (fixed LP64 layout: 8-byte header + 28 pointers + 2 int32 literals
// + 2 reserved + enum class pointer = 256 bytes). Every pointer is an already-validated il2cpp export, MethodInfo*,
// Il2CppClass* or FieldInfo*; the native layer re-checks the header and rejects any null.
typedef struct pgr_berry_config_v1 {
  uint32_t version;  // == PGR_BERRY_API_VERSION
  uint32_t size;     // == sizeof(pgr_berry_config_v1)

  // il2cpp runtime exports (pointer-sized GC-handle ABI).
  void *gchandle_new;            // void* (void* obj, int pinned) -> handle
  void *gchandle_get_target;     // void* (void* handle)
  void *gchandle_free;           // void  (void* handle)
  void *object_get_class;        // void* (void* obj)
  void *object_unbox;            // void* (void* boxed)
  void *object_new;              // void* (void* klass) -> the berry callback instance
  void *runtime_invoke;          // void* (MethodInfo*, void* obj, void** params, void** exc)
  void *field_get_value;         // void  (void* obj, FieldInfo*, void* out)
  void *field_set_value;         // void  (void* obj, FieldInfo*, void* value)  [value field]
  void *field_set_value_object;  // void  (void* obj, FieldInfo*, void* value)  [ref field, barrier]

  // Managed MethodInfo* invoked via runtime_invoke (never hooked).
  void *m_ball_type;   // Pokeball.get_BallType -> ItemId (boxed int)
  void *m_bread;       // Pokeball.get_HoldingBreadBall -> bool
  void *m_can;         // scope.get_CanUseBerry -> bool
  void *m_photo;       // scope.get_IsInPhotoMode -> bool
  void *m_quantity;    // IItemBag.GetItemCount(ItemId) -> int
  void *m_direct;      // callback.btdx(bool) -> void  (issue the feed request)
  void *m_alive;       // UnityEngine.Object.op_Implicit(Object) -> bool [static]

  // Exact Il2CppClass* identities.
  void *class_pokeball;  // Niantic.Holoholo.Encounter.Pokeball (submit self exact)
  void *class_scope;     // the berry feed-GUI encounter scope (enter a[0] exact)
  void *class_bag;       // ItemBagImpl / IItemBag concrete (scope.bag exact)
  void *class_bool;      // System.Boolean (box check)
  void *class_int;       // System.Int32 (box check)
  void *class_callback;  // generated berry confirm callback class (object_new + result match)

  // Il2CppFieldInfo* read/written via field_get_value / field_set_value[_object].
  void *f_scope_bag;       // scope -> IItemBag
  void *f_scope_ui;        // scope -> EncounterUI
  void *f_ui_state;        // EncounterUI.currentState (int32)
  void *f_callback_scope;  // callback.<>4__this -> scope (reference write + readback)
  void *f_callback_berry;  // callback.berryType (enum i32 write + readback)

  // Validated literals.
  int32_t ball_type_master;  // BallType value that bypasses berry queuing (MasterBall == 4)
  int32_t state_open;        // EncounterUI.currentState value meaning the feed UI is open
  uint32_t reserved0;        // must be 0
  uint32_t reserved1;        // must be 0
  void *class_ball_type;     // Holoholo.Rpc.Item enum box, distinct from System.Int32
} pgr_berry_config_v1;

// begin_resume output POD (40 bytes). ball + the exact packed PokeballThrow words + MethodInfo*
// re-resolved from the durable roots AFTER the detach, ready for JS to forward verbatim.
typedef struct pgr_berry_resume_v1 {
  void *ball;        // refreshed Pokeball (durable root target; valid until finish_resume)
  uint64_t first;    // packed throw bytes 0..7 (x1); low32 is BallType
  uint64_t second;   // packed throw bytes 8..11 (x2); (second>>16)&0xff is the drop flag
  void *method;      // MethodInfo* captured at submit
  uint32_t cause;    // one of PgrBerryResumeCause (DONE => JS sets getItemState().next = 0)
  uint32_t reserved; // 0
} pgr_berry_resume_v1;

// Pull-only telemetry snapshot (64 bytes on LP64). Overlay flags + serials + the conservative
// finish guard (owned_roots). Safe from any thread; never frees.
typedef struct pgr_berry_sample_v1 {
  uint32_t version;         // == PGR_BERRY_API_VERSION
  uint32_t size;            // == sizeof(pgr_berry_sample_v1)
  uint32_t overlay_active;  // scope present (1) / absent (0)
  uint32_t overlay_busy;    // a throw is in flight (held attempt, queued or claimed resume)
  uint32_t ui_open;         // last EncounterUI.currentState == state_open observation; also
                            // folded into the ENCOUNTER event aux (bit2) so JS gates the overlay
                            // on the open feed UI, like the legacy currentState check
  uint32_t failed;          // encounter fault latch (pass-through until the scope changes)
  uint64_t impact_serial;   // bumped on each queued impact
  uint64_t request_serial;  // bumped on each issued feed request
  uint64_t resume_serial;   // bumped on each completed (finished) resume
  uint32_t fault_serial;    // bumped on each fail-closed error
  uint32_t dropped_events;  // FIFO overflow count (fail-closed feed; never strands a held throw)
  uint32_t owned_roots;     // durable GC roots + active frames (coordinator finish guard)
  uint32_t reserved;        // 0
} pgr_berry_sample_v1;

// Bounded-FIFO event record (16 bytes). Drained in strict order by next_event().
typedef struct pgr_berry_event_v1 {
  uint32_t code;      // PgrBerryEventCode
  int32_t  id;        // berry ItemId (impact/feed/error) or 0
  int32_t  aux;       // encounter flags / action kind / error reason
  int32_t  reserved;  // 0
} pgr_berry_event_v1;

/* install(config): validate + publish the config once. A repeat of the identical config is OK;
 * a different one returns ALREADY_BOUND. BAD_CONFIG on header/size/pointer/literal problems;
 * NO_CLOCK if CLOCK_BOOTTIME is unavailable. Installs NO hooks (the JS Gum layer owns the site). */
typedef int (*pgr_berry_install_fn)(const pgr_berry_config_v1 *config);

/* update(ids, count, remaining_lease_ms, proven_tid): arm the priority list (<= 3 ids; count 0
 * == Off) and the residual lease (clamped to PGR_BERRY_MAX_LEASE_MS, turned into an absolute
 * CLOCK_BOOTTIME deadline). A wrong-thread call can only disarm. Any thread. */
typedef int (*pgr_berry_update_fn)(const int32_t *ids, uint32_t count,
                                   uint64_t remaining_lease_ms, uint64_t proven_tid);

/* submit(ball, first, second, method, proven_tid): the Gum submit onEnter decision. Returns
 * HOLD (do not call the original -- a fresh impact was queued, or the same held ball was
 * re-submitted) or PASS (call the original). Snapshots the priority list at queue time and
 * roots the ball. PASS while a resume is queued/claimed (the resumed ball must pass through),
 * while telemetry is fail-closed (no new hold), or off the latched Unity owner thread. */
typedef int (*pgr_berry_submit_fn)(void *ball, uint64_t first, uint64_t second, void *method,
                                   uint64_t proven_tid);

/* peek_held(ball, proven_tid): HOLD (return null to the game) while `ball` is the held pending
 * ball, else PASS. Off the proven thread -> PASS. */
typedef int (*pgr_berry_peek_fn)(void *ball, uint64_t proven_tid);

/* observe_enter(scope, proven_tid): a new encounter scope. Clears any stale pending WITHOUT a
 * submit, roots the scope (if it is exactly class_scope) and bumps the scope epoch. */
typedef int (*pgr_berry_enter_fn)(void *scope, uint64_t proven_tid);

/* observe_exit(scope, proven_tid): the current scope is leaving/replaced. Cancels an UNCLAIMED
 * pending WITHOUT a submit, drops the scope, bumps the epoch, emits cooldown-encounter. A
 * claimed resume frame is left to finish_resume (deferred cleanup). */
typedef int (*pgr_berry_exit_fn)(void *scope, uint64_t proven_tid);

/* result_begin(callback, proven_tid): the result/error observer onEnter. Returns a nonzero token
 * when `callback` is the current started pending's callback (captured generation), else 0. */
typedef uint64_t (*pgr_berry_result_begin_fn)(void *callback, uint64_t proven_tid);

/* result_end(token, proven_tid): the matching onLeave (AFTER the original ran). Latches the
 * pending done IFF the token + generation still match. */
typedef int (*pgr_berry_result_end_fn)(uint64_t token, uint64_t proven_tid);

/* tick(proven_tid, stopping): one owner-thread pass (berryTick). Drives the overlay pulse, scope
 * liveness, and -- for a pending throw -- the done/timeout/lease branches or the preflight +
 * single feed request, QUEUING a resume where berryTick called berryResume. stopping != 0 queues
 * the held throw's resume (not a discard) and, with nothing pending, clears the scope. Off the
 * proven thread it is a no-op (never frees). */
typedef int (*pgr_berry_tick_fn)(uint64_t proven_tid, int stopping);

/* begin_resume(proven_tid, out): claim the queued resume. Returns a nonzero ticket and fills
 * `out` (refreshed ball/first/second/method + cause) IFF the scope + ball are alive and the
 * epoch is unchanged; else cancels + frees the frame WITHOUT a submit and returns 0. */
typedef uint64_t (*pgr_berry_begin_resume_fn)(uint64_t proven_tid, pgr_berry_resume_v1 *out);

/* finish_resume(ticket, proven_tid): release ONLY the claimed frame `ticket` (frees its ball +
 * callback roots, updates the overlay). A stale/mismatched ticket is rejected. */
typedef int (*pgr_berry_finish_resume_fn)(uint64_t ticket, uint64_t proven_tid);

/* sample(out): copy the telemetry snapshot. Any thread; never frees. */
typedef int (*pgr_berry_sample_fn)(pgr_berry_sample_v1 *out);

/* next_event(out): dequeue one FIFO event (returns 1) or report empty (returns 0). Owner thread. */
typedef int (*pgr_berry_next_event_fn)(pgr_berry_event_v1 *out);

// Ordered, read-only table (8-byte header + 13 function pointers = 112 bytes).
typedef struct pgr_berry_api_v1 {
  uint32_t version;  // == PGR_BERRY_API_VERSION
  uint32_t size;     // == sizeof(pgr_berry_api_v1)
  pgr_berry_install_fn       install;
  pgr_berry_update_fn        update;
  pgr_berry_submit_fn        submit;
  pgr_berry_peek_fn          peek_held;
  pgr_berry_enter_fn         observe_enter;
  pgr_berry_exit_fn          observe_exit;
  pgr_berry_result_begin_fn  result_begin;
  pgr_berry_result_end_fn    result_end;
  pgr_berry_tick_fn          tick;
  pgr_berry_begin_resume_fn  begin_resume;
  pgr_berry_finish_resume_fn finish_resume;
  pgr_berry_sample_fn        sample;
  pgr_berry_next_event_fn    next_event;
} pgr_berry_api_v1;

/* Hidden getter. NOT exported by the version script. The coordinator registers it in
 * query_feature under feature id 18; it answers only on an exact version match. The returned
 * table lives for the process lifetime. */
const void *pgrazor_encounter_berries_api(uint32_t version);

#ifdef __cplusplus
} /* extern "C" */
#endif
