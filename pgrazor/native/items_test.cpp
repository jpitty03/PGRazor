// Standalone Android (x86_64 host) test of the real native items module. Modelled on
// buddy_feeding_test.cpp (real Dobby hooks against NOP-padded targets) and incubation_test.cpp
// (fork-per-scenario for clean process-lifetime state). It dlopen()s the built core, pulls the
// pgrazor_native_api v2 table and asks query_feature(14, 1) for the items API (it invents no
// getter), installs against real init/list/recycle/result/error targets, and drives a compact
// fake IL2CPP that models FindObjectOfType / get_Container / DiContainer.Resolve (service +
// gui), the singleton static field, CreateDelegate, the service -> bag field, the ItemData list
// (get_Count / get_Item), GetItemCount, ExpiringItemsCopy, RecycleItem and the result/error
// callbacks. The fake RecycleItem re-enters the hooked recycle target (so our own recycle is
// suppressed as non-manual) and can synchronously deliver a result or an error through the
// hooked callback targets.
//
// This is a SECOND DRAFT harness: request/confirm/error now come from the module's bounded FIFO
// event queue (drain()) instead of shared "last_*" sample fields, and several scenarios are
// regressions for concrete first-draft defects -- the pending bag is a durable root matched by
// identity (no address-reuse confirm), uncertain outcomes are quarantined for the process (not
// cleared by a config generation change) while definite rejections are settings-scoped, a nested
// tick with a new cap/epoch revokes the in-flight selection, a stop during a result callback does
// not confirm, and VGP's scoped service_acquire() hands back an independently-owned GC handle.
//
// Coverage (the task's minimum set + the regressions): over-cap recycle, live-quantity recheck,
// catalog with auto-recycle disabled, synchronous success, in-flight no-duplicate with the pending
// bag root counted, manual-recycle ambiguity + uncertain quarantine surviving a generation change,
// result failure (block, no retry) + the block clearing on a generation change, reentrant clear
// and reentrant disable during a managed call, a nested tick with a new epoch revoking selection,
// result identity mismatch not confirming, a stop during the result callback not confirming, the
// scoped owned service handle (acquire/free + foreign-thread refusal), a foreign thread that
// cannot steal the owner, balanced durable roots after clear, immutable-config rebind refusal,
// the static/instance ABI contract, and (slow) the confirmation-deadline timeout. The service plus
// the two filter delegates are durable roots retained until clear(), so a "balanced" idle state is
// owned_roots == 3 outside a pass and 0 only after clear(). This harness is never packaged.
#include "items.h"
#include "pgrazor_native_api.h"
#include <dlfcn.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>
#include <ctime>
#include <cstdio>
#include <cstdint>
#include <cstring>

// ---- symbolic il2cpp identities (compared by address only) -----------------------
static int c_map, c_ctx, c_svc_contract, c_service, c_gui_contract, c_gui, c_singleton,
           c_bag, c_data, c_callback, c_response, c_int, c_list, c_pred, c_itemproto, c_functype;
static int f_bag, f_id, f_count, f_recyclable, f_cbbag, f_prediction, f_protoid, f_status,
           f_singleton_inst;
static int m_find, m_container, m_resolve, m_list, m_count, m_get, m_quantity, m_expiring,
           m_recycle, m_proto, m_create_delegate, m_usable, m_show, m_init, m_result, m_error,
           c_reflection, g_delegate0, g_delegate1, g_expiry, g_promise;

struct Obj  { void *klass; };
struct Svc  { void *klass; void *bag; };
struct Data { void *klass; int32_t id; int32_t count; uint8_t recyclable; };
struct List { void *klass; };
struct Cb   { void *klass; void *bag; void *prediction; };
struct Proto{ void *klass; int32_t item; };
struct Resp { void *klass; int32_t status; };
struct Box  { void *klass; union { uint8_t b; int32_t i; } v; };

// ---- scenario knobs / observations (per-process; fork resets them) ---------------
static bool    g_sync_result = false, g_bad_free = false;
static int32_t g_result_status = 1;     // success literal == 1
static int32_t g_current = 50;          // live GetItemCount for the over-cap item
static int     g_roots = 0, g_recycle_calls = 0, g_delegates = 0;
static int32_t g_last_recycle_id = 0, g_last_recycle_amount = 0;
static int     g_quantity_action = 0;   // 0 none, 1 reentrant clear, 2 reentrant disable, 3 new-epoch tick
static int g_bag_allocations = 0, g_cancel_bag_allocation = 0;
static bool g_cancel_service_allocation = false;
static int     g_result_action = 0;     // 0 none, 1 clear() from inside the original result callback
static const pgrazor_items_api_v1 *g_api = nullptr;

// Drained-event tallies (pumped out of the module's FIFO queue).
static int     ev_req = 0, ev_con = 0, ev_err = 0;
static int32_t last_req_id = 0, last_req_amount = 0, last_req_keep = 0;
static int32_t last_con_id = 0, last_con_amount = 0, last_err_reason = 0, last_err_id = 0;

static Obj   g_map{&c_map}, g_container{&c_ctx}, g_bag{&c_bag}, g_gui{&c_gui},
             g_singleton{&c_singleton}, g_list{&c_list}, g_pred{&c_pred};
static Svc   g_service{&c_service, &g_bag};
static Cb    g_callback{&c_callback, &g_bag, &g_pred};
static Proto g_itemproto{&c_itemproto, 101};
static Resp  g_response{&c_response, 1};
static Box   g_box;

// Fake inventory: a recyclable over-cap item (101) and a small recyclable item (202).
static Data g_inv[512] = { {&c_data, 101, 50, 1}, {&c_data, 202, 5, 1} };
static int32_t g_list_count = 2;
static int g_list_calls = 0, g_get_calls = 0, g_quantity_calls = 0;
static int32_t g_quantity_ids[4096]{};

// ---- fake il2cpp primitives ------------------------------------------------------
static void *obj_class(void *o) { return o ? *static_cast<void **>(o) : nullptr; }
static void *gc_new(void *o, int) {
    if (!o) return nullptr;
    ++g_roots;
    const bool cancelBag = o == &g_bag && ++g_bag_allocations == g_cancel_bag_allocation;
    const bool cancelService = o == &g_service && g_cancel_service_allocation;
    if (cancelBag || cancelService) {
        g_cancel_service_allocation = false;
        g_api->clear(static_cast<uint64_t>(gettid()));
    }
    return o;
}
static void *gc_target(void *h) { return h; }
static void gc_free(void *h) {
    if (h) { pgrazor_items_sample_v1 v{}; g_api->sample(&v); if (!v.owned_roots) g_bad_free = true; --g_roots; }
}
static void *object_unbox(void *o) { return &static_cast<Box *>(o)->v; }
static void *class_get_type(void *k) { return k; }
static void *type_get_object(void *t) { return t; }
static void runtime_class_init(void *) {}
static void *method_get_object(void *, void *) { return &c_reflection; }
static void field_static_get(void *f, void *out) {
    if (f == &f_singleton_inst) *static_cast<void **>(out) = &g_singleton;
}
static void field_get(void *o, void *f, void *out) {
    if      (f == &f_bag)        *static_cast<void **>(out)    = static_cast<Svc *>(o)->bag;
    else if (f == &f_id)         *static_cast<int32_t *>(out)  = static_cast<Data *>(o)->id;
    else if (f == &f_count)      *static_cast<int32_t *>(out)  = static_cast<Data *>(o)->count;
    else if (f == &f_recyclable) *static_cast<uint8_t *>(out)  = static_cast<Data *>(o)->recyclable;
    else if (f == &f_cbbag)      *static_cast<void **>(out)    = static_cast<Cb *>(o)->bag;
    else if (f == &f_prediction) *static_cast<void **>(out)    = static_cast<Cb *>(o)->prediction;
    else if (f == &f_protoid)    *static_cast<int32_t *>(out)  = static_cast<Proto *>(o)->item;
    else if (f == &f_status)     *static_cast<int32_t *>(out)  = static_cast<Resp *>(o)->status;
}

// Real hooked targets (NOP-padded so Dobby has room). call_* go through the trampoline.
#define PGR_NOP8() asm volatile("nop; nop; nop; nop; nop; nop; nop; nop;")
__attribute__((noinline)) void target_init(void *self, void *method) { PGR_NOP8(); (void)self; (void)method; }
__attribute__((noinline)) void *target_list(void *self, void *p0, int32_t p1, void *p2, void *p3, void *p4, void *method) {
    PGR_NOP8(); (void)self; (void)p0; (void)p1; (void)p2; (void)p3; (void)p4; (void)method; return &g_list;
}
__attribute__((noinline)) void *target_recycle(void *self, void *data, int32_t count, void *expiry, void *method) {
    PGR_NOP8(); ++g_recycle_calls; g_last_recycle_id = data ? static_cast<Data*>(data)->id : 0;
    g_last_recycle_amount = count; (void)self; (void)expiry; (void)method; return &g_promise;
}
__attribute__((noinline)) void target_result(void *self, void *resp, void *method) {
    PGR_NOP8(); (void)self; (void)resp; (void)method;
    // A stop (clear) delivered from INSIDE the original result callback must not let the hook
    // confirm this (or any) request afterwards.
    if (g_result_action == 1) { g_result_action = 0; g_api->clear(static_cast<uint64_t>(gettid())); }
}
__attribute__((noinline)) void target_error(void *self, void *str, void *method) { PGR_NOP8(); (void)self; (void)str; (void)method; }
void  (*volatile call_init)(void *, void *) = &target_init;
void *(*volatile call_list)(void *, void *, int32_t, void *, void *, void *, void *) = &target_list;
void *(*volatile call_recycle)(void *, void *, int32_t, void *, void *) = &target_recycle;
void  (*volatile call_result)(void *, void *, void *) = &target_result;
void  (*volatile call_error)(void *, void *, void *) = &target_error;

static void *runtime_invoke(void *method, void *obj, void **params, void **exc) {
    asm volatile("nop; nop; nop; nop" ::: "memory");
    *exc = nullptr;
    const bool stat = method == &m_find || method == &m_create_delegate;
    if ((stat && obj) || (!stat && !obj)) _exit(80);   // static/instance ABI contract
    if (method == &m_find)      return &g_map;
    if (method == &m_container) return &g_container;
    if (method == &m_resolve) {
        void *t = params ? params[0] : nullptr;
        if (t == &c_svc_contract) return &g_service;
        if (t == &c_gui_contract) return &g_gui;
        return nullptr;
    }
    if (method == &m_create_delegate) return (++g_delegates == 1) ? static_cast<void *>(&g_delegate0)
                                                                  : static_cast<void *>(&g_delegate1);
    if (method == &m_list) {
        ++g_list_calls;
        void *p0 = params ? params[0] : nullptr, *p2 = params ? params[2] : nullptr,
             *p3 = params ? params[3] : nullptr, *p4 = params ? params[4] : nullptr;
        int32_t p1 = params ? *static_cast<int32_t *>(params[1]) : 0;
        return call_list(obj, p0, p1, p2, p3, p4, &m_list);  // re-enter the hooked list target
    }
    if (method == &m_count)    { g_box.klass = &c_int; g_box.v.i = g_list_count; return &g_box; }
    if (method == &m_get) {
        ++g_get_calls;
        int32_t i = params ? *static_cast<int32_t *>(params[0]) : 0;
        return (i >= 0 && i < g_list_count) ? static_cast<void *>(&g_inv[i]) : nullptr;
    }
    if (method == &m_quantity) {
        if (g_quantity_calls < 4096) g_quantity_ids[g_quantity_calls] = params ? *static_cast<int32_t*>(params[0]) : 0;
        ++g_quantity_calls;
        if (g_quantity_action) {
            int a = g_quantity_action; g_quantity_action = 0;
            if (a == 1) g_api->clear(static_cast<uint64_t>(gettid()));
            else if (a == 2) { pgrazor_items_rules_v1 off{}; off.version = PGRAZOR_ITEMS_API_VERSION;
                   off.size = sizeof(off); g_api->tick(&off, 0, 2500, static_cast<uint64_t>(gettid())); }
            else { // a == 3: nested tick, STILL enabled but a new epoch -> must revoke selection
                   pgrazor_items_rules_v1 nr{}; nr.version = PGRAZOR_ITEMS_API_VERSION;
                   nr.size = sizeof(nr); nr.enabled = 1u; nr.generation = 1; nr.epoch = 999;
                   nr.count = 1; nr.entries[0].id = 101; nr.entries[0].cap = 10;
                   g_api->tick(&nr, 0, 2500, static_cast<uint64_t>(gettid())); }
        }
        g_box.klass = &c_int; g_box.v.i = g_current; return &g_box;
    }
    if (method == &m_expiring) return &g_expiry;
    if (method == &m_recycle) {
        void *pr = call_recycle(obj, params ? params[0] : nullptr,
                                params ? *static_cast<int32_t *>(params[1]) : 0,
                                params ? params[2] : nullptr, &m_recycle);   // re-enter the hooked recycle target
        if (g_sync_result) {
            g_response.status = g_result_status; call_result(&g_callback, &g_response, &m_result);
        }
        return pr;
    }
    if (method == &m_proto)    return &g_itemproto;
    return nullptr;
}

// ---- config + rules helpers ------------------------------------------------------
static pgrazor_items_config_v1 make_config(bool lookup) {
    pgrazor_items_config_v1 c{};
    c.version = PGRAZOR_ITEMS_API_VERSION; c.size = sizeof(c); c.has_lookup = lookup ? 1u : 0u;
    c.runtime_invoke = (void *)&runtime_invoke; c.gchandle_new = (void *)&gc_new;
    c.gchandle_get_target = (void *)&gc_target; c.gchandle_free = (void *)&gc_free;
    c.object_get_class = (void *)&obj_class; c.object_unbox = (void *)&object_unbox;
    c.field_get_value = (void *)&field_get;
    c.m_list = &m_list; c.m_count = &m_count; c.m_get = &m_get; c.m_quantity = &m_quantity;
    c.m_expiring = &m_expiring; c.m_recycle = &m_recycle; c.m_proto = &m_proto;
    c.init_address = (void *)&target_init; c.list_address = (void *)&target_list;
    c.recycle_address = (void *)&target_recycle; c.result_address = (void *)&target_result;
    c.error_address = (void *)&target_error;
    c.class_service = &c_service; c.class_bag = &c_bag; c.class_data = &c_data;
    c.class_callback = &c_callback; c.class_response = &c_response; c.class_int = &c_int;
    c.f_bag = &f_bag; c.f_id = &f_id; c.f_count = &f_count; c.f_recyclable = &f_recyclable;
    c.f_callback_bag = &f_cbbag; c.f_prediction = &f_prediction; c.f_proto_id = &f_protoid;
    c.f_status = &f_status;
    if (lookup) {
        c.class_get_type = (void *)&class_get_type; c.type_get_object = (void *)&type_get_object;
        c.field_static_get_value = (void *)&field_static_get; c.runtime_class_init = (void *)&runtime_class_init;
        c.method_get_object = (void *)&method_get_object;
        c.m_find = &m_find; c.m_container = &m_container; c.m_resolve = &m_resolve;
        c.m_create_delegate = &m_create_delegate; c.m_usable = &m_usable; c.m_show = &m_show;
        c.class_map = &c_map; c.class_service_contract = &c_svc_contract;
        c.class_gui_contract = &c_gui_contract; c.class_gui = &c_gui; c.class_singleton = &c_singleton;
        c.f_singleton_instance = &f_singleton_inst; c.func_type = &c_functype;
    }
    c.success = 1; c.reserved1 = 0;
    return c;
}

static pgrazor_items_rules_v1 make_rules(bool enabled, uint64_t gen) {
    pgrazor_items_rules_v1 r{};
    r.version = PGRAZOR_ITEMS_API_VERSION; r.size = sizeof(r);
    r.enabled = enabled ? 1u : 0u; r.generation = gen; r.epoch = 1;
    r.count = 1; r.entries[0].id = 101; r.entries[0].cap = 10;  // keep 10 of item 101
    return r;
}

static pgrazor_items_sample_v1 snap() { pgrazor_items_sample_v1 s{}; g_api->sample(&s); return s; }

// Drain the module's FIFO event queue into the per-kind tallies.
static void pump_events() {
    pgrazor_item_event buf[PGRAZOR_ITEMS_MAX_EVENTS]; uint32_t w = 0;
    while (g_api->drain(buf, PGRAZOR_ITEMS_MAX_EVENTS, &w) == PGR_ITEMS_OK && w > 0) {
        for (uint32_t i = 0; i < w; ++i) {
            const pgrazor_item_event &e = buf[i];
            if (e.kind == PGR_ITEMS_EV_REQUEST) { ++ev_req; last_req_id = e.id; last_req_amount = e.amount; last_req_keep = e.aux; }
            else if (e.kind == PGR_ITEMS_EV_CONFIRM) { ++ev_con; last_con_id = e.id; last_con_amount = e.amount; }
            else if (e.kind == PGR_ITEMS_EV_ERROR) { ++ev_err; last_err_id = e.id; last_err_reason = e.aux; }
        }
        if (w < PGRAZOR_ITEMS_MAX_EVENTS) break;
    }
}

// ---- scenarios -------------------------------------------------------------------
static int scenario(int which) {
    const uint64_t tid = static_cast<uint64_t>(gettid());
    pgrazor_items_config_v1 cfg = make_config(true);
    if (g_api->install(&cfg) != PGR_ITEMS_OK) return 90;

    switch (which) {
    case 0: { // over-cap + synchronous success result consumed once; roots balanced to 3
        g_sync_result = true; g_result_status = 1; g_current = 50;
        pgrazor_items_rules_v1 r = make_rules(true, 1);
        g_api->tick(&r, 0, 2500, tid); pump_events();      // lookup + enumerate + recycle + sync result
        if (ev_req != 1 || g_recycle_calls != 1) return 1;
        if (ev_con != 1 || last_con_id != 101 || last_con_amount != 40) return 2;
        pgrazor_items_sample_v1 s = snap();
        if (s.catalog_serial == 0 || s.catalog_count != 2) return 3;
        if (s.owned_roots != 3 || g_roots != 3) return 4;  // service + two filter delegates
        g_api->clear(tid);
        if (snap().owned_roots != 0 || g_roots != 0 || g_bad_free) return 5;
        return 0;
    }
    case 1: { // live-quantity recheck: list says 50 but the bag already has <= cap -> no recycle
        g_sync_result = true; g_current = 8;               // GetItemCount(101) == 8 <= cap 10
        pgrazor_items_rules_v1 r = make_rules(true, 1);
        g_api->tick(&r, 2, 2500, tid); pump_events();
        if (ev_req != 0 || g_recycle_calls != 0) return 1;
        pgrazor_items_sample_v1 s = snap();
        if (s.has_candidate != 1) return 2;                // candidate found, but recycle declined
        if (s.catalog_count != 2) return 3;
        return 0;
    }
    case 2: { // explicit UI catalog while auto-recycle is disabled
        pgrazor_items_rules_v1 r = make_rules(false, 1);
        g_api->tick(&r, 2, 2500, tid); pump_events();
        if (ev_req != 0 || g_recycle_calls != 0) return 1;
        pgrazor_items_sample_v1 s = snap();
        if (s.catalog_serial == 0 || s.catalog_count != 2) return 2;
        if (s.has_candidate != 0) return 3;
        return 0;
    }
    case 3: { // in-flight async request is never auto-duplicated; the pending bag is a counted root
        g_sync_result = false; g_current = 50;
        pgrazor_items_rules_v1 r = make_rules(true, 1);
        g_api->tick(&r, 0, 2500, tid); pump_events();
        if (ev_req != 1 || g_recycle_calls != 1) return 1;
        if (snap().owned_roots != 4 || g_roots != 4) return 2; // service + 2 filters + pending bag
        g_api->tick(&r, 1, 2500, tid); pump_events();      // force re-poll; still pending
        if (ev_req != 1 || g_recycle_calls != 1) return 3;
        g_api->clear(tid);
        if (g_roots != 0 || g_bad_free) return 4;          // pending bag root freed on clear
        return 0;
    }
    case 4: { // manual recycle while pending -> ambiguous: no attribution, uncertain quarantine
        g_sync_result = false; g_current = 50;
        pgrazor_items_rules_v1 r = make_rules(true, 1);
        g_api->tick(&r, 0, 2500, tid); pump_events();      // our async recycle
        if (ev_req != 1) return 1;
        call_recycle(&g_bag, &g_inv[0], 1, &g_expiry, &m_recycle);     // the player's own manual recycle
        g_response.status = 1; call_result(&g_callback, &g_response, &m_result); // result for our rpc
        pump_events();
        if (ev_con != 0) return 2;                         // dropped -> never attributed
        g_api->tick(&r, 1, 2500, tid); pump_events();
        if (ev_req != 1 || g_recycle_calls != 2) return 3; // (1 ours + 1 manual) no resubmit
        // The uncertain quarantine survives a config generation change (unlike a definite block).
        pgrazor_items_rules_v1 r2 = make_rules(true, 2);
        g_api->tick(&r2, 1, 2500, tid); pump_events();
        if (ev_req != 1 || g_recycle_calls != 2) return 4;
        return 0;
    }
    case 5: { // result failure -> error (REJECTED) + blocked; the block clears on a generation change
        g_sync_result = true; g_result_status = 7; g_current = 50;  // not success(1)
        pgrazor_items_rules_v1 r = make_rules(true, 1);
        g_api->tick(&r, 0, 2500, tid); pump_events();
        if (ev_err != 1 || last_err_reason != PGR_ITEMS_REASON_REJECTED) return 1;
        if (ev_con != 0) return 2;
        g_api->tick(&r, 1, 2500, tid); pump_events();      // blocked id -> no retry (same generation)
        if (ev_req != 1 || g_recycle_calls != 1) return 3;
        g_result_status = 1;                               // now succeeds
        pgrazor_items_rules_v1 r2 = make_rules(true, 2);   // a new generation clears the block
        g_api->tick(&r2, 1, 2500, tid); pump_events();
        if (ev_req != 2 || g_recycle_calls != 2 || ev_con != 1) return 4;
        return 0;
    }
    case 6: { // reentrant clear during a managed call: no recycle, roots freed, no bad free
        g_sync_result = false; g_current = 50; g_quantity_action = 1;
        pgrazor_items_rules_v1 r = make_rules(true, 1);
        g_api->tick(&r, 0, 2500, tid); pump_events();
        if (ev_req != 0 || g_recycle_calls != 0) return 1;
        if (g_roots != 0 || snap().owned_roots != 0 || g_bad_free) return 2; // deferred clear freed roots
        return 0;
    }
    case 7: { // reentrant disable during a managed call: revoke -> no recycle
        g_sync_result = false; g_current = 50; g_quantity_action = 2;
        pgrazor_items_rules_v1 r = make_rules(true, 1);
        g_api->tick(&r, 0, 2500, tid); pump_events();
        if (ev_req != 0 || g_recycle_calls != 0) return 1;
        g_api->clear(tid); if (g_roots != 0 || g_bad_free) return 2;
        return 0;
    }
    case 8: { // foreign thread cannot steal ownership or latch the Unity identity
        pgrazor_items_rules_v1 r = make_rules(false, 1);
        g_api->tick(&r, 0, 2500, tid);                     // main latches Unity + builds roots
        const int before = g_roots; if (before != 3) return 1;
        pthread_t th;
        if (pthread_create(&th, nullptr, [](void *) -> void * {
            pgrazor_items_rules_v1 rr = make_rules(true, 1);
            g_api->tick(&rr, 0, 2500, static_cast<uint64_t>(gettid()));  // foreign tid
            g_api->clear(static_cast<uint64_t>(gettid()));
            return nullptr; }, nullptr) != 0) return 2;
        pthread_join(th, nullptr);
        if (g_roots != before) return 3;                   // foreign thread froze nothing / freed nothing
        g_api->clear(tid); if (g_roots != 0) return 4;
        return 0;
    }
    case 9: { // immutable-config rebind refusal + idempotent re-install
        pgrazor_items_config_v1 changed = make_config(true); changed.success = 7;
        if (g_api->install(&changed) != PGR_ITEMS_ERR_ALREADY_BOUND) return 1;
        if (g_api->install(&cfg) != PGR_ITEMS_OK) return 2;  // identical config is idempotent
        return 0;
    }
    case 10: { // nested tick, STILL enabled but a new epoch, revokes the in-flight selection
        g_sync_result = false; g_current = 50; g_quantity_action = 3;
        pgrazor_items_rules_v1 r = make_rules(true, 1);    // in-progress epoch == 1
        g_api->tick(&r, 0, 2500, tid); pump_events();
        if (ev_req != 0 || g_recycle_calls != 0) return 1; // new cap/epoch during the pass -> revoked
        g_api->clear(tid); if (g_roots != 0 || g_bad_free) return 2;
        return 0;
    }
    case 11: { // result identity mismatch does not confirm; a matching one does
        g_sync_result = false; g_current = 50;
        pgrazor_items_rules_v1 r = make_rules(true, 1);
        g_api->tick(&r, 0, 2500, tid); pump_events();      // async pending for id 101
        if (ev_req != 1) return 1;
        g_itemproto.item = 999; g_response.status = 1;
        call_result(&g_callback, &g_response, &m_result);  // wrong predicted id -> not matched
        pump_events();
        if (ev_con != 0) return 2;                         // identity guard: no confirmation
        g_itemproto.item = 101;
        call_result(&g_callback, &g_response, &m_result);  // correct identity -> confirmed
        pump_events();
        if (ev_con != 1 || last_con_id != 101) return 3;
        g_api->clear(tid); if (g_roots != 0 || g_bad_free) return 4;
        return 0;
    }
    case 12: { // a stop delivered INSIDE the original result callback must not confirm
        g_sync_result = true; g_result_status = 1; g_current = 50; g_result_action = 1;
        pgrazor_items_rules_v1 r = make_rules(true, 1);
        g_api->tick(&r, 0, 2500, tid); pump_events();
        if (ev_con != 0) return 1;                         // stop during callback -> not confirmed
        if (ev_req != 1) return 2;                         // the request event still emitted
        if (g_roots != 0 || g_bad_free) return 3;          // deferred clear freed the roots
        return 0;
    }
    case 13: { // scoped service_acquire(): an independently-owned handle; foreign thread refused
        pgrazor_items_rules_v1 r = make_rules(false, 1);
        g_api->tick(&r, 0, 2500, tid); pump_events();
        if (g_roots != 3) return 1;
        void *h = g_api->service_acquire();
        if (h == nullptr) return 2;
        if (g_roots != 4) return 3;                        // acquire created its own root
        pthread_t th;
        if (pthread_create(&th, nullptr, [](void *) -> void * {
            if (g_api->service_acquire() != nullptr) _exit(77);  // foreign thread cannot acquire
            return nullptr; }, nullptr) != 0) return 4;
        pthread_join(th, nullptr);
        if (g_roots != 4) return 5;                        // foreign acquire created nothing
        gc_free(h);                                        // caller frees it (il2cpp_gchandle_free)
        if (g_roots != 3 || g_bad_free) return 6;          // freeing is independent of the durable root
        g_api->clear(tid); if (g_roots != 0) return 7;
        return 0;
    }
    case 14: { // confirmation-deadline timeout (SLOW: a real >15s wait) -> error + no duplicate
        g_sync_result = false; g_current = 50;
        pgrazor_items_rules_v1 r = make_rules(true, 1);
        g_api->tick(&r, 0, 2500, tid); pump_events();
        if (ev_req != 1) return 1;
        timespec t{16, 0}; nanosleep(&t, nullptr);         // past the 15s confirmation deadline
        r = make_rules(false, 2);
        g_api->tick(&r, 0, 0, tid); pump_events(); // timeout still serviced while disabled/off lease
        if (ev_err == 0 || last_err_reason != PGR_ITEMS_REASON_TIMEOUT) return 2;
        if (ev_req != 1 || g_recycle_calls != 1) return 3; // quarantined id -> no resubmit
        return 0;
    }
    case 15: { // cancellation from the final pending-bag allocation must precede the RPC
        g_cancel_bag_allocation = 2; // enumeration bag, then pending bag
        auto r = make_rules(true, 1);
        g_api->tick(&r, 2, 2500, tid); pump_events();
        if (g_bag_allocations != 2) return 1;
        if (g_recycle_calls || ev_req || ev_con) return 2;
        if (g_roots || g_bad_free) return 3;
        return 0;
    }
    case 16: { // a stop during service clone allocation returns no escaped handle
        auto r = make_rules(false, 1);
        g_api->tick(&r, 0, 2500, tid); pump_events();
        g_cancel_service_allocation = true;
        if (g_api->service_acquire() != nullptr) return 1;
        if (g_roots || g_bad_free) return 2;
        return 0;
    }
    case 17: { // off + VGP force discovers service without inventory enumeration/publication
        auto r = make_rules(false, 1);
        g_api->tick(&r, 1, 2500, tid);
        g_api->tick(&r, 1, 2500, tid);
        if (g_list_calls || g_get_calls || g_quantity_calls || snap().catalog_serial) return 1;
        if (!g_api->service_handle()) return 2;
        r = make_rules(true, 2); g_sync_result = true;
        g_api->tick(&r, 1, 2500, tid); pump_events();
        if (g_list_calls != 1 || g_recycle_calls != 1) return 3;
        return 0;
    }
    case 18: { // berry overlay reads five quantities, retains other rows, publishes changes only
        auto r = make_rules(false, 1);
        g_api->tick(&r, 3, 2500, tid);
        const auto serial = snap().catalog_serial;
        g_api->tick(&r, 3, 2500, tid);
        if (snap().catalog_serial != serial || g_list_calls != 2) return 1;
        g_api->tick(&r, 5, 2500, tid);
        if (g_quantity_calls != 5 || g_list_calls != 2 || g_get_calls != 4 || snap().catalog_count != 7) return 2;
        const auto berries = snap().catalog_serial;
        g_api->tick(&r, 5, 2500, tid);
        if (g_quantity_calls != 10 || snap().catalog_serial != berries) return 3;
        g_api->tick(&r, 2, 2500, tid); // mode bit alone must not bypass cadence
        if (g_list_calls != 2) return 4;
        g_api->clear(tid);
        if (g_roots || g_bad_free) return 5;
        return 0;
    }
    case 19: { // explicit initial empty catalog clears stale UI once, then stays quiet
        auto r = make_rules(false, 1); g_list_count = 0;
        g_api->tick(&r, 3, 2500, tid);
        const auto first = snap();
        if (!first.catalog_serial || first.catalog_count) return 1;
        g_api->tick(&r, 3, 2500, tid);
        if (snap().catalog_serial != first.catalog_serial) return 2;
        return 0;
    }
    case 20: { // large catalog: bounded slices, one managed list, no per-frame restart
        auto r = make_rules(true, 1); g_list_count = 257;
        for (int i = 0; i < g_list_count; ++i) g_inv[i] = Data{&c_data, 1000+i, 5, 1};
        for (int frame = 0; frame < 300 && g_get_calls < 257; ++frame) {
            int before = g_get_calls;
            g_api->tick(&r, 2, 2500, tid);
            if (g_get_calls-before > 64 || g_list_calls != 1 || g_recycle_calls) return 1;
        }
        if (g_get_calls != 257 || snap().catalog_serial == 0 || g_roots != 3) return 2;
        g_api->clear(tid); if (g_roots || g_bad_free) return 3;
        return 0;
    }
    case 21:
    case 23: { // delayed success/rejection resumes the large snapshot without a second list
        auto r = make_rules(true, 1); g_list_count = 257;
        for (int i = 0; i < g_list_count; ++i) g_inv[i] = Data{&c_data, 1000+i, 5, 1};
        g_inv[0] = Data{&c_data, 101, 50, 1};
        g_api->tick(&r, 0, 2500, tid);
        if (g_list_calls != 1 || g_recycle_calls != 1 || g_get_calls != 1) return 1;
        for (int frame = 0; frame < 5; ++frame) g_api->tick(&r, 1, 2500, tid);
        if (g_list_calls != 1 || g_recycle_calls != 1 || g_get_calls != 1) return 2;
        g_response.status = which == 23 ? 7 : 1;
        call_result(&g_callback, &g_response, &m_result); pump_events();
        if (which == 21 ? ev_con != 1 : ev_err != 1) return 3;
        for (int frame = 0; frame < 300 && g_get_calls < 257; ++frame) {
            int before = g_get_calls; g_api->tick(&r, 0, 2500, tid);
            if (g_get_calls-before > 64 || g_list_calls != 1 || g_recycle_calls != 1) return 4;
        }
        if (g_get_calls != 257 || !snap().catalog_serial || g_roots != 3) return 5;
        g_api->clear(tid); if (g_roots || g_bad_free) return 6;
        return 0;
    }
    case 22: { // interrupted large scan restarts when cap generation changes
        auto r = make_rules(true, 1); g_list_count = 257;
        for (int i = 0; i < g_list_count; ++i) g_inv[i] = Data{&c_data, 1000+i, 5, 1};
        g_api->tick(&r, 2, 2500, tid);
        ++r.generation;
        g_api->tick(&r, 3, 2500, tid);
        if (g_list_calls != 2 || g_get_calls < 2 || g_get_calls > 128 || g_recycle_calls) return 1;
        g_api->clear(tid); if (g_roots || g_bad_free) return 2;
        return 0;
    }
    case 24: { // a changing managed list count invalidates the retained cursor safely
        auto r = make_rules(true, 1); g_list_count = 257;
        for (int i = 0; i < g_list_count; ++i) g_inv[i] = Data{&c_data, 1000+i, 5, 1};
        g_api->tick(&r, 2, 2500, tid);int gets=g_get_calls;
        --g_list_count;g_api->tick(&r, 2, 2500, tid);
        if(g_get_calls!=gets||g_list_calls!=1||g_roots!=3)return 1;
        g_api->tick(&r, 2, 2500, tid);if(g_list_calls!=2||g_get_calls<=gets)return 2;
        g_api->clear(tid);if(g_roots||g_bad_free)return 3;return 0;
    }
    case 25: { // a replacement bag invalidates the list even with the same service
        auto r = make_rules(true, 1); g_list_count = 257;
        for (int i = 0; i < g_list_count; ++i) g_inv[i] = Data{&c_data, 1000+i, 5, 1};
        g_api->tick(&r, 2, 2500, tid);Obj replacement{&c_bag};g_service.bag=&replacement;
        g_api->tick(&r, 2, 2500, tid);if(g_list_calls!=2||g_recycle_calls)return 1;
        g_api->clear(tid);if(g_roots||g_bad_free)return 2;return 0;
    }
    case 26: { // 128 excess types: full amounts, serial matched results, one bounded list pass
        auto r = make_rules(true, 1); r.count = 128; g_list_count = 257;
        g_sync_result = false; g_current = 50;
        for (int i = 0; i < g_list_count; ++i) g_inv[i] = Data{&c_data, 1000+i, 5, 1};
        for (int i = 0; i < 128; ++i) {
            const int index = 2*i;
            g_inv[index].count = 50;
            r.entries[i].id = g_inv[index].id; r.entries[i].cap = 10;
        }
        int removed = 0;
        for (int frame = 0; frame < 600 && !snap().catalog_serial; ++frame) {
            int gets_before = g_get_calls, requests_before = ev_req;
            g_api->tick(&r, 0, 2500, tid); pump_events();
            if (g_get_calls-gets_before > 64 || g_list_calls != 1 ||
                ev_req-requests_before > 1 || g_recycle_calls != ev_req) return 1;
            if (ev_req != requests_before) {
                int expected_index = 2*(ev_req-1);
                if (expected_index >= 256 || last_req_id != g_inv[expected_index].id ||
                    last_req_amount != 40 || last_req_keep != 10 ||
                    g_last_recycle_id != last_req_id || g_last_recycle_amount != 40) return 2;
                // A further frame before the response must neither scan nor issue another RPC.
                int scans = g_get_calls;
                g_api->tick(&r, 0, 2500, tid); pump_events();
                if (ev_req != requests_before+1 || ev_con != requests_before ||
                    g_get_calls != scans || g_recycle_calls != ev_req) return 3;
                g_itemproto.item = last_req_id; g_response.status = 1;
                g_inv[expected_index].count = 10;
                call_result(&g_callback, &g_response, &m_result); pump_events();
                if (ev_con != ev_req || last_con_id != last_req_id || last_con_amount != 40) return 4;
                removed += last_con_amount;
            }
        }
        if (ev_req != 128 || ev_con != 128 || ev_err || removed != 5120 ||
            g_recycle_calls != 128 || g_list_calls != 1 || g_get_calls != 257 ||
            snap().catalog_count != 257 || g_roots != 3) return 5;
        for (int i = 0; i < 128; ++i) if (g_inv[2*i].count != 10) return 6;
        g_api->clear(tid); if (g_roots || g_bad_free || snap().owned_roots) return 7;
        return 0;
    }
    case 27: { // no selected rules: no quantity queries, descriptor list, or UI catalog
        auto r=make_rules(true,1);r.count=0;
        g_api->tick(&r,1,2500,tid);
        if(g_quantity_calls||g_list_calls||g_get_calls||snap().catalog_serial||g_recycle_calls)return 1;
        g_api->clear(tid);if(g_roots||g_bad_free)return 2;return 0;
    }
    case 28: { // selected below-cap ID never scans unselected bag types; explicit UI still does
        auto r=make_rules(true,1);g_current=8;g_list_count=257;
        for(int i=0;i<257;++i)g_inv[i]=Data{&c_data,1000+i,5,1};
        g_api->tick(&r,0,2500,tid);
        if(g_quantity_calls!=1||g_quantity_ids[0]!=101||g_list_calls||g_get_calls||snap().catalog_serial)return 1;
        r.enabled=0;++r.generation;
        for(int frame=0;frame<300&&!snap().catalog_serial;++frame)g_api->tick(&r,3,2500,tid);
        if(g_list_calls!=1||g_get_calls!=257||snap().catalog_count!=257||g_quantity_calls!=1)return 2;
        g_api->clear(tid);if(g_roots||g_bad_free)return 3;return 0;
    }
    case 29: { // many selected rules are bounded, pause off lease, and restart on new generation
        auto r=make_rules(true,1);r.count=200;g_current=8;
        for(int i=0;i<200;++i){r.entries[i].id=1000+i;r.entries[i].cap=10;}
        g_api->tick(&r,0,2500,tid);int first=g_quantity_calls;
        if(first<1||first>64||g_list_calls||g_get_calls)return 1;
        g_api->tick(&r,0,0,tid);if(g_quantity_calls!=first)return 2;
        ++r.generation;for(int i=0;i<200;++i)r.entries[i].id=2000+i;
        g_api->tick(&r,0,2500,tid);
        if(g_quantity_ids[first]!=2000)return 3;
        for(int frame=0;frame<300&&g_quantity_calls-first<200;++frame){
            int before=g_quantity_calls;g_api->tick(&r,0,2500,tid);
            if(g_quantity_calls-before>64)return 4;
        }
        if(g_quantity_calls-first!=200||g_list_calls||g_get_calls||snap().catalog_serial||g_recycle_calls)return 5;
        for(int i=0;i<200;++i)if(g_quantity_ids[first+i]!=2000+i)return 6;
        g_api->clear(tid);if(g_roots||g_bad_free)return 7;return 0;
    }
    case 30: { // a fresh cleanup ticket rechecks already visited selected IDs
        auto r=make_rules(true,1);r.count=200;g_current=8;
        for(int i=0;i<200;++i){r.entries[i].id=1000+i;r.entries[i].cap=10;}
        g_api->tick(&r,0,2500,tid);int before=g_quantity_calls;
        g_api->tick(&r,9,2500,tid);
        if(g_quantity_calls<=before||g_quantity_ids[before]!=1000||g_list_calls)return 1;
        r.enabled=0;g_api->tick(&r,1,2500,tid);
        if(g_roots!=3||g_list_calls||g_get_calls)return 2;
        g_api->clear(tid);if(g_roots||g_bad_free)return 3;return 0;
    }
    case 31:
    case 32: { // explicit berry overlay remains fresh with automatic selected/no-rule cleanup
        auto r=make_rules(true,1);if(which==32)r.count=0;g_current=8;
        g_api->tick(&r,5,2500,tid);
        const int expected=which==32?5:6;const int berry_ids[]={701,703,705,706,707};
        if(g_quantity_calls!=expected||g_list_calls||g_get_calls||g_recycle_calls)return 1;
        for(int i=0;i<5;++i)if(g_quantity_ids[i]!=berry_ids[i])return 2;
        if(which==31&&g_quantity_ids[5]!=101)return 3;
        if(snap().catalog_count!=5||!snap().catalog_serial)return 4;
        // An ordinary forced cleanup does not repeat the explicitly requested overlay reads.
        g_api->tick(&r,1,2500,tid);
        if(g_quantity_calls!=expected+(which==31?1:0)||g_list_calls||g_get_calls)return 5;
        g_api->clear(tid);if(g_roots||g_bad_free)return 6;return 0;
    }
    default: return 0;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    const bool slow = (argc >= 3 && std::strcmp(argv[2], "slow") == 0);
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::puts(dlerror()); return 2; }
    auto getter = reinterpret_cast<const void *(*)(uint32_t)>(dlsym(lib, "pgrazor_native_api"));
    auto core = getter ? static_cast<const pgrazor_native_api_v2 *>(getter(2)) : nullptr;
    if (!core || core->query_feature(14, 99) != nullptr) { std::puts("FAIL exact-version"); return 3; }
    g_api = static_cast<const pgrazor_items_api_v1 *>(core->query_feature(14, 1));
    if (!g_api || g_api->size != 72) { std::puts("FAIL feature 14 not wired"); return 3; }

    // Config validation (parent process; all fail before any hook is attempted).
    pgrazor_items_config_v1 c = make_config(true), bad;
    if (g_api->install(nullptr) != PGR_ITEMS_ERR_NULL_ARG) { std::puts("FAIL null"); return 5; }
    bad = c; bad.version = 2;
    if (g_api->install(&bad) != PGR_ITEMS_ERR_BAD_CONFIG) { std::puts("FAIL version"); return 5; }
    bad = c; bad.size = sizeof(c) - 1;
    if (g_api->install(&bad) != PGR_ITEMS_ERR_BAD_CONFIG) { std::puts("FAIL size"); return 5; }
    bad = c; bad.m_recycle = nullptr;
    if (g_api->install(&bad) != PGR_ITEMS_ERR_BAD_CONFIG) { std::puts("FAIL missing-core"); return 5; }
    bad = c; bad.func_type = nullptr;  // has_lookup set, a lookup pointer null -> BAD_CONFIG
    if (g_api->install(&bad) != PGR_ITEMS_ERR_BAD_CONFIG) { std::puts("FAIL missing-lookup"); return 5; }
    bad = c; bad.result_address = bad.error_address;
    if (g_api->install(&bad) != PGR_ITEMS_ERR_DUP_HOOK) { std::puts("FAIL dup"); return 5; }

    const int cases = 33;
    for (int i = 0; i < cases; ++i) {
        if (i == 14 && !slow) continue;
        pid_t pid = fork();
        if (pid == 0) _exit(scenario(i));          // fresh process -> clean module/hook state
        int status = 0; waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            std::printf("FAIL items scenario %d (code %d)\n", i,
                        WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            return 4;
        }
    }
    std::printf("PASS native items: over-cap recycle, live-quantity recheck, disabled catalog, "
                "sync result, in-flight no duplicate (pending bag root), manual-recycle ambiguity "
                "+ uncertain quarantine across generation, result failure block + generation "
                "clear, reentrant clear + reentrant disable, nested new-epoch revoke, result "
                "identity mismatch, stop-during-callback no confirm, scoped service acquire, "
                "foreign-thread no steal, root balance, immutable rebind, static/instance ABI, "
                "config validation%s\n",
                slow ? " + confirmation timeout" : " (timeout skipped; pass 'slow' to run it)");
    return 0;
}
