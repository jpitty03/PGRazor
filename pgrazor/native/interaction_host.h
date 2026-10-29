#pragma once

#include <stddef.h>
#include <stdint.h>

#include "runtime_metadata.h"
#include "unity_scheduler.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PGR_IH_FEATURE_ID = 69,
    PGR_IH_API_VERSION = 1,

    /* Exact LIMITS from pgrazor/diagnostics/interaction-probe.js */
    PGR_IH_LIMIT_ATTEMPTS = 20,
    PGR_IH_LIMIT_RETRY_MS = 1000,
    PGR_IH_LIMIT_PREPARE_MS = 20000,
    PGR_IH_LIMIT_SCAN_MS = 4000,
    PGR_IH_LIMIT_ASSEMBLIES = 256,
    PGR_IH_LIMIT_LOOKUPS = 2048,
    PGR_IH_LIMIT_METHODS_PER_CLASS = 80,
    PGR_IH_LIMIT_METHODS = 400,
    PGR_IH_LIMIT_FIELDS_PER_CLASS = 96,
    PGR_IH_LIMIT_FIELDS = 320,
    PGR_IH_LIMIT_PARAMETERS = 8,
    PGR_IH_LIMIT_NAME_BYTES = 256,
    PGR_IH_LIMIT_OBSERVE_MS = 60000,
    PGR_IH_LIMIT_POLL_MS = 250,
    PGR_IH_LIMIT_EVENTS = 512,
    PGR_IH_LIMIT_HITS = 4096,
    PGR_IH_LIMIT_THREADS = 4,
    PGR_IH_LIMIT_ENUM_FIELDS = 64,
    PGR_IH_LIMIT_ENUM_LITERALS = 32,
    PGR_IH_SCHEDULE_DELAY_MS = 25000,

    /* Candidate classes & targets */
    PGR_IH_CLASS_COUNT = 11,
    PGR_IH_REQUIRED_TARGETS = 14,
    PGR_IH_OPTIONAL_TARGETS = 6,
    PGR_IH_TOTAL_TARGETS = 20,
    PGR_IH_RESULT_READER_SPECS = 2,
    PGR_IH_ARG_RESULT_CALLBACKS = 3,

    /* Pinned Pokeball::btxu RVA and prologue word count */
    PGR_IH_BTXU_RVA = 0x0ad58170u,
    PGR_IH_BTXU_PROLOGUE_WORDS = 8,
    PGR_IH_BTXU_PROLOGUE_BYTES = 32,
    PGR_IH_DEBUG_PROLOGUE_WORDS = 20,
    PGR_IH_DEBUG_PROLOGUE_BYTES = 80,

    /* Feature 57 transfer observer RVAs (zlc / zld) */
    PGR_IH_TR_RVA_ZLC = 0x0a0534a0u,
    PGR_IH_TR_RVA_ZLD = 0x0a0538b4u,

    /* Transport 52 reader, pulse, and action indices for Feature 69 */
    PGR_IH_RT_BOOL_DIAGNOSTICS_READY = 15,
    PGR_IH_RT_PULSE_STOP_REQUEST = 1,
    PGR_IH_RT_PULSE_TRACE_REQUEST = 2,
    PGR_IH_RT_ACT_EVENT = 19,
    PGR_IH_RT_ACT_DIAGNOSTIC = 20,
    PGR_IH_RT_ACT_SET_RUNNING = 36,

    /* Required Target IDs (0..13) */
    PGR_IH_TGT_CAPTURE_ENTER = 0,             /* EncounterCaptureState::EnterState() */
    PGR_IH_TGT_CAPTURE_EXIT = 1,              /* EncounterCaptureState::ExitState() */
    PGR_IH_TGT_CAPTURE_SUMMARY_DISMISSED = 2, /* EncounterCaptureState::SummaryDismissed(CatchPokemonOutProto) [Result Reader] */
    PGR_IH_TGT_POKESTOP_RECEIVED_ITEMS = 3,   /* PokestopState::NotifyReceivedItems() */
    PGR_IH_TGT_POKESTOP_NOTIFY_SPIN = 4,      /* PokestopState::NotifyPokestopSpin() */
    PGR_IH_TGT_POKESTOP_RESOLVE_COMPLETE = 5, /* PokestopState::ResolveComplete() */
    PGR_IH_TGT_SPINNER_ENTER = 6,             /* PokestopSpinnerInteractionState::EnterState() [Shared 67/11] */
    PGR_IH_TGT_SPINNER_EXIT = 7,              /* PokestopSpinnerInteractionState::ExitState() [Shared 67/11] */
    PGR_IH_TGT_RESOLVE_ENTER = 8,             /* PokestopResolveState::EnterState() [Shared 67/10] */
    PGR_IH_TGT_RESOLVE_EXIT = 9,              /* PokestopResolveState::ExitState() [Shared 67/10] */
    PGR_IH_TGT_CAPTURE_BTDD = 10,             /* EncounterCaptureState::btdd(CatchPokemonOutProto) [Result Reader, Shared 49] */
    PGR_IH_TGT_CAPTURE_BTDF = 11,             /* EncounterCaptureState::btdf(CatchPokemonOutProto) [Timing-only, Shared 49] */
    PGR_IH_TGT_CAPTURE_BTDI = 12,             /* EncounterCaptureState::btdi(CatchPokemonOutProto) [Timing-only] */
    PGR_IH_TGT_SPINNER_BGFF = 13,             /* PokestopSpinnerInteractionState::bgff(FortSearchOutProto) [Result Reader, Shared 67/11] */

    /* Optional Target IDs (14..19) */
    PGR_IH_TGT_SUMMARY_GUI_OPEN = 14,          /* EncounterSummaryGuiController::HandleOpen() */
    PGR_IH_TGT_SUMMARY_GUI_OPEN_COMPLETE = 15, /* EncounterSummaryGuiController::HandleOpenComplete() [Shared 61] */
    PGR_IH_TGT_SUMMARY_GUI_DISMISS = 16,       /* EncounterSummaryGuiController::HandleDismiss() */
    PGR_IH_TGT_SUMMARY_GUI_DISMISS_COMPLETE = 17, /* EncounterSummaryGuiController::HandleDismissComplete() */
    PGR_IH_TGT_SUMMARY_STATE_EXIT = 18,        /* EncounterSummaryState::ExitState() [Shared 61] */
    PGR_IH_TGT_POKEBALL_BTXU = 19,             /* Pokeball::btxu() -> CatchPokemonOutProto [Return Reader, Shared 49] */

    /* Bitmasks for shared hook sites owned by Features 49, 57, 61, 67 */
    PGR_IH_REQUIRED_MASK = (1u << PGR_IH_REQUIRED_TARGETS) - 1u,
    PGR_IH_OPTIONAL_MASK = ((1u << PGR_IH_TOTAL_TARGETS) - 1u) ^ PGR_IH_REQUIRED_MASK,
    PGR_IH_ALL_TARGETS_MASK = (1u << PGR_IH_TOTAL_TARGETS) - 1u,

    PGR_IH_SHARED_MASK_49 = (1u << PGR_IH_TGT_CAPTURE_BTDD) |
                            (1u << PGR_IH_TGT_CAPTURE_BTDF) |
                            (1u << PGR_IH_TGT_POKEBALL_BTXU),
    PGR_IH_SHARED_MASK_57 = 0u,
    PGR_IH_SHARED_MASK_61 = (1u << PGR_IH_TGT_SUMMARY_GUI_OPEN_COMPLETE) |
                            (1u << PGR_IH_TGT_SUMMARY_STATE_EXIT),
    PGR_IH_SHARED_MASK_67_CAMERA = (1u << PGR_IH_TGT_RESOLVE_ENTER) |
                                   (1u << PGR_IH_TGT_RESOLVE_EXIT),
    PGR_IH_SHARED_MASK_67_QUICK = (1u << PGR_IH_TGT_SPINNER_ENTER) |
                                  (1u << PGR_IH_TGT_SPINNER_EXIT) |
                                  (1u << PGR_IH_TGT_SPINNER_BGFF),
    PGR_IH_SHARED_MASK_67 = PGR_IH_SHARED_MASK_67_CAMERA | PGR_IH_SHARED_MASK_67_QUICK,
    PGR_IH_SHARED_MASK_ALL = PGR_IH_SHARED_MASK_49 | PGR_IH_SHARED_MASK_61 | PGR_IH_SHARED_MASK_67,

    /* Return codes */
    PGR_IH_OK = 0,
    PGR_IH_ERR_INVALID = -1,
    PGR_IH_ERR_UNCONFIGURED = -2,
    PGR_IH_ERR_BUSY = -3,
    PGR_IH_ERR_HOOK = -4,

    /* Probe status codes */
    PGR_IH_STATUS_IDLE = 0,
    PGR_IH_STATUS_SCHEDULED = 1,
    PGR_IH_STATUS_PREPARING = 2,
    PGR_IH_STATUS_OBSERVING = 3,
    PGR_IH_STATUS_COMPLETE = 4,
    PGR_IH_STATUS_SKIPPED = 5,
    PGR_IH_STATUS_STOPPED = 6,
    PGR_IH_STATUS_FAILED = 7,

    /* Config flags */
    PGR_IH_CFG_USE_BOOTSTRAP = 1u << 0,
    PGR_IH_CFG_NO_AUTO_SHARED_DETECT = 1u << 1,

    /* Run option flags */
    PGR_IH_OPT_DEGRADE_UNATTESTED_SHARED = 1u << 0,

    /* Event types */
    PGR_IH_EVT_NONE = 0,
    PGR_IH_EVT_SCHEDULED = 1,
    PGR_IH_EVT_METADATA_READY = 2,
    PGR_IH_EVT_CLASS = 3,
    PGR_IH_EVT_FIELD = 4,
    PGR_IH_EVT_METHOD = 5,
    PGR_IH_EVT_LIMIT = 6,
    PGR_IH_EVT_METADATA_SUMMARY = 7,
    PGR_IH_EVT_OBSERVER_TARGET_AVAILABILITY = 8,
    PGR_IH_EVT_RESULT_READ_AVAILABILITY = 9,
    PGR_IH_EVT_OBSERVATION_READY = 10,
    PGR_IH_EVT_INTERACTION_EVENT = 11,
    PGR_IH_EVT_OBSERVATION_SUMMARY = 12,
    PGR_IH_EVT_FINISHED = 13,

    /* Interaction event phases */
    PGR_IH_PHASE_NONE = 0,
    PGR_IH_PHASE_ENTER = 1,
    PGR_IH_PHASE_LEAVE = 2,

    /* Result reader statuses */
    PGR_IH_RES_NONE = 0,
    PGR_IH_RES_DECODED = 1,
    PGR_IH_RES_NULL_ARG = 2,
    PGR_IH_RES_SKIPPED = 3,
    PGR_IH_RES_DISABLED = 4,
};

/* Extra reflection/runtime exports outside Feature 47's 31 PgrMetaExports */
typedef struct PgrInteractionExtraExports {
    void *image_get_name;         /* const char *(*)(const void *image) */
    void *class_get_name;         /* const char *(*)(void *klass) */
    void *class_get_namespace;    /* const char *(*)(void *klass) */
    void *class_get_parent;       /* void *(*)(void *klass) */
    void *class_get_flags;        /* int32_t (*)(void *klass) */
    void *class_instance_size;    /* int32_t (*)(void *klass) */
    void *class_get_fields;       /* void *(*)(void *klass, void **iter) */
    void *field_get_name;         /* const char *(*)(void *field) */
    void *field_static_get_value; /* void (*)(void *field, void *out) */
    void *method_get_param_name;  /* const char *(*)(void *method, uint32_t index) */
    void *thread_current;         /* void *(*)(void) */
    void *thread_attach;          /* void *(*)(void *domain) */
    void *thread_detach;          /* void (*)(void *thread) */
    void *gchandle_new;           /* uintptr_t (*)(void *obj, int pinned) */
    void *gchandle_get_target;    /* void *(*)(uintptr_t handle) */
    void *gchandle_free;          /* void (*)(uintptr_t handle) */
} PgrInteractionExtraExports;

typedef struct PgrInteractionHostConfig {
    PgrMetaBindConfig binding;        /* 64B: Feature 47 bind config (exports + module_base/size + seg_query) */
    PgrInteractionExtraExports extra; /* 128B: Extra reflection + thread + GC handle exports */
    const void *debug_code_address;   /* Optional override for debug_get_method_info code check in fixture; null -> binding.exports->debug_get_method_info */
    void *now_ms_fn;                  /* int64_t (*)(void); null -> clock_gettime(CLOCK_MONOTONIC) */
    uint32_t flags;                   /* PGR_IH_CFG_* */
    uint32_t shared_owned_mask;       /* Initial owned_mask for 49/57/61/67 */
    uint32_t shared_integrated_mask;  /* Initial integrated_mask for 49/57/61/67 */
    uint32_t reserved0;
} PgrInteractionHostConfig;

typedef struct PgrInteractionHostOptions {
    uint8_t observe_runtime; /* 0 = metadata-only, 1 = runtime callback capture */
    uint8_t read_results;    /* 0 = timing-only, 1 = decode CatchPokemonOutProto.Status & FortSearchOutProto.Result */
    uint8_t reserved0[2];
    uint32_t flags;          /* PGR_IH_OPT_* */
} PgrInteractionHostOptions;

typedef struct PgrInteractionHostEvent {
    uint64_t seq;
    int64_t ts_ms;
    int64_t elapsed_ms;
    int64_t duration_ms;      /* >= 0 on leave events, -1 otherwise */
    uint32_t type;            /* PGR_IH_EVT_* */
    uint32_t target_id;       /* 0..19 for interaction-event, 0xffffffffu otherwise */
    uint32_t call_id;         /* 1..N for interaction-event, 0 otherwise */
    uint32_t depth;           /* 1..N for interaction-event, 0 otherwise */
    int32_t tid;              /* thread id for interaction-event */
    uint8_t phase;            /* PGR_IH_PHASE_* */
    uint8_t result_status;    /* PGR_IH_RES_* */
    uint8_t is_return_result; /* 1 for Pokeball::btxu return result */
    uint8_t available;        /* 1/0 for availability events */
    int32_t result_code;      /* enum int32 value when result_status == PGR_IH_RES_DECODED */
    char result_name[64];     /* enum literal name or reason when applicable */
    char channel[32];         /* "interaction-probe" or "interaction-probe-scheduled" */
    char json[768];           /* serialized JSON payload matching diagnostics/interaction-probe.js */
} PgrInteractionHostEvent;

typedef struct PgrInteractionHostSample {
    uint32_t status;                     /* PGR_IH_STATUS_* */
    uint8_t configured;
    uint8_t running;
    uint8_t scheduled;
    uint8_t observe_runtime;
    uint8_t read_results;
    uint8_t stopping;
    uint8_t truncated;
    uint8_t argument_readers_available;
    uint8_t return_reader_available;
    uint8_t deferred_cleanup_pending;
    uint16_t reserved0;
    uint32_t attempt_count;
    uint32_t assemblies_scanned;
    uint32_t lookups_used;
    uint32_t classes_resolved;
    uint32_t fields_emitted;
    uint32_t methods_emitted;
    uint32_t resolved_required_mask;
    uint32_t resolved_optional_mask;
    uint32_t ambiguous_target_mask;
    uint32_t hooked_direct_mask;
    uint32_t shared_owned_mask;
    uint32_t shared_integrated_mask;
    uint32_t blocked_shared_mask;
    uint32_t installed_target_mask;
    uint32_t hit_count;
    uint32_t event_count;
    uint32_t reserved_events;
    uint32_t thread_count;
    uint32_t dropped_threads;
    uint32_t in_flight;
    uint32_t btxu_pending_polls;
    uint32_t total_emitted_events;
    int32_t threads[PGR_IH_LIMIT_THREADS];
    uint32_t target_hits[PGR_IH_TOTAL_TARGETS];
    char reason[96];
    char last_diagnostic[160];
} PgrInteractionHostSample;

typedef struct PgrInteractionHostApi {
    uint32_t version;
    uint32_t feature_id;
    int (*configure)(const PgrInteractionHostConfig *cfg);
    int (*schedule)(int64_t delay_ms);
    int (*start)(const PgrInteractionHostOptions *opts);
    int (*stop)(const char *reason);
    int (*tick)(int32_t current_tid);
    void (*on_unity_pulse)(void *ctx, const PgrUnityPulse *pulse);
    int (*shared_sites)(uint32_t owned_mask, uint32_t integrated_mask);
    uint64_t (*observe_enter)(uint32_t target_id, void *receiver, void *arg0);
    void (*observe_leave)(uint64_t token, void *retval);
    int (*take_event)(PgrInteractionHostEvent *out_event);
    int (*sample)(PgrInteractionHostSample *out_sample);
    void (*reset_for_tests)(void);
} PgrInteractionHostApi;

#if defined(__cplusplus) && (defined(__LP64__) || defined(_LP64))
static_assert(sizeof(PgrInteractionExtraExports) == 128, "PgrInteractionExtraExports size");
static_assert(sizeof(PgrInteractionHostConfig) == 224, "PgrInteractionHostConfig size");
static_assert(sizeof(PgrInteractionHostOptions) == 8, "PgrInteractionHostOptions size");
static_assert(sizeof(PgrInteractionHostEvent) == 928, "PgrInteractionHostEvent size");
static_assert(sizeof(PgrInteractionHostSample) == 456, "PgrInteractionHostSample size");
static_assert(sizeof(PgrInteractionHostApi) == 104, "PgrInteractionHostApi size");
#endif

const void *pgrazor_interaction_host_api(uint32_t version);
// API2 is the immutable observer surface for existing compiled hook owners.
// wants_target is allocation-free and requires an already-attached caller.
// abort consumes only native counters/reservations; it performs no GC work.
struct PgrInteractionHostApi2 {
    uint32_t version,size;
    int (*wants_target)(uint32_t target_id);
    uint64_t (*observe_enter)(uint32_t target_id,void *receiver,void *arg0);
    void (*observe_leave)(uint64_t token,void *retval);
    void (*observe_abort)(uint64_t token);
};
static_assert(sizeof(PgrInteractionHostApi2)==40,"Interaction observer ABI2");

#ifdef __cplusplus
}
#endif
