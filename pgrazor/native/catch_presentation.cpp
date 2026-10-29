#include "catch_presentation.h"
#include "hook_engine.h"
#include <atomic>
#include <mutex>
#include <cstring>
#include <cstdlib>
#include <unistd.h>

namespace {
// The verified ARM64 overload passes 64-bit register/stack slots. Preserve the
// complete words when forwarding; inspect only the low byte for Boolean gates.
using Show = void (*)(void *, void *, uint64_t, uint64_t, uint64_t,
    void *, void *, void *, uint64_t, uint64_t, uint64_t, void *);
std::atomic<Show> original{nullptr};
std::mutex mutex;
PgrCatchConfig config{};
PgrCatchCapture capture{};
void *response_root = nullptr;
bool failed = false, attached = false, owner = false, enabled = false, captured = false;
int32_t unity_thread = 0;
template<class T> T field(void *object, size_t offset) {
    T value{}; std::memcpy(&value, static_cast<char *>(object) + offset, sizeof(value)); return value;
}
void release_response() {
    if(response_root) config.free_root(response_root);
    response_root = nullptr;
}
void replacement(void *instance, void *pokemon, uint64_t confirm, uint64_t persist,
    uint64_t hyper, void *dismissed, void *displayed, void *params,
    uint64_t hide_ar, uint64_t post, uint64_t suppress, void *info) {
    bool consumed = false;
    Show call;
    {
        // Install publishes the trampoline under the same lock. No call can
        // observe an active hook with an unpublished original pointer.
        std::lock_guard<std::mutex> guard(mutex);
        call = original.load(std::memory_order_acquire);
        if(!call) std::abort();
        if(owner && enabled && !captured && gettid() == unity_thread &&
           instance && pokemon && dismissed && !displayed && !params && (post & 255) == 1 &&
           config.object_class(dismissed) == config.action_class &&
           config.object_class(pokemon) == config.pokemon_class) {
            void *response = response_root ? config.target(response_root) : nullptr;
            void *view = field<void *>(dismissed, 0x20);
            if(response && view && config.object_class(view) == config.view_class &&
               field<void *>(view, 0xe0) == response &&
               field<uint64_t>(pokemon, 0x20) == field<uint64_t>(response, 0x20)) {
                void *hs = config.root(instance, 0), *hp = config.root(pokemon, 0),
                     *hc = config.root(dismissed, 0);
                if(hs && hp && hc) {
                    capture = {hs, hp, hc, uint32_t(confirm & 255), uint32_t(persist & 255),
                        int32_t(hyper & 0xffffffff), uint32_t(hide_ar & 255),
                        uint32_t(post & 255), uint32_t(suppress & 255)};
                    captured = true; enabled = false; consumed = true;
                } else {
                    if(hs) config.free_root(hs);
                    if(hp) config.free_root(hp);
                    if(hc) config.free_root(hc);
                }
            }
        }
    }
    // Never call the game under our mutex: it can synchronously re-enter.
    if(!consumed) call(instance,pokemon,confirm,persist,hyper,dismissed,displayed,params,hide_ar,post,suppress,info);
}
int install(const PgrCatchConfig *value) {
    if(!value || value->version != 1 || value->size != sizeof(*value) ||
       !value->address || !value->root || !value->free_root || !value->target || !value->object_class ||
       !value->view_class || !value->action_class || !value->pokemon_class) return -1;
    std::lock_guard<std::mutex> guard(mutex);
    if(failed || owner) return -2;
    if(attached) {
        if(std::memcmp(&config, value, sizeof(config))) return -3;
        owner = true; return 0;
    }
    config = *value;
    Show trampoline = nullptr;
    const int rc = pgrazor_hook(value->address, reinterpret_cast<void *>(&replacement),
                             reinterpret_cast<void **>(&trampoline));
    if(rc || !trampoline) {
        if(!rc) pgrazor_unhook(value->address);
        failed = true; return -4;
    }
    original.store(trampoline, std::memory_order_release);
    attached = true; owner = true; return 0;
}
int begin(void *response, int32_t thread) {
    std::lock_guard<std::mutex> guard(mutex);
    if(!owner || !response || thread <= 0 || gettid() != thread || enabled || captured) return -1;
    release_response();
    response_root = config.root(response, 0);
    if(!response_root) return -3;
    unity_thread = thread; enabled = true;
    return 0;
}
int end(PgrCatchCapture *out) {
    if(!out) return -1;
    std::lock_guard<std::mutex> guard(mutex);
    if(!owner || (unity_thread && unity_thread != gettid())) return -2;
    enabled = false; release_response();
    if(!captured) { *out = {}; return 0; }
    *out = capture; capture = {}; captured = false;
    return 1; // rooted handles now belong to the caller, exactly once
}
int dispose() {
    std::lock_guard<std::mutex> guard(mutex);
    if(!owner) return 0;
    if(enabled || captured) return -1;
    release_response(); owner = false; unity_thread = 0;
    // Keep the hook/library/trampoline alive, passing through while unowned.
    return 0;
}
const PgrCatchApi api{1, sizeof(PgrCatchApi), install, begin, end, dispose};
static_assert(sizeof(PgrCatchConfig)==72, "catch config ABI");
static_assert(sizeof(PgrCatchCapture)==48, "capture ABI");
static_assert(sizeof(PgrCatchApi)==40, "catch API ABI");
}
const void *pgrazor_catch_presentation_api(uint32_t version) { return version == 1 ? &api : nullptr; }
