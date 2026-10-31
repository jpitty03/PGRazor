// Standalone host/NDK fixture. Compile this TU only, with an Android log shim
// on a desktop host, or -llog -ldl for an Android executable. Never package it.
#include <cassert>
#include <cstdio>
#include <map>
#include "buddy_emotion.cpp"

namespace {
std::map<uintptr_t, void*> test_roots;
uintptr_t test_handle = 1;
unsigned test_allocations = 0, test_frees = 0, test_polls = 0, test_events = 0;
unsigned test_account_slot_reads = 0, test_live_slot_reads = 0;
int test_objects[3]{};
int enum_result=0,enum_normal=11,enum_loot=21,enum_souvenir=22,enum_field=0;
void* enum_class(void*){return &enum_field;}
void* enum_lookup(void*,const char* name){
 if(!strcmp(name,"Result"))return &enum_field;
 if(!strcmp(name,"Success"))return &enum_normal;
 if(!strcmp(name,"SuccessAddedLootToInventory"))return &enum_loot;
 if(!strcmp(name,"SuccessAddedSouvenirToCollections"))return &enum_souvenir;
 return nullptr;
}
int enum_flags(void*){return 0x8050;}
void enum_read(void*,void*,void* out){*static_cast<int*>(out)=enum_result;}
void enum_static(void* field,void* out){*static_cast<int*>(out)=*static_cast<int*>(field);}
int test_result = PGR_VR_DONE;
bool test_live = true, test_map = true, test_ready = true, test_fresh = true;
bool test_sequence_change = false;
unsigned test_samples = 0;
std::string test_config = "1;1;703;50;0";

int test_sample(PgrRuntimeTransportSample* out) {
    *out = {};
    out->snapshot_fresh = test_fresh;
    out->snapshot_seq = 100 + (test_sequence_change ? test_samples : 0);
    ++test_samples;
    return 0;
}
int test_bool(uint32_t field, int* out) {
    switch (field) {
    case 0: ++test_account_slot_reads; *out = 1; break;
    case 9: ++test_live_slot_reads; *out = test_live; break;
    case 16: *out = test_ready; break;
    case 38: *out = test_map; break;
    default: assert(false && "unexpected permission slot");
    }
    return 0;
}
int test_string(uint32_t field, char* out, uint32_t capacity, uint32_t* len) {
    assert(field == 9 || field == 10);
    const std::string value = field == 9 ? test_config : "";
    *len = static_cast<uint32_t>(value.size());
    assert(capacity > *len);
    memcpy(out, value.data(), *len);
    out[*len] = 0;
    return 0;
}
uintptr_t test_new(void* object, int pinned) {
    assert(object && !pinned);
    const auto handle = test_handle++;
    assert(test_roots.emplace(handle, object).second);
    ++test_allocations;
    return handle;
}
void* test_get(uintptr_t handle) {
    auto it = test_roots.find(handle);
    assert(it != test_roots.end());
    return it->second;
}
void test_free(uintptr_t handle) {
    assert(test_roots.erase(handle) == 1);
    ++test_frees;
}
void* test_forbidden_class(void*) {
    assert(false && "canceled or rejected response must not be interpreted");
    return nullptr;
}
void* test_forbidden_field(void*, const char*) {
    assert(false && "canceled response must not read field metadata");
    return nullptr;
}
int test_emit(uint32_t opcode, const int64_t* scalars, uint32_t scalar_count,
              const PgrRuntimeTransportString* strings, uint32_t string_count) {
    assert(opcode == 19 && !scalars && !scalar_count && strings && string_count == 2);
    ++test_events;
    return 0;
}
int test_poll(void* promise, uint32_t phase, uint64_t tid, PgrVgpResultsResult* out) {
    assert(promise == &test_objects[0] && phase == 1 && tid == owner);
    ++test_polls;
    *out = {};
    if (test_result == PGR_VR_DONE) {
        out->version = 1;
        out->size = sizeof(*out);
        out->value_handle = test_new(&test_objects[2], 0);
    }
    return test_result;
}
PgrRuntimeTransportApi test_transport{};
PgrVgpResultsApi test_results{1, sizeof(PgrVgpResultsApi), nullptr, test_poll, nullptr};

void setup_transport() {
    test_transport = {};
    test_transport.sample = test_sample;
    test_transport.get_bool = test_bool;
    test_transport.get_string = test_string;
    test_transport.emit = test_emit;
    test_config = "1;1;703;50;0";
    test_live = test_map = test_ready = test_fresh = true;
    test_sequence_change = false;
    test_samples = 0;
}
Permission setup(int phase) {
    assert(test_roots.empty());
    clear();
    setup_transport();
    test_allocations = test_frees = test_polls = test_events = 0;
    test_result = PGR_VR_DONE;
    boot = {};
    boot.runtime.gchandle_new = reinterpret_cast<void*>(test_new);
    boot.runtime.gchandle_get_target = reinterpret_cast<void*>(test_get);
    boot.runtime.gchandle_free = reinterpret_cast<void*>(test_free);
    boot.runtime.object_get_class = reinterpret_cast<void*>(test_forbidden_class);
    boot.meta.class_get_field_from_name = reinterpret_cast<void*>(test_forbidden_field);
    initialized = true;
    busy = paused = false;
    owner = static_cast<uint64_t>(gettid());
    history = {};
    history.buddy = 42;
    account = "fixture-account";
    status.clear();
    next_action = next_poll = 0;
    feed_burst = 3;
    pending.promise = Root(&test_objects[0]);
    pending.reflection = Root(&test_objects[1]);
    pending.phase = phase;
    pending.config = test_config;
    pending.account = account;
    pending.buddy = history.buddy;
    pending.since = clock_ms();
    return permission();
}
void cleared(unsigned polls, unsigned events, unsigned allocations) {
    assert(!pending.phase && !pending.promise.h && !pending.reflection.h);
    assert(test_roots.empty() && test_allocations == allocations && test_frees == allocations);
    assert(test_polls == polls && test_events == events && feed_burst == 0);
    assert(history.interaction == 0);
}
}

extern "C" const void* pgrazor_runtime_bootstrap_api(uint32_t) {
    assert(false && "fixture must not bootstrap"); return nullptr;
}
extern "C" const void* pgrazor_runtime_metadata_api(uint32_t) {
    assert(false && "fixture must not resolve metadata"); return nullptr;
}
extern "C" const void* pgrazor_runtime_transport_api(uint32_t version) {
    assert(version == 1); return &test_transport;
}
extern "C" const void* pgrazor_vgp_results_api(uint32_t version) {
    assert(version == 1); return &test_results;
}

int main() {
    boot.runtime.object_get_class=reinterpret_cast<void*>(enum_class);
    boot.meta.class_get_field_from_name=reinterpret_cast<void*>(enum_lookup);
    boot.meta.class_from_type=reinterpret_cast<void*>(enum_class);
    boot.meta.field_get_type=reinterpret_cast<void*>(enum_class);
    boot.meta.field_get_flags=reinterpret_cast<void*>(enum_flags);
    boot.runtime.field_get_value=reinterpret_cast<void*>(enum_read);
    boot.runtime.field_static_get_value=reinterpret_cast<void*>(enum_static);
    enum_result=enum_normal;assert(success(&enum_field)&&!success(&enum_field,true));
    enum_result=enum_loot;assert(success(&enum_field,true)&&!success(&enum_field));
    enum_result=enum_souvenir;assert(success(&enum_field,true));
    enum_result=0;assert(!success(&enum_field,true));
    setup_transport();
    test_live = false;
    assert(!permission().allowed);
    assert(test_live_slot_reads && !test_account_slot_reads);
    test_live = true;
    assert(permission().allowed);
    test_ready = false; assert(!permission().allowed); test_ready = true;
    test_map = false; assert(!permission().allowed); test_map = true;
    test_fresh = false; assert(!permission().valid); test_fresh = true;
    test_sequence_change = true; assert(!permission().valid); test_sequence_change = false;

    for (int phase : {1, 2, 3, 4, 5}) {
        for (int reason = 0; reason < 5; ++reason) {
            auto p = setup(phase);
            switch (reason) {
            case 0: test_config = "1;0;703;50;0"; p = permission(); break;
            case 1: test_map = false; p = permission(); break;
            case 2: test_config = "1;1;705;50;0"; p = permission(); break;
            case 3: ++history.buddy; break;
            case 4: account = "other-account"; break;
            }
            // DONE still adopts/frees its returned root, but never inspects it.
            poll(p);
            cleared(1, 0, 3);
            assert(!paused && next_action > clock_ms());
        }
    }
    {
        auto p = setup(2);
        test_result = PGR_VR_PENDING;
        p.allowed = false;
        poll(p);
        assert(pending.canceled && pending.phase == 2 && test_roots.size() == 2);
        test_result = PGR_VR_DONE;
        p.allowed = true;
        poll(p); // Permission returning cannot uncancel an already canceled attempt.
        cleared(2, 0, 3);
    }
    for (int result : {PGR_VR_REJECTED, PGR_VR_ERR_SHAPE}) {
        auto p = setup(3);
        test_result = result;
        poll(p);
        cleared(1, 1, 2);
        assert(paused && status.find("rejected") != std::string::npos);
    }
    {
        auto p = setup(3);
        pending.since = clock_ms() - 15001;
        poll(p);
        cleared(0, 1, 2);
        assert(paused && status.find("timed out") != std::string::npos);
        // A later tick cannot recreate a promise while paused, even with the
        // enable switch retained. A stale transport avoids identity resolution.
        test_fresh = false;
        pgr_buddy_emotion_tick(owner, nullptr);
        pgr_buddy_emotion_tick(owner, nullptr);
        cleared(0, 1, 2);
        assert(paused);
    }
    for(bool timeout:{false,true}){
        auto p=setup(4);paused=true;next_action=999999;last_stats=999999;
        adopt_buddy(account,43,"");
        assert(!paused&&pending.canceled&&history.buddy==43&&last_stats==0&&next_action==0&&feed_burst==0);
        if(timeout)pending.since=clock_ms()-15001;else test_result=PGR_VR_REJECTED;
        poll(p);cleared(timeout?0:1,1,2);assert(!paused);
    }
    {
        auto p=setup(3);clear();paused=true;
        History saved{43,{10,20,30,40},50};adopt_buddy("another-account",43,"another-account|"+encode(saved));
        assert(!paused&&encode(history)==encode(saved));
        paused=true;adopt_buddy("another-account",43,"");assert(paused); // Same buddy cannot erase an unresolved pause.
    }
    std::puts("PASS: buddy runtime permissions, gift enums, buddy swap recovery, cancellation, roots, rejection, timeout pause");
}
