#include "interaction_host.h"
#include "interaction_observer.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <atomic>

#if !defined(_WIN32)
#include <dlfcn.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "catch_observers.h"
#include "hook_engine.h"
#include "runtime_bootstrap.h"
#include "runtime_transport.h"
#include "shared_host.h"
#include "spin_host.h"
#include "transfer_observers.h"

namespace {

#if defined(__GNUC__) || defined(__clang__)
#define PGR_WEAK __attribute__((weak))
#else
#define PGR_WEAK
#endif

extern "C" const void *pgrazor_runtime_metadata_api(uint32_t version) PGR_WEAK;
extern "C" const void *pgrazor_runtime_bootstrap_api(uint32_t version) PGR_WEAK;
extern "C" const void *pgrazor_runtime_transport_api(uint32_t version) PGR_WEAK;
extern "C" const void *pgrazor_unity_scheduler_api(uint32_t version) PGR_WEAK;
extern "C" const void *pgrazor_shared_host_api(uint32_t version) PGR_WEAK;
extern "C" const void *pgrazor_spin_host_api(uint32_t version) PGR_WEAK;
extern "C" const void *pgrazor_catch_observers_api(uint32_t version) PGR_WEAK;
extern "C" const void *pgrazor_transfer_observers_api(uint32_t version) PGR_WEAK;
extern "C" int pgrazor_hook(void *target, void *replacement, void **original) PGR_WEAK;
extern "C" int pgrazor_unhook(void *target) PGR_WEAK;

static const PgrMetaApi *resolve_metadata_api() {
    if (pgrazor_runtime_metadata_api != nullptr) {
        return static_cast<const PgrMetaApi *>(
            pgrazor_runtime_metadata_api(PGR_MD_API_VERSION));
    }
#if !defined(_WIN32) && defined(PGR_INTERACTION_HOST_STANDALONE_TEST)
    using Fn = const void *(*)(uint32_t);
    auto fn = reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, "pgrazor_runtime_metadata_api"));
    if (fn != nullptr) {
        return static_cast<const PgrMetaApi *>(fn(PGR_MD_API_VERSION));
    }
#endif
    return nullptr;
}

static const PgrBootApi *resolve_bootstrap_api() {
    if (pgrazor_runtime_bootstrap_api != nullptr) {
        return static_cast<const PgrBootApi *>(
            pgrazor_runtime_bootstrap_api(PGR_BOOT_API_VERSION));
    }
#if !defined(_WIN32) && defined(PGR_INTERACTION_HOST_STANDALONE_TEST)
    using Fn = const void *(*)(uint32_t);
    auto fn = reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, "pgrazor_runtime_bootstrap_api"));
    if (fn != nullptr) {
        return static_cast<const PgrBootApi *>(fn(PGR_BOOT_API_VERSION));
    }
#endif
    return nullptr;
}

static const PgrRuntimeTransportApi *resolve_transport_api() {
    if (pgrazor_runtime_transport_api != nullptr) {
        return static_cast<const PgrRuntimeTransportApi *>(
            pgrazor_runtime_transport_api(PGR_TRANSPORT_API_VERSION));
    }
#if !defined(_WIN32) && defined(PGR_INTERACTION_HOST_STANDALONE_TEST)
    using Fn = const void *(*)(uint32_t);
    auto fn = reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, "pgrazor_runtime_transport_api"));
    if (fn != nullptr) {
        return static_cast<const PgrRuntimeTransportApi *>(fn(PGR_TRANSPORT_API_VERSION));
    }
#endif
    return nullptr;
}

static const PgrUnitySchedulerApi *resolve_scheduler_api() {
    if (pgrazor_unity_scheduler_api != nullptr) {
        const void *p = pgrazor_unity_scheduler_api(2);
        if (p == nullptr) {
            p = pgrazor_unity_scheduler_api(3);
        }
        return static_cast<const PgrUnitySchedulerApi *>(p);
    }
#if !defined(_WIN32) && defined(PGR_INTERACTION_HOST_STANDALONE_TEST)
    using Fn = const void *(*)(uint32_t);
    auto fn = reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, "pgrazor_unity_scheduler_api"));
    if (fn != nullptr) {
        const void *p = fn(2);
        if (p == nullptr) {
            p = fn(3);
        }
        return static_cast<const PgrUnitySchedulerApi *>(p);
    }
#endif
    return nullptr;
}

static const PgrSharedHostApi *resolve_shared_host_api() {
    if (pgrazor_shared_host_api != nullptr) {
        return static_cast<const PgrSharedHostApi *>(pgrazor_shared_host_api(1));
    }
#if !defined(_WIN32) && defined(PGR_INTERACTION_HOST_STANDALONE_TEST)
    using Fn = const void *(*)(uint32_t);
    auto fn = reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, "pgrazor_shared_host_api"));
    if (fn != nullptr) {
        return static_cast<const PgrSharedHostApi *>(fn(1));
    }
#endif
    return nullptr;
}

static const PgrSpinHostApi *resolve_spin_host_api() {
    if (pgrazor_spin_host_api != nullptr) {
        return static_cast<const PgrSpinHostApi *>(pgrazor_spin_host_api(1));
    }
#if !defined(_WIN32) && defined(PGR_INTERACTION_HOST_STANDALONE_TEST)
    using Fn = const void *(*)(uint32_t);
    auto fn = reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, "pgrazor_spin_host_api"));
    if (fn != nullptr) {
        return static_cast<const PgrSpinHostApi *>(fn(1));
    }
#endif
    return nullptr;
}

static const PgrObsApi *resolve_catch_observers_api() {
    if (pgrazor_catch_observers_api != nullptr) {
        return static_cast<const PgrObsApi *>(pgrazor_catch_observers_api(PGR_OBS_API_VERSION));
    }
#if !defined(_WIN32) && defined(PGR_INTERACTION_HOST_STANDALONE_TEST)
    using Fn = const void *(*)(uint32_t);
    auto fn = reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, "pgrazor_catch_observers_api"));
    if (fn != nullptr) {
        return static_cast<const PgrObsApi *>(fn(PGR_OBS_API_VERSION));
    }
#endif
    return nullptr;
}

static int call_pgrazor_hook(void *target, void *replacement, void **original) {
    if (pgrazor_hook != nullptr) {
        return pgrazor_hook(target, replacement, original);
    }
#if !defined(_WIN32) && defined(PGR_INTERACTION_HOST_STANDALONE_TEST)
    using Fn = int (*)(void *, void *, void **);
    auto fn = reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, "pgrazor_hook"));
    if (fn != nullptr) {
        return fn(target, replacement, original);
    }
#endif
    return -1;
}

static int call_pgrazor_unhook(void *target) {
    if (pgrazor_unhook != nullptr) {
        return pgrazor_unhook(target);
    }
#if !defined(_WIN32) && defined(PGR_INTERACTION_HOST_STANDALONE_TEST)
    using Fn = int (*)(void *);
    auto fn = reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, "pgrazor_unhook"));
    if (fn != nullptr) {
        return fn(target);
    }
#endif
    return -1;
}

/* Exact 20-word (80-byte) prologue for il2cpp_debug_get_method_info */
static const uint32_t kDebugWords[PGR_IH_DEBUG_PROLOGUE_WORDS] = {
    0x90051008u, 0xf9474508u, 0xb4000148u, 0x90051009u, 0xf9474929u,
    0xb40000e9u, 0xf940040au, 0xeb0a011fu, 0x2a1f03e8u, 0x54000128u,
    0xeb0a013fu, 0x540000e3u, 0xb40000a1u, 0xf9400408u, 0xb900083fu,
    0xf900083fu, 0xf9000028u, 0x52800028u, 0x2a0803e0u, 0xd65f03c0u,
};

/* Exact 8-word (32-byte) prologue for Pokeball::btxu at RVA 0xad58170 */
static const uint32_t kBtxuWords[PGR_IH_BTXU_PROLOGUE_WORDS] = {
    0xf81d0ffeu, 0xa90157f6u, 0xa9024ff4u, 0xf0021bb4u,
    0xaa0003f3u, 0x39628688u, 0x37000428u, 0xd001efe0u,
};

struct ClassDesc {
    const char *ns;
    const char *name;
    const char *full_name;
    bool exact_lookup_only;
};

static const ClassDesc kClasses[PGR_IH_CLASS_COUNT] = {
    {"Niantic.Holoholo.Encounter", "EncounterCaptureState",
     "Niantic.Holoholo.Encounter.EncounterCaptureState", false},
    {"Niantic.Holoholo.Pokestop", "PokestopState",
     "Niantic.Holoholo.Pokestop.PokestopState", false},
    {"Niantic.Holoholo.Pokestop", "PokestopSpinnerInteractionState",
     "Niantic.Holoholo.Pokestop.PokestopSpinnerInteractionState", false},
    {"Niantic.Holoholo.Pokestop", "PokestopResolveState",
     "Niantic.Holoholo.Pokestop.PokestopResolveState", false},
    {"Niantic.Holoholo.Pokestop", "PokestopInteractionState",
     "Niantic.Holoholo.Pokestop.PokestopInteractionState", false},
    {"Holoholo.Rpc", "CatchPokemonOutProto",
     "Holoholo.Rpc.CatchPokemonOutProto", false},
    {"Holoholo.Rpc", "FortSearchOutProto",
     "Holoholo.Rpc.FortSearchOutProto", false},
    {"Niantic.Holoholo.Encounter", "EncounterResult",
     "Niantic.Holoholo.Encounter.EncounterResult", false},
    {"Niantic.Holoholo.Encounter", "EncounterSummaryGuiController",
     "Niantic.Holoholo.Encounter.EncounterSummaryGuiController", false},
    {"Niantic.Holoholo.Encounter", "EncounterSummaryState",
     "Niantic.Holoholo.Encounter.EncounterSummaryState", false},
    {"Niantic.Holoholo.Encounter", "Pokeball",
     "Niantic.Holoholo.Encounter.Pokeball", true},
};

struct TargetDesc {
    uint32_t id;
    uint32_t class_index;
    const char *class_full_name;
    const char *method_name;
    const char *return_type;
    uint32_t param_count;
    const char *param0_type;
    bool optional_target;
    bool read_return;
    int32_t result_spec_index; /* -1 = timing-only, 0 = CatchPokemonOutProto.Status, 1 = FortSearchOutProto.Result */
    uintptr_t expected_rva;
    const uint32_t *expected_words;
    uint32_t expected_byte_len;
};

static const TargetDesc kTargets[PGR_IH_TOTAL_TARGETS] = {
    /* 14 Required Targets (0..13) */
    {0, 0, "Niantic.Holoholo.Encounter.EncounterCaptureState", "EnterState",
     "System.Void", 0, nullptr, false, false, -1, 0, nullptr, 0},
    {1, 0, "Niantic.Holoholo.Encounter.EncounterCaptureState", "ExitState",
     "System.Void", 0, nullptr, false, false, -1, 0, nullptr, 0},
    {2, 0, "Niantic.Holoholo.Encounter.EncounterCaptureState", "SummaryDismissed",
     "System.Void", 1, "Holoholo.Rpc.CatchPokemonOutProto", false, false, 0, 0, nullptr, 0},
    {3, 1, "Niantic.Holoholo.Pokestop.PokestopState", "NotifyReceivedItems",
     "System.Void", 0, nullptr, false, false, -1, 0, nullptr, 0},
    {4, 1, "Niantic.Holoholo.Pokestop.PokestopState", "NotifyPokestopSpin",
     "System.Void", 0, nullptr, false, false, -1, 0, nullptr, 0},
    {5, 1, "Niantic.Holoholo.Pokestop.PokestopState", "ResolveComplete",
     "System.Void", 0, nullptr, false, false, -1, 0, nullptr, 0},
    {6, 2, "Niantic.Holoholo.Pokestop.PokestopSpinnerInteractionState", "EnterState",
     "System.Void", 0, nullptr, false, false, -1, 0, nullptr, 0},
    {7, 2, "Niantic.Holoholo.Pokestop.PokestopSpinnerInteractionState", "ExitState",
     "System.Void", 0, nullptr, false, false, -1, 0, nullptr, 0},
    {8, 3, "Niantic.Holoholo.Pokestop.PokestopResolveState", "EnterState",
     "System.Void", 0, nullptr, false, false, -1, 0, nullptr, 0},
    {9, 3, "Niantic.Holoholo.Pokestop.PokestopResolveState", "ExitState",
     "System.Void", 0, nullptr, false, false, -1, 0, nullptr, 0},
    {10, 0, "Niantic.Holoholo.Encounter.EncounterCaptureState", "btdd",
     "System.Void", 1, "Holoholo.Rpc.CatchPokemonOutProto", false, false, 0, 0, nullptr, 0},
    {11, 0, "Niantic.Holoholo.Encounter.EncounterCaptureState", "btdf",
     "System.Void", 1, "Holoholo.Rpc.CatchPokemonOutProto", false, false, -1, 0, nullptr, 0},
    {12, 0, "Niantic.Holoholo.Encounter.EncounterCaptureState", "btdi",
     "System.Void", 1, "Holoholo.Rpc.CatchPokemonOutProto", false, false, -1, 0, nullptr, 0},
    {13, 2, "Niantic.Holoholo.Pokestop.PokestopSpinnerInteractionState", "bgff",
     "System.Void", 1, "Holoholo.Rpc.FortSearchOutProto", false, false, 1, 0, nullptr, 0},

    /* 6 Optional Targets (14..19) */
    {14, 8, "Niantic.Holoholo.Encounter.EncounterSummaryGuiController", "HandleOpen",
     "System.Void", 0, nullptr, true, false, -1, 0, nullptr, 0},
    {15, 8, "Niantic.Holoholo.Encounter.EncounterSummaryGuiController", "HandleOpenComplete",
     "System.Void", 0, nullptr, true, false, -1, 0, nullptr, 0},
    {16, 8, "Niantic.Holoholo.Encounter.EncounterSummaryGuiController", "HandleDismiss",
     "System.Void", 0, nullptr, true, false, -1, 0, nullptr, 0},
    {17, 8, "Niantic.Holoholo.Encounter.EncounterSummaryGuiController", "HandleDismissComplete",
     "System.Void", 0, nullptr, true, false, -1, 0, nullptr, 0},
    {18, 9, "Niantic.Holoholo.Encounter.EncounterSummaryState", "ExitState",
     "System.Void", 0, nullptr, true, false, -1, 0, nullptr, 0},
    {19, 10, "Niantic.Holoholo.Encounter.Pokeball", "btxu",
     "Holoholo.Rpc.CatchPokemonOutProto", 0, nullptr, true, true, 0,
     PGR_IH_BTXU_RVA, kBtxuWords, PGR_IH_BTXU_PROLOGUE_BYTES},
};

struct ResultReaderSpecDesc {
    uint32_t class_index;
    const char *class_full_name;
    const char *field_name;
    const char *field_type;
    int32_t expected_size;
    int32_t expected_align;
};

static const ResultReaderSpecDesc kResultSpecs[PGR_IH_RESULT_READER_SPECS] = {
    {5, "Holoholo.Rpc.CatchPokemonOutProto", "Status",
     "Holoholo.Rpc.CatchPokemonOutProto.Types.Status", 4, 4},
    {6, "Holoholo.Rpc.FortSearchOutProto", "Result",
     "Holoholo.Rpc.FortSearchOutProto.Types.Result", 4, 4},
};

struct EnumLiteralEntry {
    int32_t code;
    char name[64];
};

struct ResultReaderState {
    bool valid;
    void *klass;
    int32_t instance_size;
    int32_t offset;
    char field_name[32];
    char enum_type[96];
    char value_type[32];
    uint32_t literal_count;
    EnumLiteralEntry literals[PGR_IH_LIMIT_ENUM_LITERALS];
    char error_reason[96];
};

struct ResolvedClassState {
    bool found;
    void *klass;
    void *image;
    void *assembly;
    char image_name[128];
    char parent_name[128];
    int32_t flags;
    bool value_type;
    bool is_enum;
    int32_t instance_size;
    int32_t align;
    uint32_t field_count;
    uint32_t method_count;
};

struct ResolvedTargetState {
    bool resolved;
    bool ambiguous;
    uint32_t match_count;
    void *klass;
    void *method;
    uintptr_t code;
    uintptr_t rva;
    uint32_t flags;
    uint32_t iflags;
    char unavail_reason[96];
};

struct InFlightSlot {
    bool active;
    uint32_t generation;
    uint32_t target_id;
    uint32_t call_id;
    uint32_t depth;
    uint32_t reserved_events;
    int32_t tid;
    int64_t enter_ts_ms;
    int64_t enter_elapsed_ms;
};

enum {
    kEventQueueCapacity = 2048,
    kMaxInFlightSlots = 64,
};

using FnDomainGet = void *(*)();
using FnDomainGetAssemblies = void **(*)(void *, size_t *);
using FnAssemblyGetImage = void *(*)(void *);
using FnImageGetName = const char *(*)(const void *);
using FnClassFromName = void *(*)(void *, const char *, const char *);
using FnClassGetType = void *(*)(void *);
using FnClassGetName = const char *(*)(void *);
using FnClassGetNamespace = const char *(*)(void *);
using FnClassGetParent = void *(*)(void *);
using FnClassGetFlags = int32_t (*)(void *);
using FnClassIsValuetype = bool (*)(void *);
using FnClassIsEnum = bool (*)(void *);
using FnClassInstanceSize = int32_t (*)(void *);
using FnClassValueSize = int (*)(void *, int *);
using FnClassFromType = void *(*)(const void *);
using FnClassEnumBasetype = const void *(*)(void *);
using FnClassGetFields = void *(*)(void *, void **);
using FnFieldGetName = const char *(*)(void *);
using FnFieldGetType = const void *(*)(void *);
using FnFieldGetFlags = int (*)(void *);
using FnFieldGetOffset = size_t (*)(void *);
using FnFieldStaticGetValue = void (*)(void *, void *);
using FnClassGetMethods = void *(*)(void *, void **);
using FnClassGetMethodFromName = void *(*)(void *, const char *, int);
using FnMethodGetName = const char *(*)(void *);
using FnMethodGetReturnType = const void *(*)(void *);
using FnMethodGetParamCount = uint32_t (*)(void *);
using FnMethodGetParam = const void *(*)(void *, uint32_t);
using FnMethodGetParamName = const char *(*)(void *, uint32_t);
using FnMethodGetFlags = uint32_t (*)(void *, uint32_t *);
using FnTypeGetName = char *(*)(const void *);
using FnMemFree = void (*)(void *);
using FnDebugGetMethodInfo = bool (*)(void *, void *);
using FnThreadCurrent = void *(*)();
using FnThreadAttach = void *(*)(void *);
using FnThreadDetach = void (*)(void *);
using FnGcHandleNew = uintptr_t (*)(void *, int);
using FnGcHandleGetTarget = void *(*)(uintptr_t);
using FnGcHandleFree = void (*)(uintptr_t);
using FnNowMs = int64_t (*)();

using HookVoid0Fn = void (*)(void *, const void *);
using HookVoid1Fn = void (*)(void *, void *, const void *);
using HookRet0Fn = void *(*)(void *, const void *);

struct HostState {
    bool configured;
    PgrMetaExports meta_exports;
    PgrInteractionHostConfig cfg;

    /* Scheduled metadata check state */
    bool scheduled;
    int64_t schedule_due_ms;

    /* Active run state */
    uint32_t status;
    bool running;
    bool stopping;
    bool deferred_cleanup_pending;
    PgrInteractionHostOptions opts;
    int64_t prepare_started_ms;
    int64_t next_retry_ms;
    int64_t observe_started_ms;
    int64_t observe_deadline_ms;
    int64_t last_poll_ms;
    uint32_t attempt_count;

    /* Shared hook ownership & integration masks */
    uint32_t shared_owned_mask;
    uint32_t shared_integrated_mask;
    uint32_t effective_owned_mask;
    uint32_t blocked_shared_mask;
    uint32_t hooked_direct_mask;
    uint32_t installed_target_mask;

    /* Metadata scan counters & states */
    bool truncated;
    uint32_t assemblies_total;
    uint32_t assemblies_scanned;
    uint32_t lookups_used;
    uint32_t classes_resolved;
    uint32_t fields_emitted;
    uint32_t methods_emitted;
    uint32_t resolved_required_mask;
    uint32_t resolved_optional_mask;
    uint32_t ambiguous_target_mask;
    uintptr_t debug_rva;
    ResolvedClassState classes[PGR_IH_CLASS_COUNT];
    ResolvedTargetState targets[PGR_IH_TOTAL_TARGETS];
    ResultReaderState result_specs[PGR_IH_RESULT_READER_SPECS];

    /* Runtime observation state */
    bool argument_readers_available;
    bool return_reader_available;
    char arg_reader_reason[96];
    char return_reader_reason[96];
    uint32_t hit_count;
    uint32_t event_count;
    uint32_t reserved_events;
    uint32_t next_call_id;
    uint32_t thread_count;
    uint32_t dropped_threads;
    uint32_t in_flight;
    uint32_t btxu_pending_polls;
    int32_t threads[PGR_IH_LIMIT_THREADS];
    uint32_t thread_depths[PGR_IH_LIMIT_THREADS];
    uint32_t target_hits[PGR_IH_TOTAL_TARGETS];
    uint32_t target_pending_polls[PGR_IH_TOTAL_TARGETS];
    uint32_t slot_generation;
    InFlightSlot slots[kMaxInFlightSlots];

    /* Direct Dobby hook originals */
    void *orig_targets[PGR_IH_TOTAL_TARGETS];

    /* Ring buffer of emitted events */
    uint64_t next_seq;
    uint32_t queue_head;
    uint32_t queue_count;
    uint32_t total_emitted_events;
    PgrInteractionHostEvent queue[kEventQueueCapacity];

    char reason[96];
    char last_diagnostic[160];
};

static HostState g_state;
// Hook code and trampolines live for the process lifetime, independently of a
// bounded diagnostic session. Dobby publishes the original before patch commit.
static void *g_direct_originals[PGR_IH_TOTAL_TARGETS]{};
static uintptr_t g_direct_codes[PGR_IH_TOTAL_TARGETS]{};
static uint32_t g_direct_installed=0,g_direct_failed=0;
static int32_t g_owner_tid=0;
static std::atomic<uintptr_t> g_deferred_btxu_root{0};
static std::atomic_flag g_btxu_busy=ATOMIC_FLAG_INIT;
static bool g_transport_failed=false;
static PgrInteractionHostEvent g_transport_event{};

#if !defined(_WIN32)
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
struct ScopedLock {
    ScopedLock() { pthread_mutex_lock(&g_mu); }
    ~ScopedLock() { pthread_mutex_unlock(&g_mu); }
};
#else
struct ScopedLock {
    ScopedLock() = default;
    ~ScopedLock() = default;
};
#endif

static int64_t default_now_ms() {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 +
           static_cast<int64_t>(ts.tv_nsec / 1000000);
}

static int64_t now_ms() {
    if (g_state.cfg.now_ms_fn != nullptr) {
        auto fn = reinterpret_cast<FnNowMs>(g_state.cfg.now_ms_fn);
        return fn();
    }
    return default_now_ms();
}

static int32_t current_thread_id() {
#if defined(__linux__)
    return static_cast<int32_t>(syscall(SYS_gettid));
#elif !defined(_WIN32)
    return static_cast<int32_t>(reinterpret_cast<uintptr_t>(pthread_self()) & 0x7fffffffu);
#else
    return 1;
#endif
}

static void copy_str(char *dst, size_t cap, const char *src) {
    if (cap == 0) {
        return;
    }
    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }
    size_t n = 0;
    while (n + 1 < cap && src[n] != '\0') {
        dst[n] = src[n];
        ++n;
    }
    dst[n] = '\0';
}

static bool str_eq(const char *a, const char *b) {
    if (a == nullptr || b == nullptr) {
        return a == b;
    }
    return strcmp(a, b) == 0;
}

static void read_safe_string(const char *ptr, char *out, size_t out_cap) {
    if (out_cap == 0) {
        return;
    }
    if (ptr == nullptr) {
        out[0] = '\0';
        return;
    }
    size_t limit = PGR_IH_LIMIT_NAME_BYTES;
    if (out_cap - 1 < limit) {
        limit = out_cap - 1;
    }
    size_t i = 0;
    for (; i < limit; ++i) {
        char c = ptr[i];
        if (c == '\0') {
            break;
        }
        out[i] = c;
    }
    out[i] = '\0';
}

static const char *status_to_string(uint32_t status) {
    switch (status) {
        case PGR_IH_STATUS_IDLE:
            return "idle";
        case PGR_IH_STATUS_SCHEDULED:
            return "scheduled";
        case PGR_IH_STATUS_PREPARING:
            return "preparing";
        case PGR_IH_STATUS_OBSERVING:
            return "observing";
        case PGR_IH_STATUS_COMPLETE:
            return "complete";
        case PGR_IH_STATUS_SKIPPED:
            return "skipped";
        case PGR_IH_STATUS_STOPPED:
            return "stopped";
        case PGR_IH_STATUS_FAILED:
        default:
            return "failed";
    }
}

static void emit_transport_diagnostic(const char *text) {
    copy_str(g_state.last_diagnostic, sizeof(g_state.last_diagnostic), text);
    const PgrRuntimeTransportApi *rt = resolve_transport_api();
    if (rt != nullptr && rt->emit != nullptr && text != nullptr) {
        PgrRuntimeTransportString s {text, static_cast<uint32_t>(strlen(text))};
        int rc=rt->emit(PGR_IH_RT_ACT_DIAGNOSTIC, nullptr, 0, &s, 1);
        if(rc!=PGR_TRANSPORT_OK&&rc!=PGR_TRANSPORT_ERR_NOT_INSTALLED)g_transport_failed=true;
    }
}

static void emit_transport_running(bool running) {
    const PgrRuntimeTransportApi *rt = resolve_transport_api();
    if (rt != nullptr && rt->emit != nullptr) {
        int64_t val = running ? 1 : 0;
        int rc=rt->emit(PGR_IH_RT_ACT_SET_RUNNING, &val, 1, nullptr, 0);
        if(rc!=PGR_TRANSPORT_OK&&rc!=PGR_TRANSPORT_ERR_NOT_INSTALLED)g_transport_failed=true;
    }
}

static PgrInteractionHostEvent *alloc_event_slot(uint32_t type, const char *channel) {
    PgrRuntimeTransportSample sample{};const auto *rt=resolve_transport_api();
    const bool transported=rt&&rt->sample&&rt->sample(&sample)==PGR_TRANSPORT_OK;
    uint32_t idx = (g_state.queue_head + g_state.queue_count) % kEventQueueCapacity;
    if (!transported&&g_state.queue_count == kEventQueueCapacity) {
        g_state.queue_head = (g_state.queue_head + 1) % kEventQueueCapacity;
        g_state.truncated=true;
    } else if(!transported) {
        ++g_state.queue_count;
    }
    PgrInteractionHostEvent *ev = transported?&g_transport_event:&g_state.queue[idx];
    memset(ev, 0, sizeof(*ev));
    ev->seq = ++g_state.next_seq;
    ev->ts_ms = now_ms();
    ev->elapsed_ms = g_state.observe_started_ms > 0 ? (ev->ts_ms - g_state.observe_started_ms) : 0;
    ev->duration_ms = -1;
    ev->type = type;
    ev->target_id = 0xffffffffu;
    copy_str(ev->channel, sizeof(ev->channel), channel);
    ++g_state.total_emitted_events;
    return ev;
}

static void publish_event(PgrInteractionHostEvent *ev, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ev->json, sizeof(ev->json), fmt, ap);
    va_end(ap);

    const PgrRuntimeTransportApi *rt = resolve_transport_api();
    if (rt != nullptr && rt->emit != nullptr) {
        PgrRuntimeTransportString strs[2] = {
            {ev->channel, static_cast<uint32_t>(strlen(ev->channel))},
            {ev->json, static_cast<uint32_t>(strlen(ev->json))},
        };
        int rc=rt->emit(PGR_IH_RT_ACT_EVENT, nullptr, 0, strs, 2);
        if(rc!=PGR_TRANSPORT_OK&&rc!=PGR_TRANSPORT_ERR_NOT_INSTALLED)g_transport_failed=true;
    }
}

/* Preparation is driven only by an already-attached proven owner. Never attach
 * a background diagnostics caller to the managed runtime. */
struct ScopedIl2CppThread {
    ScopedIl2CppThread(const PgrInteractionExtraExports &,void *){}
};

/* Scoped GC pin handle helper */
struct ScopedGcPin {
    FnGcHandleGetTarget get_fn;
    FnGcHandleFree free_fn;
    uintptr_t handle;

    ScopedGcPin(const PgrInteractionExtraExports &extra, void *obj)
        : get_fn(reinterpret_cast<FnGcHandleGetTarget>(extra.gchandle_get_target)),
          free_fn(reinterpret_cast<FnGcHandleFree>(extra.gchandle_free)),
          handle(0) {
        if (obj != nullptr && extra.gchandle_new != nullptr && get_fn != nullptr &&
            free_fn != nullptr) {
            auto new_fn = reinterpret_cast<FnGcHandleNew>(extra.gchandle_new);
            handle = new_fn(obj, 1);
        }
    }

    void *target(void *fallback) const {
        if (handle != 0 && get_fn != nullptr) {
            void *resolved = get_fn(handle);
            if (resolved != nullptr) {
                return resolved;
            }
        }
        return fallback;
    }

    ~ScopedGcPin() {
        reset();
    }
    void reset(){
        if (handle != 0 && free_fn != nullptr) {
            free_fn(handle);
            handle = 0;
        }
    }
};

static uint64_t observe_enter_internal(uint32_t target_id, void *receiver, void *arg0);
static void observe_leave_internal(uint64_t token, void *retval, bool aborted=false);
static void finish_observation_locked(uint32_t final_status, const char *reason);

static void unhook_direct_targets_locked() {
    // Retire observation only; a callback may still be returning through its
    // trampoline even after its event token has been consumed.
    g_state.hooked_direct_mask = g_direct_installed;
    g_state.installed_target_mask &= ~PGR_IH_ALL_TARGETS_MASK;
    g_state.deferred_cleanup_pending = false;
}

/* 20 dedicated C-ABI replacement thunks for direct Dobby hooks (when not owned by 49/57/61/67) */
#define DEFINE_VOID0_HOOK(IDX)                                                      \
    static void hook_target_##IDX(void *self, const void *method) {                 \
        auto orig = reinterpret_cast<HookVoid0Fn>(__atomic_load_n(&g_direct_originals[IDX],__ATOMIC_ACQUIRE)); \
        if(!orig)__builtin_trap();                                                  \
        pgr_interaction_owner::Phase phase(IDX);                                    \
        pgr_interaction_owner::Pins pins(phase.wanted(),g_state.cfg.extra.gchandle_new,g_state.cfg.extra.gchandle_get_target,g_state.cfg.extra.gchandle_free,self); \
        pgr_interaction_owner::AbortBeforeRoots cleanup{phase};                     \
        if(pins.valid)phase.enter(pins.at(0));                                      \
        orig(pins.at(0),method);phase.leave();                                      \
    }

#define DEFINE_VOID1_HOOK(IDX)                                                      \
    static void hook_target_##IDX(void *self, void *arg0, const void *method) {     \
        auto orig = reinterpret_cast<HookVoid1Fn>(__atomic_load_n(&g_direct_originals[IDX],__ATOMIC_ACQUIRE)); \
        if(!orig)__builtin_trap();                                                  \
        pgr_interaction_owner::Phase phase(IDX);                                    \
        pgr_interaction_owner::Pins pins(phase.wanted(),g_state.cfg.extra.gchandle_new,g_state.cfg.extra.gchandle_get_target,g_state.cfg.extra.gchandle_free,self,arg0); \
        pgr_interaction_owner::AbortBeforeRoots cleanup{phase};                     \
        if(pins.valid)phase.enter(pins.at(0),pins.at(1));                            \
        orig(pins.at(0),pins.at(1),method);phase.leave();                            \
    }

DEFINE_VOID0_HOOK(0)
DEFINE_VOID0_HOOK(1)
DEFINE_VOID1_HOOK(2)
DEFINE_VOID0_HOOK(3)
DEFINE_VOID0_HOOK(4)
DEFINE_VOID0_HOOK(5)
DEFINE_VOID0_HOOK(6)
DEFINE_VOID0_HOOK(7)
DEFINE_VOID0_HOOK(8)
DEFINE_VOID0_HOOK(9)
DEFINE_VOID1_HOOK(10)
DEFINE_VOID1_HOOK(11)
DEFINE_VOID1_HOOK(12)
DEFINE_VOID1_HOOK(13)
DEFINE_VOID0_HOOK(14)
DEFINE_VOID0_HOOK(15)
DEFINE_VOID0_HOOK(16)
DEFINE_VOID0_HOOK(17)
DEFINE_VOID0_HOOK(18)

static void *hook_target_19(void *self, const void *method) {
    auto orig=reinterpret_cast<HookRet0Fn>(__atomic_load_n(&g_direct_originals[19],__ATOMIC_ACQUIRE));if(!orig)__builtin_trap();
    pgr_interaction_owner::Phase phase(19);
    if(!phase.wanted()||g_deferred_btxu_root.load()||g_btxu_busy.test_and_set())return orig(self,method);
    struct Busy{~Busy(){g_btxu_busy.clear();}} busy;
    ScopedGcPin self_pin(g_state.cfg.extra,self);
    if(!self_pin.handle)return orig(self,method);
    pgr_interaction_owner::AbortBeforeRoots cleanup{phase};
    phase.enter(self_pin.target(self));
    void *result=orig(self_pin.target(self),method);
    if(!result){phase.leave(nullptr);return nullptr;}
    ScopedGcPin result_pin(g_state.cfg.extra,result);
    if(!result_pin.handle){phase.abort();g_deferred_btxu_root.store(self_pin.handle);self_pin.handle=0;return result;}
    phase.leave(result_pin.target(result));
    self_pin.reset(); // may relocate result; its root remains live
    void *current=result_pin.target(result);result_pin.reset();return current;
}

static void *const kHookReplacements[PGR_IH_TOTAL_TARGETS] = {
    reinterpret_cast<void *>(&hook_target_0),
    reinterpret_cast<void *>(&hook_target_1),
    reinterpret_cast<void *>(&hook_target_2),
    reinterpret_cast<void *>(&hook_target_3),
    reinterpret_cast<void *>(&hook_target_4),
    reinterpret_cast<void *>(&hook_target_5),
    reinterpret_cast<void *>(&hook_target_6),
    reinterpret_cast<void *>(&hook_target_7),
    reinterpret_cast<void *>(&hook_target_8),
    reinterpret_cast<void *>(&hook_target_9),
    reinterpret_cast<void *>(&hook_target_10),
    reinterpret_cast<void *>(&hook_target_11),
    reinterpret_cast<void *>(&hook_target_12),
    reinterpret_cast<void *>(&hook_target_13),
    reinterpret_cast<void *>(&hook_target_14),
    reinterpret_cast<void *>(&hook_target_15),
    reinterpret_cast<void *>(&hook_target_16),
    reinterpret_cast<void *>(&hook_target_17),
    reinterpret_cast<void *>(&hook_target_18),
    reinterpret_cast<void *>(&hook_target_19),
};

static bool check_elf64_aarch64(const void *base, uint64_t size) {
    if (base == nullptr || size < 20) {
        return false;
    }
    const auto *elf = static_cast<const uint8_t *>(base);
    return elf[0] == 0x7fu && elf[1] == 0x45u && elf[2] == 0x4cu && elf[3] == 0x46u &&
           elf[4] == 2u && elf[5] == 1u && elf[18] == 183u && elf[19] == 0u;
}

static void describe_type_name(const void *type_ptr, char *out, size_t out_cap) {
    if (out_cap == 0) {
        return;
    }
    out[0] = '\0';
    if (type_ptr == nullptr || g_state.meta_exports.type_get_name == nullptr) {
        return;
    }
    auto get_fn = reinterpret_cast<FnTypeGetName>(g_state.meta_exports.type_get_name);
    auto free_fn = reinterpret_cast<FnMemFree>(g_state.meta_exports.mem_free);
    char *raw = get_fn(type_ptr);
    if (raw == nullptr) {
        return;
    }
    read_safe_string(raw, out, out_cap);
    if (free_fn != nullptr) {
        free_fn(raw);
    }
}

static void describe_class_full(void *klass, char *out, size_t out_cap) {
    if (out_cap == 0) {
        return;
    }
    out[0] = '\0';
    if (klass == nullptr) {
        return;
    }
    if (g_state.meta_exports.class_get_type != nullptr &&
        g_state.meta_exports.type_get_name != nullptr) {
        auto ct_fn = reinterpret_cast<FnClassGetType>(g_state.meta_exports.class_get_type);
        const void *t = ct_fn(klass);
        if (t != nullptr) {
            describe_type_name(t, out, out_cap);
            if (out[0] != '\0') {
                return;
            }
        }
    }
    auto name_fn = reinterpret_cast<FnClassGetName>(g_state.cfg.extra.class_get_name);
    auto ns_fn = reinterpret_cast<FnClassGetNamespace>(g_state.cfg.extra.class_get_namespace);
    char ns[128] = {};
    char name[128] = {};
    if (ns_fn != nullptr) {
        read_safe_string(ns_fn(klass), ns, sizeof(ns));
    }
    if (name_fn != nullptr) {
        read_safe_string(name_fn(klass), name, sizeof(name));
    }
    if (ns[0] != '\0') {
        snprintf(out, out_cap, "%s.%s", ns, name);
    } else {
        copy_str(out, out_cap, name);
    }
}

enum InspectResultKind {
    kInspectReady = 0,
    kInspectRetry = 1,
    kInspectSkipped = 2,
};

static InspectResultKind run_inspect_locked(char *out_reason, size_t reason_cap) {
    const PgrMetaApi *meta = resolve_metadata_api();
    if (meta == nullptr) {
        copy_str(out_reason, reason_cap, "metadata-api-unavailable");
        return kInspectRetry;
    }

    auto domain_get = reinterpret_cast<FnDomainGet>(g_state.meta_exports.domain_get);
    auto domain_get_assemblies =
        reinterpret_cast<FnDomainGetAssemblies>(g_state.meta_exports.domain_get_assemblies);
    if (domain_get == nullptr || domain_get_assemblies == nullptr) {
        copy_str(out_reason, reason_cap, "domain-exports-missing");
        return kInspectRetry;
    }

    void *domain = domain_get();
    if (domain == nullptr) {
        copy_str(out_reason, reason_cap, "domain-unavailable");
        return kInspectRetry;
    }

    ScopedIl2CppThread thread_guard(g_state.cfg.extra, domain);

    size_t assembly_count = 0;
    void **assemblies = domain_get_assemblies(domain, &assembly_count);
    if (assemblies == nullptr || assembly_count == 0) {
        copy_str(out_reason, reason_cap, "assemblies-empty");
        return kInspectRetry;
    }

    if (!check_elf64_aarch64(g_state.cfg.binding.module_base, g_state.cfg.binding.module_size)) {
        copy_str(out_reason, reason_cap, "library-architecture-mismatch");
        return kInspectSkipped;
    }

    /* Validate il2cpp_debug_get_method_info against 80-byte AArch64 fingerprint via Feature 47 */
    const void *debug_addr = g_state.cfg.debug_code_address != nullptr
                                 ? g_state.cfg.debug_code_address
                                 : g_state.meta_exports.debug_get_method_info;
    PgrMetaCodeQuery dcq {};
    dcq.version = PGR_MD_QUERY_VERSION;
    dcq.size = sizeof(dcq);
    dcq.address = debug_addr;
    dcq.length = PGR_IH_DEBUG_PROLOGUE_BYTES;
    dcq.prologue = kDebugWords;
    dcq.prologue_len = PGR_IH_DEBUG_PROLOGUE_BYTES;
    int rc = meta->validate_code(&dcq);
    if (rc != PGR_MD_OK) {
        uintptr_t a = reinterpret_cast<uintptr_t>(debug_addr);
        if ((a & 0x3u) != 0) {
            copy_str(out_reason, reason_cap, "debug-get-method-info-unaligned");
        } else if (rc == PGR_MD_ERR_CODE_RANGE || rc == PGR_MD_ERR_SEGMENT) {
            copy_str(out_reason, reason_cap, "debug-get-method-info-not-in-module");
        } else {
            copy_str(out_reason, reason_cap, "debug-get-method-info-prologue-mismatch");
        }
        return kInspectRetry;
    }
    uintptr_t mod_base = reinterpret_cast<uintptr_t>(g_state.cfg.binding.module_base);
    g_state.debug_rva = reinterpret_cast<uintptr_t>(debug_addr) - mod_base;

    /* Reset per-attempt metadata counters */
    g_state.truncated = false;
    g_state.assemblies_total = static_cast<uint32_t>(assembly_count);
    g_state.assemblies_scanned = 0;
    g_state.lookups_used = 0;
    g_state.classes_resolved = 0;
    g_state.fields_emitted = 0;
    g_state.methods_emitted = 0;
    g_state.resolved_required_mask = 0;
    g_state.resolved_optional_mask = 0;
    g_state.ambiguous_target_mask = 0;
    memset(g_state.classes, 0, sizeof(g_state.classes));
    memset(g_state.targets, 0, sizeof(g_state.targets));

    {
        PgrInteractionHostEvent *ev =
            alloc_event_slot(PGR_IH_EVT_METADATA_READY, "interaction-probe");
        publish_event(ev,
                      "{\"type\":\"metadata-ready\",\"attempt\":%u,\"assemblyCount\":%u,"
                      "\"debugGetMethodInfoRva\":\"0x%" PRIxPTR "\"}",
                      g_state.attempt_count,
                      g_state.assemblies_total,
                      g_state.debug_rva);
    }

    int64_t scan_started_ms = now_ms();
    uint32_t max_assemblies = g_state.assemblies_total;
    if (max_assemblies > PGR_IH_LIMIT_ASSEMBLIES) {
        max_assemblies = PGR_IH_LIMIT_ASSEMBLIES;
        g_state.truncated = true;
        PgrInteractionHostEvent *ev = alloc_event_slot(PGR_IH_EVT_LIMIT, "interaction-probe");
        publish_event(ev,
                      "{\"type\":\"limit\",\"reason\":\"assembly-limit\",\"limit\":%d,\"total\":%u}",
                      PGR_IH_LIMIT_ASSEMBLIES,
                      g_state.assemblies_total);
    }

    auto asm_get_image =
        reinterpret_cast<FnAssemblyGetImage>(g_state.meta_exports.assembly_get_image);
    auto img_get_name = reinterpret_cast<FnImageGetName>(g_state.cfg.extra.image_get_name);
    auto class_from_name =
        reinterpret_cast<FnClassFromName>(g_state.meta_exports.class_from_name);
    auto class_get_parent =
        reinterpret_cast<FnClassGetParent>(g_state.cfg.extra.class_get_parent);
    auto class_get_flags = reinterpret_cast<FnClassGetFlags>(g_state.cfg.extra.class_get_flags);
    auto class_is_valuetype =
        reinterpret_cast<FnClassIsValuetype>(g_state.meta_exports.class_is_valuetype);
    auto class_is_enum = reinterpret_cast<FnClassIsEnum>(g_state.meta_exports.class_is_enum);
    auto class_instance_size =
        reinterpret_cast<FnClassInstanceSize>(g_state.cfg.extra.class_instance_size);
    auto class_value_size =
        reinterpret_cast<FnClassValueSize>(g_state.meta_exports.class_value_size);
    auto class_get_fields =
        reinterpret_cast<FnClassGetFields>(g_state.cfg.extra.class_get_fields);
    auto field_get_name = reinterpret_cast<FnFieldGetName>(g_state.cfg.extra.field_get_name);
    auto field_get_type = reinterpret_cast<FnFieldGetType>(g_state.meta_exports.field_get_type);
    auto field_get_flags =
        reinterpret_cast<FnFieldGetFlags>(g_state.meta_exports.field_get_flags);
    auto field_get_offset =
        reinterpret_cast<FnFieldGetOffset>(g_state.meta_exports.field_get_offset);
    auto class_get_methods =
        reinterpret_cast<FnClassGetMethods>(g_state.meta_exports.class_get_methods);
    auto method_get_name = reinterpret_cast<FnMethodGetName>(g_state.meta_exports.method_get_name);
    auto method_get_return_type =
        reinterpret_cast<FnMethodGetReturnType>(g_state.meta_exports.method_get_return_type);
    auto method_get_param_count =
        reinterpret_cast<FnMethodGetParamCount>(g_state.meta_exports.method_get_param_count);
    auto method_get_param =
        reinterpret_cast<FnMethodGetParam>(g_state.meta_exports.method_get_param);
    auto method_get_param_name =
        reinterpret_cast<FnMethodGetParamName>(g_state.cfg.extra.method_get_param_name);
    auto method_get_flags =
        reinterpret_cast<FnMethodGetFlags>(g_state.meta_exports.method_get_flags);
    auto debug_get_method_info =
        reinterpret_cast<FnDebugGetMethodInfo>(g_state.meta_exports.debug_get_method_info);

    for (uint32_t a = 0; a < max_assemblies; ++a) {
        if (now_ms() - scan_started_ms >= PGR_IH_LIMIT_SCAN_MS) {
            g_state.truncated = true;
            PgrInteractionHostEvent *ev = alloc_event_slot(PGR_IH_EVT_LIMIT, "interaction-probe");
            publish_event(ev, "{\"type\":\"limit\",\"reason\":\"scan-time-limit\",\"limit\":%d}",
                          PGR_IH_LIMIT_SCAN_MS);
            break;
        }
        if (g_state.classes_resolved == PGR_IH_CLASS_COUNT) {
            break;
        }
        void *assembly = assemblies[a];
        if (assembly == nullptr) {
            continue;
        }
        ++g_state.assemblies_scanned;
        void *image = asm_get_image != nullptr ? asm_get_image(assembly) : nullptr;
        if (image == nullptr) {
            continue;
        }
        char image_name[128] = {};
        if (img_get_name != nullptr) {
            read_safe_string(img_get_name(image), image_name, sizeof(image_name));
        }

        for (uint32_t c = 0; c < PGR_IH_CLASS_COUNT; ++c) {
            if (g_state.classes[c].found) {
                continue;
            }
            if (g_state.lookups_used >= PGR_IH_LIMIT_LOOKUPS) {
                g_state.truncated = true;
                PgrInteractionHostEvent *ev =
                    alloc_event_slot(PGR_IH_EVT_LIMIT, "interaction-probe");
                publish_event(ev, "{\"type\":\"limit\",\"reason\":\"lookup-limit\",\"limit\":%d}",
                              PGR_IH_LIMIT_LOOKUPS);
                break;
            }
            ++g_state.lookups_used;
            void *klass = class_from_name(image, kClasses[c].ns, kClasses[c].name);
            if (klass == nullptr) {
                continue;
            }

            ResolvedClassState &cs = g_state.classes[c];
            cs.found = true;
            cs.klass = klass;
            cs.image = image;
            cs.assembly = assembly;
            copy_str(cs.image_name, sizeof(cs.image_name), image_name);
            if (class_get_parent != nullptr) {
                void *parent = class_get_parent(klass);
                if (parent != nullptr) {
                    describe_class_full(parent, cs.parent_name, sizeof(cs.parent_name));
                }
            }
            cs.flags = class_get_flags != nullptr ? class_get_flags(klass) : 0;
            cs.value_type = class_is_valuetype != nullptr && class_is_valuetype(klass);
            cs.is_enum = class_is_enum != nullptr && class_is_enum(klass);
            cs.instance_size = class_instance_size != nullptr ? class_instance_size(klass) : 0;
            int align = 0;
            if ((cs.value_type || cs.is_enum) && class_value_size != nullptr) {
                class_value_size(klass, &align);
            }
            cs.align = align;
            ++g_state.classes_resolved;

            {
                PgrInteractionHostEvent *ev =
                    alloc_event_slot(PGR_IH_EVT_CLASS, "interaction-probe");
                publish_event(ev,
                              "{\"type\":\"class\",\"className\":\"%s\",\"image\":\"%s\","
                              "\"flags\":%d,\"valueType\":%s,\"enumType\":%s,"
                              "\"instanceSize\":%d,\"align\":%d}",
                              kClasses[c].full_name,
                              cs.image_name,
                              cs.flags,
                              cs.value_type ? "true" : "false",
                              cs.is_enum ? "true" : "false",
                              cs.instance_size,
                              cs.align);
            }

            /* Special case for Pokeball (class 10): exact btxu lookup via Feature 47 validate_method */
            if (kClasses[c].exact_lookup_only) {
                ResolvedTargetState &ts = g_state.targets[PGR_IH_TGT_POKEBALL_BTXU];
                ts.klass = klass;
                PgrMetaMethodQuery mq {};
                mq.version = PGR_MD_QUERY_VERSION;
                mq.size = sizeof(mq);
                mq.owner = klass;
                mq.name = "btxu";
                mq.return_type = "Holoholo.Rpc.CatchPokemonOutProto";
                mq.return_match = PGR_MD_MATCH_EXACT;
                mq.param_count = 0;
                mq.is_static = 0;
                mq.allow_inflated = 0;
                mq.expected_code = reinterpret_cast<const void *>(mod_base + PGR_IH_BTXU_RVA);
                mq.prologue = kBtxuWords;
                mq.prologue_len = PGR_IH_BTXU_PROLOGUE_BYTES;

                PgrMetaMethodResult mres {};
                int vrc = meta->validate_method(&mq, &mres);
                if (vrc != PGR_MD_OK) {
                    if (vrc == PGR_MD_ERR_NOT_FOUND) {
                        copy_str(ts.unavail_reason, sizeof(ts.unavail_reason), "method-not-found");
                    } else if (vrc == PGR_MD_ERR_TYPE) {
                        copy_str(ts.unavail_reason, sizeof(ts.unavail_reason), "return-type-mismatch");
                    } else if (vrc == PGR_MD_ERR_PROLOGUE) {
                        copy_str(ts.unavail_reason, sizeof(ts.unavail_reason), "prologue-mismatch");
                    } else if (vrc == PGR_MD_ERR_CODE_RANGE || vrc == PGR_MD_ERR_SEGMENT) {
                        copy_str(ts.unavail_reason, sizeof(ts.unavail_reason), "rva-or-code-mismatch");
                    } else {
                        copy_str(ts.unavail_reason, sizeof(ts.unavail_reason),
                                 "method-validation-failed");
                    }
                    continue;
                }
                ts.resolved = true;
                ts.match_count = 1;
                ts.method = const_cast<void *>(mres.method);
                ts.code = reinterpret_cast<uintptr_t>(mres.code);
                ts.rva = ts.code - mod_base;
                ts.flags = mres.flags;
                ts.iflags = mres.iflags;
                g_state.resolved_optional_mask |= (1u << PGR_IH_TGT_POKEBALL_BTXU);

                PgrInteractionHostEvent *mev =
                    alloc_event_slot(PGR_IH_EVT_METHOD, "interaction-probe");
                publish_event(mev,
                              "{\"type\":\"method\",\"className\":\"%s\",\"name\":\"btxu\","
                              "\"returnType\":\"Holoholo.Rpc.CatchPokemonOutProto\","
                              "\"rva\":\"0x%" PRIxPTR "\",\"source\":\"class_get_method_from_name\"}",
                              kClasses[c].full_name,
                              ts.rva);
                continue;
            }

            /* Scan fields for classes 0..9 */
            if (class_get_fields != nullptr && field_get_name != nullptr &&
                field_get_type != nullptr && field_get_flags != nullptr &&
                field_get_offset != nullptr) {
                void *f_iter = nullptr;
                while (true) {
                    if (now_ms() - scan_started_ms >= PGR_IH_LIMIT_SCAN_MS) {
                        g_state.truncated = true;
                        PgrInteractionHostEvent *ev =
                            alloc_event_slot(PGR_IH_EVT_LIMIT, "interaction-probe");
                        publish_event(ev,
                                      "{\"type\":\"limit\",\"reason\":\"scan-time-limit\",\"limit\":%d}",
                                      PGR_IH_LIMIT_SCAN_MS);
                        break;
                    }
                    if (cs.field_count >= PGR_IH_LIMIT_FIELDS_PER_CLASS ||
                        g_state.fields_emitted >= PGR_IH_LIMIT_FIELDS) {
                        g_state.truncated = true;
                        const char *lreason = cs.field_count >= PGR_IH_LIMIT_FIELDS_PER_CLASS
                                                  ? "class-field-limit"
                                                  : "total-field-limit";
                        PgrInteractionHostEvent *ev =
                            alloc_event_slot(PGR_IH_EVT_LIMIT, "interaction-probe");
                        publish_event(ev,
                                      "{\"type\":\"limit\",\"reason\":\"%s\",\"className\":\"%s\"}",
                                      lreason,
                                      kClasses[c].full_name);
                        break;
                    }
                    void *field = class_get_fields(klass, &f_iter);
                    if (field == nullptr) {
                        break;
                    }
                    ++cs.field_count;
                    ++g_state.fields_emitted;
                    char fname[128] = {};
                    char ftype[128] = {};
                    read_safe_string(field_get_name(field), fname, sizeof(fname));
                    describe_type_name(field_get_type(field), ftype, sizeof(ftype));
                    int32_t fflags = field_get_flags(field);
                    size_t foffset = field_get_offset(field);

                    PgrInteractionHostEvent *fev =
                        alloc_event_slot(PGR_IH_EVT_FIELD, "interaction-probe");
                    publish_event(fev,
                                  "{\"type\":\"field\",\"className\":\"%s\",\"index\":%u,"
                                  "\"name\":\"%s\",\"fieldType\":\"%s\",\"flags\":%d,\"offset\":%zu}",
                                  kClasses[c].full_name,
                                  cs.field_count - 1,
                                  fname,
                                  ftype,
                                  fflags,
                                  foffset);
                }
            }

            /* Scan methods for classes 0..9 */
            if (class_get_methods != nullptr && method_get_name != nullptr &&
                method_get_return_type != nullptr && method_get_param_count != nullptr &&
                method_get_param != nullptr) {
                void *m_iter = nullptr;
                while (true) {
                    if (now_ms() - scan_started_ms >= PGR_IH_LIMIT_SCAN_MS) {
                        g_state.truncated = true;
                        PgrInteractionHostEvent *ev =
                            alloc_event_slot(PGR_IH_EVT_LIMIT, "interaction-probe");
                        publish_event(ev,
                                      "{\"type\":\"limit\",\"reason\":\"scan-time-limit\",\"limit\":%d}",
                                      PGR_IH_LIMIT_SCAN_MS);
                        break;
                    }
                    if (cs.method_count >= PGR_IH_LIMIT_METHODS_PER_CLASS ||
                        g_state.methods_emitted >= PGR_IH_LIMIT_METHODS) {
                        g_state.truncated = true;
                        const char *lreason = cs.method_count >= PGR_IH_LIMIT_METHODS_PER_CLASS
                                                  ? "class-method-limit"
                                                  : "total-method-limit";
                        PgrInteractionHostEvent *ev =
                            alloc_event_slot(PGR_IH_EVT_LIMIT, "interaction-probe");
                        publish_event(ev,
                                      "{\"type\":\"limit\",\"reason\":\"%s\",\"className\":\"%s\"}",
                                      lreason,
                                      kClasses[c].full_name);
                        break;
                    }
                    void *method = class_get_methods(klass, &m_iter);
                    if (method == nullptr) {
                        break;
                    }
                    ++cs.method_count;
                    ++g_state.methods_emitted;

                    char mname[128] = {};
                    char rtype[128] = {};
                    read_safe_string(method_get_name(method), mname, sizeof(mname));
                    describe_type_name(method_get_return_type(method), rtype, sizeof(rtype));
                    uint32_t pcount = method_get_param_count(method);
                    uint32_t mflags = 0;
                    uint32_t miflags = 0;
                    if (method_get_flags != nullptr) {
                        mflags = method_get_flags(method, &miflags);
                    }

                    uint32_t pmax = pcount;
                    if (pmax > PGR_IH_LIMIT_PARAMETERS) {
                        pmax = PGR_IH_LIMIT_PARAMETERS;
                        g_state.truncated = true;
                        PgrInteractionHostEvent *ev =
                            alloc_event_slot(PGR_IH_EVT_LIMIT, "interaction-probe");
                        publish_event(ev,
                                      "{\"type\":\"limit\",\"reason\":\"parameter-limit\","
                                      "\"className\":\"%s\",\"method\":\"%s\",\"total\":%u}",
                                      kClasses[c].full_name,
                                      mname,
                                      pcount);
                    }

                    char param0_type[128] = {};
                    for (uint32_t p = 0; p < pmax; ++p) {
                        char ptype[128] = {};
                        describe_type_name(method_get_param(method, p), ptype, sizeof(ptype));
                        if (p == 0) {
                            copy_str(param0_type, sizeof(param0_type), ptype);
                        }
                        if (method_get_param_name != nullptr) {
                            char pname[64] = {};
                            read_safe_string(method_get_param_name(method, p), pname, sizeof(pname));
                        }
                    }

                    /* Obtain compiled code pointer via debug_get_method_info */
                    void *dbg_info[3] = {nullptr, nullptr, nullptr};
                    if (debug_get_method_info != nullptr) {
                        debug_get_method_info(method, dbg_info);
                    }
                    const void *code_ptr = dbg_info[0];
                    uintptr_t code_addr = reinterpret_cast<uintptr_t>(code_ptr);
                    uintptr_t m_rva = 0;

                    if (code_ptr != nullptr) {
                        PgrMetaCodeQuery cq {};
                        cq.version = PGR_MD_QUERY_VERSION;
                        cq.size = sizeof(cq);
                        cq.address = code_ptr;
                        cq.length = 4;
                        if (meta->validate_code(&cq) == PGR_MD_OK) {
                            m_rva = code_addr - mod_base;
                            /* Match against targets 0..18 belonging to class c */
                            for (uint32_t t = 0; t < PGR_IH_TOTAL_TARGETS - 1; ++t) {
                                const TargetDesc &td = kTargets[t];
                                if (td.class_index != c) {
                                    continue;
                                }
                                if (!str_eq(mname, td.method_name) ||
                                    !str_eq(rtype, td.return_type) ||
                                    pcount != td.param_count) {
                                    continue;
                                }
                                if (td.param_count == 1 && !str_eq(param0_type, td.param0_type)) {
                                    continue;
                                }
                                if ((mflags & 0x0010u) != 0) {
                                    continue;
                                }

                                ResolvedTargetState &ts = g_state.targets[t];
                                ++ts.match_count;
                                if (ts.match_count == 1) {
                                    ts.resolved = true;
                                    ts.klass = klass;
                                    ts.method = method;
                                    ts.code = code_addr;
                                    ts.rva = m_rva;
                                    ts.flags = mflags;
                                    ts.iflags = miflags;
                                    if (td.optional_target) {
                                        g_state.resolved_optional_mask |= (1u << t);
                                    } else {
                                        g_state.resolved_required_mask |= (1u << t);
                                    }
                                } else {
                                    ts.ambiguous = true;
                                    ts.resolved = false;
                                    copy_str(ts.unavail_reason, sizeof(ts.unavail_reason),
                                             "ambiguous-method-overload");
                                    g_state.ambiguous_target_mask |= (1u << t);
                                    if (td.optional_target) {
                                        g_state.resolved_optional_mask &= ~(1u << t);
                                    } else {
                                        g_state.resolved_required_mask &= ~(1u << t);
                                    }
                                }
                            }
                        }
                    }

                    PgrInteractionHostEvent *mev =
                        alloc_event_slot(PGR_IH_EVT_METHOD, "interaction-probe");
                    publish_event(mev,
                                  "{\"type\":\"method\",\"className\":\"%s\",\"index\":%u,"
                                  "\"name\":\"%s\",\"returnType\":\"%s\",\"parameterCount\":%u,"
                                  "\"rva\":\"0x%" PRIxPTR "\"}",
                                  kClasses[c].full_name,
                                  cs.method_count - 1,
                                  mname,
                                  rtype,
                                  pcount,
                                  m_rva);
                }
            }
        }
        if (g_state.lookups_used >= PGR_IH_LIMIT_LOOKUPS) {
            break;
        }
    }

    uint32_t resolved_req_count = 0;
    for (uint32_t t = 0; t < PGR_IH_REQUIRED_TARGETS; ++t) {
        if ((g_state.resolved_required_mask & (1u << t)) != 0) {
            ++resolved_req_count;
        }
    }
    uint32_t resolved_opt_count = 0;
    for (uint32_t t = PGR_IH_REQUIRED_TARGETS; t < PGR_IH_TOTAL_TARGETS; ++t) {
        if ((g_state.resolved_optional_mask & (1u << t)) != 0) {
            ++resolved_opt_count;
        }
    }

    {
        PgrInteractionHostEvent *ev =
            alloc_event_slot(PGR_IH_EVT_METADATA_SUMMARY, "interaction-probe");
        publish_event(ev,
                      "{\"type\":\"metadata-summary\",\"attempt\":%u,\"assembliesScanned\":%u,"
                      "\"lookupsUsed\":%u,\"resolvedClasses\":%u,\"resolvedTargets\":%u,"
                      "\"requiredTargets\":%d,\"resolvedOptionalTargets\":%u,"
                      "\"optionalTargets\":%d,\"fields\":%u,\"methods\":%u,\"truncated\":%s}",
                      g_state.attempt_count,
                      g_state.assemblies_scanned,
                      g_state.lookups_used,
                      g_state.classes_resolved,
                      resolved_req_count,
                      PGR_IH_REQUIRED_TARGETS,
                      resolved_opt_count,
                      PGR_IH_OPTIONAL_TARGETS,
                      g_state.fields_emitted,
                      g_state.methods_emitted,
                      g_state.truncated ? "true" : "false");
    }

    if (resolved_req_count == PGR_IH_REQUIRED_TARGETS &&
        (g_state.ambiguous_target_mask & PGR_IH_REQUIRED_MASK) == 0 &&
        !g_state.truncated) {
        copy_str(out_reason, reason_cap, "ok");
        return kInspectReady;
    }

    if ((g_state.ambiguous_target_mask & PGR_IH_REQUIRED_MASK) != 0) {
        copy_str(out_reason, reason_cap, "ambiguous-targets");
    } else if (g_state.truncated) {
        copy_str(out_reason, reason_cap, "metadata-truncated");
    } else {
        copy_str(out_reason, reason_cap, "targets-unresolved");
    }
    return kInspectRetry;
}

static bool prepare_single_result_spec_locked(uint32_t spec_idx, ResultReaderState &rs) {
    memset(&rs, 0, sizeof(rs));
    const ResultReaderSpecDesc &spec = kResultSpecs[spec_idx];
    const ResolvedClassState &cs = g_state.classes[spec.class_index];
    if (!cs.found || cs.klass == nullptr) {
        copy_str(rs.error_reason, sizeof(rs.error_reason), "class-not-resolved");
        return false;
    }
    if (cs.value_type || cs.is_enum || cs.instance_size < 16) {
        copy_str(rs.error_reason, sizeof(rs.error_reason), "unexpected-message-layout");
        return false;
    }

    auto class_get_fields =
        reinterpret_cast<FnClassGetFields>(g_state.cfg.extra.class_get_fields);
    auto field_get_name = reinterpret_cast<FnFieldGetName>(g_state.cfg.extra.field_get_name);
    auto field_get_type = reinterpret_cast<FnFieldGetType>(g_state.meta_exports.field_get_type);
    auto field_get_flags =
        reinterpret_cast<FnFieldGetFlags>(g_state.meta_exports.field_get_flags);
    auto field_get_offset =
        reinterpret_cast<FnFieldGetOffset>(g_state.meta_exports.field_get_offset);
    auto class_from_type =
        reinterpret_cast<FnClassFromType>(g_state.meta_exports.class_from_type);
    auto class_is_enum = reinterpret_cast<FnClassIsEnum>(g_state.meta_exports.class_is_enum);
    auto class_enum_basetype =
        reinterpret_cast<FnClassEnumBasetype>(g_state.meta_exports.class_enum_basetype);
    auto class_value_size =
        reinterpret_cast<FnClassValueSize>(g_state.meta_exports.class_value_size);
    auto class_instance_size =
        reinterpret_cast<FnClassInstanceSize>(g_state.cfg.extra.class_instance_size);
    auto field_static_get_value =
        reinterpret_cast<FnFieldStaticGetValue>(g_state.cfg.extra.field_static_get_value);

    if (class_get_fields == nullptr || field_get_name == nullptr || field_get_type == nullptr ||
        field_get_flags == nullptr || field_get_offset == nullptr || class_from_type == nullptr ||
        class_is_enum == nullptr || class_enum_basetype == nullptr || class_value_size == nullptr ||
        class_instance_size == nullptr || field_static_get_value == nullptr) {
        copy_str(rs.error_reason, sizeof(rs.error_reason), "enum-reflection-exports-missing");
        return false;
    }

    void *iter = nullptr;
    void *matched_field = nullptr;
    uint32_t match_count = 0;
    uint32_t scanned = 0;
    while (scanned < PGR_IH_LIMIT_FIELDS_PER_CLASS) {
        void *f = class_get_fields(cs.klass, &iter);
        if (f == nullptr) {
            break;
        }
        ++scanned;
        char fname[64] = {};
        read_safe_string(field_get_name(f), fname, sizeof(fname));
        if (str_eq(fname, spec.field_name)) {
            matched_field = f;
            ++match_count;
        }
    }
    if (matched_field == nullptr) {
        copy_str(rs.error_reason, sizeof(rs.error_reason), "result-field-missing");
        return false;
    }
    if (match_count > 1) {
        copy_str(rs.error_reason, sizeof(rs.error_reason), "ambiguous-result-field");
        return false;
    }

    int32_t fflags = field_get_flags(matched_field);
    if ((fflags & 0x0010) != 0) {
        copy_str(rs.error_reason, sizeof(rs.error_reason), "result-field-static");
        return false;
    }
    int32_t offset = static_cast<int32_t>(field_get_offset(matched_field));
    if (offset <= 0 || offset + spec.expected_size > cs.instance_size) {
        copy_str(rs.error_reason, sizeof(rs.error_reason), "result-field-offset-out-of-range");
        return false;
    }

    const void *ftype = field_get_type(matched_field);
    char ftype_name[128] = {};
    describe_type_name(ftype, ftype_name, sizeof(ftype_name));
    if (!str_eq(ftype_name, spec.field_type)) {
        copy_str(rs.error_reason, sizeof(rs.error_reason), "result-field-type-mismatch");
        return false;
    }

    void *enum_klass = class_from_type(ftype);
    if (enum_klass == nullptr || !class_is_enum(enum_klass)) {
        copy_str(rs.error_reason, sizeof(rs.error_reason), "result-field-not-enum");
        return false;
    }

    const void *base_type = class_enum_basetype(enum_klass);
    char base_type_name[64] = {};
    describe_type_name(base_type, base_type_name, sizeof(base_type_name));
    if (!str_eq(base_type_name, "System.Int32")) {
        copy_str(rs.error_reason, sizeof(rs.error_reason), "unsupported-enum-underlying-type");
        return false;
    }

    int enum_align = 0;
    int enum_size = class_value_size(enum_klass, &enum_align);
    int32_t enum_inst_size = class_instance_size(enum_klass);
    if (enum_size != spec.expected_size || enum_align != spec.expected_align ||
        enum_inst_size < spec.expected_size || enum_inst_size > 32) {
        copy_str(rs.error_reason, sizeof(rs.error_reason), "unexpected-enum-size");
        return false;
    }

    /* Inspect enum fields: value__ + literals */
    void *e_iter = nullptr;
    uint32_t e_scanned = 0;
    uint32_t instance_fields = 0;
    char value_field_name[32] = {};
    char value_field_type[64] = {};
    while (true) {
        if (e_scanned >= PGR_IH_LIMIT_ENUM_FIELDS) {
            copy_str(rs.error_reason, sizeof(rs.error_reason), "enum-field-limit");
            return false;
        }
        void *ef = class_get_fields(enum_klass, &e_iter);
        if (ef == nullptr) {
            break;
        }
        ++e_scanned;
        char ef_name[64] = {};
        char ef_type[128] = {};
        read_safe_string(field_get_name(ef), ef_name, sizeof(ef_name));
        describe_type_name(field_get_type(ef), ef_type, sizeof(ef_type));
        int32_t ef_flags = field_get_flags(ef);
        if ((ef_flags & 0x0010) == 0) {
            ++instance_fields;
            copy_str(value_field_name, sizeof(value_field_name), ef_name);
            copy_str(value_field_type, sizeof(value_field_type), ef_type);
            continue;
        }
        if ((ef_flags & 0x8050) != 0x8050 || !str_eq(ef_type, spec.field_type)) {
            continue;
        }
        if (rs.literal_count >= PGR_IH_LIMIT_ENUM_LITERALS) {
            copy_str(rs.error_reason, sizeof(rs.error_reason), "enum-literal-limit");
            return false;
        }
        int32_t lit_val = 0;
        field_static_get_value(ef, &lit_val);
        rs.literals[rs.literal_count].code = lit_val;
        copy_str(rs.literals[rs.literal_count].name,
                 sizeof(rs.literals[rs.literal_count].name),
                 ef_name);
        ++rs.literal_count;
    }

    if (instance_fields != 1 || !str_eq(value_field_name, "value__") ||
        !str_eq(value_field_type, "System.Int32")) {
        copy_str(rs.error_reason, sizeof(rs.error_reason), "enum-value-field-mismatch");
        return false;
    }
    if (rs.literal_count == 0) {
        copy_str(rs.error_reason, sizeof(rs.error_reason), "enum-literals-empty");
        return false;
    }

    rs.valid = true;
    rs.klass = cs.klass;
    rs.instance_size = cs.instance_size;
    rs.offset = offset;
    copy_str(rs.field_name, sizeof(rs.field_name), spec.field_name);
    copy_str(rs.enum_type, sizeof(rs.enum_type), spec.field_type);
    copy_str(rs.value_type, sizeof(rs.value_type), value_field_type);
    return true;
}

static void prepare_result_readers_locked() {
    g_state.argument_readers_available = false;
    g_state.return_reader_available = false;
    copy_str(g_state.arg_reader_reason, sizeof(g_state.arg_reader_reason), "disabled-by-option");
    copy_str(g_state.return_reader_reason, sizeof(g_state.return_reader_reason),
             "disabled-by-option");

    if (g_state.opts.read_results == 0) {
        PgrInteractionHostEvent *ev =
            alloc_event_slot(PGR_IH_EVT_RESULT_READ_AVAILABILITY, "interaction-probe");
        ev->available = 0;
        copy_str(ev->result_name, sizeof(ev->result_name), "disabled-by-option");
        publish_event(ev,
                      "{\"type\":\"result-read-availability\",\"available\":false,"
                      "\"reason\":\"disabled-by-option\"}");
        return;
    }

    auto domain_get = reinterpret_cast<FnDomainGet>(g_state.meta_exports.domain_get);
    void *domain = domain_get != nullptr ? domain_get() : nullptr;
    ScopedIl2CppThread thread_guard(g_state.cfg.extra, domain);

    bool s0 = prepare_single_result_spec_locked(0, g_state.result_specs[0]);
    bool s1 = prepare_single_result_spec_locked(1, g_state.result_specs[1]);

    if (s0 && s1) {
        g_state.argument_readers_available = true;
        copy_str(g_state.arg_reader_reason, sizeof(g_state.arg_reader_reason), "ok");
    } else {
        g_state.argument_readers_available = false;
        copy_str(g_state.arg_reader_reason,
                 sizeof(g_state.arg_reader_reason),
                 !s0 ? g_state.result_specs[0].error_reason : g_state.result_specs[1].error_reason);
    }

    /* Pokeball::btxu return reader depends only on Spec 0 (CatchPokemonOutProto.Status) + Target 19 */
    if (s0 && g_state.targets[PGR_IH_TGT_POKEBALL_BTXU].resolved) {
        g_state.return_reader_available = true;
        copy_str(g_state.return_reader_reason, sizeof(g_state.return_reader_reason), "ok");
    } else {
        g_state.return_reader_available = false;
        copy_str(g_state.return_reader_reason,
                 sizeof(g_state.return_reader_reason),
                 !s0 ? g_state.result_specs[0].error_reason
                     : g_state.targets[PGR_IH_TGT_POKEBALL_BTXU].unavail_reason);
    }

    PgrInteractionHostEvent *ev =
        alloc_event_slot(PGR_IH_EVT_RESULT_READ_AVAILABILITY, "interaction-probe");
    ev->available = g_state.argument_readers_available ? 1 : 0;
    copy_str(ev->result_name, sizeof(ev->result_name), g_state.arg_reader_reason);
    publish_event(ev,
                  "{\"type\":\"result-read-availability\",\"available\":%s,\"reason\":\"%s\","
                  "\"returnReaderAvailable\":%s,\"returnReaderReason\":\"%s\"}",
                  g_state.argument_readers_available ? "true" : "false",
                  g_state.arg_reader_reason,
                  g_state.return_reader_available ? "true" : "false",
                  g_state.return_reader_reason);
}

static uint32_t compute_effective_owned_mask_locked() {
    uint32_t mask = g_state.shared_owned_mask;
    if ((g_state.cfg.flags & PGR_IH_CFG_NO_AUTO_SHARED_DETECT) == 0) {
        const PgrObsApi *co = resolve_catch_observers_api();
        if (co != nullptr && co->tick != nullptr) {
            if (co->tick(0) == PGR_OBS_OK) {
                mask |= PGR_IH_SHARED_MASK_49;
            }
        }
        const PgrSharedHostApi *sh = resolve_shared_host_api();
        if (sh != nullptr && sh->sample != nullptr) {
            PgrSharedHostSample sh_s {};
            sh_s.version = 1;
            sh_s.size = sizeof(sh_s);
            if (sh->sample(&sh_s) == 0 && sh_s.ready != 0) {
                mask |= PGR_IH_SHARED_MASK_61;
            }
        }
        const PgrSpinHostApi *sp = resolve_spin_host_api();
        if (sp != nullptr && sp->sample != nullptr) {
            PgrSpinHostSample sp_s {};
            sp_s.version = 1;
            sp_s.size = sizeof(sp_s);
            if (sp->sample(&sp_s) == PGR_SPH_OK && sp_s.configured != 0) {
                if ((sp_s.capabilities & PGR_SPH_CAMERA) != 0) {
                    mask |= PGR_IH_SHARED_MASK_67_CAMERA;
                }
                if ((sp_s.capabilities & PGR_SPH_QUICK) != 0) {
                    mask |= PGR_IH_SHARED_MASK_67_QUICK;
                }
            }
        }
    }
    for (uint32_t t = 0; t < PGR_IH_TOTAL_TARGETS; ++t) {
        if (g_state.targets[t].resolved &&
            (g_state.targets[t].rva == PGR_IH_TR_RVA_ZLC ||
             g_state.targets[t].rva == PGR_IH_TR_RVA_ZLD)) {
            mask |= (1u << t);
        }
    }
    return mask & PGR_IH_ALL_TARGETS_MASK;
}

static const char *shared_owner_reason(uint32_t target_id) {
    if (g_state.targets[target_id].resolved &&
        (g_state.targets[target_id].rva == PGR_IH_TR_RVA_ZLC ||
         g_state.targets[target_id].rva == PGR_IH_TR_RVA_ZLD)) {
        return "hook-site-owned-by-feature-57";
    }
    uint32_t bit = 1u << target_id;
    if ((bit & PGR_IH_SHARED_MASK_49) != 0) {
        return "hook-site-owned-by-feature-49";
    }
    if ((bit & PGR_IH_SHARED_MASK_61) != 0) {
        return "hook-site-owned-by-feature-61";
    }
    if ((bit & PGR_IH_SHARED_MASK_67) != 0) {
        return "hook-site-owned-by-feature-67";
    }
    return "hook-site-owned-by-external-feature";
}

static int start_observation_locked() {
    prepare_result_readers_locked();

    g_state.effective_owned_mask = compute_effective_owned_mask_locked();
    g_state.blocked_shared_mask = 0;
    g_state.hooked_direct_mask = 0;
    g_state.installed_target_mask = 0;

    uint32_t unavailable_optional_count = 0;
    char first_required_failure[96] = {};

    for (uint32_t t = 0; t < PGR_IH_TOTAL_TARGETS; ++t) {
        const TargetDesc &td = kTargets[t];
        ResolvedTargetState &ts = g_state.targets[t];
        uint32_t bit = 1u << t;

        if (!ts.resolved || ts.code == 0) {
            if (td.optional_target) {
                ++unavailable_optional_count;
                if (ts.unavail_reason[0] == '\0') {
                    copy_str(ts.unavail_reason, sizeof(ts.unavail_reason), "target-unresolved");
                }
                continue;
            }
            copy_str(first_required_failure, sizeof(first_required_failure), "target-unresolved");
            break;
        }

        /* Never call pgrazor_hook on any target owned by Features 49, 57, 61, 67 */
        if ((g_state.effective_owned_mask & bit) != 0) {
            if ((g_state.shared_integrated_mask & bit) != 0) {
                g_state.installed_target_mask |= bit;
                continue;
            }
            g_state.blocked_shared_mask |= bit;
            const char *owner_reason = shared_owner_reason(t);
            copy_str(ts.unavail_reason, sizeof(ts.unavail_reason), owner_reason);
            if (td.optional_target ||
                (g_state.opts.flags & PGR_IH_OPT_DEGRADE_UNATTESTED_SHARED) != 0) {
                ++unavailable_optional_count;
                continue;
            }
            copy_str(first_required_failure, sizeof(first_required_failure), owner_reason);
            break;
        }

        /* Direct Dobby hook installation for unowned target */
        if(g_direct_codes[t]){
            if(g_direct_codes[t]!=ts.code||(g_direct_failed&bit)||!g_direct_originals[t]){
                if(td.optional_target){++unavailable_optional_count;copy_str(ts.unavail_reason,sizeof(ts.unavail_reason),"permanent-hook-unavailable");continue;}
                copy_str(first_required_failure,sizeof(first_required_failure),"permanent-hook-unavailable");break;
            }
            g_state.hooked_direct_mask|=bit;g_state.installed_target_mask|=bit;continue;
        }
        g_direct_codes[t]=ts.code;
        int hrc =
            call_pgrazor_hook(reinterpret_cast<void *>(ts.code), kHookReplacements[t], &g_direct_originals[t]);
        if(g_direct_originals[t])g_direct_installed|=bit;
        if (hrc != 0 || !g_direct_originals[t]) {
            g_direct_failed|=bit;
            copy_str(ts.unavail_reason, sizeof(ts.unavail_reason), "dobby-hook-failed");
            if (td.optional_target) {
                ++unavailable_optional_count;
                continue;
            }
            copy_str(first_required_failure, sizeof(first_required_failure), "dobby-hook-failed");
            break;
        }
        g_state.orig_targets[t] = g_direct_originals[t];
        g_state.hooked_direct_mask |= bit;
        g_state.installed_target_mask |= bit;
    }

    if (first_required_failure[0] != '\0') {
        unhook_direct_targets_locked();
        PgrInteractionHostEvent *ev =
            alloc_event_slot(PGR_IH_EVT_OBSERVER_TARGET_AVAILABILITY, "interaction-probe");
        ev->available = 0;
        copy_str(ev->result_name, sizeof(ev->result_name), first_required_failure);
        publish_event(ev,
                      "{\"type\":\"observer-target-availability\",\"available\":false,"
                      "\"reason\":\"%s\",\"blockedSharedMask\":%u}",
                      first_required_failure,
                      g_state.blocked_shared_mask);
        finish_observation_locked(PGR_IH_STATUS_FAILED, "observer-setup-failed");
        return PGR_IH_ERR_HOOK;
    }

    if (unavailable_optional_count > 0 || g_state.blocked_shared_mask != 0) {
        PgrInteractionHostEvent *ev =
            alloc_event_slot(PGR_IH_EVT_OBSERVER_TARGET_AVAILABILITY, "interaction-probe");
        ev->available = 0;
        publish_event(ev,
                      "{\"type\":\"observer-target-availability\",\"available\":false,"
                      "\"unavailableCount\":%u,\"blockedSharedMask\":%u}",
                      unavailable_optional_count,
                      g_state.blocked_shared_mask);
    }

    g_state.status = PGR_IH_STATUS_OBSERVING;
    g_state.observe_started_ms = now_ms();
    g_state.observe_deadline_ms = g_state.observe_started_ms + PGR_IH_LIMIT_OBSERVE_MS;
    g_state.last_poll_ms = g_state.observe_started_ms;

    uint32_t installed_count = 0;
    for (uint32_t t = 0; t < PGR_IH_TOTAL_TARGETS; ++t) {
        if ((g_state.installed_target_mask & (1u << t)) != 0) {
            ++installed_count;
        }
    }

    PgrInteractionHostEvent *ev =
        alloc_event_slot(PGR_IH_EVT_OBSERVATION_READY, "interaction-probe");
    ev->available = 1;
    publish_event(ev,
                  "{\"type\":\"observation-ready\",\"windowMs\":%d,\"maxEvents\":%d,"
                  "\"maxHits\":%d,\"maxThreads\":%d,\"readResults\":%s,"
                  "\"returnResultReads\":%s,\"targetCount\":%u}",
                  PGR_IH_LIMIT_OBSERVE_MS,
                  PGR_IH_LIMIT_EVENTS,
                  PGR_IH_LIMIT_HITS,
                  PGR_IH_LIMIT_THREADS,
                  g_state.argument_readers_available ? "true" : "false",
                  g_state.return_reader_available ? "true" : "false",
                  installed_count);

    emit_transport_diagnostic(
        "Recording for 60s: catch normally, then spin a stop. Minimize this panel.");
    return PGR_IH_OK;
}

static void finish_observation_locked(uint32_t final_status, const char *reason) {
    g_state.status = final_status;
    copy_str(g_state.reason, sizeof(g_state.reason), reason);

    if (g_state.opts.observe_runtime != 0 &&
        (final_status == PGR_IH_STATUS_COMPLETE || final_status == PGR_IH_STATUS_STOPPED ||
         g_state.observe_started_ms > 0)) {
        int64_t end_ms = now_ms();
        int64_t elapsed = g_state.observe_started_ms > 0 ? (end_ms - g_state.observe_started_ms) : 0;
        PgrInteractionHostEvent *sev =
            alloc_event_slot(PGR_IH_EVT_OBSERVATION_SUMMARY, "interaction-probe");
        publish_event(sev,
                      "{\"type\":\"observation-summary\",\"reason\":\"%s\",\"elapsedMs\":%" PRId64 ","
                      "\"eventCount\":%u,\"hitCount\":%u,\"observedThreads\":%u,"
                      "\"droppedThreads\":%u,\"inFlight\":%u,\"btxuPendingPolls\":%u,"
                      "\"resultReadAvailable\":%s,\"returnResultReadAvailable\":%s}",
                      g_state.reason,
                      elapsed,
                      g_state.event_count,
                      g_state.hit_count,
                      g_state.thread_count,
                      g_state.dropped_threads,
                      g_state.in_flight,
                      g_state.btxu_pending_polls,
                      g_state.argument_readers_available ? "true" : "false",
                      g_state.return_reader_available ? "true" : "false");
    }

    if (g_state.in_flight == 0) {
        unhook_direct_targets_locked();
    } else {
        g_state.deferred_cleanup_pending = true;
    }

    g_state.running = false;
    g_state.stopping = false;

    const char *st_str = status_to_string(final_status);
    PgrInteractionHostEvent *fev = alloc_event_slot(PGR_IH_EVT_FINISHED, "interaction-probe");
    publish_event(fev,
                  "{\"type\":\"finished\",\"status\":\"%s\",\"reason\":\"%s\","
                  "\"observeRuntime\":%s,\"readResults\":%s}",
                  st_str,
                  g_state.reason,
                  g_state.opts.observe_runtime != 0 ? "true" : "false",
                  g_state.opts.read_results != 0 ? "true" : "false");

    emit_transport_running(false);

    char diag[160] = {};
    if (g_state.opts.observe_runtime != 0) {
        if (g_state.reason[0] != '\0' && !str_eq(g_state.reason, "ok")) {
            snprintf(diag, sizeof(diag), "Capture %s; see Run log: %s", st_str, g_state.reason);
        } else {
            snprintf(diag, sizeof(diag), "Capture %s; see Run log", st_str);
        }
    } else if (final_status == PGR_IH_STATUS_COMPLETE) {
        copy_str(diag, sizeof(diag), "Ready to record catch and spin callbacks");
    } else {
        if (g_state.reason[0] != '\0') {
            snprintf(diag, sizeof(diag), "Metadata %s: %s", st_str, g_state.reason);
        } else {
            snprintf(diag, sizeof(diag), "Metadata %s", st_str);
        }
    }
    emit_transport_diagnostic(diag);
}

static void disable_arg_readers_locked(const char *reason) {
    if (!g_state.argument_readers_available) {
        return;
    }
    g_state.argument_readers_available = false;
    copy_str(g_state.arg_reader_reason, sizeof(g_state.arg_reader_reason), reason);
    PgrInteractionHostEvent *ev =
        alloc_event_slot(PGR_IH_EVT_RESULT_READ_AVAILABILITY, "interaction-probe");
    ev->available = 0;
    ev->is_return_result = 0;
    copy_str(ev->result_name, sizeof(ev->result_name), reason);
    publish_event(ev,
                  "{\"type\":\"result-read-availability\",\"available\":false,\"reason\":\"%s\"}",
                  reason);
}

static void disable_return_reader_locked(const char *reason) {
    if (!g_state.return_reader_available) {
        return;
    }
    g_state.return_reader_available = false;
    copy_str(g_state.return_reader_reason, sizeof(g_state.return_reader_reason), reason);
    PgrInteractionHostEvent *ev =
        alloc_event_slot(PGR_IH_EVT_RESULT_READ_AVAILABILITY, "interaction-probe");
    ev->available = 0;
    ev->is_return_result = 1;
    copy_str(ev->result_name, sizeof(ev->result_name), reason);
    publish_event(ev,
                  "{\"type\":\"result-read-availability\",\"returnResultRead\":true,"
                  "\"available\":false,\"reason\":\"%s\"}",
                  reason);
}

static void decode_result_object_locked(uint32_t target_id,
                                        void *receiver,
                                        void *msg_obj,
                                        bool is_return,
                                        uint8_t *out_status,
                                        int32_t *out_code,
                                        char *out_name,
                                        size_t out_name_cap) {
    *out_status = PGR_IH_RES_NONE;
    *out_code = 0;
    if (out_name_cap > 0) {
        out_name[0] = '\0';
    }

    const TargetDesc &td = kTargets[target_id];
    if (td.result_spec_index < 0) {
        return;
    }
    if (is_return ? !g_state.return_reader_available : !g_state.argument_readers_available) {
        return;
    }

    const ResultReaderState &rs = g_state.result_specs[td.result_spec_index];
    if (!rs.valid) {
        return;
    }

    /* Validate receiver object header (klass pointer at offset 0) */
    const ResolvedTargetState &ts = g_state.targets[target_id];
    if (receiver == nullptr || (reinterpret_cast<uintptr_t>(receiver) & 0x7u) != 0) {
        if (is_return) {
            disable_return_reader_locked("receiver-class-mismatch");
        } else {
            disable_arg_readers_locked("receiver-class-mismatch");
        }
        *out_status = PGR_IH_RES_DISABLED;
        copy_str(out_name, out_name_cap, "receiver-class-mismatch");
        return;
    }
    void *recv_klass = *reinterpret_cast<void *const *>(receiver);
    if (ts.klass != nullptr && recv_klass != ts.klass) {
        if (is_return) {
            disable_return_reader_locked("receiver-class-mismatch");
        } else {
            disable_arg_readers_locked("receiver-class-mismatch");
        }
        *out_status = PGR_IH_RES_DISABLED;
        copy_str(out_name, out_name_cap, "receiver-class-mismatch");
        return;
    }

    if (msg_obj == nullptr) {
        *out_status = PGR_IH_RES_NULL_ARG;
        return;
    }
    if ((reinterpret_cast<uintptr_t>(msg_obj) & 0x7u) != 0) {
        const char *err = is_return ? "return-class-mismatch" : "response-class-mismatch";
        if (is_return) {
            disable_return_reader_locked(err);
        } else {
            disable_arg_readers_locked(err);
        }
        *out_status = PGR_IH_RES_DISABLED;
        copy_str(out_name, out_name_cap, err);
        return;
    }

    void *msg_klass = *reinterpret_cast<void *const *>(msg_obj);
    if (msg_klass != rs.klass) {
        const char *err = is_return ? "return-class-mismatch" : "response-class-mismatch";
        if (is_return) {
            disable_return_reader_locked(err);
        } else {
            disable_arg_readers_locked(err);
        }
        *out_status = PGR_IH_RES_DISABLED;
        copy_str(out_name, out_name_cap, err);
        return;
    }

    int32_t code = *reinterpret_cast<const int32_t *>(
        reinterpret_cast<const uint8_t *>(msg_obj) + rs.offset);
    *out_status = PGR_IH_RES_DECODED;
    *out_code = code;
    copy_str(out_name, out_name_cap, "UNKNOWN");
    for (uint32_t i = 0; i < rs.literal_count; ++i) {
        if (rs.literals[i].code == code) {
            copy_str(out_name, out_name_cap, rs.literals[i].name);
            break;
        }
    }
}

static int find_or_add_thread_locked(int32_t tid) {
    for (uint32_t i = 0; i < g_state.thread_count; ++i) {
        if (g_state.threads[i] == tid) {
            return static_cast<int>(i);
        }
    }
    if (g_state.thread_count >= PGR_IH_LIMIT_THREADS) {
        ++g_state.dropped_threads;
        return -1;
    }
    uint32_t idx = g_state.thread_count++;
    g_state.threads[idx] = tid;
    g_state.thread_depths[idx] = 0;
    return static_cast<int>(idx);
}

static uint64_t observe_enter_internal(uint32_t target_id, void *receiver, void *arg0) {
    if (target_id >= PGR_IH_TOTAL_TARGETS) {
        return 0;
    }
    ScopedLock lock;
    auto current=reinterpret_cast<FnThreadCurrent>(g_state.cfg.extra.thread_current);
    if (!current||!current()||g_transport_failed||!g_state.running || g_state.stopping || g_state.status != PGR_IH_STATUS_OBSERVING) {
        return 0;
    }
    if ((g_state.installed_target_mask & (1u << target_id)) == 0) {
        return 0;
    }

    int64_t t_ms = now_ms();
    ++g_state.hit_count;
    ++g_state.target_hits[target_id];

    if (t_ms >= g_state.observe_deadline_ms) {
        finish_observation_locked(
            PGR_IH_STATUS_COMPLETE,
            g_state.event_count > 0 ? "observation-complete" : "no-natural-calls");
        return 0;
    }
    if (g_state.hit_count > PGR_IH_LIMIT_HITS) {
        finish_observation_locked(PGR_IH_STATUS_COMPLETE, "hit-limit");
        return 0;
    }

    const TargetDesc &td = kTargets[target_id];
    uint32_t needed_events = td.read_return ? 2u : 1u;
    if (g_state.event_count + g_state.reserved_events + 2u > PGR_IH_LIMIT_EVENTS) {
        finish_observation_locked(PGR_IH_STATUS_COMPLETE, "event-limit");
        return 0;
    }

    int32_t tid = current_thread_id();
    int thread_idx = find_or_add_thread_locked(tid);
    if (thread_idx < 0) {
        finish_observation_locked(PGR_IH_STATUS_COMPLETE, "thread-limit");
        return 0;
    }

    /* Allocate an in-flight slot */
    int slot_idx = -1;
    for (int i = 0; i < kMaxInFlightSlots; ++i) {
        if (!g_state.slots[i].active) {
            slot_idx = i;
            break;
        }
    }
    if (slot_idx < 0) {
        return 0;
    }

    uint32_t depth = ++g_state.thread_depths[thread_idx];
    uint32_t call_id = td.read_return ? 0u : ++g_state.next_call_id;
    int64_t elapsed_ms = t_ms - g_state.observe_started_ms;

    /* For non-readReturn targets, emit the enter event immediately */
    if (!td.read_return) {
        uint8_t res_status = PGR_IH_RES_NONE;
        int32_t res_code = 0;
        char res_name[64] = {};

        if (td.result_spec_index >= 0 && g_state.argument_readers_available) {
            ScopedGcPin recv_pin(g_state.cfg.extra, receiver);
            ScopedGcPin arg_pin(g_state.cfg.extra, arg0);
            decode_result_object_locked(target_id,
                                        recv_pin.target(receiver),
                                        arg_pin.target(arg0),
                                        false,
                                        &res_status,
                                        &res_code,
                                        res_name,
                                        sizeof(res_name));
        }

        ++g_state.event_count;
        PgrInteractionHostEvent *ev =
            alloc_event_slot(PGR_IH_EVT_INTERACTION_EVENT, "interaction-probe");
        ev->ts_ms = t_ms;
        ev->elapsed_ms = elapsed_ms;
        ev->target_id = target_id;
        ev->call_id = call_id;
        ev->depth = depth;
        ev->tid = tid;
        ev->phase = PGR_IH_PHASE_ENTER;
        ev->result_status = res_status;
        ev->is_return_result = 0;
        ev->result_code = res_code;
        copy_str(ev->result_name, sizeof(ev->result_name), res_name);

        if (res_status == PGR_IH_RES_DECODED) {
            publish_event(ev,
                          "{\"type\":\"interaction-event\",\"phase\":\"enter\",\"callId\":%u,"
                          "\"index\":%u,\"ts\":%" PRId64 ",\"elapsedMs\":%" PRId64 ","
                          "\"tid\":%d,\"depth\":%u,\"className\":\"%s\",\"method\":\"%s\","
                          "\"rva\":\"0x%" PRIxPTR "\",\"result\":{\"status\":\"decoded\","
                          "\"code\":%d,\"name\":\"%s\"}}",
                          call_id,
                          g_state.event_count,
                          t_ms,
                          elapsed_ms,
                          tid,
                          depth,
                          td.class_full_name,
                          td.method_name,
                          g_state.targets[target_id].rva,
                          res_code,
                          res_name);
        } else {
            publish_event(ev,
                          "{\"type\":\"interaction-event\",\"phase\":\"enter\",\"callId\":%u,"
                          "\"index\":%u,\"ts\":%" PRId64 ",\"elapsedMs\":%" PRId64 ","
                          "\"tid\":%d,\"depth\":%u,\"className\":\"%s\",\"method\":\"%s\","
                          "\"rva\":\"0x%" PRIxPTR "\"}",
                          call_id,
                          g_state.event_count,
                          t_ms,
                          elapsed_ms,
                          tid,
                          depth,
                          td.class_full_name,
                          td.method_name,
                          g_state.targets[target_id].rva);
        }
    }

    InFlightSlot &slot = g_state.slots[slot_idx];
    slot.active = true;
    slot.generation = ++g_state.slot_generation;
    if (slot.generation == 0) {
        slot.generation = ++g_state.slot_generation;
    }
    slot.target_id = target_id;
    slot.call_id = call_id;
    slot.depth = depth;
    slot.reserved_events = needed_events;
    slot.tid = tid;
    slot.enter_ts_ms = t_ms;
    slot.enter_elapsed_ms = elapsed_ms;

    g_state.reserved_events += needed_events;
    ++g_state.in_flight;

    return (static_cast<uint64_t>(slot.generation) << 32) | static_cast<uint32_t>(slot_idx + 1);
}

static void observe_leave_internal(uint64_t token, void *retval, bool aborted) {
    if (token == 0) {
        return;
    }
    uint32_t slot_1based = static_cast<uint32_t>(token & 0xffffffffu);
    uint32_t gen = static_cast<uint32_t>(token >> 32);
    if (slot_1based == 0 || slot_1based > kMaxInFlightSlots) {
        return;
    }

    ScopedLock lock;
    InFlightSlot &slot = g_state.slots[slot_1based - 1];
    if (!slot.active || slot.generation != gen) {
        return;
    }

    uint32_t target_id = slot.target_id;
    uint32_t call_id = slot.call_id;
    uint32_t depth = slot.depth;
    uint32_t reserved = slot.reserved_events;
    int32_t tid = slot.tid;
    int64_t enter_ts = slot.enter_ts_ms;
    int64_t enter_elapsed = slot.enter_elapsed_ms;
    slot.active = false;

    if (g_state.reserved_events >= reserved) {
        g_state.reserved_events -= reserved;
    } else {
        g_state.reserved_events = 0;
    }
    if (g_state.in_flight > 0) {
        --g_state.in_flight;
    }

    for (uint32_t i = 0; i < g_state.thread_count; ++i) {
        if (g_state.threads[i] == tid) {
            if (g_state.thread_depths[i] > 0) {
                --g_state.thread_depths[i];
            }
            break;
        }
    }

    const TargetDesc &td = kTargets[target_id];
    if (aborted) {if(!g_state.in_flight)g_state.deferred_cleanup_pending=false;return;}
    if (!g_state.running || g_state.stopping || g_state.status != PGR_IH_STATUS_OBSERVING) {
        if (g_state.deferred_cleanup_pending && g_state.in_flight == 0) {
            unhook_direct_targets_locked();
        }
        return;
    }

    /* Special handling for Pokeball::btxu (readReturn = true) */
    if (td.read_return) {
        if (retval == nullptr) {
            ++g_state.btxu_pending_polls;
            ++g_state.target_pending_polls[target_id];
            return;
        }
        call_id = ++g_state.next_call_id;
        ++g_state.event_count;
        PgrInteractionHostEvent *enter_ev =
            alloc_event_slot(PGR_IH_EVT_INTERACTION_EVENT, "interaction-probe");
        enter_ev->ts_ms = enter_ts;
        enter_ev->elapsed_ms = enter_elapsed;
        enter_ev->target_id = target_id;
        enter_ev->call_id = call_id;
        enter_ev->depth = depth;
        enter_ev->tid = tid;
        enter_ev->phase = PGR_IH_PHASE_ENTER;
        publish_event(enter_ev,
                      "{\"type\":\"interaction-event\",\"phase\":\"enter\",\"callId\":%u,"
                      "\"index\":%u,\"ts\":%" PRId64 ",\"elapsedMs\":%" PRId64 ","
                      "\"tid\":%d,\"depth\":%u,\"className\":\"%s\",\"method\":\"%s\","
                      "\"rva\":\"0x%" PRIxPTR "\"}",
                      call_id,
                      g_state.event_count,
                      enter_ts,
                      enter_elapsed,
                      tid,
                      depth,
                      td.class_full_name,
                      td.method_name,
                      g_state.targets[target_id].rva);
    }

    int64_t leave_ts = now_ms();
    int64_t leave_elapsed = leave_ts - g_state.observe_started_ms;
    int64_t duration_ms = leave_ts >= enter_ts ? (leave_ts - enter_ts) : 0;

    uint8_t ret_status = PGR_IH_RES_NONE;
    int32_t ret_code = 0;
    char ret_name[64] = {};

    if (td.read_return && g_state.return_reader_available && retval != nullptr) {
        // The direct/shared hook owner roots its return through this call. The
        // reader does only bounded native loads; never allocate/free under g_mu.
        void *pinned_ret = retval;
        const ResultReaderState &rs = g_state.result_specs[td.result_spec_index];
        if ((reinterpret_cast<uintptr_t>(pinned_ret) & 0x7u) != 0 ||
            *reinterpret_cast<void *const *>(pinned_ret) != rs.klass) {
            disable_return_reader_locked("return-class-mismatch");
            ret_status = PGR_IH_RES_DISABLED;
            copy_str(ret_name, sizeof(ret_name), "return-class-mismatch");
        } else {
            int32_t code = *reinterpret_cast<const int32_t *>(
                reinterpret_cast<const uint8_t *>(pinned_ret) + rs.offset);
            ret_status = PGR_IH_RES_DECODED;
            ret_code = code;
            copy_str(ret_name, sizeof(ret_name), "UNKNOWN");
            for (uint32_t i = 0; i < rs.literal_count; ++i) {
                if (rs.literals[i].code == code) {
                    copy_str(ret_name, sizeof(ret_name), rs.literals[i].name);
                    break;
                }
            }
        }
    }

    ++g_state.event_count;
    PgrInteractionHostEvent *leave_ev =
        alloc_event_slot(PGR_IH_EVT_INTERACTION_EVENT, "interaction-probe");
    leave_ev->ts_ms = leave_ts;
    leave_ev->elapsed_ms = leave_elapsed;
    leave_ev->duration_ms = duration_ms;
    leave_ev->target_id = target_id;
    leave_ev->call_id = call_id;
    leave_ev->depth = depth;
    leave_ev->tid = tid;
    leave_ev->phase = PGR_IH_PHASE_LEAVE;
    leave_ev->result_status = ret_status;
    leave_ev->is_return_result = td.read_return ? 1 : 0;
    leave_ev->result_code = ret_code;
    copy_str(leave_ev->result_name, sizeof(leave_ev->result_name), ret_name);

    if (td.read_return && ret_status == PGR_IH_RES_DECODED) {
        publish_event(leave_ev,
                      "{\"type\":\"interaction-event\",\"phase\":\"leave\",\"callId\":%u,"
                      "\"index\":%u,\"ts\":%" PRId64 ",\"elapsedMs\":%" PRId64 ","
                      "\"durationMs\":%" PRId64 ",\"tid\":%d,\"depth\":%u,"
                      "\"className\":\"%s\",\"method\":\"%s\",\"rva\":\"0x%" PRIxPTR "\","
                      "\"returnResult\":{\"status\":\"decoded\",\"code\":%d,\"name\":\"%s\"}}",
                      call_id,
                      g_state.event_count,
                      leave_ts,
                      leave_elapsed,
                      duration_ms,
                      tid,
                      depth,
                      td.class_full_name,
                      td.method_name,
                      g_state.targets[target_id].rva,
                      ret_code,
                      ret_name);
    } else {
        publish_event(leave_ev,
                      "{\"type\":\"interaction-event\",\"phase\":\"leave\",\"callId\":%u,"
                      "\"index\":%u,\"ts\":%" PRId64 ",\"elapsedMs\":%" PRId64 ","
                      "\"durationMs\":%" PRId64 ",\"tid\":%d,\"depth\":%u,"
                      "\"className\":\"%s\",\"method\":\"%s\",\"rva\":\"0x%" PRIxPTR "\"}",
                      call_id,
                      g_state.event_count,
                      leave_ts,
                      leave_elapsed,
                      duration_ms,
                      tid,
                      depth,
                      td.class_full_name,
                      td.method_name,
                      g_state.targets[target_id].rva);
    }

    if (g_state.event_count >= PGR_IH_LIMIT_EVENTS) {
        finish_observation_locked(PGR_IH_STATUS_COMPLETE, "event-limit");
    } else if (leave_ts >= g_state.observe_deadline_ms) {
        finish_observation_locked(
            PGR_IH_STATUS_COMPLETE,
            g_state.event_count > 0 ? "observation-complete" : "no-natural-calls");
    }
}

static int step_prepare_attempt_locked() {
    ++g_state.attempt_count;
    char reason_buf[96] = {};
    InspectResultKind kind = run_inspect_locked(reason_buf, sizeof(reason_buf));
    if (kind == kInspectSkipped) {
        finish_observation_locked(PGR_IH_STATUS_SKIPPED, reason_buf);
        return PGR_IH_OK;
    }
    if (kind == kInspectReady) {
        if (g_state.opts.observe_runtime == 0) {
            finish_observation_locked(PGR_IH_STATUS_COMPLETE, "metadata-only");
            return PGR_IH_OK;
        }
        return start_observation_locked();
    }

    int64_t t_ms = now_ms();
    if (g_state.attempt_count >= PGR_IH_LIMIT_ATTEMPTS ||
        (t_ms - g_state.prepare_started_ms) >= PGR_IH_LIMIT_PREPARE_MS) {
        finish_observation_locked(PGR_IH_STATUS_FAILED, reason_buf);
        return PGR_IH_OK;
    }
    copy_str(g_state.reason, sizeof(g_state.reason), reason_buf);
    g_state.next_retry_ms = t_ms + PGR_IH_LIMIT_RETRY_MS;
    return PGR_IH_OK;
}

static int start_internal_locked(const PgrInteractionHostOptions *opts) {
    if (!g_state.configured) {
        return PGR_IH_ERR_UNCONFIGURED;
    }
    auto current=reinterpret_cast<FnThreadCurrent>(g_state.cfg.extra.thread_current);
    const int32_t tid=current_thread_id();
    if(!current||!current()||(g_owner_tid&&g_owner_tid!=tid))return PGR_IH_ERR_INVALID;
    if (g_state.running||g_state.in_flight||g_deferred_btxu_root.load()) {
        return PGR_IH_ERR_BUSY;
    }
    if(opts&&(opts->observe_runtime>1||opts->read_results>1||opts->reserved0[0]||opts->reserved0[1]||
        (opts->flags&~PGR_IH_OPT_DEGRADE_UNATTESTED_SHARED)))return PGR_IH_ERR_INVALID;
    if(g_transport_failed)return PGR_IH_ERR_INVALID;
    g_owner_tid=tid;

    g_state.scheduled = false;
    g_state.schedule_due_ms = 0;

    if (opts != nullptr) {
        g_state.opts = *opts;
    } else {
        memset(&g_state.opts, 0, sizeof(g_state.opts));
    }

    g_state.running = true;
    g_state.stopping = false;
    g_state.status = PGR_IH_STATUS_PREPARING;
    g_state.prepare_started_ms = now_ms();
    g_state.next_retry_ms = g_state.prepare_started_ms;
    g_state.observe_started_ms = 0;
    g_state.observe_deadline_ms = 0;
    g_state.last_poll_ms = 0;
    g_state.attempt_count = 0;
    g_state.hit_count = 0;
    g_state.event_count = 0;
    g_state.reserved_events = 0;
    g_state.next_call_id = 0;
    g_state.thread_count = 0;
    g_state.dropped_threads = 0;
    g_state.in_flight = 0;
    g_state.btxu_pending_polls = 0;
    memset(g_state.threads, 0, sizeof(g_state.threads));
    memset(g_state.thread_depths, 0, sizeof(g_state.thread_depths));
    memset(g_state.target_hits, 0, sizeof(g_state.target_hits));
    memset(g_state.target_pending_polls, 0, sizeof(g_state.target_pending_polls));
    memset(g_state.slots, 0, sizeof(g_state.slots));
    copy_str(g_state.reason, sizeof(g_state.reason), "preparing");

    emit_transport_running(true);
    emit_transport_diagnostic(g_state.opts.observe_runtime != 0
                                  ? "Preparing catch/spin capture..."
                                  : "Checking catch/spin metadata...");

    return step_prepare_attempt_locked();
}

static int api_configure(const PgrInteractionHostConfig *cfg) {
    ScopedLock lock;
    if (g_state.running || g_state.deferred_cleanup_pending||g_state.in_flight||g_direct_installed||g_deferred_btxu_root.load()) {
        return PGR_IH_ERR_BUSY;
    }

    PgrInteractionHostConfig merged {};
    PgrMetaExports local_exports {};
    if (cfg != nullptr) {
        merged = *cfg;
        if (cfg->binding.exports != nullptr) {
            local_exports = *cfg->binding.exports;
        }
    } else {
        merged.flags = PGR_IH_CFG_USE_BOOTSTRAP;
    }

    if ((merged.flags & PGR_IH_CFG_USE_BOOTSTRAP) != 0) {
        const PgrBootApi *boot = resolve_bootstrap_api();
        if (boot == nullptr || boot->snapshot == nullptr) {
            return PGR_IH_ERR_UNCONFIGURED;
        }
        PgrBootBinding b {};
        b.version = PGR_BOOT_BINDING_VERSION;
        b.size = sizeof(b);
        if (boot->snapshot(&b) != PGR_BOOT_OK || b.status != PGR_BOOT_OK) {
            return PGR_IH_ERR_UNCONFIGURED;
        }
        local_exports = b.meta;
        merged.binding.version = PGR_MD_CONFIG_VERSION;
        merged.binding.size = sizeof(PgrMetaBindConfig);
        merged.binding.module_base = b.module_base;
        merged.binding.module_size = b.module_size;
        merged.extra.thread_current = b.runtime.thread_current;
        merged.extra.thread_attach = b.runtime.thread_attach;
        merged.extra.thread_detach = b.runtime.thread_detach;
        merged.extra.field_static_get_value = b.runtime.field_static_get_value;
        merged.extra.gchandle_new = b.runtime.gchandle_new;
        merged.extra.gchandle_get_target = b.runtime.gchandle_get_target;
        merged.extra.gchandle_free = b.runtime.gchandle_free;
#if !defined(_WIN32)
        if (b.handle != nullptr) {
            auto sym = [&](const char *name) -> void * { return dlsym(b.handle, name); };
            if (merged.extra.image_get_name == nullptr)
                merged.extra.image_get_name = sym("il2cpp_image_get_name");
            if (merged.extra.class_get_name == nullptr)
                merged.extra.class_get_name = sym("il2cpp_class_get_name");
            if (merged.extra.class_get_namespace == nullptr)
                merged.extra.class_get_namespace = sym("il2cpp_class_get_namespace");
            if (merged.extra.class_get_parent == nullptr)
                merged.extra.class_get_parent = sym("il2cpp_class_get_parent");
            if (merged.extra.class_get_flags == nullptr)
                merged.extra.class_get_flags = sym("il2cpp_class_get_flags");
            if (merged.extra.class_instance_size == nullptr)
                merged.extra.class_instance_size = sym("il2cpp_class_instance_size");
            if (merged.extra.class_get_fields == nullptr)
                merged.extra.class_get_fields = sym("il2cpp_class_get_fields");
            if (merged.extra.field_get_name == nullptr)
                merged.extra.field_get_name = sym("il2cpp_field_get_name");
            if (merged.extra.method_get_param_name == nullptr)
                merged.extra.method_get_param_name = sym("il2cpp_method_get_param_name");
        }
#endif
    }

    const PgrMetaApi *meta = resolve_metadata_api();
    if (meta == nullptr || meta->bind == nullptr) {
        return PGR_IH_ERR_UNCONFIGURED;
    }
    if(!merged.extra.thread_current||!merged.extra.gchandle_new||!merged.extra.gchandle_get_target||!merged.extra.gchandle_free)return PGR_IH_ERR_INVALID;

    if (merged.binding.module_base == nullptr && meta->resolve_module != nullptr) {
        const void *resolved_base = nullptr;
        uint64_t resolved_size = 0;
        if (meta->resolve_module("libil2cpp.so", &resolved_base, &resolved_size) == PGR_MD_OK) {
            merged.binding.module_base = resolved_base;
            merged.binding.module_size = resolved_size;
        }
    }

    g_state.meta_exports = local_exports;
    merged.binding.version = PGR_MD_CONFIG_VERSION;
    merged.binding.size = sizeof(PgrMetaBindConfig);
    merged.binding.exports = &g_state.meta_exports;

    int brc = meta->bind(&merged.binding);
    if (brc != PGR_MD_OK) {
        return PGR_IH_ERR_INVALID;
    }

    g_state.cfg = merged;
    g_state.cfg.binding.exports = &g_state.meta_exports;
    g_state.configured = true;
    g_state.shared_owned_mask = merged.shared_owned_mask & PGR_IH_ALL_TARGETS_MASK;
    g_state.shared_integrated_mask = merged.shared_integrated_mask & PGR_IH_ALL_TARGETS_MASK;
    if (g_state.status == PGR_IH_STATUS_IDLE) {
        copy_str(g_state.reason, sizeof(g_state.reason), "configured");
    }
    return PGR_IH_OK;
}

static int api_schedule(int64_t delay_ms) {
    ScopedLock lock;
    if (!g_state.configured) {
        return PGR_IH_ERR_UNCONFIGURED;
    }
    if (g_state.running) {
        return PGR_IH_ERR_BUSY;
    }
    int64_t d = delay_ms >= 0 ? delay_ms : PGR_IH_SCHEDULE_DELAY_MS;
    g_state.scheduled = true;
    g_state.schedule_due_ms = now_ms() + d;
    g_state.status = PGR_IH_STATUS_SCHEDULED;
    copy_str(g_state.reason, sizeof(g_state.reason), "scheduled");

    PgrInteractionHostEvent *ev =
        alloc_event_slot(PGR_IH_EVT_SCHEDULED, "interaction-probe-scheduled");
    publish_event(
        ev,
        "Metadata check in 25 seconds; event capture requires an explicit button press");
    return PGR_IH_OK;
}

static int api_start(const PgrInteractionHostOptions *opts) {
    ScopedLock lock;
    return start_internal_locked(opts);
}

static int api_stop(const char *reason) {
    ScopedLock lock;
    const char *stop_reason = (reason != nullptr && reason[0] != '\0') ? reason : "stopped";
    if (g_state.scheduled) {
        g_state.scheduled = false;
        g_state.schedule_due_ms = 0;
        if (!g_state.running) {
            g_state.status = PGR_IH_STATUS_STOPPED;
            copy_str(g_state.reason, sizeof(g_state.reason), stop_reason);
            emit_transport_diagnostic("Interaction diagnostic stopped");
            return PGR_IH_OK;
        }
    }
    if (!g_state.running) {
        return PGR_IH_OK;
    }
    g_state.stopping = true;
    finish_observation_locked(PGR_IH_STATUS_STOPPED, stop_reason);
    return PGR_IH_OK;
}

static int api_tick(int32_t current_tid) {
    if(current_tid<=0||current_tid!=current_thread_id())return PGR_IH_ERR_INVALID;
    // Drain deferred managed ownership only on the attached preparation owner,
    // outside the native state mutex and with no borrowed return value live.
    PgrInteractionExtraExports extra{};
    {ScopedLock proof;if(!g_state.configured)return PGR_IH_ERR_UNCONFIGURED;
        auto cur=reinterpret_cast<FnThreadCurrent>(g_state.cfg.extra.thread_current);
        if(!cur||!cur()||(g_owner_tid&&g_owner_tid!=current_tid))return PGR_IH_ERR_INVALID;
        g_owner_tid=current_tid;extra=g_state.cfg.extra;}
    uintptr_t deferred=g_deferred_btxu_root.exchange(0);
    if(deferred)reinterpret_cast<FnGcHandleFree>(extra.gchandle_free)(deferred);
    ScopedLock lock;
    if (!g_state.configured) {
        return PGR_IH_ERR_UNCONFIGURED;
    }
    if(g_transport_failed){g_state.running=false;g_state.scheduled=false;g_state.stopping=true;
        g_state.status=PGR_IH_STATUS_FAILED;g_state.installed_target_mask=0;copy_str(g_state.reason,sizeof(g_state.reason),"transport-failed");return PGR_IH_ERR_INVALID;}

    if (g_state.deferred_cleanup_pending && g_state.in_flight == 0) {
        unhook_direct_targets_locked();
    }

    /* Check Transport 52 reader 15 and pulses 1 & 2 */
    const PgrRuntimeTransportApi *rt = resolve_transport_api();
    bool diag_ready = false;
    if (rt != nullptr && rt->get_bool != nullptr) {
        int ready_val = 0;
        if (rt->get_bool(PGR_IH_RT_BOOL_DIAGNOSTICS_READY, &ready_val) == PGR_TRANSPORT_OK) {
            diag_ready = (ready_val != 0);
        }
    }

    if (rt != nullptr && rt->take_pulse != nullptr) {
        int stop_fired = 0;
        if (rt->take_pulse(PGR_IH_RT_PULSE_STOP_REQUEST, &stop_fired) == PGR_TRANSPORT_OK &&
            stop_fired != 0) {
            if (g_state.scheduled || g_state.running) {
                g_state.scheduled = false;
                g_state.schedule_due_ms = 0;
                if (g_state.running) {
                    g_state.stopping = true;
                    finish_observation_locked(PGR_IH_STATUS_STOPPED, "stopped");
                } else {
                    g_state.status = PGR_IH_STATUS_STOPPED;
                    copy_str(g_state.reason, sizeof(g_state.reason), "stopped");
                    emit_transport_diagnostic("Interaction diagnostic stopped");
                }
            }
        }

        if (diag_ready && !g_state.running) {
            int trace_fired = 0;
            if (rt->take_pulse(PGR_IH_RT_PULSE_TRACE_REQUEST, &trace_fired) == PGR_TRANSPORT_OK &&
                trace_fired != 0) {
                PgrInteractionHostOptions trace_opts {};
                trace_opts.observe_runtime = 1;
                trace_opts.read_results = 1;
                start_internal_locked(&trace_opts);
            }
        }
    }

    int64_t t_ms = now_ms();

    /* Check scheduled 25s metadata-only check */
    if (g_state.scheduled && !g_state.running && t_ms >= g_state.schedule_due_ms) {
        g_state.scheduled = false;
        g_state.schedule_due_ms = 0;
        if (!diag_ready) {
            g_state.status = PGR_IH_STATUS_SKIPPED;
            copy_str(g_state.reason, sizeof(g_state.reason), "not-ready");
            emit_transport_diagnostic("Interaction diagnostic skipped (native hooks not ready)");
            return PGR_IH_OK;
        }
        PgrInteractionHostOptions meta_opts {};
        meta_opts.observe_runtime = 0;
        meta_opts.read_results = 0;
        return start_internal_locked(&meta_opts);
    }

    /* Drive prepare retry or active observation window */
    if (g_state.running && g_state.status == PGR_IH_STATUS_PREPARING) {
        if ((t_ms - g_state.prepare_started_ms) >= PGR_IH_LIMIT_PREPARE_MS) {
            finish_observation_locked(PGR_IH_STATUS_FAILED, g_state.reason);
            return PGR_IH_OK;
        }
        if (t_ms >= g_state.next_retry_ms) {
            return step_prepare_attempt_locked();
        }
    } else if (g_state.running && g_state.status == PGR_IH_STATUS_OBSERVING) {
        if (t_ms >= g_state.observe_deadline_ms) {
            finish_observation_locked(
                PGR_IH_STATUS_COMPLETE,
                g_state.event_count > 0 ? "observation-complete" : "no-natural-calls");
        }
    }

    return PGR_IH_OK;
}

static void api_on_unity_pulse(void *ctx, const PgrUnityPulse *pulse) {
    (void)ctx;
    if (pulse == nullptr||pulse->tid<=0||pulse->tid!=current_thread_id()) {
        return;
    }
    const PgrUnitySchedulerApi *sched = resolve_scheduler_api();
    PgrUnitySchedulerSnapshot proof{};
    if(!sched||!sched->valid||!sched->snapshot||sched->valid(pulse->generation)!=1||
       sched->snapshot(&proof)!=PGR_US_OK||!proof.installed||proof.owner_tid!=pulse->tid)return;
    api_tick(pulse->tid);
}

static int api_shared_sites(uint32_t owned_mask, uint32_t integrated_mask) {
    ScopedLock lock;
    g_state.shared_owned_mask = owned_mask & PGR_IH_ALL_TARGETS_MASK;
    g_state.shared_integrated_mask = integrated_mask & PGR_IH_ALL_TARGETS_MASK;
    return PGR_IH_OK;
}

static uint64_t api_observe_enter(uint32_t target_id, void *receiver, void *arg0) {
    return observe_enter_internal(target_id, receiver, arg0);
}

static void api_observe_leave(uint64_t token, void *retval) {
    observe_leave_internal(token, retval);
}

static int api_take_event(PgrInteractionHostEvent *out_event) {
    if (out_event == nullptr) {
        return PGR_IH_ERR_INVALID;
    }
    ScopedLock lock;
    if (g_state.queue_count == 0) {
        return 0;
    }
    *out_event = g_state.queue[g_state.queue_head];
    g_state.queue_head = (g_state.queue_head + 1) % kEventQueueCapacity;
    --g_state.queue_count;
    return 1;
}

static int api_sample(PgrInteractionHostSample *out_sample) {
    if (out_sample == nullptr) {
        return PGR_IH_ERR_INVALID;
    }
    ScopedLock lock;
    memset(out_sample, 0, sizeof(*out_sample));
    out_sample->status = g_state.status;
    out_sample->configured = g_state.configured ? 1 : 0;
    out_sample->running = g_state.running ? 1 : 0;
    out_sample->scheduled = g_state.scheduled ? 1 : 0;
    out_sample->observe_runtime = g_state.opts.observe_runtime;
    out_sample->read_results = g_state.opts.read_results;
    out_sample->stopping = g_state.stopping ? 1 : 0;
    out_sample->truncated = g_state.truncated ? 1 : 0;
    out_sample->argument_readers_available = g_state.argument_readers_available ? 1 : 0;
    out_sample->return_reader_available = g_state.return_reader_available ? 1 : 0;
    out_sample->deferred_cleanup_pending = g_state.deferred_cleanup_pending ? 1 : 0;
    out_sample->attempt_count = g_state.attempt_count;
    out_sample->assemblies_scanned = g_state.assemblies_scanned;
    out_sample->lookups_used = g_state.lookups_used;
    out_sample->classes_resolved = g_state.classes_resolved;
    out_sample->fields_emitted = g_state.fields_emitted;
    out_sample->methods_emitted = g_state.methods_emitted;
    out_sample->resolved_required_mask = g_state.resolved_required_mask;
    out_sample->resolved_optional_mask = g_state.resolved_optional_mask;
    out_sample->ambiguous_target_mask = g_state.ambiguous_target_mask;
    out_sample->hooked_direct_mask = g_state.hooked_direct_mask;
    out_sample->shared_owned_mask = g_state.effective_owned_mask != 0
                                        ? g_state.effective_owned_mask
                                        : g_state.shared_owned_mask;
    out_sample->shared_integrated_mask = g_state.shared_integrated_mask;
    out_sample->blocked_shared_mask = g_state.blocked_shared_mask;
    out_sample->installed_target_mask = g_state.installed_target_mask;
    out_sample->hit_count = g_state.hit_count;
    out_sample->event_count = g_state.event_count;
    out_sample->reserved_events = g_state.reserved_events;
    out_sample->thread_count = g_state.thread_count;
    out_sample->dropped_threads = g_state.dropped_threads;
    out_sample->in_flight = g_state.in_flight;
    out_sample->btxu_pending_polls = g_state.btxu_pending_polls;
    out_sample->total_emitted_events = g_state.total_emitted_events;
    memcpy(out_sample->threads, g_state.threads, sizeof(out_sample->threads));
    memcpy(out_sample->target_hits, g_state.target_hits, sizeof(out_sample->target_hits));
    copy_str(out_sample->reason, sizeof(out_sample->reason), g_state.reason);
    copy_str(out_sample->last_diagnostic, sizeof(out_sample->last_diagnostic),
             g_state.last_diagnostic);
    return PGR_IH_OK;
}

static void api_reset_for_tests() {
    ScopedLock lock;
    if(g_direct_installed||g_state.in_flight||g_deferred_btxu_root.load())return;
    unhook_direct_targets_locked();
    memset(&g_state, 0, sizeof(g_state));
    g_state.status = PGR_IH_STATUS_IDLE;
    g_owner_tid=0;g_transport_failed=false;
}

static const PgrInteractionHostApi kInteractionHostApi = {
    PGR_IH_API_VERSION,
    PGR_IH_FEATURE_ID,
    &api_configure,
    &api_schedule,
    &api_start,
    &api_stop,
    &api_tick,
    &api_on_unity_pulse,
    &api_shared_sites,
    &api_observe_enter,
    &api_observe_leave,
    &api_take_event,
    &api_sample,
    &api_reset_for_tests,
};

static int api_wants_target(uint32_t id) {
    if(id>=PGR_IH_TOTAL_TARGETS)return 0;
#if !defined(_WIN32)
    if(pthread_mutex_trylock(&g_mu)!=0)return 0;
    struct Unlock{~Unlock(){pthread_mutex_unlock(&g_mu);}} unlock;
#endif
    if(g_transport_failed||!g_state.running||g_state.stopping||g_state.status!=PGR_IH_STATUS_OBSERVING||
       !(g_state.installed_target_mask&(1u<<id)))return 0;
    auto current=reinterpret_cast<FnThreadCurrent>(g_state.cfg.extra.thread_current);
    return current&&current()!=nullptr?1:0;
}
static void api_observe_abort(uint64_t token){observe_leave_internal(token,nullptr,true);}
static const PgrInteractionHostApi2 kObserverApi={2,sizeof(PgrInteractionHostApi2),
    api_wants_target,api_observe_enter,api_observe_leave,api_observe_abort};

}  // namespace

extern "C" const void *pgrazor_interaction_host_api(uint32_t version) {
    if(version==2)return &kObserverApi;
    if (version != PGR_IH_API_VERSION) {
        return nullptr;
    }
    return &kInteractionHostApi;
}
