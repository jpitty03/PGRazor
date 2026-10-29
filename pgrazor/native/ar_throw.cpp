#include "ar_throw.h"
#include "hook_engine.h"

#include <atomic>
#include <mutex>
#include <cstring>
#include <ctime>
#include <cstdlib>

#if defined(__linux__)
#include <unistd.h>
#include <sys/syscall.h>
#endif

// Built -fno-exceptions on both the ARM64 target ABI and the host x86 test ABI.
static_assert(sizeof(PgrArThrowConfig) == 112, "PgrArThrowConfig must be 112 bytes");
static_assert(sizeof(PgrArThrowSample) == 16,  "PgrArThrowSample must be 16 bytes");
static_assert(sizeof(PgrArThrowApi)    == 32,  "PgrArThrowApi must be 32 bytes");
static_assert(sizeof(PgrThrowValue)    == 12,  "PokeballThrow by-value must be 12 bytes");
static_assert(offsetof(PgrArThrowConfig, capture_code)        == 8,   "layout: capture_code");
static_assert(offsetof(PgrArThrowConfig, wild_class)          == 16,  "layout: wild_class");
static_assert(offsetof(PgrArThrowConfig, values_class)        == 24,  "layout: values_class");
static_assert(offsetof(PgrArThrowConfig, object_get_class)    == 32,  "layout: object_get_class");
static_assert(offsetof(PgrArThrowConfig, class_is_assignable) == 40,  "layout: class_is_assignable");
static_assert(offsetof(PgrArThrowConfig, object_new)          == 48,  "layout: object_new");
static_assert(offsetof(PgrArThrowConfig, gchandle_new)        == 56,  "layout: gchandle_new");
static_assert(offsetof(PgrArThrowConfig, gchandle_get_target) == 64,  "layout: gchandle_get_target");
static_assert(offsetof(PgrArThrowConfig, gchandle_free)       == 72,  "layout: gchandle_free");
static_assert(offsetof(PgrArThrowConfig, field_set_value)     == 80,  "layout: field_set_value");
static_assert(offsetof(PgrArThrowConfig, field_proximity)     == 88,  "layout: field_proximity");
static_assert(offsetof(PgrArThrowConfig, field_awareness)     == 96,  "layout: field_awareness");
static_assert(offsetof(PgrArThrowConfig, field_frightened)    == 104, "layout: field_frightened");

namespace {

// Validated game ABI: TryCapture returns a managed IPromise pointer and takes the
// instance, the 12-byte PokeballThrow by-value, the AR values proto and the MethodInfo*.
typedef void *(*ArCaptureFn)(void *self, PgrThrowValue thr, void *values, void *method);

typedef void *(*Il2cppObjectGetClass)(void *obj);
typedef int    (*Il2cppClassIsAssignable)(void *klass, void *oklass);
typedef void *(*Il2cppObjectNew)(void *klass);
typedef void *(*Il2cppGchandleNew)(void *obj, int pinned);   // pointer-sized handle
typedef void *(*Il2cppGchandleGetTarget)(void *handle);
typedef void   (*Il2cppGchandleFree)(void *handle);
typedef void   (*Il2cppFieldSetValue)(void *obj, void *field, void *value);

std::mutex        g_mutex;             // publication mutex: install + original handoff
PgrArThrowConfig  g_config{};          // stable after a successful install
bool              g_installed = false; // process-lifetime: never unhooked

ArCaptureFn        g_original = nullptr;
std::atomic<bool>  g_original_ready{false};

// Atomic fast-path gate, written only by update().
std::atomic<int>      g_enabled{0};
std::atomic<int>      g_allowed{0};
std::atomic<uint64_t> g_lease_deadline{0};  // CLOCK_BOOTTIME ms; 0 == closed
std::atomic<uint64_t> g_tid{0};             // proven Unity thread id

std::atomic<uint64_t> g_applied{0};
std::atomic<uint64_t> g_errors{0};
std::atomic<int>      g_fault{0};           // latched after an alloc/root failure

uint64_t current_tid() {
#if defined(__linux__)
    return (uint64_t)syscall(SYS_gettid);
#else
    return 0;
#endif
}

uint64_t boottime_ms() {
    struct timespec ts;
#if defined(CLOCK_BOOTTIME)
    clock_gettime(CLOCK_BOOTTIME, &ts);
#elif defined(CLOCK_MONOTONIC)
    clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    timespec_get(&ts, TIME_UTC);
#endif
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

void gate_close() {
    g_enabled.store(0, std::memory_order_relaxed);
    g_allowed.store(0, std::memory_order_relaxed);
    g_lease_deadline.store(0, std::memory_order_relaxed);
    g_tid.store(0, std::memory_order_relaxed);
}

void *ar_capture_replacement(void *self, PgrThrowValue thr, void *values, void *method) {
    ArCaptureFn orig = g_original_ready.load(std::memory_order_acquire) ? g_original : nullptr;
    if (!orig) {
        std::lock_guard<std::mutex> lock(g_mutex);
        orig = g_original;
        if (!orig) std::abort();
    }

    // Fast path: forward verbatim unless the lease is live on this exact thread.
    if (g_fault.load(std::memory_order_relaxed) || !g_enabled.load(std::memory_order_relaxed) ||
        !g_allowed.load(std::memory_order_relaxed)) {
        return orig ? orig(self, thr, values, method) : nullptr;
    }
    uint64_t tid = g_tid.load(std::memory_order_relaxed);
    if (tid == 0 || current_tid() != tid) {
        return orig ? orig(self, thr, values, method) : nullptr;
    }
    uint64_t deadline = g_lease_deadline.load(std::memory_order_relaxed);
    if (deadline == 0 || boottime_ms() > deadline) {
        return orig ? orig(self, thr, values, method) : nullptr;
    }
    if (self == nullptr) {
        return orig ? orig(self, thr, values, method) : nullptr;
    }
    if (thr.item < 1 || thr.item > 3 || thr.missed != 0) {
        return orig ? orig(self, thr, values, method) : nullptr;
    }

    Il2cppObjectGetClass    get_class     = (Il2cppObjectGetClass)g_config.object_get_class;
    Il2cppClassIsAssignable is_assignable = (Il2cppClassIsAssignable)g_config.class_is_assignable;
    void *self_class = get_class(self);
    if (self_class == nullptr || !is_assignable(g_config.wild_class, self_class)) {
        return orig ? orig(self, thr, values, method) : nullptr;
    }
    if (orig == nullptr) {
        return nullptr;
    }

    Il2cppObjectNew         object_new    = (Il2cppObjectNew)g_config.object_new;
    Il2cppGchandleNew       gc_new        = (Il2cppGchandleNew)g_config.gchandle_new;
    Il2cppGchandleGetTarget gc_target     = (Il2cppGchandleGetTarget)g_config.gchandle_get_target;
    Il2cppGchandleFree      gc_free       = (Il2cppGchandleFree)g_config.gchandle_free;
    Il2cppFieldSetValue     set_value     = (Il2cppFieldSetValue)g_config.field_set_value;

    // Allocate the replacement proto and pin it before touching managed memory. Any
    // failure latches a fault, counts an error and forwards the ORIGINAL arguments so the
    // encounter still resolves normally.
    void *obj = object_new(g_config.values_class);
    if (obj == nullptr) {
        g_fault.store(1, std::memory_order_relaxed);
        g_errors.fetch_add(1, std::memory_order_relaxed);
        return orig(self, thr, values, method);
    }
    void *handle = gc_new(obj, 1 /*pinned root*/);
    if (handle == nullptr) {
        g_fault.store(1, std::memory_order_relaxed);
        g_errors.fetch_add(1, std::memory_order_relaxed);
        return orig(self, thr, values, method);
    }
    void *rooted = gc_target(handle);
    if (rooted == nullptr) {
        gc_free(handle);
        g_fault.store(1, std::memory_order_relaxed);
        g_errors.fetch_add(1, std::memory_order_relaxed);
        return orig(self, thr, values, method);
    }

    float   zero  = 0.0f;
    uint8_t clear = 0;
    set_value(rooted, g_config.field_proximity,  &zero);
    set_value(rooted, g_config.field_awareness,  &zero);
    set_value(rooted, g_config.field_frightened, &clear);

    // Replace only the AR values argument; every other argument and the IPromise return
    // pass through untouched. Free the root after the original returns.
    void *result = orig(self, thr, rooted, method);
    gc_free(handle);
    g_applied.fetch_add(1, std::memory_order_relaxed);
    return result;
}

int ar_install(const PgrArThrowConfig *config) {
    if (config == nullptr) return 1;
    if (config->version != 1 || config->size != sizeof(PgrArThrowConfig)) return 2;

    const void *const *ptrs = reinterpret_cast<const void *const *>(&config->capture_code);
    for (int i = 0; i < 13; ++i) {
        if (ptrs[i] == nullptr) return 3;
    }

    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_installed) {
        // Identical config repeats are idempotent; a different config is refused. There
        // is no fallback engine once a patch has been attempted.
        if (memcmp(&g_config, config, sizeof(PgrArThrowConfig)) == 0) return 0;
        return 4;
    }

    void *original = nullptr;
    if (pgrazor_hook(config->capture_code, (void *)&ar_capture_replacement, &original) != 0) {
        return 5;
    }
    if (!original) {
        pgrazor_unhook(config->capture_code);
        return 6;
    }
    g_config   = *config;
    g_original = (ArCaptureFn)original;
    g_original_ready.store(true, std::memory_order_release);
    g_installed = true;
    return 0;
}

int ar_update(int enabled, int allowed, uint64_t lease_ms, uint64_t unity_tid) {
    // An out-of-policy lease is an invalid update: disable and report.
    if (lease_ms > 2500) { gate_close(); return 1; }

    if (!enabled) {                 // clear enabled on stop
        gate_close();
        return 0;
    }
    if (!allowed) {                 // armed but not currently permitted
        g_enabled.store(1, std::memory_order_relaxed);
        g_allowed.store(0, std::memory_order_relaxed);
        g_lease_deadline.store(0, std::memory_order_relaxed);
        g_tid.store(0, std::memory_order_relaxed);
        return 0;
    }
    if (unity_tid == 0 || lease_ms == 0) { gate_close(); return 2; }

    uint64_t deadline = boottime_ms() + lease_ms;
    g_tid.store(unity_tid, std::memory_order_relaxed);
    g_lease_deadline.store(deadline, std::memory_order_relaxed);
    g_allowed.store(1, std::memory_order_relaxed);
    g_enabled.store(1, std::memory_order_relaxed);
    return 0;
}

int ar_sample(PgrArThrowSample *out) {
    if (out == nullptr) return 1;
    out->applied = g_applied.load(std::memory_order_relaxed);
    out->errors  = g_errors.load(std::memory_order_relaxed);
    return 0;
}

const PgrArThrowApi g_api = {
    1, sizeof(PgrArThrowApi),
    &ar_install,
    &ar_update,
    &ar_sample,
};

} // namespace

const void *pgrazor_ar_throw_api(uint32_t version) {
    if (version != 1) return nullptr;
    return &g_api;
}
