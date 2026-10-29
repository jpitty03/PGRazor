#include "throws.h"
#include "hook_engine.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <unistd.h>

namespace {

// Status codes (0 == success); values align with the other native modules' conventions.
enum {
    OK = 0, ERR_NULL = 1, ERR_BOUND = 2, ERR_HOOK = 3, ERR_LEASE = 4,
    ERR_CLOCK = 5, ERR_NOT_INSTALLED = 6, ERR_BAD_CONFIG = 7, ERR_BAD_ARG = 9
};

enum { ROUTE_SUBMIT = 0, ROUTE_HIT = 1 };

// Lease / pairing cap. Matches LIMITS.leaseMs (2500 ms) in quick-ball.js.
constexpr uint64_t kMaxLeaseMs = 2500u;
constexpr uint64_t kWindowNs   = kMaxLeaseMs * 1000000ull;

// The game-side value arrives by value; this is its true ABI so the compiler marshals
// the two integer register halves (ARM64) exactly as the original function expects.
typedef void (*ThrowFn)(void *self, PgrThrowValue value, void *method_info);

// Rating bands, indexed by grade. [0] unused (grade 0 never samples a radius).
constexpr float kBandLow[4]  = {0.0f, 0.75f, 0.35f, 0.10f}; // Nice / Great / Excellent
constexpr float kBandHigh[4] = {0.0f, 0.95f, 0.65f, 0.25f};

// --- lifetime / publication -------------------------------------------------------
std::mutex g_install_mutex;               // serialises install() and original publication
PgrThrowConfig g_config{};                // validated copy, published under install mutex
std::atomic<bool> g_installed{false};     // true only after a fully successful install
bool g_install_failed = false;            // sticky; protected by install mutex
std::atomic<ThrowFn> g_orig_hit{nullptr};

// --- policy window (armed by update) ----------------------------------------------
std::atomic<int>      g_allowed{0};
std::atomic<uint32_t> g_grade{0};
std::atomic<uint32_t> g_curve{0};
std::atomic<uint64_t> g_deadline_ns{0};
std::atomic<uint64_t> g_unity_tid{0};

// --- sampled-radius cache + telemetry (guarded by g_policy_mutex) ------------------
std::mutex g_policy_mutex;
struct Sample {
    bool     valid = false;
    uint32_t grade = 0;
    uint64_t at_ns = 0;
    uint64_t original = 0;  // submit input x1 word (item low32 | radius-bits high32)
    uint64_t changed = 0;   // submit output x1 word
    float    radius = 0.0f; // the single sampled reticle radius
};
Sample g_sample;
struct Telemetry { uint64_t count = 0; uint32_t route = 0, grade = 0, curve = 0; float radius = 0.0f; };
Telemetry g_tel;

// Native RNG (NOT a CSPRNG; cosmetic radius jitter only). Seeded lazily under the policy
// lock from the boot clock, so all RNG use is single-threaded.
uint64_t g_rng = 0;
inline uint32_t rng_next() {
    if (g_rng == 0) {
        struct timespec ts{};
        clock_gettime(CLOCK_BOOTTIME, &ts);
        g_rng = (static_cast<uint64_t>(ts.tv_sec) << 20) ^ static_cast<uint64_t>(ts.tv_nsec) ^ 0x9e3779b97f4a7c15ull;
        if (g_rng == 0) g_rng = 0x106689d45497fdb5ull;
    }
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return static_cast<uint32_t>(g_rng >> 32);
}
inline float rng_band(uint32_t grade) {
    const float u = static_cast<float>(rng_next() >> 8) * (1.0f / 16777216.0f); // [0,1)
    return kBandLow[grade] + (kBandHigh[grade] - kBandLow[grade]) * u;
}

uint64_t now_ns() {
    struct timespec ts{};
    if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0) return 0;
    if (ts.tv_sec < 0 || ts.tv_nsec < 0) return 0;
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

inline void *obj_class(void *o) {
    return reinterpret_cast<void *(*)(void *)>(g_config.object_get_class)(o);
}
inline bool class_assignable(void *klass, void *oklass) {
    return reinterpret_cast<bool (*)(void *, void *)>(g_config.class_is_assignable)(klass, oklass);
}
inline uint8_t read_bread(void *o) {
    uint8_t v = 1;
    reinterpret_cast<void (*)(void *, void *, void *)>(g_config.field_get_value)(o, g_config.bread_field, &v);
    return v;
}

inline uint64_t x1_word(const PgrThrowValue &v) {
    uint32_t bits; std::memcpy(&bits, &v.radius, sizeof(bits));
    return static_cast<uint64_t>(static_cast<uint32_t>(v.item)) | (static_cast<uint64_t>(bits) << 32);
}

// ---------------------------------------------------------------------------
// Policy transform. Called only after the cheap atomic + class gates passed, with
// `now` already read. Holds g_policy_mutex; NEVER calls the original (the caller does,
// after releasing the lock), so a reentrant submit->hit call cannot deadlock.
// ---------------------------------------------------------------------------
PgrThrowValue transform(const PgrThrowValue &v, uint32_t grade, uint32_t curve, int route, uint64_t now) {
    std::lock_guard<std::mutex> lock(g_policy_mutex);
    const Sample prev = g_sample;
    g_sample.valid = false;
    // Value-level gates, mirroring throwWords(): eligible items, clean flags, sane radius.
    if (!(v.item == 1 || v.item == 2 || v.item == 3 || v.item == 5)) return v;
    if (v.bullseye > 1 || v.spinning > 1 || v.missed != 0) return v;
    if (!(std::isfinite(v.radius) && v.radius >= 0.0f && v.radius <= 1.0f)) return v;

    const uint64_t first = x1_word(v);


    const bool paired = route == ROUTE_HIT && prev.valid && prev.grade == grade &&
                        now >= prev.at_ns && now - prev.at_ns < kWindowNs &&
                        (first == prev.original || first == prev.changed);

    PgrThrowValue out = v;                 // preserve item, missed and pad verbatim
    float chosen = v.radius;
    if (grade) {
        chosen = paired ? prev.radius : rng_band(grade);
        out.radius = chosen;
    }
    out.bullseye = static_cast<uint8_t>(v.bullseye | (grade ? 1u : 0u));
    out.spinning = static_cast<uint8_t>(v.spinning | (curve ? 1u : 0u));

    // Submit seeds the one-shot sample the paired hit may reuse; hit never re-stores.
    if (grade && route == ROUTE_SUBMIT) {
        g_sample = Sample{true, grade, now, first, x1_word(out), chosen};
    }

    g_tel.count += 1;
    g_tel.route = static_cast<uint32_t>(route);
    g_tel.grade = grade;
    g_tel.curve = curve ? 1u : 0u;
    g_tel.radius = out.radius;
    return out;
}

// ---------------------------------------------------------------------------
// Shared replacement body for both hooks.
// ---------------------------------------------------------------------------
PgrThrowValue apply(void *self, PgrThrowValue value, int route, void *klass_gate) {
    PgrThrowValue out = value;
    if (self && g_installed.load(std::memory_order_acquire)) {
        const uint32_t grade = g_grade.load(std::memory_order_acquire);
        const uint32_t curve = g_curve.load(std::memory_order_acquire);
        if (g_allowed.load(std::memory_order_acquire) && (grade || curve) &&
            static_cast<uint64_t>(gettid()) == g_unity_tid.load(std::memory_order_acquire)) {
            const uint64_t now = now_ns();
            if (now && now < g_deadline_ns.load(std::memory_order_acquire)) {
                void *klass = obj_class(self);
                const bool gate = route == ROUTE_SUBMIT ? klass == klass_gate && read_bread(self) == 0 :
                    klass && class_assignable(klass_gate, klass);
                if (gate) out = transform(value, grade, curve, route, now);
            }
        }
    }
    return out;
}
void run(void *self, PgrThrowValue value, void *info, std::atomic<ThrowFn> &slot, int route, void *klass_gate) {
    // Publication race: the trampoline is published under g_install_mutex after the hook
    // goes live. Fast path reads it atomically; on a null read we take the install lock
    // (which install() holds across the store) and re-read. We never skip the original
    // or fabricate a result.
    ThrowFn orig = slot.load(std::memory_order_acquire);
    if (!orig) {
        std::lock_guard<std::mutex> lock(g_install_mutex);
        orig = slot.load(std::memory_order_acquire);
    }
    if (!orig) std::abort();  // a live hook must have a published original

    const PgrThrowValue out = apply(self, value, route, klass_gate);
    orig(self, out, info);  // always call the game, outside every policy lock
}

void hit_replacement(void *self, PgrThrowValue value, void *info) {
    run(self, value, info, g_orig_hit, ROUTE_HIT, g_config.owner_class);
}

// ---------------------------------------------------------------------------
// Config validation: every pointer present, headers exact, addresses unique pre-patch.
// ---------------------------------------------------------------------------
bool config_ok(const PgrThrowConfig *c) {
    const void *const ptrs[] = {
        c->submit_code, c->hit_code, c->ball_class, c->owner_class,
        c->object_get_class, c->class_is_assignable, c->field_get_value, c->bread_field};
    for (const void *p : ptrs) if (!p) return false;
    if (c->submit_code == c->hit_code) return false;      // two distinct hook targets
    if (c->ball_class == c->owner_class) return false;     // distinct class identities
    if (c->object_get_class == c->class_is_assignable ||
        c->object_get_class == c->field_get_value ||
        c->class_is_assignable == c->field_get_value) return false; // distinct fns
    return true;
}

// ---------------------------------------------------------------------------
// C API table functions.
// ---------------------------------------------------------------------------
int api_install(const PgrThrowConfig *config) {
    if (!config) return ERR_NULL;

    std::lock_guard<std::mutex> lock(g_install_mutex);
    if (g_install_failed) return ERR_HOOK;

    if (config->version != 1 || config->size != sizeof(PgrThrowConfig) || !config_ok(config))
        return ERR_BAD_CONFIG;

    if (g_installed.load(std::memory_order_acquire)) {
        // Hooks live for the process lifetime: an identical repeat install is a no-op
        // success, a different config is refused.
        return std::memcmp(&g_config, config, sizeof(g_config)) == 0 ? OK : ERR_BOUND;
    }

    g_config = *config;  // publish before any callback can read it (still under lock)

    // Submit is shared with the existing Gum berry replacement. Until that entire
    // site migrates, its listener calls rewrite_submit without patching it twice.
    ThrowFn h = nullptr;
    const int rh = pgrazor_hook(g_config.hit_code, reinterpret_cast<void *>(&hit_replacement),
                                reinterpret_cast<void **>(&h));

    if (rh || !h) {
        // Partial failure is permanent and inert. Publish whatever originals exist so the
        // affected trampolines merely chain through (policy can never arm: g_installed
        // stays false and update() refuses). No second engine is stacked, no JS fallback.
        if(!rh && !h) pgrazor_unhook(g_config.hit_code);
        g_orig_hit.store(h, std::memory_order_release);
        g_install_failed = true;
        return ERR_HOOK;
    }

    g_orig_hit.store(h, std::memory_order_release);
    g_installed.store(true, std::memory_order_release);
    return OK;
}

int api_update(int grade, int curve, int allowed, uint64_t lease_ms, uint64_t unity_tid) {
    if (!g_installed.load(std::memory_order_acquire)) return ERR_NOT_INSTALLED;
    if (grade < 0 || grade > 3) {
        g_allowed.store(0, std::memory_order_release);
        g_deadline_ns.store(0, std::memory_order_release);
        std::lock_guard<std::mutex> lock(g_policy_mutex); g_sample.valid=false;
        return ERR_BAD_ARG;
    }
    if (unity_tid != 0) g_unity_tid.store(unity_tid, std::memory_order_release);

    if (allowed == 0) {
        g_allowed.store(0, std::memory_order_release);
        g_deadline_ns.store(0, std::memory_order_release);
        g_grade.store(0, std::memory_order_release);
        g_curve.store(0, std::memory_order_release);
        std::lock_guard<std::mutex> lock(g_policy_mutex);
        g_sample.valid = false;           // drop any cached sample when the window closes
        return OK;
    }

    g_allowed.store(0, std::memory_order_release);
    g_deadline_ns.store(0, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(g_policy_mutex);
        if(g_grade.load()!=static_cast<uint32_t>(grade)||g_curve.load()!=static_cast<uint32_t>(curve?1:0)||
           !lease_ms||lease_ms>kMaxLeaseMs)g_sample.valid=false;
    }
    if (lease_ms == 0 || lease_ms > kMaxLeaseMs) return ERR_LEASE;
    const uint64_t now = now_ns();
    if (now == 0) return ERR_CLOCK;

    g_grade.store(static_cast<uint32_t>(grade), std::memory_order_release);
    g_curve.store(curve ? 1u : 0u, std::memory_order_release);
    g_deadline_ns.store(now + lease_ms * 1000000ull, std::memory_order_release);
    g_allowed.store(allowed, std::memory_order_release);
    return OK;
}

int api_sample(PgrThrowSample *out) {
    if (!out) return ERR_NULL;
    std::lock_guard<std::mutex> lock(g_policy_mutex);
    out->count = g_tel.count;
    out->route = g_tel.route;
    out->grade = g_tel.grade;
    out->curve = g_tel.curve;
    out->radius = g_tel.radius;
    return OK;
}

int rewrite_submit(void *self, PgrThrowValue *value) {
    if(!value)return ERR_NULL;
    if(!g_installed.load(std::memory_order_acquire))return ERR_NOT_INSTALLED;
    *value=apply(self,*value,ROUTE_SUBMIT,g_config.ball_class);return OK;
}
const PgrThrowApi g_api = {1, sizeof(PgrThrowApi), &api_install, &api_update, &api_sample, &rewrite_submit};

static_assert(sizeof(PgrThrowValue) == 12, "throw value ABI");
static_assert(sizeof(PgrThrowConfig) == 72, "throw config ABI");
static_assert(sizeof(PgrThrowSample) == 24, "throw sample ABI");
static_assert(sizeof(PgrThrowApi) == 40, "throw API ABI");

}  // namespace

const void *pgrazor_throws_api(uint32_t version) {
    return version == 1 ? &g_api : nullptr;
}
