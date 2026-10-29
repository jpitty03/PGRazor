#include "spin_camera.h"
#include "pgrazor_native_api.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <limits>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace {

using GetHookApiFn = const void *(*)(uint32_t version);

struct FakeObject {
    void *klass = nullptr;
    void *camera = nullptr;
    int32_t state = 0;
    uint8_t pad[0x50 - sizeof(void *) * 2 - sizeof(int32_t)]{};
    float speed = 0.0f;
};
static_assert(offsetof(FakeObject, speed) == 0x50, "Iterator speed must sit at +0x50");

int g_sentinel_in_class = 1;
int g_sentinel_out_class = 2;
int g_sentinel_spline_class = 3;
int g_sentinel_iter_class = 4;
int g_sentinel_other_class = 5;

int g_field_in_cam = 10;
int g_field_out_cam = 11;
int g_field_owner = 12;
int g_field_state = 13;

uintptr_t g_next_handle_id = 0x1000;
std::unordered_map<uintptr_t, void *> g_live_handles;
int g_fail_gchandle_new_countdown = -1;

void *fake_gchandle_new(void *obj, int /*pinned*/) {
    if (g_fail_gchandle_new_countdown == 0) {
        g_fail_gchandle_new_countdown = -1;
        return nullptr;
    }
    if (g_fail_gchandle_new_countdown > 0) {
        --g_fail_gchandle_new_countdown;
    }
    const uintptr_t id = g_next_handle_id++;
    g_live_handles[id] = obj;
    return reinterpret_cast<void *>(id);
}

void *fake_gchandle_get_target(void *handle) {
    auto it = g_live_handles.find(reinterpret_cast<uintptr_t>(handle));
    return it != g_live_handles.end() ? it->second : nullptr;
}

void fake_gchandle_free(void *handle) {
    auto it = g_live_handles.find(reinterpret_cast<uintptr_t>(handle));
    assert(it != g_live_handles.end() && "double or invalid gchandle_free");
    g_live_handles.erase(it);
}

void *fake_object_get_class(void *obj) {
    return obj ? static_cast<FakeObject *>(obj)->klass : nullptr;
}

void fake_field_get_value(void *obj, void *field, void *out) {
    auto *fo = static_cast<FakeObject *>(obj);
    if (field == &g_field_in_cam || field == &g_field_out_cam || field == &g_field_owner) {
        * static_cast<void **>(out) = fo->camera;
    } else if (field == &g_field_state) {
        *static_cast<int32_t *>(out) = fo->state;
    }
}

struct CallLog {
    void *last_self = nullptr;
    void *last_method = nullptr;
    int count = 0;
};

CallLog g_log_in_enter, g_log_in_exit, g_log_out_enter, g_log_out_exit, g_log_move_next;
bool g_throw_out=false;
int32_t g_move_next_next_state = 1;
bool g_move_next_ret = true;

#define PGR_NOP8() asm volatile("nop; nop; nop; nop; nop; nop; nop; nop;")

__attribute__((noinline)) void target_in_enter(void *self, void *method_info) {
    PGR_NOP8();
    g_log_in_enter.last_self = self;
    g_log_in_enter.last_method = method_info;
    ++g_log_in_enter.count;
}

__attribute__((noinline)) void target_in_exit(void *self, void *method_info) {
    PGR_NOP8();
    g_log_in_exit.last_self = self;
    g_log_in_exit.last_method = method_info;
    ++g_log_in_exit.count;
}

__attribute__((noinline)) void target_out_enter(void *self, void *method_info) {
    PGR_NOP8();
    g_log_out_enter.last_self = self;
    g_log_out_enter.last_method = method_info;
    ++g_log_out_enter.count;
    if(g_throw_out)throw 710;
}

__attribute__((noinline)) void target_out_exit(void *self, void *method_info) {
    PGR_NOP8();
    g_log_out_exit.last_self = self;
    g_log_out_exit.last_method = method_info;
    ++g_log_out_exit.count;
    if(g_throw_out)throw 710;
}

__attribute__((noinline)) bool target_move_next(void *self, void *method_info) {
    PGR_NOP8();
    g_log_move_next.last_self = self;
    g_log_move_next.last_method = method_info;
    ++g_log_move_next.count;
    if (self != nullptr) {
        static_cast<FakeObject *>(self)->state = g_move_next_next_state;
    }
    return g_move_next_ret;
}

void (*volatile call_in_enter)(void *, void *) = &target_in_enter;
void (*volatile call_in_exit)(void *, void *) = &target_in_exit;
void (*volatile call_out_enter)(void *, void *) = &target_out_enter;
void (*volatile call_out_exit)(void *, void *) = &target_out_exit;
bool (*volatile call_move_next)(void *, void *) = &target_move_next;

uint64_t this_tid() {
    return static_cast<uint64_t>(static_cast<pid_t>(::gettid()));
}

PgrSpinCameraConfig make_valid_config() {
    PgrSpinCameraConfig cfg{};
    cfg.version = 1;
    cfg.size = sizeof(PgrSpinCameraConfig);
    cfg.gchandle_new = &fake_gchandle_new;
    cfg.gchandle_get_target = &fake_gchandle_get_target;
    cfg.gchandle_free = &fake_gchandle_free;
    cfg.object_get_class = &fake_object_get_class;
    cfg.field_get_value = &fake_field_get_value;
    cfg.in_enter_addr = reinterpret_cast<void *>(&target_in_enter);
    cfg.in_exit_addr = reinterpret_cast<void *>(&target_in_exit);
    cfg.out_enter_addr = reinterpret_cast<void *>(&target_out_enter);
    cfg.out_exit_addr = reinterpret_cast<void *>(&target_out_exit);
    cfg.move_next_addr = reinterpret_cast<void *>(&target_move_next);
    cfg.in_class = &g_sentinel_in_class;
    cfg.out_class = &g_sentinel_out_class;
    cfg.spline_class = &g_sentinel_spline_class;
    cfg.iterator_class = &g_sentinel_iter_class;
    cfg.in_camera_field = &g_field_in_cam;
    cfg.out_camera_field = &g_field_out_cam;
    cfg.owner_field = &g_field_owner;
    cfg.state_field = &g_field_state;
    return cfg;
}

} // namespace

int main(int argc, char **argv) {
    const char *so_path = argc > 1 ? argv[1] : "./libpgrazor.so";
    void *lib = ::dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        std::fprintf(stderr, "dlopen(%s) failed: %s\n", so_path, ::dlerror());
        return 1;
    }

    auto getter = reinterpret_cast<GetHookApiFn>(::dlsym(lib, "pgrazor_native_api"));
    assert(getter);
    const auto *core = static_cast<const pgrazor_native_api_v2 *>(getter(2));
    assert(core && core->size == 48);
    assert(core->query_feature(10, 99) == nullptr);
    const auto *api = static_cast<const PgrSpinCameraApi *>(core->query_feature(10, 1));
    assert(api != nullptr && api->version == 1 && api->size == sizeof(PgrSpinCameraApi));

    const uint64_t tid = this_tid();
    PgrSpinCameraConfig cfg = make_valid_config();

    // Validation before install
    assert(api->install(nullptr) == PGR_SCAM_ERR_NULL_ARG);
    PgrSpinCameraConfig bad_cfg = cfg;
    bad_cfg.in_exit_addr = bad_cfg.in_enter_addr;
    assert(api->install(&bad_cfg) == PGR_SCAM_ERR_BAD_CONFIG);
    assert(api->update(1, 1000, tid, 1) == PGR_SCAM_ERR_HOOK_FAILED);

    // Install & idempotent re-install
    assert(api->install(&cfg) == PGR_SCAM_OK);
    assert(api->install(&cfg) == PGR_SCAM_OK);
    bad_cfg = cfg;
    bad_cfg.state_field = &g_field_owner;
    assert(api->install(&bad_cfg) == PGR_SCAM_ERR_ALREADY_BOUND);

    FakeObject cam_in{&g_sentinel_spline_class, nullptr, 0, {}, 0.0f};
    FakeObject cam_out{&g_sentinel_spline_class, nullptr, 0, {}, 0.0f};
    FakeObject cam_other{&g_sentinel_spline_class, nullptr, 0, {}, 0.0f};
    FakeObject state_in{&g_sentinel_in_class, &cam_in, 0, {}, 0.0f};
    FakeObject state_out{&g_sentinel_out_class, &cam_out, 0, {}, 0.0f};
    FakeObject iter{&g_sentinel_iter_class, &cam_in, 0, {}, 5.0f};
    int dummy_method = 42;

    // 1. Chaining while disabled (off)
    assert(api->update(0, 0, tid, 1) == PGR_SCAM_OK);
    call_in_enter(&state_in, &dummy_method);
    assert(g_log_in_enter.count == 1 && g_log_in_enter.last_self == &state_in && g_log_in_enter.last_method == &dummy_method);
    assert(call_move_next(&iter, &dummy_method) == true);
    assert(g_log_move_next.count == 1 && iter.speed == 5.0f);
    assert(g_live_handles.empty());

    // 2. Enable policy, Enter ('in') -> MoveNext clamp max(speed, 25)
    assert(api->update(1, 2000, tid, 1) == PGR_SCAM_OK);
    call_in_enter(&state_in, &dummy_method);
    PgrSpinCameraSample s{};
    assert(api->sample(&s) == PGR_SCAM_OK);
    assert(s.enter_count == 1 && s.active_roots_mask == 1U && g_live_handles.size() == 1);

    iter.state = 0;
    iter.speed = 4.0f;
    g_move_next_next_state = 1;
    g_move_next_ret = true;
    assert(call_move_next(&iter, &dummy_method) == true);
    assert(iter.speed == 25.0f);
    assert(api->sample(&s) == PGR_SCAM_OK);
    assert(s.fast_count == 1 && s.fast_serial == 1 && s.last_phase == 1U);
    assert(s.last_original_speed == 4.0f && s.last_applied_speed == 25.0f);
    assert(g_live_handles.size() == 1); // iterator root freed by ScopedGcHandle

    // Speed >= 25 preserved on 'out' phase
    call_out_enter(&state_out, &dummy_method);
    assert(api->sample(&s) == PGR_SCAM_OK && s.active_roots_mask == 3U);
    iter.camera = &cam_out;
    iter.state = 0;
    iter.speed = 40.0f;
    assert(call_move_next(&iter, &dummy_method) == true);
    assert(iter.speed == 40.0f);
    assert(api->sample(&s) == PGR_SCAM_OK);
    assert(s.fast_count == 2 && s.last_phase == 2U && s.last_original_speed == 40.0f && s.last_applied_speed == 40.0f);

    // 3. Untouched cases: wrong camera, wrong state_before, wrong state_after, false return, wrong class, non-finite/<=0 speed
    iter.camera = &cam_other;
    iter.state = 0;
    iter.speed = 5.0f;
    assert(call_move_next(&iter, &dummy_method) == true && iter.speed == 5.0f);

    iter.camera = &cam_in;
    iter.state = 1; // state_before != 0
    iter.speed = 5.0f;
    assert(call_move_next(&iter, &dummy_method) == true && iter.speed == 5.0f);

    iter.state = 0;
    g_move_next_next_state = 2; // state_after != 1
    assert(call_move_next(&iter, &dummy_method) == true && iter.speed == 5.0f);
    g_move_next_next_state = 1;

    iter.state = 0;
    g_move_next_ret = false; // MoveNext returns false
    assert(call_move_next(&iter, &dummy_method) == false && iter.speed == 5.0f);
    g_move_next_ret = true;

    iter.klass = &g_sentinel_other_class;
    iter.state = 0;
    assert(call_move_next(&iter, &dummy_method) == true && iter.speed == 5.0f);
    iter.klass = &g_sentinel_iter_class;

    iter.state = 0;
    iter.speed = 0.0f;
    assert(call_move_next(&iter, &dummy_method) == true && iter.speed == 0.0f);
    iter.state = 0;
    iter.speed = std::numeric_limits<float>::infinity();
    assert(call_move_next(&iter, &dummy_method) == true && std::isinf(iter.speed));
    assert(api->sample(&s) == PGR_SCAM_OK && s.fast_count == 2);

    // 4. Exit root cleanup (non-matching ignored, matching frees root)
    FakeObject other_in{&g_sentinel_in_class, &cam_other, 0, {}, 0.0f};
    call_in_exit(&other_in, &dummy_method);
    assert(api->sample(&s) == PGR_SCAM_OK && s.active_roots_mask == 3U && s.exit_count == 0);
    call_in_exit(&state_in, &dummy_method);
    assert(api->sample(&s) == PGR_SCAM_OK && s.active_roots_mask == 2U && s.exit_count == 1);
    call_out_exit(&state_out, &dummy_method);
    assert(api->sample(&s) == PGR_SCAM_OK && s.active_roots_mask == 0U && s.exit_count == 2);
    assert(g_live_handles.empty());

    // 5. Prune on epoch change, off, and lease expiry; wrong-thread tick cannot poison pending prune
    call_in_enter(&state_in, &dummy_method);
    assert(g_live_handles.size() == 1);
    // Update epoch from a non-owner thread so g_pending_prune=1 is queued without immediate owner prune
    std::thread([&]() {
        assert(api->update(1, 2000, tid, 2) == PGR_SCAM_OK);
        // Wrong-thread tick must be a no-op and must NOT clear g_pending_prune
        assert(api->tick( tid ) == PGR_SCAM_OK);
        assert(api->tick(this_tid()) == PGR_SCAM_OK);
    }).join();
    assert(g_live_handles.size() == 1);
    assert(api->tick(tid) == PGR_SCAM_OK);
    assert(api->sample(&s) == PGR_SCAM_OK && s.active_roots_mask == 0U && s.prune_count == 1);
    assert(g_live_handles.empty());

    // Lease expiry prune
    call_in_enter(&state_in, &dummy_method);
    assert(api->update(1, 1, tid, 2) == PGR_SCAM_OK);
    ::usleep(5000);
    assert(api->tick(tid) == PGR_SCAM_OK);
    assert(api->sample(&s) == PGR_SCAM_OK && s.active_roots_mask == 0U && s.prune_count == 2);

    // 6. Policy thread-change rejected while cleanup still works
    assert(api->update(1, 2000, tid, 3) == PGR_SCAM_OK);
    call_in_enter(&state_in, &dummy_method);
    assert(g_live_handles.size() == 1);
    assert(api->update(1, 2000, tid + 999, 3) == PGR_SCAM_ERR_BAD_CONFIG);
    assert(api->tick(tid) == PGR_SCAM_OK);
    assert(api->sample(&s) == PGR_SCAM_OK && s.active_roots_mask == 0U && s.prune_count == 3);

    // 7. Partial root failure (gchandle_new failure on Enter and on Iterator) without leaks
    assert(api->update(1, 2000, tid, 4) == PGR_SCAM_OK);
    g_fail_gchandle_new_countdown = 0;
    call_in_enter(&state_in, &dummy_method);
    assert(api->sample(&s) == PGR_SCAM_OK);
    assert(s.last_error == PGR_SCAM_RTERR_GCHANDLE_ENTER && s.error_serial == 1 && g_live_handles.empty());

    call_in_enter(&state_in, &dummy_method);
    assert(g_live_handles.size() == 1);
    iter.camera = &cam_in;
    iter.state = 0;
    iter.speed = 5.0f;
    g_fail_gchandle_new_countdown = 0; // fail iterator handle allocation
    assert(call_move_next(&iter, &dummy_method) == true);
    assert(iter.speed == 5.0f);
    assert(api->sample(&s) == PGR_SCAM_OK);
    assert(s.last_error == PGR_SCAM_RTERR_GCHANDLE_ITER && s.error_serial == 2);
    assert(g_live_handles.size() == 1);

    // Final disable cleans remaining slot root
    assert(api->update(0, 0, tid, 4) == PGR_SCAM_OK);
    assert(g_live_handles.empty());

    std::printf("spin_camera_test passed\n");
    return 0;
}
