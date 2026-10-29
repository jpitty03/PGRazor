#pragma once
#include <stdint.h>

// Feature 52 (API 1). COMPILED TYPED RUNTIME TRANSPORT: the native owner of the settings/event boundary
// that runtime/translated.js (host + client) and the Java RuntimeMailbox used to carry as JSON through two
// mailbox files. It replaces that wire with an in-process, strictly-typed, versioned BINARY exchange over a
// JNI DirectByteBuffer (both directions). There is NO JSON, NO JS interpreter, NO JS callback and NO file
// here. See .artifacts/native-migration/runtime-transport-contract.txt for the full division of labour.
//
// TWO DIRECTIONS, ONE COHERENT SNAPSHOT + ONE EVENT QUEUE:
//   * SETTINGS (host -> gameplay): the Java host (its own 50 ms daemon, NOT a Unity thread) builds a bounded
//     typed snapshot frame from the live Control getters and hands it to exchange(). Native stores exactly
//     ONE coherent snapshot under a mutex and stamps it with a CLOCK_BOOTTIME accept time. Future COMPILED
//     gameplay consumers read typed fields through get_bool/get_int/get_double/get_string/get_command_snapshot
//     and take_pulse. Every getter FAILS CLOSED with PGR_TRANSPORT_STALE once the snapshot is older than the lease,
//     stopped, or flagged inert -- EXCEPT the guarded booleans (active, vgpEnabled, *Allowed, *Requested),
//     which return value 0 on a stale snapshot, reproducing the legacy translated.js client's false default.
//   * ACTIONS (gameplay -> host): native gameplay PRODUCERS (any thread) enqueue bounded typed action events
//     through emit() under a mutex; NO managed/JS callback is ever made and NO lock is held across a callback
//     (there is none). The Java host drains a bounded batch through the SAME exchange() call, validates the
//     WHOLE batch against the compiled schema, then dispatches to Control. Drain REMOVES events from the queue
//     (consume-before-dispatch): a setter exception or host reentry can never replay a gameplay action.
//
// BACKPRESSURE IS EXPLICIT: emit() returns PGR_TRANSPORT_ERR_OVERFLOW (and latches sample.fault) when the queue is
// full by count or by the byte budget. Actions are NEVER silently dropped. seq is monotonic per emit and a
// session generation bumps on stop() so a stale frame cannot straddle a restart.
//
// SCOPE / NOT: installs NO hook and makes NO managed/il2cpp call; it is pure bounded typed bookkeeping. It
// carries NO pointer across the boundary -- only compiled primitive fields and length-prefixed UTF-8. The
// ARM gameplay hooks, Control's own feature/lease/domain authorization, and the guest Frida runtime are NOT
// retired by this module; this is the HOST transport only. The three exported symbols stay JNI_OnLoad and
// pgrazor_native_api; this table is handed out through query_feature(52, 1), never exported by name.

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "RuntimeTransport native ABI requires LP64");

enum PgrRuntimeTransportInfo {
  PGR_TRANSPORT_FEATURE_ID      = 52,
  PGR_TRANSPORT_API_VERSION     = 1,
  PGR_TRANSPORT_CONFIG_VERSION  = 1,
  PGR_TRANSPORT_SAMPLE_VERSION  = 1,
  PGR_TRANSPORT_FRAME_VERSION   = 1,

  // Compiled schema cardinalities (exact legacy translated.js order). 39+10+2+5 scalar strings = 56 scalar
  // readers; accountCommandSnapshot adds its String[2] pair for 57 readers total; 3 pulses; 59 actions.
  PGR_TRANSPORT_BOOLS           = 39,
  PGR_TRANSPORT_INTS            = 10,
  PGR_TRANSPORT_DOUBLES         = 2,
  PGR_TRANSPORT_STRINGS         = 7,   // 5 scalar strings + accountCommandSnapshot[0] + accountCommandSnapshot[1]
  PGR_TRANSPORT_PULSES          = 3,
  PGR_TRANSPORT_ACTIONS         = 59,

  // Bounded transport limits.
  PGR_TRANSPORT_MAX_LEASE_MS    = 2500,      // freshness-window ceiling (legacy default 500)
  PGR_TRANSPORT_MAX_EVENTS      = 2048,      // queue depth ceiling (legacy client queue cap)
  PGR_TRANSPORT_MAX_STRING      = 1048576,   // per-string UTF-8 byte ceiling (legacy 1 MiB)
  PGR_TRANSPORT_MAX_FRAME       = 1048576,   // per-frame byte ceiling, both directions
  PGR_TRANSPORT_DRAIN_CAP       = 128,       // events drained per exchange (legacy calls batch cap)
  PGR_TRANSPORT_MAX_SCALARS     = 5,         // widest action scalar count (applyMapTap)
  PGR_TRANSPORT_MAX_STR_ARGS    = 2          // widest action string count (accountIdentity / event)
};

// Frame magics (little-endian wire). 'PGRS' settings snapshot, 'PGRE' event batch.
enum PgrRuntimeTransportMagic {
  PGR_TRANSPORT_SNAPSHOT_MAGIC = 0x53524750u, // "PGRS"
  PGR_TRANSPORT_EVENTS_MAGIC   = 0x45524750u  // "PGRE"
};

// Snapshot frame flags.
enum PgrRuntimeTransportFlag {
  PGR_TRANSPORT_FLAG_INERT = 0x1u  // empty values; stopped/faulted host -> never fresh (legacy empty-values stop)
};

// Fixed little-endian snapshot layout (byte offsets). Both the Java writer and native/compiled readers
// compile these offsets; no field is ever reordered. Header is 32 bytes.
enum PgrRuntimeTransportLayout {
  PGR_TRANSPORT_OFF_MAGIC      = 0,
  PGR_TRANSPORT_OFF_VERSION    = 4,
  PGR_TRANSPORT_OFF_FRAME_SIZE = 8,
  PGR_TRANSPORT_OFF_FLAGS      = 12,
  PGR_TRANSPORT_OFF_SEQ        = 16,   // u64
  PGR_TRANSPORT_OFF_WALL_MS    = 24,   // u64 System.currentTimeMillis() (telemetry; native uses its own clock)
  PGR_TRANSPORT_OFF_BOOLS      = 32,   // u8[39] (0/1)
  PGR_TRANSPORT_OFF_INTS       = 72,   // i32[10]  (32 + 39 -> pad to 72)
  PGR_TRANSPORT_OFF_DOUBLES    = 112,  // f64[2]  (lat, lon; NaN == "null" coord)
  PGR_TRANSPORT_OFF_PULSES     = 128,  // u64[3]  monotonic pulse counts
  PGR_TRANSPORT_OFF_STRINGS    = 152,  // string heap: 7 * (u32 len + len UTF-8 bytes), in reader order
  PGR_TRANSPORT_SNAPSHOT_FIXED = 152,
  // Event frame header is also 32 bytes: magic,version,frame_size,count,base_seq(u64),generation(u64).
  PGR_TRANSPORT_EVENT_HEADER   = 32
};

enum PgrRuntimeTransportStatus {
  PGR_TRANSPORT_OK                = 0,
  PGR_TRANSPORT_ERR_NULL_ARG      = -1,
  PGR_TRANSPORT_ERR_BAD_VERSION   = -2,
  PGR_TRANSPORT_ERR_BAD_SIZE      = -3,
  PGR_TRANSPORT_ERR_BAD_BOUND     = -4,   // config bound out of range
  PGR_TRANSPORT_ERR_NOT_INSTALLED = -6,
  PGR_TRANSPORT_ERR_RECONFIG      = -9,   // install() with a different config after success
  PGR_TRANSPORT_ERR_CLOCK         = -10,  // CLOCK_BOOTTIME unavailable
  PGR_TRANSPORT_ERR_OVERFLOW      = -13,  // queue full by count or byte budget (fail closed; fault latched)
  PGR_TRANSPORT_ERR_BAD_FRAME     = -14,  // magic/version/size/count malformed
  PGR_TRANSPORT_ERR_BAD_OPCODE    = -15,  // opcode out of 0..PGR_TRANSPORT_ACTIONS-1
  PGR_TRANSPORT_ERR_BAD_SCHEMA    = -16,  // scalar/string arity mismatch vs compiled schema
  PGR_TRANSPORT_ERR_BAD_UTF8      = -17,  // string not valid UTF-8
  PGR_TRANSPORT_ERR_RANGE         = -18,  // field id / capacity out of range
  PGR_TRANSPORT_STALE             = -19,  // snapshot expired / inert / absent (guarded booleans excepted)
  PGR_TRANSPORT_STOPPED           = -20,  // transport stopped; emit is a clean no-op (not a fault)
  PGR_TRANSPORT_ERR_TOO_LARGE     = -21,  // emit: event can never fit a bounded frame (un-drainable); rejected up front
  PGR_TRANSPORT_ERR_REPLAY        = -22,  // snapshot seq not advancing or a pulse count regressed (stale/replay)
  PGR_TRANSPORT_ERR_DOMAIN        = -23   // emit: typed scalar value out of its compiled domain (bool/int/double/range)
};

// Fixed LP64 config (32 bytes). No pointers -- this module calls nothing.
struct PgrRuntimeTransportConfig {
  uint32_t version;           // == PGR_TRANSPORT_CONFIG_VERSION
  uint32_t size;              // == sizeof(PgrRuntimeTransportConfig)
  uint32_t lease_ms;          // 1..PGR_TRANSPORT_MAX_LEASE_MS (freshness window)
  uint32_t max_events;        // 1..PGR_TRANSPORT_MAX_EVENTS (queue depth)
  uint32_t max_string_bytes;  // 1..PGR_TRANSPORT_MAX_STRING (per-string ceiling)
  uint32_t queue_byte_budget; // 1..PGR_TRANSPORT_MAX_FRAME (total queued string bytes)
  uint32_t reserved0;         // 0
  uint32_t reserved1;         // 0
};

static_assert(sizeof(PgrRuntimeTransportConfig) == 32, "config must remain 32 bytes on LP64");

// Fixed LP64 telemetry gauge (88 bytes). Pull-only; the native layer never calls the host.
struct PgrRuntimeTransportSample {
  uint32_t version;            // == PGR_TRANSPORT_SAMPLE_VERSION
  uint32_t size;               // == sizeof(PgrRuntimeTransportSample)
  uint64_t snapshots_accepted; // bumps on each validated snapshot push
  uint64_t snapshots_rejected; // bumps on each rejected snapshot frame
  uint64_t events_emitted;     // bumps on each accepted emit()
  uint64_t events_drained;     // bumps on each event handed to the host via exchange()
  uint64_t events_dropped;     // bumps on each OVERFLOW (fail closed; never a silent drop)
  uint64_t last_event_seq;     // seq of the most recently emitted event
  uint64_t generation;         // session generation (bumps on stop())
  uint64_t snapshot_seq;       // seq of the currently stored snapshot
  uint32_t queue_depth;        // events currently queued
  uint32_t queue_bytes;        // queued string bytes currently held
  uint32_t snapshot_fresh;     // 1 if the snapshot is fresh at sample time, else 0
  uint32_t fault;              // 1 if overflow/malformed input was ever latched
};

static_assert(sizeof(PgrRuntimeTransportSample) == 88, "sample must remain 88 bytes on LP64");

// A producer-supplied string argument (gameplay owns the storage for the duration of the emit() call only;
// native copies it). ptr may be null iff len == 0.
struct PgrRuntimeTransportString {
  const char *ptr;
  uint32_t    len;
};

// Fixed LP64 table: 8-byte header + 11 function pointers = 96 bytes.
struct PgrRuntimeTransportApi {
  uint32_t version;  // == PGR_TRANSPORT_API_VERSION
  uint32_t size;     // == sizeof(PgrRuntimeTransportApi)

  // Idempotent install. A second call with an identical config returns OK; a different config ERR_RECONFIG.
  int (*install)(const PgrRuntimeTransportConfig *config);

  // Host-thread exchange. snap_len == 0 drains only (no snapshot update); otherwise the snapshot frame in
  // [snapshot, snapshot+snap_len) is validated and stored as the single coherent snapshot. Up to
  // PGR_TRANSPORT_DRAIN_CAP queued events that fit in [out, out+out_cap) are serialized (and CONSUMED) into an event
  // frame; *out_len receives its byte length (>= PGR_TRANSPORT_EVENT_HEADER). A stored snapshot is only replaced by a
  // STRICTLY NEWER frame: a snapshot whose seq does not advance, or any of whose monotonic pulse counts has
  // regressed, is rejected ERR_REPLAY (it neither overwrites the stored snapshot nor refreshes the lease).
  // If the head event cannot fit even an empty out buffer the call returns ERR_RANGE rather than looping on an
  // empty frame forever. Returns OK or a negative status; a rejected snapshot does not corrupt the stored one
  // and the drain still runs. Call only from the host daemon thread -- never a Unity/gameplay thread.
  int (*exchange)(const uint8_t *snapshot, uint32_t snap_len, uint8_t *out, uint32_t out_cap, uint32_t *out_len);

  // Producer enqueue (any thread). opcode in 0..PGR_TRANSPORT_ACTIONS-1; n_scalars/n_strings must equal the compiled
  // schema arity for that opcode. Scalars are 8-byte little-endian and are TYPE-VALIDATED against the compiled
  // per-opcode schema (NOT merely arity, so a bad native producer cannot poison the host): booleans must be
  // 0/1; ints must be integral within signed-32; longs carry the EXACT 64-bit value (no JS safe-integer clamp,
  // so Long.MAX_VALUE is valid) subject to the action's domain; doubles are raw IEEE-754 bits that must decode
  // finite; the per-opcode domain (applyMapTap lat/lon/mode/sampledAt, updateCameraHeadingAt timestamp>=0) is
  // enforced too. Strings are copied and must be valid UTF-8 within max_string_bytes. An event whose serialized
  // size could never fit a bounded frame is rejected with ERR_TOO_LARGE (it would otherwise wedge the queue).
  // Returns OK, ERR_OVERFLOW (fail closed), STOPPED (clean shutdown no-op), ERR_DOMAIN/ERR_BAD_* (validation),
  // or ERR_TOO_LARGE. Holds the state mutex only; makes no callback.
  int (*emit)(uint32_t opcode, const int64_t *scalars, uint32_t n_scalars,
              const PgrRuntimeTransportString *strings, uint32_t n_strings);

  // Typed settings getters for compiled gameplay consumers. Return STALE when the snapshot is not fresh,
  // except get_bool returns OK with *out == 0 for a guarded field on a stale snapshot. field is the reader's
  // index within its type group (see the contract). *out buffers are only written on OK.
  int (*get_bool)(uint32_t field, int *out);
  int (*get_int)(uint32_t field, int32_t *out);
  // has_value receives 0 when the stored coordinate is a NaN "null" sentinel (legacy JSON null), else 1.
  int (*get_double)(uint32_t field, double *out, int *has_value);
  // Copies the field's UTF-8 bytes (no NUL terminator) into buf; *out_len receives the true length. If
  // cap < length, returns ERR_RANGE and writes nothing (but still sets *out_len to the required length).
  int (*get_string)(uint32_t field, char *buf, uint32_t cap, uint32_t *out_len);
  // The atomic account command/token pair (reader string slots 5 and 6). Both parts always present together.
  int (*get_command_snapshot)(char *cmd, uint32_t cmd_cap, uint32_t *cmd_len,
                              char *tok, uint32_t tok_cap, uint32_t *tok_len);
  // One-shot pulse edge for a single consumer: *out_fired == 1 iff the monotonic count advanced since the
  // last take_pulse for this pulse id. STALE snapshot yields OK with *out_fired == 0 (legacy client default).
  int (*take_pulse)(uint32_t pulse, int *out_fired);

  int (*sample)(PgrRuntimeTransportSample *out);

  // Stop: mark stopped (snapshot getters go STALE), clear the queue, bump the session generation. A later
  // install()/exchange() does not auto-restart gameplay; the host owns restart. Any thread; no callback.
  int (*stop)(void);
};

static_assert(sizeof(PgrRuntimeTransportApi) == 96, "RuntimeTransport table must remain a 96-byte v1 table");

// Hidden getter. NOT exported by the version script; the coordinator registers it in query_feature under
// feature id 52 and answers only on an exact version match.
const void *pgrazor_runtime_transport_api(uint32_t version);

#ifdef __cplusplus
}
#endif
