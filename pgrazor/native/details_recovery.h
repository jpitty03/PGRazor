#pragma once
#include <stdint.h>

#include "details_completion.h"  // feature 37: shared 272-byte config + request/validate ABI

// Feature 38 (API 1). Native POST-CATCH DETAILS RECOVERY: the C++ port of the confirmed
// diagnostics/quick-close.js recoverDetails(gui) (~1507). From ONLY a borrowed details GUI handle and
// the expected CatchPokemonOutProto, it re-derives the full details record -- data (gui.data),
// dismissed (data.onDismissed, also gui.callback), view/"state" (dismissed.delegateTarget),
// response (view.response), and the caught Pokemon (data.list.item(0)) -- roots each one, and proves
// the whole details identity before handing the six native-owned GC handles back to the host. It
// installs NO hook, NEVER calls back into JS, and makes NO gameplay request or dismissal (it is pure
// recovery; the host feeds the recovered record into feature 37 for the actual dismiss click).
//
// Relationship to feature 37 (details_completion): feature 37 is unchanged and remains the validator.
// Feature 38 reuses feature 37's EXACT 272-byte PgrDetailsCompletionConfig (aliased below as
// PgrDetailsRecoveryConfig: identical runtime exports, 32 validated pointers, success/STATE_OPEN
// literals). install() verifies and publishes the immutable config and ALSO installs feature 37 with
// the same config (idempotent) so the prerequisite validator is guaranteed present; recover()'s final
// snapshot is proven by calling feature 37's validate() on the rooted handles. The correct op_Implicit
// "alive" ABI is handled entirely inside feature 37.
//
// Read-only recovery: unlike complete(), recover() tolerates a still-OPENING canvas (no STATE_OPEN /
// openerDepth==0 requirement). It requires the current OS tid to equal the proven owner (owner latched
// only after an actual tid match), a nonzero 8-byte caught key, and a live request lease (remaining_ms
// clamped to 2500 ms against CLOCK_BOOTTIME; remaining 0 refuses). A busy guard spanning root cleanup
// prevents managed reentry. The deadline is re-checked after the managed calls and again right before
// publishing, and every class / scalar relationship is re-proven after each heap-relocating managed
// call. No raw managed pointer survives a managed call or a root allocation: targets are always
// re-resolved freshly through the owned temporary roots.

#ifdef __cplusplus
extern "C" {
#endif

static_assert(sizeof(void *) == 8, "DetailsRecovery native ABI requires LP64");

// Feature 38 shares feature 37's 272-byte config verbatim; the host packs it once and both modules
// re-check the header and reject any null pointer.
using PgrDetailsRecoveryConfig = PgrDetailsCompletionConfig;
static_assert(sizeof(PgrDetailsRecoveryConfig) == 272, "recovery config must stay 272 bytes on LP64");

enum PgrDetailsRecoveryInfo {
  PGR_DR_FEATURE_ID      = 38,
  PGR_DR_API_VERSION     = 1,
  PGR_DR_REQUEST_VERSION = 1,
  PGR_DR_OUTPUT_VERSION  = 1,
  PGR_DR_MAX_LEASE_MS    = 2500  // request remaining_ms ceiling (mirrors feature 37's lease cap)
};

// recover() returns 1 RECOVERED / 0 REFUSED (gate/identity/validate) / negative error. release() and
// install() return PGR_DR_OK or negative.
enum PgrDetailsRecoveryStatus {
  PGR_DR_RECOVERED        = 1,
  PGR_DR_OK               = 0,   // generic success / recover(): refused
  PGR_DR_ERR_NULL_ARG     = -1,
  PGR_DR_ERR_BAD_VERSION  = -2,
  PGR_DR_ERR_BAD_SIZE     = -3,
  PGR_DR_ERR_MISSING_PTR  = -4,
  PGR_DR_ERR_NOT_INSTALLED= -6,
  PGR_DR_ERR_PREREQUISITE = -7,  // feature 37 validator table/install unavailable
  PGR_DR_ERR_RECONFIG     = -9,
  PGR_DR_ERR_CLOCK_FAILURE= -10
};

// Fixed LP64 request (56 bytes). gui and response are opaque borrowed GC HANDLE IDs (not managed object
// addresses); the caller retains them for the call. expected_key is the 8-byte caught id written
// verbatim and compared byte-for-byte, never numerically. A zero key or a zero remaining refuses.
struct PgrDetailsRecoveryRequest {
  uint32_t version;         // == PGR_DR_REQUEST_VERSION
  uint32_t size;            // == sizeof(PgrDetailsRecoveryRequest)
  uint8_t  expected_key[8]; // proven caught id (nonzero); 8 raw bytes, never numerically converted
  uint64_t epoch;           // caller's current epoch (carried into the feature 37 validate request)
  uint64_t tid;             // proven owner tid (must equal the current OS tid)
  uint64_t remaining_ms;    // residual lease; clamped to PGR_DR_MAX_LEASE_MS, 0 refuses
  void *gui;                // borrowed handle -> DetailsGuiController (the only scene anchor)
  void *response;           // borrowed handle -> the expected authoritative CatchPokemonOutProto
};

static_assert(sizeof(PgrDetailsRecoveryRequest) == 56,
              "PgrDetailsRecoveryRequest must remain 56 bytes on LP64");

// Fixed LP64 output (64 bytes). On RECOVERED the six slots carry NATIVE-OWNED GC handle IDs in record
// order; ownership passes to the caller, which MUST call release() (typically after duplicating each
// into a host-owned handle). On refusal / error the six handles stay zero (the header and status are
// always set). The handles are the derived details objects, never the borrowed inputs.
struct PgrDetailsRecoveryOutput {
  uint32_t version;  // == PGR_DR_OUTPUT_VERSION
  uint32_t size;     // == sizeof(PgrDetailsRecoveryOutput)
  int32_t  status;   // mirrors the recover() return code
  uint32_t reserved; // 0 in v1
  void *gui;         // native-owned handle -> DetailsGuiController
  void *state;       // native-owned handle -> details VIEW (host record field is r.state)
  void *response;    // native-owned handle -> CatchPokemonOutProto
  void *data;        // native-owned handle -> details DATA
  void *pokemon;     // native-owned handle -> caught Pokemon (list.item(0))
  void *dismissed;   // native-owned handle -> captured System.Action
};

static_assert(sizeof(PgrDetailsRecoveryOutput) == 64,
              "PgrDetailsRecoveryOutput must remain 64 bytes on LP64");

// Fixed LP64 table: 8-byte header + 3 function pointers = 32 bytes.
struct PgrDetailsRecoveryApi {
  uint32_t version;  // == PGR_DR_API_VERSION
  uint32_t size;     // == sizeof(PgrDetailsRecoveryApi)
  // Verify + publish the immutable config, then install feature 37 with the same config (idempotent).
  int (*install)(const PgrDetailsRecoveryConfig *config);
  // Owner-thread recovery. Re-derives + roots the whole details record from request->gui and
  // request->response, proves the identity, then calls feature 37 validate() on the rooted handles.
  // On success publishes the six native-owned handles into *out (ownership transfers to the caller).
  // Always fills *out (header + status; handles zero unless RECOVERED). Off-owner / gate / identity /
  // validate failures refuse (0). Owner thread only.
  int (*recover)(const PgrDetailsRecoveryRequest *request, PgrDetailsRecoveryOutput *out);
  // Free any nonzero native-owned handle in *out and zero it (idempotent). Never frees a borrowed
  // input. Any thread.
  int (*release)(PgrDetailsRecoveryOutput *out);
};

static_assert(sizeof(PgrDetailsRecoveryApi) == 32,
              "PgrDetailsRecoveryApi must remain a 32-byte version-1 table");

// Hidden getter. NOT exported by the version script; the coordinator registers it in query_feature
// under feature id 38 and answers only on an exact version match.
const void *pgrazor_details_recovery_api(uint32_t version);

#ifdef __cplusplus
}
#endif
