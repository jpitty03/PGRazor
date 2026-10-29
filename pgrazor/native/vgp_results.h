#pragma once
#include <stdint.h>

// Feature 30 (API 1). Native, bounded, synchronous READ of a VGP request promise plus the catch/spin
// result classification -- a focused C++ port of the pending-resolution block in
// diagnostics/quick-close.js nativePromise.js (vgpField/vgpPromise) and the catch/spin status reads in
// runtime/features/vgp.js (~179-201, state.error/value.isNull, exact(value,RESP)->catchStatus,
// exact(value,SPINRESULT)->result===spinSuccess).
//
// SCOPE / NOT: this module replaces ONLY the promise polling and the catch/spin result classification.
// It issues NO RPC, NO transfer, NO cooldown call and NO managed invoke of any kind -- it only reads
// fields, compares classes, and allocates/frees GC roots. The shared JS nativePromise helper stays for
// the other features that poll non-VGP promises. The pending-record lifecycle, the manual-exclusion /
// inventory / service gating, the auto-transfer, the spin cooldown write and the marker all remain in
// the feature-25 VGP host (JS). This module installs NO Dobby/Gum hook and NEVER calls back into JS.
//
// poll() is one owner-thread read. The caller supplies the already-rooted promise object pointer (the
// host keeps q.promise alive), the phase, and the proven Unity owner tid. poll() roots the promise,
// resolves completeCalled/errorCalled/completedValue(+optional statusErrorCalled) by NAME walking the
// class hierarchy (max 16 parents, existing 16..512 offset guard), validates the two required bool
// fields + optional status bool + the reference-typed completedValue via the il2cpp field metadata API
// (field_get_type/class_from_type/class_is_valuetype) so a wrong shape can never over-read, then returns:
//   negative  -> native/shape error (host throws, no silent fallback)
//   PENDING 0 -> not resolved yet (or not the owner thread) : host keeps waiting
//   REJECTED 1-> errorCalled/statusErrorCalled set, OR a null completedValue (error dominates completed)
//   DONE 2    -> completeCalled set with a non-null value : *out carries success/status + a GC root
//                adopted by the host EXACTLY ONCE (freed at the end of the host tick). Root count is
//                zero on every path except the single returned owned root.
//
// Gate: the owner tid must equal the current OS tid. VGP mask/lease is deliberately NOT required -- an
// in-flight result MUST still resolve after the user toggles VGP off, exactly as the existing JS host.
// The host enforces s.active/ready/!stopping before calling (passing owner==0 otherwise).

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "VgpResults native ABI requires LP64");

enum PgrVgpResultsInfo {
  PGR_VR_FEATURE_ID     = 30,
  PGR_VR_API_VERSION    = 1,
  PGR_VR_CONFIG_VERSION = 1,
  PGR_VR_RESULT_VERSION = 1,
  PGR_VR_SAMPLE_VERSION = 1,
  PGR_VR_OFFSET_MIN     = 16,   // existing vgpField guard: reject a field offset below this
  PGR_VR_OFFSET_MAX     = 512,  // ...or above this
  PGR_VR_PARENT_MAX     = 16    // existing vgpField guard: class_get_parent walk depth
};

// phase argument to poll().
enum PgrVgpResultsPhase {
  PGR_VR_PHASE_ENCOUNTER = 1,  // any non-null resolved value is valid (old semantics); no status/success
  PGR_VR_PHASE_CATCH     = 2,  // exact RESP -> status + success; else status unknown, success false
  PGR_VR_PHASE_SPIN      = 3   // exact SPINRESULT + Result==spin_success -> success; else success false
};

// poll() return codes.
enum PgrVgpResultsStatus {
  PGR_VR_DONE              = 2,   // completed: *out populated, out->value_handle set (adopt exactly once)
  PGR_VR_REJECTED         = 1,   // promise error OR null resolved value (host: "Request rejected")
  PGR_VR_PENDING          = 0,   // not resolved yet / not the owner thread (host keeps waiting)
  PGR_VR_ERR_NULL_ARG     = -1,  // null out / promise null-but-owner-ok is PENDING, not this
  PGR_VR_ERR_NOT_INSTALLED= -2,
  PGR_VR_ERR_BAD_PHASE    = -3,
  PGR_VR_ERR_GCHANDLE     = -4,  // a GC root could not be allocated (fail closed: nothing returned)
  PGR_VR_ERR_SHAPE        = -5,  // required field missing / bad offset / wrong field type (unknown shape)
  PGR_VR_ERR_REENTRANT    = -6   // a poll was already in progress (defensive: no managed invokes exist)
};

// Fixed LP64 config (176 bytes): 8-byte header + 20 validated il2cpp pointers + 2 int32. Every pointer
// is an already-validated export / Il2CppClass* / FieldInfo* / UTF-8 name buffer packed by the host off
// p.vgp; the native layer re-checks the header and rejects any null. No hook address. No invoked method.
struct PgrVgpResultsConfig {
  uint32_t version;  // == PGR_VR_CONFIG_VERSION
  uint32_t size;     // == sizeof(PgrVgpResultsConfig)

  // il2cpp runtime exports (metadata reads only; NONE run managed code).
  void *gchandle_new;            // uintptr_t (void* obj, int pinned)
  void *gchandle_get_target;     // void*     (uintptr_t handle)
  void *gchandle_free;           // void      (uintptr_t handle)
  void *object_get_class;        // void*     (void* obj)
  void *class_get_parent;        // void*     (void* klass)
  void *class_get_field_from_name; // void*   (void* klass, const char* name)
  void *field_get_offset;        // size_t    (void* field)
  void *field_get_type;          // void*     (void* field)
  void *class_from_type;         // void*     (void* type)
  void *class_is_valuetype;      // bool      (void* klass)
  void *field_get_value;         // void      (void* obj, void* field, void* out)

  // Exact Il2CppClass* identities.
  void *class_bool;         // System.Boolean (required bool-field type check)
  void *class_resp;         // Holoholo.Rpc.CatchPokemonOutProto (RESP, exact for catch status)
  void *class_spinresult;   // Holoholo.Rpc.FortSearchOutProto (SPINRESULT, exact for spin result)

  // UTF-8 field-name buffers (looked up with class_get_field_from_name, walking parents).
  void *name_complete;      // "completeCalled"      (required bool)
  void *name_error;         // "errorCalled"         (required bool)
  void *name_value;         // "completedValue"      (required reference)
  void *name_status;        // "statusErrorCalled"   (optional bool; absence accepted)

  // FieldInfo* for the classified responses (read via field_get_value as Int32).
  void *f_catch_status;     // RESP.Status (Int32 enum)
  void *f_spin_result;      // SPINRESULT.Result (Int32 enum)

  // Validated enum literals.
  int32_t catch_success;    // p.success (RESP status CatchSuccess)
  int32_t spin_success;     // t.spinSuccess (SPINRESULT Result Success)
};

static_assert(sizeof(PgrVgpResultsConfig) == 176,
              "PgrVgpResultsConfig must remain 176 bytes on LP64");

// Fixed LP64 poll output (32 bytes). Populated on DONE; otherwise state==0 and all fields clear.
struct PgrVgpResultsResult {
  uint32_t version;       // == PGR_VR_RESULT_VERSION
  uint32_t size;          // == sizeof(PgrVgpResultsResult)
  uint32_t state;         // 0 pending, 1 completed (DONE), 2 rejected (REJECTED)
  uint32_t status_known;  // 1 iff the catch status was read from an exact RESP; else 0
  int32_t  status;        // catch Status enum value when status_known; else 0 (spin never sets this)
  uint32_t success;       // 1 iff catch status == catch_success / spin result == spin_success
  uint64_t value_handle;  // DONE only: GC handle of the rooted completed value; 0 otherwise
};

static_assert(sizeof(PgrVgpResultsResult) == 32,
              "PgrVgpResultsResult must remain 32 bytes on LP64");

// Fixed LP64 telemetry gauge (48 bytes). Pull-only; the native layer never calls JS.
struct PgrVgpResultsSample {
  uint32_t version;         // == PGR_VR_SAMPLE_VERSION
  uint32_t size;            // == sizeof(PgrVgpResultsSample)
  uint64_t poll_serial;     // owner-thread polls that passed the owner gate
  uint64_t completed_serial;// DONE results returned
  uint64_t rejected_serial; // REJECTED results (promise error or null resolved value)
  uint64_t error_serial;    // negative data errors (gchandle / shape)
  uint32_t fault;           // 1 once a shape/gchandle error has been observed
  uint32_t reserved;        // 0
};

static_assert(sizeof(PgrVgpResultsSample) == 48,
              "PgrVgpResultsSample must remain 48 bytes on LP64");

// Fixed LP64 table: 8-byte header + 3 function pointers = 32 bytes.
struct PgrVgpResultsApi {
  uint32_t version;  // == PGR_VR_API_VERSION
  uint32_t size;     // == sizeof(PgrVgpResultsApi)
  int (*install)(const PgrVgpResultsConfig *config);
  // One owner-thread read of an already-rooted promise. See the file header for return semantics.
  // out is always initialized. owner must equal the current OS tid or the call reports PENDING (0).
  int (*poll)(void *promise, uint32_t phase, uint64_t owner, PgrVgpResultsResult *out);
  int (*sample)(PgrVgpResultsSample *out);
};

static_assert(sizeof(PgrVgpResultsApi) == 32,
              "PgrVgpResultsApi must remain a 32-byte version-1 table");

// Hidden getter. NOT exported by the version script; the coordinator registers it in query_feature
// under feature id 30 and answers only on an exact version match.
const void *pgrazor_vgp_results_api(uint32_t version);

#ifdef __cplusplus
}
#endif
