#include <assert.h>
#include "dobby.h"
#include "pgrazor_native_api.h"
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

#define PGR_INTERACTION_HOST_STANDALONE_TEST 1
#include "interaction_host.cpp"

namespace {

static int64_t g_fake_now_ms = 100000;
static int64_t fake_now_ms() { return g_fake_now_ms; }

/* Sparse fake module mapping holding real ELF64 AArch64 header, kDebugWords, and kBtxuWords */
static uint8_t *g_module_base = nullptr;
static uint64_t g_module_size = 0;

enum {
    kDebugRva = 0x1000u,
    kTargetBaseRva = 0x2000u,
    kTargetStride = 0x100u,
};

struct FakeClass;
struct FakeType {
    const char *name;
    FakeClass *klass;
};

struct FakeParam {
    const char *name;
    FakeType *type;
};

struct FakeMethod {
    FakeClass *owner;
    const char *name;
    FakeType *return_type;
    uint32_t param_count;
    FakeParam params[12];
    uint32_t flags;
    uint32_t iflags;
    bool generic;
    bool inflated;
    void *code_addr;
};

struct FakeField {
    FakeClass *owner;
    const char *name;
    FakeType *type;
    int32_t flags;
    size_t offset;
    int32_t static_val;
};

struct FakeClass {
    const char *ns;
    const char *name;
    const char *image_name;
    FakeType *self_type;
    FakeClass *parent;
    int32_t flags;
    bool value_type;
    bool is_enum;
    int32_t instance_size;
    int32_t value_size;
    int32_t align;
    FakeType *enum_base_type;
    uint32_t field_count;
    FakeField fields[120];
    uint32_t method_count;
    FakeMethod methods[100];
};

struct FakeAssembly {
    const char *image_name;
    uint32_t class_count;
    FakeClass *classes[16];
};

static FakeType g_type_pool[512];
static uint32_t g_type_pool_used = 0;

static FakeType *alloc_fake_type(const char *name, FakeClass *klass = nullptr) {
    for (uint32_t i = 0; i < g_type_pool_used; ++i) {
        if (strcmp(g_type_pool[i].name, name) == 0 && g_type_pool[i].klass == klass) {
            return &g_type_pool[i];
        }
    }
    assert(g_type_pool_used < 512);
    FakeType *t = &g_type_pool[g_type_pool_used++];
    t->name = name;
    t->klass = klass;
    return t;
}

static FakeClass g_enum_catch_status;
static FakeClass g_enum_fort_result;
static FakeClass g_classes[PGR_IH_CLASS_COUNT];
static FakeMethod g_btxu_method;
static bool g_btxu_present = true;
static FakeAssembly g_assemblies[300];
static void *g_assembly_ptrs[300];
static size_t g_assembly_count = 1;
static void *g_domain_ptr = reinterpret_cast<void *>(0xdead0001ULL);
static void *g_current_thread = reinterpret_cast<void *>(0xdead0002ULL);
static uint32_t g_thread_attach_calls = 0;
static uint32_t g_thread_detach_calls = 0;
static int64_t g_scan_advance_per_method_ms = 0;

/* GC handle tracking */
struct FakeGcSlot {
    bool used;
    void *obj;
};
static FakeGcSlot g_gc_slots[64];
static uint32_t g_gc_new_calls = 0;
static uint32_t g_gc_get_calls = 0;
static uint32_t g_gc_free_calls = 0;
static void *gc_fail_object=nullptr,*gc_move_from=nullptr,*gc_move_to=nullptr;
static bool gc_move_on_free=false;

static uintptr_t fake_gchandle_new(void *obj, int pinned) {
    (void)pinned;
    if(obj==gc_fail_object)return 0;
    ++g_gc_new_calls;
    for (uintptr_t i = 1; i < 64; ++i) {
        if (!g_gc_slots[i].used) {
            g_gc_slots[i].used = true;
            g_gc_slots[i].obj = obj;
            return i;
        }
    }
    return 0;
}

static void *fake_gchandle_get_target(uintptr_t h) {
    ++g_gc_get_calls;
    if (h > 0 && h < 64 && g_gc_slots[h].used) {
        return g_gc_slots[h].obj;
    }
    return nullptr;
}

static void fake_gchandle_free(uintptr_t h) {
    ++g_gc_free_calls;
    if(gc_move_on_free){gc_move_on_free=false;for(auto &slot:g_gc_slots)if(slot.used&&slot.obj==gc_move_from)slot.obj=gc_move_to;}
    if (h > 0 && h < 64) {
        g_gc_slots[h].used = false;
        g_gc_slots[h].obj = nullptr;
    }
}

/* Fake IL2CPP exports matching PgrMetaExports + PgrInteractionExtraExports */
static void *fx_domain_get() { return g_domain_ptr; }
static void *fx_domain_get_assemblies(void *domain, size_t *out_count) {
    if (domain == nullptr) {
        *out_count = 0;
        return nullptr;
    }
    *out_count = g_assembly_count;
    return g_assembly_ptrs;
}
static void *fx_assembly_get_image(void *assembly) { return assembly; }
static const char *fx_image_get_name(const void *image) {
    const auto *a = static_cast<const FakeAssembly *>(image);
    return a ? a->image_name : "Assembly-CSharp.dll";
}
static void *fx_class_from_name(void *image, const char *ns, const char *name) {
    auto *a = static_cast<FakeAssembly *>(image);
    if (a == nullptr) {
        return nullptr;
    }
    for (uint32_t i = 0; i < a->class_count; ++i) {
        FakeClass *c = a->classes[i];
        if (c != nullptr && strcmp(c->ns, ns) == 0 && strcmp(c->name, name) == 0) {
            return c;
        }
    }
    return nullptr;
}
static void *fx_class_get_type(void *klass) {
    return klass ? static_cast<FakeClass *>(klass)->self_type : nullptr;
}
static char *fx_type_get_name(void *type) {
    if (type == nullptr) {
        return nullptr;
    }
    const char *s = static_cast<FakeType *>(type)->name;
    size_t n = strlen(s);
    char *copy = static_cast<char *>(malloc(n + 1));
    memcpy(copy, s, n + 1);
    return copy;
}
static void fx_mem_free(void *ptr) { free(ptr); }
static void *fx_class_get_field_from_name(void *klass, const char *name) {
    auto *c = static_cast<FakeClass *>(klass);
    if (c == nullptr) {
        return nullptr;
    }
    for (uint32_t i = 0; i < c->field_count; ++i) {
        if (strcmp(c->fields[i].name, name) == 0) {
            return &c->fields[i];
        }
    }
    return nullptr;
}
static void *fx_field_get_parent(void *field) {
    return field ? static_cast<FakeField *>(field)->owner : nullptr;
}
static int fx_field_get_flags(void *field) {
    return field ? static_cast<FakeField *>(field)->flags : 0;
}
static size_t fx_field_get_offset(void *field) {
    return field ? static_cast<FakeField *>(field)->offset : 0;
}
static void *fx_field_get_type(void *field) {
    return field ? static_cast<FakeField *>(field)->type : nullptr;
}
static void *fx_class_from_type(void *type) {
    return type ? static_cast<FakeType *>(type)->klass : nullptr;
}
static bool fx_class_is_valuetype(void *klass) {
    return klass && static_cast<FakeClass *>(klass)->value_type;
}
static bool fx_class_is_enum(void *klass) {
    return klass && static_cast<FakeClass *>(klass)->is_enum;
}
static void *fx_class_enum_basetype(void *klass) {
    return klass ? static_cast<FakeClass *>(klass)->enum_base_type : nullptr;
}
static int fx_class_value_size(void *klass, int *out_align) {
    if (klass == nullptr) {
        return 0;
    }
    auto *c = static_cast<FakeClass *>(klass);
    if (out_align != nullptr) {
        *out_align = c->align;
    }
    return c->value_size;
}
static void *fx_class_get_method_from_name(void *klass, const char *name, int argc) {
    if (klass == &g_classes[10] && strcmp(name, "btxu") == 0 && argc == 0) {
        return g_btxu_present ? &g_btxu_method : nullptr;
    }
    auto *c = static_cast<FakeClass *>(klass);
    if (c == nullptr) {
        return nullptr;
    }
    for (uint32_t i = 0; i < c->method_count; ++i) {
        if (strcmp(c->methods[i].name, name) == 0 &&
            static_cast<int>(c->methods[i].param_count) == argc) {
            return &c->methods[i];
        }
    }
    return nullptr;
}
static void *fx_method_get_class(void *method) {
    return method ? static_cast<FakeMethod *>(method)->owner : nullptr;
}
static char *fx_method_get_name(void *method) {
    return method ? const_cast<char *>(static_cast<FakeMethod *>(method)->name) : nullptr;
}
static uint32_t fx_method_get_flags(void *method, uint32_t *out_iflags) {
    if (method == nullptr) {
        return 0;
    }
    auto *m = static_cast<FakeMethod *>(method);
    if (out_iflags != nullptr) {
        *out_iflags = m->iflags;
    }
    return m->flags;
}
static uint32_t fx_method_get_param_count(void *method) {
    return method ? static_cast<FakeMethod *>(method)->param_count : 0;
}
static void *fx_method_get_param(void *method, uint32_t index) {
    if (method == nullptr) {
        return nullptr;
    }
    auto *m = static_cast<FakeMethod *>(method);
    return index < m->param_count ? m->params[index].type : nullptr;
}
static const char *fx_method_get_param_name(void *method, uint32_t index) {
    if (method == nullptr) {
        return "";
    }
    auto *m = static_cast<FakeMethod *>(method);
    return (index < m->param_count && m->params[index].name) ? m->params[index].name : "arg";
}
static void *fx_method_get_return_type(void *method) {
    return method ? static_cast<FakeMethod *>(method)->return_type : nullptr;
}
static bool fx_method_is_generic(void *method) {
    return method && static_cast<FakeMethod *>(method)->generic;
}
static bool fx_method_is_inflated(void *method) {
    return method && static_cast<FakeMethod *>(method)->inflated;
}
static bool fx_debug_get_method_info(void *method, void *info) {
    if (method == nullptr || info == nullptr) {
        return false;
    }
    static_cast<void **>(info)[0] = static_cast<FakeMethod *>(method)->code_addr;
    return true;
}
static bool fx_class_is_assignable_from(void *, void *) { return false; }
static void *fx_class_get_methods(void *klass, void **iter) {
    if (klass == nullptr || iter == nullptr) {
        return nullptr;
    }
    if (g_scan_advance_per_method_ms > 0) {
        g_fake_now_ms += g_scan_advance_per_method_ms;
    }
    auto *c = static_cast<FakeClass *>(klass);
    uintptr_t idx = reinterpret_cast<uintptr_t>(*iter);
    if (idx >= c->method_count) {
        return nullptr;
    }
    *iter = reinterpret_cast<void *>(idx + 1);
    return &c->methods[idx];
}
static void *fx_class_get_nested_types(void *, void **) { return nullptr; }
static void *fx_class_get_declaring_type(void *) { return nullptr; }

static const char *fx_class_get_name(void *klass) {
    return klass ? static_cast<FakeClass *>(klass)->name : "";
}
static const char *fx_class_get_namespace(void *klass) {
    return klass ? static_cast<FakeClass *>(klass)->ns : "";
}
static void *fx_class_get_parent(void *klass) {
    return klass ? static_cast<FakeClass *>(klass)->parent : nullptr;
}
static int32_t fx_class_get_flags(void *klass) {
    return klass ? static_cast<FakeClass *>(klass)->flags : 0;
}
static int32_t fx_class_instance_size(void *klass) {
    return klass ? static_cast<FakeClass *>(klass)->instance_size : 0;
}
static void *fx_class_get_fields(void *klass, void **iter) {
    if (klass == nullptr || iter == nullptr) {
        return nullptr;
    }
    auto *c = static_cast<FakeClass *>(klass);
    uintptr_t idx = reinterpret_cast<uintptr_t>(*iter);
    if (idx >= c->field_count) {
        return nullptr;
    }
    *iter = reinterpret_cast<void *>(idx + 1);
    return &c->fields[idx];
}
static const char *fx_field_get_name(void *field) {
    return field ? static_cast<FakeField *>(field)->name : "";
}
static void fx_field_static_get_value(void *field, void *out) {
    if (field != nullptr && out != nullptr) {
        *static_cast<int32_t *>(out) = static_cast<FakeField *>(field)->static_val;
    }
}
static void *fx_thread_current() { return g_current_thread; }
static void *fx_thread_attach(void *domain) {
    (void)domain;
    ++g_thread_attach_calls;
    return reinterpret_cast<void *>(0xdead0003ULL);
}
static void fx_thread_detach(void *thread) {
    (void)thread;
    ++g_thread_detach_calls;
}

static int fx_seg_query(void *, const void *addr, uint64_t size, PgrMetaSegment *out) {
    memset(out, 0, sizeof(*out));
    uintptr_t a = reinterpret_cast<uintptr_t>(addr);
    uintptr_t base = reinterpret_cast<uintptr_t>(g_module_base);
    if (a >= base && size <= g_module_size && (a - base) <= g_module_size - size) {
        out->found = 1;
        out->readable = 1;
        out->host_exec = 1;
        out->seg_base = g_module_base;
        out->seg_size = g_module_size;
    }
    return 0;
}

/* Mock Dobby hook engine tracking every hook/unhook and rejecting duplicate hooks */
struct HookRecord {
    void *target;
    void *replacement;
};
static HookRecord g_hooks[32];
static uint32_t g_hook_count = 0;
static uint32_t g_total_hook_calls = 0;
static uint32_t g_total_unhook_calls = 0;
static void *g_btxu_orig_return_value = nullptr;

static void dummy_orig_void0(void *self, const void *method) {
    (void)self;
    (void)method;
}
static void dummy_orig_void1(void *self, void *arg0, const void *method) {
    (void)self;
    (void)arg0;
    (void)method;
}
static void *dummy_orig_btxu(void *self, const void *method) {
    (void)self;
    (void)method;
    return g_btxu_orig_return_value;
}

extern "C" int pgrazor_hook(void *target, void *replacement, void **original) {
    ++g_total_hook_calls;
    for (uint32_t i = 0; i < g_hook_count; ++i) {
        if (g_hooks[i].target == target) {
            return -2; /* Duplicate hook on same site! */
        }
    }
    assert(g_hook_count < 32);
    g_hooks[g_hook_count].target = target;
    g_hooks[g_hook_count].replacement = replacement;
    ++g_hook_count;

    return DobbyHook(target,replacement,original);
}

extern "C" int pgrazor_unhook(void *target) {++g_total_unhook_calls;return DobbyDestroy(target);}

static void *find_installed_replacement(uint32_t target_id) {
    uintptr_t code = g_state.targets[target_id].code;
    for (uint32_t i = 0; i < g_hook_count; ++i) {
        if (reinterpret_cast<uintptr_t>(g_hooks[i].target) == code) {
            return g_hooks[i].replacement;
        }
    }
    return nullptr;
}

static PgrMetaExports g_fx_meta_exports;

static void init_sparse_module_once() {
    if (g_module_base != nullptr) {
        return;
    }
    long page = sysconf(_SC_PAGESIZE);
    assert(page > 0);
    uint64_t needed = PGR_IH_BTXU_RVA + 0x10000ULL;
    g_module_size = ((needed + page - 1) / page) * page;
    void *mapped = mmap(
        nullptr, g_module_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(mapped != MAP_FAILED);
    g_module_base = static_cast<uint8_t *>(mapped);

    g_fx_meta_exports.domain_get = reinterpret_cast<void *>(&fx_domain_get);
    g_fx_meta_exports.domain_get_assemblies =
        reinterpret_cast<void *>(&fx_domain_get_assemblies);
    g_fx_meta_exports.assembly_get_image = reinterpret_cast<void *>(&fx_assembly_get_image);
    g_fx_meta_exports.class_from_name = reinterpret_cast<void *>(&fx_class_from_name);
    g_fx_meta_exports.class_get_type = reinterpret_cast<void *>(&fx_class_get_type);
    g_fx_meta_exports.type_get_name = reinterpret_cast<void *>(&fx_type_get_name);
    g_fx_meta_exports.mem_free = reinterpret_cast<void *>(&fx_mem_free);
    g_fx_meta_exports.class_get_field_from_name =
        reinterpret_cast<void *>(&fx_class_get_field_from_name);
    g_fx_meta_exports.field_get_parent = reinterpret_cast<void *>(&fx_field_get_parent);
    g_fx_meta_exports.field_get_flags = reinterpret_cast<void *>(&fx_field_get_flags);
    g_fx_meta_exports.field_get_offset = reinterpret_cast<void *>(&fx_field_get_offset);
    g_fx_meta_exports.field_get_type = reinterpret_cast<void *>(&fx_field_get_type);
    g_fx_meta_exports.class_from_type = reinterpret_cast<void *>(&fx_class_from_type);
    g_fx_meta_exports.class_is_valuetype = reinterpret_cast<void *>(&fx_class_is_valuetype);
    g_fx_meta_exports.class_is_enum = reinterpret_cast<void *>(&fx_class_is_enum);
    g_fx_meta_exports.class_enum_basetype = reinterpret_cast<void *>(&fx_class_enum_basetype);
    g_fx_meta_exports.class_value_size = reinterpret_cast<void *>(&fx_class_value_size);
    g_fx_meta_exports.class_get_method_from_name =
        reinterpret_cast<void *>(&fx_class_get_method_from_name);
    g_fx_meta_exports.method_get_class = reinterpret_cast<void *>(&fx_method_get_class);
    g_fx_meta_exports.method_get_name = reinterpret_cast<void *>(&fx_method_get_name);
    g_fx_meta_exports.method_get_flags = reinterpret_cast<void *>(&fx_method_get_flags);
    g_fx_meta_exports.method_get_param_count =
        reinterpret_cast<void *>(&fx_method_get_param_count);
    g_fx_meta_exports.method_get_param = reinterpret_cast<void *>(&fx_method_get_param);
    g_fx_meta_exports.method_get_return_type =
        reinterpret_cast<void *>(&fx_method_get_return_type);
    g_fx_meta_exports.method_is_generic = reinterpret_cast<void *>(&fx_method_is_generic);
    g_fx_meta_exports.method_is_inflated = reinterpret_cast<void *>(&fx_method_is_inflated);
    g_fx_meta_exports.debug_get_method_info =
        reinterpret_cast<void *>(&fx_debug_get_method_info);
    g_fx_meta_exports.class_is_assignable_from =
        reinterpret_cast<void *>(&fx_class_is_assignable_from);
    g_fx_meta_exports.class_get_methods = reinterpret_cast<void *>(&fx_class_get_methods);
    g_fx_meta_exports.class_get_nested_types =
        reinterpret_cast<void *>(&fx_class_get_nested_types);
    g_fx_meta_exports.class_get_declaring_type =
        reinterpret_cast<void *>(&fx_class_get_declaring_type);
}

static void reset_fake_universe(PgrInteractionHostConfig *out_cfg) {
    init_sparse_module_once();

    /* Restore ELF64 AArch64 header, kDebugWords, and kBtxuWords */
    memset(g_module_base, 0, 64);
    g_module_base[0] = 0x7fu;
    g_module_base[1] = 'E';
    g_module_base[2] = 'L';
    g_module_base[3] = 'F';
    g_module_base[4] = 2u;
    g_module_base[5] = 1u;
    g_module_base[18] = 183u;
    g_module_base[19] = 0u;

    memcpy(g_module_base + kDebugRva, kDebugWords, sizeof(kDebugWords));
    memcpy(g_module_base + PGR_IH_BTXU_RVA, kBtxuWords, sizeof(kBtxuWords));

    g_type_pool_used = 0;
    FakeType *t_void = alloc_fake_type("System.Void", nullptr);
    FakeType *t_int32 = alloc_fake_type("System.Int32", nullptr);

    memset(g_classes, 0, sizeof(g_classes));
    for (uint32_t c = 0; c < PGR_IH_CLASS_COUNT; ++c) {
        g_classes[c].ns = kClasses[c].ns;
        g_classes[c].name = kClasses[c].name;
        g_classes[c].image_name = "Assembly-CSharp.dll";
        g_classes[c].self_type = alloc_fake_type(kClasses[c].full_name, &g_classes[c]);
        g_classes[c].flags = 0x20;
        g_classes[c].instance_size = 32;
    }

    /* Setup CatchPokemonOutProto.Types.Status enum (Spec 0) */
    memset(&g_enum_catch_status, 0, sizeof(g_enum_catch_status));
    g_enum_catch_status.ns = "Holoholo.Rpc.CatchPokemonOutProto.Types";
    g_enum_catch_status.name = "Status";
    g_enum_catch_status.self_type = alloc_fake_type(
        "Holoholo.Rpc.CatchPokemonOutProto.Types.Status", &g_enum_catch_status);
    g_enum_catch_status.value_type = true;
    g_enum_catch_status.is_enum = true;
    g_enum_catch_status.instance_size = 20;
    g_enum_catch_status.value_size = 4;
    g_enum_catch_status.align = 4;
    g_enum_catch_status.enum_base_type = t_int32;
    g_enum_catch_status.field_count = 4;
    g_enum_catch_status.fields[0] = {
        &g_enum_catch_status, "value__", t_int32, 0x0001, 16, 0};
    g_enum_catch_status.fields[1] = {
        &g_enum_catch_status, "CatchError", g_enum_catch_status.self_type, 0x8050, 0, 0};
    g_enum_catch_status.fields[2] = {
        &g_enum_catch_status, "CatchSuccess", g_enum_catch_status.self_type, 0x8050, 0, 1};
    g_enum_catch_status.fields[3] = {
        &g_enum_catch_status, "CatchEscape", g_enum_catch_status.self_type, 0x8050, 0, 2};

    /* Setup FortSearchOutProto.Types.Result enum (Spec 1) */
    memset(&g_enum_fort_result, 0, sizeof(g_enum_fort_result));
    g_enum_fort_result.ns = "Holoholo.Rpc.FortSearchOutProto.Types";
    g_enum_fort_result.name = "Result";
    g_enum_fort_result.self_type = alloc_fake_type(
        "Holoholo.Rpc.FortSearchOutProto.Types.Result", &g_enum_fort_result);
    g_enum_fort_result.value_type = true;
    g_enum_fort_result.is_enum = true;
    g_enum_fort_result.instance_size = 20;
    g_enum_fort_result.value_size = 4;
    g_enum_fort_result.align = 4;
    g_enum_fort_result.enum_base_type = t_int32;
    g_enum_fort_result.field_count = 3;
    g_enum_fort_result.fields[0] = {
        &g_enum_fort_result, "value__", t_int32, 0x0001, 16, 0};
    g_enum_fort_result.fields[1] = {
        &g_enum_fort_result, "NoResultSet", g_enum_fort_result.self_type, 0x8050, 0, 0};
    g_enum_fort_result.fields[2] = {
        &g_enum_fort_result, "Success", g_enum_fort_result.self_type, 0x8050, 0, 1};

    g_classes[5].field_count = 1;
    g_classes[5].fields[0] = {
        &g_classes[5], "Status", g_enum_catch_status.self_type, 0x0001, 16, 0};
    g_classes[6].field_count = 1;
    g_classes[6].fields[0] = {
        &g_classes[6], "Result", g_enum_fort_result.self_type, 0x0001, 20, 0};

    for (uint32_t t = 0; t < PGR_IH_TOTAL_TARGETS - 1; ++t) {
        const TargetDesc &td = kTargets[t];
        FakeClass &fc = g_classes[td.class_index];
        FakeMethod &fm = fc.methods[fc.method_count++];
        fm.owner = &fc;
        fm.name = td.method_name;
        fm.return_type = t_void;
        fm.param_count = td.param_count;
        if (td.param_count == 1) {
            fm.params[0].name = "proto";
            fm.params[0].type = alloc_fake_type(td.param0_type);
        }
        fm.flags = 0x0006;
        fm.iflags = 0;
        fm.code_addr = g_module_base + kTargetBaseRva + t * kTargetStride;
    }

    memset(&g_btxu_method, 0, sizeof(g_btxu_method));
    g_btxu_method.owner = &g_classes[10];
    g_btxu_method.name = "btxu";
    g_btxu_method.return_type = g_classes[5].self_type;
    g_btxu_method.param_count = 0;
    g_btxu_method.flags = 0x0006;
    g_btxu_method.code_addr = g_module_base + PGR_IH_BTXU_RVA;
    g_btxu_present = true;

    memset(g_assemblies, 0, sizeof(g_assemblies));
    g_assemblies[0].image_name = "Assembly-CSharp.dll";
    g_assemblies[0].class_count = PGR_IH_CLASS_COUNT;
    for (uint32_t c = 0; c < PGR_IH_CLASS_COUNT; ++c) {
        g_assemblies[0].classes[c] = &g_classes[c];
    }
    for (uint32_t a = 0; a < 300; ++a) {
        if (a > 0) {
            g_assemblies[a].image_name = "Dummy.dll";
            g_assemblies[a].class_count = 0;
        }
        g_assembly_ptrs[a] = &g_assemblies[a];
    }
    g_assembly_count = 1;
    g_domain_ptr = reinterpret_cast<void *>(0xdead0001ULL);
    g_current_thread = reinterpret_cast<void *>(0xdead0002ULL);
    g_thread_attach_calls = 0;
    g_thread_detach_calls = 0;
    g_scan_advance_per_method_ms = 0;
    g_hook_count = 0;
    g_total_hook_calls = 0;
    g_total_unhook_calls = 0;
    g_gc_new_calls = 0;
    g_gc_get_calls = 0;
    g_gc_free_calls = 0;
    g_btxu_orig_return_value = nullptr;
    memset(g_gc_slots, 0, sizeof(g_gc_slots));

    memset(out_cfg, 0, sizeof(*out_cfg));
    out_cfg->binding.version = PGR_MD_CONFIG_VERSION;
    out_cfg->binding.size = sizeof(PgrMetaBindConfig);
    out_cfg->binding.exports = &g_fx_meta_exports;
    out_cfg->binding.module_base = g_module_base;
    out_cfg->binding.module_size = g_module_size;
    out_cfg->binding.seg_query = &fx_seg_query;

    out_cfg->extra.image_get_name = reinterpret_cast<void *>(&fx_image_get_name);
    out_cfg->extra.class_get_name = reinterpret_cast<void *>(&fx_class_get_name);
    out_cfg->extra.class_get_namespace = reinterpret_cast<void *>(&fx_class_get_namespace);
    out_cfg->extra.class_get_parent = reinterpret_cast<void *>(&fx_class_get_parent);
    out_cfg->extra.class_get_flags = reinterpret_cast<void *>(&fx_class_get_flags);
    out_cfg->extra.class_instance_size = reinterpret_cast<void *>(&fx_class_instance_size);
    out_cfg->extra.class_get_fields = reinterpret_cast<void *>(&fx_class_get_fields);
    out_cfg->extra.field_get_name = reinterpret_cast<void *>(&fx_field_get_name);
    out_cfg->extra.field_static_get_value = reinterpret_cast<void *>(&fx_field_static_get_value);
    out_cfg->extra.method_get_param_name = reinterpret_cast<void *>(&fx_method_get_param_name);
    out_cfg->extra.thread_current = reinterpret_cast<void *>(&fx_thread_current);
    out_cfg->extra.thread_attach = reinterpret_cast<void *>(&fx_thread_attach);
    out_cfg->extra.thread_detach = reinterpret_cast<void *>(&fx_thread_detach);
    out_cfg->extra.gchandle_new = reinterpret_cast<void *>(&fake_gchandle_new);
    out_cfg->extra.gchandle_get_target = reinterpret_cast<void *>(&fake_gchandle_get_target);
    out_cfg->extra.gchandle_free = reinterpret_cast<void *>(&fake_gchandle_free);

    out_cfg->debug_code_address = g_module_base + kDebugRva;
    out_cfg->now_ms_fn = reinterpret_cast<void *>(&fake_now_ms);
    out_cfg->flags = PGR_IH_CFG_NO_AUTO_SHARED_DETECT;
}

/* Helper to push a Transport 52 settings snapshot frame */
static void push_transport_snapshot(const PgrRuntimeTransportApi *rt,
                                    uint64_t seq,
                                    bool diag_ready,
                                    uint64_t stop_pulse,
                                    uint64_t trace_pulse) {
    if (rt == nullptr) {
        return;
    }
    uint8_t frame[256] = {};
    uint32_t total_size = PGR_TRANSPORT_SNAPSHOT_FIXED + 7 * 4;
    uint32_t magic = PGR_TRANSPORT_SNAPSHOT_MAGIC;
    uint32_t ver = PGR_TRANSPORT_FRAME_VERSION;
    uint32_t flags = 0;
    uint64_t wall_ms = static_cast<uint64_t>(g_fake_now_ms);
    memcpy(frame + PGR_TRANSPORT_OFF_MAGIC, &magic, 4);
    memcpy(frame + PGR_TRANSPORT_OFF_VERSION, &ver, 4);
    memcpy(frame + PGR_TRANSPORT_OFF_FRAME_SIZE, &total_size, 4);
    memcpy(frame + PGR_TRANSPORT_OFF_FLAGS, &flags, 4);
    memcpy(frame + PGR_TRANSPORT_OFF_SEQ, &seq, 8);
    memcpy(frame + PGR_TRANSPORT_OFF_WALL_MS, &wall_ms, 8);
    frame[PGR_TRANSPORT_OFF_BOOLS + PGR_IH_RT_BOOL_DIAGNOSTICS_READY] = diag_ready ? 1 : 0;
    memcpy(frame + PGR_TRANSPORT_OFF_PULSES + 8 * PGR_IH_RT_PULSE_STOP_REQUEST, &stop_pulse, 8);
    memcpy(frame + PGR_TRANSPORT_OFF_PULSES + 8 * PGR_IH_RT_PULSE_TRACE_REQUEST, &trace_pulse, 8);

    uint8_t out_buf[65536];
    uint32_t out_len = 0;
    int rc = rt->exchange(frame, total_size, out_buf, sizeof(out_buf), &out_len);
    assert(rc == PGR_TRANSPORT_OK);
}

static void drain_transport_events(const PgrRuntimeTransportApi *rt) {
    if (rt == nullptr) {
        return;
    }
    uint8_t out_buf[65536];
    uint32_t out_len = 0;
    while (true) {
        int rc = rt->exchange(nullptr, 0, out_buf, sizeof(out_buf), &out_len);
        assert(rc == PGR_TRANSPORT_OK);
        uint32_t count = 0;
        memcpy(&count, out_buf + 12, 4);
        if (count == 0) {
            break;
        }
    }
}

static void test_passive_startup_and_scheduled_metadata_only(const PgrInteractionHostApi *api) {
    api->reset_for_tests();
    PgrInteractionHostConfig cfg {};
    reset_fake_universe(&cfg);

    /* Detached caller thread must be attached and detached cleanly by ScopedIl2CppThread */
    g_current_thread = nullptr;
    assert(api->configure(&cfg) == PGR_IH_OK);

    PgrInteractionHostSample s {};
    assert(api->sample(&s) == PGR_IH_OK);
    assert(s.configured == 1 && s.running == 0 && s.scheduled == 0);
    assert(g_total_hook_calls == 0);

    /* Schedule 25s metadata-only check */
    assert(api->schedule(PGR_IH_SCHEDULE_DELAY_MS) == PGR_IH_OK);
    assert(api->sample(&s) == PGR_IH_OK);
    assert(s.status == PGR_IH_STATUS_SCHEDULED && s.scheduled == 1 && s.running == 0);
    assert(g_total_hook_calls == 0);

    PgrInteractionHostEvent ev {};
    assert(api->take_event(&ev) == 1);
    assert(ev.type == PGR_IH_EVT_SCHEDULED);
    assert(strcmp(ev.channel, "interaction-probe-scheduled") == 0);

    /* Before 25000ms, tick() does nothing */
    g_fake_now_ms += 24999;
    assert(api->tick(100) == PGR_IH_OK);
    assert(api->sample(&s) == PGR_IH_OK);
    assert(s.status == PGR_IH_STATUS_SCHEDULED && s.scheduled == 1);
    assert(g_total_hook_calls == 0);

    /* At 25000ms, tick() runs metadata-only check and finishes with zero hooks */
    g_fake_now_ms += 1;
    assert(api->tick(100) == PGR_IH_OK);
    assert(api->sample(&s) == PGR_IH_OK);
    assert(s.status == PGR_IH_STATUS_COMPLETE);
    assert(strcmp(s.reason, "metadata-only") == 0);
    assert(strcmp(s.last_diagnostic, "Ready to record catch and spin callbacks") == 0);
    assert(s.classes_resolved == PGR_IH_CLASS_COUNT);
    assert(s.resolved_required_mask == PGR_IH_REQUIRED_MASK);
    assert(s.resolved_optional_mask == PGR_IH_OPTIONAL_MASK);
    assert(s.hooked_direct_mask == 0 && g_total_hook_calls == 0);
    assert(g_thread_attach_calls == 1 && g_thread_detach_calls == 1);
}

static void test_elf_and_debug_prologue_and_btxu_validation(const PgrInteractionHostApi *api) {
    /* 1. ELF64 AArch64 mismatch -> skipped (library-architecture-mismatch) */
    api->reset_for_tests();
    PgrInteractionHostConfig cfg {};
    reset_fake_universe(&cfg);
    assert(api->configure(&cfg) == PGR_IH_OK);
    g_module_base[18] = 62u; /* EM_X86_64 instead of EM_AARCH64 (183) */

    PgrInteractionHostOptions opts {};
    assert(api->start(&opts) == PGR_IH_OK);
    PgrInteractionHostSample s {};
    assert(api->sample(&s) == PGR_IH_OK);
    assert(s.status == PGR_IH_STATUS_SKIPPED);
    assert(strcmp(s.reason, "library-architecture-mismatch") == 0);
    assert(s.attempt_count == 1);

    /* 2. debug_get_method_info 80-byte prologue mismatch -> retries 20 times then fails */
    api->reset_for_tests();
    reset_fake_universe(&cfg);
    assert(api->configure(&cfg) == PGR_IH_OK);
    g_module_base[kDebugRva] ^= 0xffu;

    assert(api->start(&opts) == PGR_IH_OK);
    assert(api->sample(&s) == PGR_IH_OK);
    assert(s.status == PGR_IH_STATUS_PREPARING && s.attempt_count == 1);
    assert(strcmp(s.reason, "debug-get-method-info-prologue-mismatch") == 0);

    for (uint32_t a = 2; a <= PGR_IH_LIMIT_ATTEMPTS; ++a) {
        g_fake_now_ms += PGR_IH_LIMIT_RETRY_MS;
        assert(api->tick(100) == PGR_IH_OK);
    }
    assert(api->sample(&s) == PGR_IH_OK);
    assert(s.status == PGR_IH_STATUS_FAILED);
    assert(s.attempt_count == PGR_IH_LIMIT_ATTEMPTS);
    assert(strcmp(s.reason, "debug-get-method-info-prologue-mismatch") == 0);

    /* 3. Pokeball::btxu 32-byte prologue mismatch degrades ONLY optional target 19 */
    api->reset_for_tests();
    reset_fake_universe(&cfg);
    assert(api->configure(&cfg) == PGR_IH_OK);
    g_module_base[PGR_IH_BTXU_RVA] ^= 0xffu;

    assert(api->start(&opts) == PGR_IH_OK);
    assert(api->sample(&s) == PGR_IH_OK);
    assert(s.status == PGR_IH_STATUS_COMPLETE);
    assert(s.resolved_required_mask == PGR_IH_REQUIRED_MASK);
    assert((s.resolved_optional_mask & (1u << PGR_IH_TGT_POKEBALL_BTXU)) == 0);
}

static void test_metadata_limits_and_ambiguity(const PgrInteractionHostApi *api) {
    PgrInteractionHostConfig cfg {};
    PgrInteractionHostOptions opts {};
    PgrInteractionHostSample s {};

    /* 1. Assembly limit (256) */
    api->reset_for_tests();
    reset_fake_universe(&cfg);
    g_assembly_count = 260;
    assert(api->configure(&cfg) == PGR_IH_OK);
    assert(api->start(&opts) == PGR_IH_OK);
    assert(api->sample(&s) == PGR_IH_OK);
    assert(s.truncated == 1 && strcmp(s.reason, "metadata-truncated") == 0);

    /* 2. Lookup limit (2048): 200 empty assemblies * 11 classes = 2200 lookups > 2048 */
    api->reset_for_tests();
    reset_fake_universe(&cfg);
    g_assembly_count = 200;
    g_assemblies[0].class_count = 0; /* Put classes nowhere so every assembly uses 11 lookups */
    assert(api->configure(&cfg) == PGR_IH_OK);
    assert(api->start(&opts) == PGR_IH_OK);
    assert(api->sample(&s) == PGR_IH_OK);
    assert(s.truncated == 1 && s.lookups_used == PGR_IH_LIMIT_LOOKUPS);

    /* 3. Fields per class limit (96) */
    api->reset_for_tests();
    reset_fake_universe(&cfg);
    g_classes[0].field_count = 100;
    for (uint32_t i = 0; i < 100; ++i) {
        g_classes[0].fields[i] = {
            &g_classes[0], "f", alloc_fake_type("System.Int32"), 0x0001, 16 + i * 4, 0};
    }
    assert(api->configure(&cfg) == PGR_IH_OK);
    assert(api->start(&opts) == PGR_IH_OK);
    assert(api->sample(&s) == PGR_IH_OK);
    assert(s.truncated == 1 && s.fields_emitted >= PGR_IH_LIMIT_FIELDS_PER_CLASS);

    /* 4. Methods per class limit (80) */
    api->reset_for_tests();
    reset_fake_universe(&cfg);
    uint32_t base_m = g_classes[0].method_count;
    for (uint32_t i = base_m; i < 85; ++i) {
        FakeMethod &fm = g_classes[0].methods[i];
        fm.owner = &g_classes[0];
        fm.name = "ExtraMethod";
        fm.return_type = alloc_fake_type("System.Void");
        fm.param_count = 0;
        fm.code_addr = g_module_base + 0x8000u + i * 4;
    }
    g_classes[0].method_count = 85;
    assert(api->configure(&cfg) == PGR_IH_OK);
    assert(api->start(&opts) == PGR_IH_OK);
    assert(api->sample(&s) == PGR_IH_OK);
    assert(s.truncated == 1 && s.methods_emitted >= PGR_IH_LIMIT_METHODS_PER_CLASS);

    /* 5. Parameter limit (8) */
    api->reset_for_tests();
    reset_fake_universe(&cfg);
    {
        FakeMethod &fm = g_classes[0].methods[g_classes[0].method_count++];
        fm.owner = &g_classes[0];
        fm.name = "ManyParams";
        fm.return_type = alloc_fake_type("System.Void");
        fm.param_count = 9;
        for (uint32_t p = 0; p < 9; ++p) {
            fm.params[p].name = "p";
            fm.params[p].type = alloc_fake_type("System.Int32");
        }
        fm.code_addr = g_module_base + 0x9000u;
    }
    assert(api->configure(&cfg) == PGR_IH_OK);
    assert(api->start(&opts) == PGR_IH_OK);
    assert(api->sample(&s) == PGR_IH_OK);
    assert(s.truncated == 1);

    /* 6. Scan time limit (4000ms) */
    api->reset_for_tests();
    reset_fake_universe(&cfg);
    g_scan_advance_per_method_ms = 500;
    assert(api->configure(&cfg) == PGR_IH_OK);
    assert(api->start(&opts) == PGR_IH_OK);
    assert(api->sample(&s) == PGR_IH_OK);
    assert(s.truncated == 1);

    /* 7. Ambiguous required method overload fails inspect */
    api->reset_for_tests();
    reset_fake_universe(&cfg);
    {
        FakeMethod &dup = g_classes[0].methods[g_classes[0].method_count++];
        dup = g_classes[0].methods[0]; /* Duplicate EncounterCaptureState::EnterState() */
    }
    assert(api->configure(&cfg) == PGR_IH_OK);
    assert(api->start(&opts) == PGR_IH_OK);
    assert(api->sample(&s) == PGR_IH_OK);
    assert((s.ambiguous_target_mask & (1u << PGR_IH_TGT_CAPTURE_ENTER)) != 0);
    assert(strcmp(s.reason, "ambiguous-targets") == 0);
}

static void test_runtime_observation_and_result_readers(const PgrInteractionHostApi *api) {
    api->reset_for_tests();
    PgrInteractionHostConfig cfg {};
    reset_fake_universe(&cfg);
    assert(api->configure(&cfg) == PGR_IH_OK);

    PgrInteractionHostOptions opts {};
    opts.observe_runtime = 1;
    opts.read_results = 1;
    assert(api->start(&opts) == PGR_IH_OK);

    PgrInteractionHostSample s {};
    assert(api->sample(&s) == PGR_IH_OK);
    assert(s.status == PGR_IH_STATUS_OBSERVING && s.running == 1);
    assert(s.argument_readers_available == 1 && s.return_reader_available == 1);
    assert(s.hooked_direct_mask == PGR_IH_ALL_TARGETS_MASK);
    assert(s.installed_target_mask == PGR_IH_ALL_TARGETS_MASK);
    assert(g_hook_count == PGR_IH_TOTAL_TARGETS);

    /* Construct fake managed instances with valid klass pointers at offset 0 */
    alignas(8) uint8_t capture_state[32] = {};
    *reinterpret_cast<void **>(capture_state) = &g_classes[0];
    alignas(8) uint8_t spinner_state[32] = {};
    *reinterpret_cast<void **>(spinner_state) = &g_classes[2];
    alignas(8) uint8_t pokeball_obj[32] = {};
    *reinterpret_cast<void **>(pokeball_obj) = &g_classes[10];

    alignas(8) uint8_t catch_proto[32] = {};
    *reinterpret_cast<void **>(catch_proto) = &g_classes[5];
    *reinterpret_cast<int32_t *>(catch_proto + 16) = 1; /* CatchSuccess */

    alignas(8) uint8_t fort_proto[32] = {};
    *reinterpret_cast<void **>(fort_proto) = &g_classes[6];
    *reinterpret_cast<int32_t *>(fort_proto + 20) = 1; /* Success */

    /* Drain setup events before triggering callbacks */
    PgrInteractionHostEvent ev {};
    while (api->take_event(&ev) == 1) {
    }

    /* 1. Trigger SummaryDismissed (target 2) -> decodes CatchSuccess */
    auto fn_summary_dismissed =
        reinterpret_cast<HookVoid1Fn>(find_installed_replacement(PGR_IH_TGT_CAPTURE_SUMMARY_DISMISSED));
    assert(fn_summary_dismissed != nullptr);
    g_fake_now_ms += 10;
    fn_summary_dismissed(capture_state, catch_proto, nullptr);

    assert(api->take_event(&ev) == 1);
    assert(ev.type == PGR_IH_EVT_INTERACTION_EVENT && ev.phase == PGR_IH_PHASE_ENTER);
    assert(ev.target_id == PGR_IH_TGT_CAPTURE_SUMMARY_DISMISSED);
    assert(ev.result_status == PGR_IH_RES_DECODED && ev.result_code == 1);
    assert(strcmp(ev.result_name, "CatchSuccess") == 0);
    assert(api->take_event(&ev) == 1);
    assert(ev.type == PGR_IH_EVT_INTERACTION_EVENT && ev.phase == PGR_IH_PHASE_LEAVE);

    /* 2. Trigger btdd (target 10) -> decodes CatchEscape (2), and btdf (target 11) -> timing-only */
    *reinterpret_cast<int32_t *>(catch_proto + 16) = 2; /* CatchEscape */
    auto fn_btdd = reinterpret_cast<HookVoid1Fn>(find_installed_replacement(PGR_IH_TGT_CAPTURE_BTDD));
    auto fn_btdf = reinterpret_cast<HookVoid1Fn>(find_installed_replacement(PGR_IH_TGT_CAPTURE_BTDF));
    fn_btdd(capture_state, catch_proto, nullptr);
    fn_btdf(capture_state, catch_proto, nullptr);

    assert(api->take_event(&ev) == 1);
    assert(ev.target_id == PGR_IH_TGT_CAPTURE_BTDD && ev.phase == PGR_IH_PHASE_ENTER);
    assert(ev.result_status == PGR_IH_RES_DECODED && ev.result_code == 2);
    assert(strcmp(ev.result_name, "CatchEscape") == 0);
    assert(api->take_event(&ev) == 1);
    assert(ev.target_id == PGR_IH_TGT_CAPTURE_BTDD && ev.phase == PGR_IH_PHASE_LEAVE);

    assert(api->take_event(&ev) == 1);
    assert(ev.target_id == PGR_IH_TGT_CAPTURE_BTDF && ev.phase == PGR_IH_PHASE_ENTER);
    assert(ev.result_status == PGR_IH_RES_NONE); /* btdf is timing-only! */
    assert(api->take_event(&ev) == 1);
    assert(ev.target_id == PGR_IH_TGT_CAPTURE_BTDF && ev.phase == PGR_IH_PHASE_LEAVE);

    /* 3. Trigger bgff (target 13) -> decodes FortSearchOutProto.Result == Success (1) */
    auto fn_bgff = reinterpret_cast<HookVoid1Fn>(find_installed_replacement(PGR_IH_TGT_SPINNER_BGFF));
    fn_bgff(spinner_state, fort_proto, nullptr);
    assert(api->take_event(&ev) == 1);
    assert(ev.target_id == PGR_IH_TGT_SPINNER_BGFF && ev.phase == PGR_IH_PHASE_ENTER);
    assert(ev.result_status == PGR_IH_RES_DECODED && ev.result_code == 1);
    assert(strcmp(ev.result_name, "Success") == 0);
    assert(api->take_event(&ev) == 1);
    assert(ev.target_id == PGR_IH_TGT_SPINNER_BGFF && ev.phase == PGR_IH_PHASE_LEAVE);

    /* 4. Trigger Pokeball::btxu (target 19) with NULL return -> increments btxu_pending_polls, emits 0 events */
    auto fn_btxu = reinterpret_cast<HookRet0Fn>(find_installed_replacement(PGR_IH_TGT_POKEBALL_BTXU));
    g_btxu_orig_return_value = nullptr;
    assert(fn_btxu(pokeball_obj, nullptr) == nullptr);
    assert(api->take_event(&ev) == 0);
    assert(api->sample(&s) == PGR_IH_OK);
    assert(s.btxu_pending_polls == 1 && s.event_count == 8);

    /* 5. Trigger Pokeball::btxu with non-null catch_proto -> pins self & retval, emits enter + leave with returnResult */
    *reinterpret_cast<int32_t *>(catch_proto + 16) = 1; /* CatchSuccess */
    g_btxu_orig_return_value = catch_proto;
    uint32_t gc_before = g_gc_new_calls;
    assert(fn_btxu(pokeball_obj, nullptr) == catch_proto);
    assert(g_gc_new_calls == gc_before + 2); /* self + retval pinned and freed */
    assert(g_gc_free_calls == g_gc_new_calls);

    assert(api->take_event(&ev) == 1);
    assert(ev.target_id == PGR_IH_TGT_POKEBALL_BTXU && ev.phase == PGR_IH_PHASE_ENTER);
    assert(api->take_event(&ev) == 1);
    assert(ev.target_id == PGR_IH_TGT_POKEBALL_BTXU && ev.phase == PGR_IH_PHASE_LEAVE);
    assert(ev.is_return_result == 1 && ev.result_status == PGR_IH_RES_DECODED);
    assert(ev.result_code == 1 && strcmp(ev.result_name, "CatchSuccess") == 0);

    /* 6. Independent degradation: bad return object on btxu disables ONLY return_reader_available,
          while argument_readers_available remains 1 and SummaryDismissed still decodes! */
    alignas(8) uint8_t wrong_obj[32] = {};
    *reinterpret_cast<void **>(wrong_obj) = &g_classes[0]; /* Not CatchPokemonOutProto! */
    g_btxu_orig_return_value = wrong_obj;
    assert(fn_btxu(pokeball_obj, nullptr) == wrong_obj);

    assert(api->sample(&s) == PGR_IH_OK);
    assert(s.return_reader_available == 0);
    assert(s.argument_readers_available == 1); /* Argument readers unaffected! */

    /* Drain btxu enter + availability change + leave */
    while (api->take_event(&ev) == 1) {
    }

    /* Verify SummaryDismissed still decodes CatchSuccess after btxu return reader faulted */
    fn_summary_dismissed(capture_state, catch_proto, nullptr);
    assert(api->take_event(&ev) == 1);
    assert(ev.target_id == PGR_IH_TGT_CAPTURE_SUMMARY_DISMISSED && ev.phase == PGR_IH_PHASE_ENTER);
    assert(ev.result_status == PGR_IH_RES_DECODED && ev.result_code == 1);

    assert(api->stop("test-done") == PGR_IH_OK);
    assert(g_hook_count == 0);
}

static void test_shared_hook_sites_coexistence(const PgrInteractionHostApi *api) {
    PgrInteractionHostConfig cfg {};
    PgrInteractionHostOptions opts {};
    opts.observe_runtime = 1;
    opts.read_results = 1;
    PgrInteractionHostSample s {};

    /* 1. Unattested required shared site (Feature 49 owns 10, 11, 19; integrated_mask == 0)
          -> NEVER calls pgrazor_hook on 10, 11, 19, fails closed with observer-setup-failed */
    api->reset_for_tests();
    reset_fake_universe(&cfg);
    assert(api->configure(&cfg) == PGR_IH_OK);
    assert(api->shared_sites(PGR_IH_SHARED_MASK_49, 0) == PGR_IH_OK);
    assert(api->start(&opts) == PGR_IH_ERR_HOOK);
    assert(api->sample(&s) == PGR_IH_OK);
    assert(s.status == PGR_IH_STATUS_FAILED);
    assert(strcmp(s.reason, "observer-setup-failed") == 0);
    assert((s.blocked_shared_mask & (1u << PGR_IH_TGT_CAPTURE_BTDD)) != 0);
    assert(g_hook_count == 0);

    /* 2. Unattested optional shared sites (Feature 61 owns optional targets 15 & 18; integrated_mask == 0)
          -> Degrades targets 15 & 18 without calling pgrazor_hook on them, succeeds on remaining 18 targets! */
    api->reset_for_tests();
    reset_fake_universe(&cfg);
    assert(api->configure(&cfg) == PGR_IH_OK);
    assert(api->shared_sites(PGR_IH_SHARED_MASK_61, 0) == PGR_IH_OK);
    assert(api->start(&opts) == PGR_IH_OK);
    assert(api->sample(&s) == PGR_IH_OK);
    assert(s.status == PGR_IH_STATUS_OBSERVING);
    assert(s.blocked_shared_mask == PGR_IH_SHARED_MASK_61);
    assert((s.hooked_direct_mask & PGR_IH_SHARED_MASK_61) == 0);
    assert(g_hook_count == PGR_IH_TOTAL_TARGETS - 2);
    assert(api->stop("done") == PGR_IH_OK);

    /* 3. All shared sites owned by 49, 61, 67 attested as integrated:
          -> Zero direct Dobby hooks on PGR_IH_SHARED_MASK_ALL (10 targets), 10 direct hooks on unowned targets,
             and observe_enter / observe_leave record full paired events + decoded results! */
    api->reset_for_tests();
    reset_fake_universe(&cfg);
    assert(api->configure(&cfg) == PGR_IH_OK);
    assert(api->shared_sites(PGR_IH_SHARED_MASK_ALL, PGR_IH_SHARED_MASK_ALL) == PGR_IH_OK);
    assert(api->start(&opts) == PGR_IH_OK);
    assert(api->sample(&s) == PGR_IH_OK);
    assert(s.status == PGR_IH_STATUS_OBSERVING);
    assert((s.hooked_direct_mask & PGR_IH_SHARED_MASK_ALL) == 0);
    assert(s.installed_target_mask == PGR_IH_ALL_TARGETS_MASK);
    assert(g_hook_count == 10); /* Only the 10 unowned targets hooked via Dobby */

    PgrInteractionHostEvent ev {};
    while (api->take_event(&ev) == 1) {
    }

    /* Simulate Feature 49 forwarding btdd (target 10) and btxu (target 19) */
    alignas(8) uint8_t capture_state[32] = {};
    *reinterpret_cast<void **>(capture_state) = &g_classes[0];
    alignas(8) uint8_t pokeball_obj[32] = {};
    *reinterpret_cast<void **>(pokeball_obj) = &g_classes[10];
    alignas(8) uint8_t catch_proto[32] = {};
    *reinterpret_cast<void **>(catch_proto) = &g_classes[5];
    *reinterpret_cast<int32_t *>(catch_proto + 16) = 1; /* CatchSuccess */

    uint64_t tok10 = api->observe_enter(PGR_IH_TGT_CAPTURE_BTDD, capture_state, catch_proto);
    assert(tok10 != 0);
    g_fake_now_ms += 5;
    api->observe_leave(tok10, nullptr);

    uint64_t tok19 = api->observe_enter(PGR_IH_TGT_POKEBALL_BTXU, pokeball_obj, nullptr);
    assert(tok19 != 0);
    g_fake_now_ms += 7;
    api->observe_leave(tok19, catch_proto);

    assert(api->take_event(&ev) == 1);
    assert(ev.target_id == PGR_IH_TGT_CAPTURE_BTDD && ev.phase == PGR_IH_PHASE_ENTER);
    assert(ev.result_status == PGR_IH_RES_DECODED && ev.result_code == 1);
    assert(api->take_event(&ev) == 1);
    assert(ev.target_id == PGR_IH_TGT_CAPTURE_BTDD && ev.phase == PGR_IH_PHASE_LEAVE);
    assert(ev.duration_ms == 5);

    assert(api->take_event(&ev) == 1);
    assert(ev.target_id == PGR_IH_TGT_POKEBALL_BTXU && ev.phase == PGR_IH_PHASE_ENTER);
    assert(api->take_event(&ev) == 1);
    assert(ev.target_id == PGR_IH_TGT_POKEBALL_BTXU && ev.phase == PGR_IH_PHASE_LEAVE);
    assert(ev.duration_ms == 7 && ev.is_return_result == 1 && ev.result_code == 1);

    assert(api->stop("done") == PGR_IH_OK);
    assert(g_hook_count == 0);
}

static void test_stop_bounds_and_deferred_inflight_cleanup(const PgrInteractionHostApi *api) {
    PgrInteractionHostConfig cfg {};
    PgrInteractionHostOptions opts {};
    opts.observe_runtime = 1;
    opts.read_results = 0;
    PgrInteractionHostSample s {};

    /* 1. Event limit (512 events = 256 paired enter+leave calls) */
    api->reset_for_tests();
    reset_fake_universe(&cfg);
    assert(api->configure(&cfg) == PGR_IH_OK);
    assert(api->start(&opts) == PGR_IH_OK);
    for (int i = 0; i < 256; ++i) {
        uint64_t tok = api->observe_enter(PGR_IH_TGT_CAPTURE_ENTER, nullptr, nullptr);
        if (tok != 0) {
            api->observe_leave(tok, nullptr);
        }
    }
    assert(api->sample(&s) == PGR_IH_OK);
    assert(s.status == PGR_IH_STATUS_COMPLETE && strcmp(s.reason, "event-limit") == 0);
    assert(s.event_count == PGR_IH_LIMIT_EVENTS);
    assert(g_hook_count == 0);

    /* 2. Hit limit (4096 hits via btxu null-return polls, which do not emit events) */
    api->reset_for_tests();
    reset_fake_universe(&cfg);
    assert(api->configure(&cfg) == PGR_IH_OK);
    assert(api->start(&opts) == PGR_IH_OK);
    for (int i = 0; i <= PGR_IH_LIMIT_HITS; ++i) {
        uint64_t tok = api->observe_enter(PGR_IH_TGT_POKEBALL_BTXU, nullptr, nullptr);
        if (tok != 0) {
            api->observe_leave(tok, nullptr);
        }
    }
    assert(api->sample(&s) == PGR_IH_OK);
    assert(s.status == PGR_IH_STATUS_COMPLETE && strcmp(s.reason, "hit-limit") == 0);
    assert(s.hit_count == PGR_IH_LIMIT_HITS + 1);
    assert(s.btxu_pending_polls == PGR_IH_LIMIT_HITS);
    assert(g_hook_count == 0);

    /* 3. Thread limit (4 distinct threads allowed; 5th distinct thread triggers thread-limit) */
    api->reset_for_tests();
    reset_fake_universe(&cfg);
    assert(api->configure(&cfg) == PGR_IH_OK);
    assert(api->start(&opts) == PGR_IH_OK);
    for (int i = 0; i < PGR_IH_LIMIT_THREADS; ++i) {
        std::thread worker([api]() {
            uint64_t tok = api->observe_enter(PGR_IH_TGT_CAPTURE_ENTER, nullptr, nullptr);
            assert(tok != 0);
            api->observe_leave(tok, nullptr);
        });
        worker.join();
    }
    assert(api->sample(&s) == PGR_IH_OK);
    assert(s.status == PGR_IH_STATUS_OBSERVING && s.thread_count == PGR_IH_LIMIT_THREADS);

    std::thread fifth_thread([api]() {
        uint64_t tok = api->observe_enter(PGR_IH_TGT_CAPTURE_ENTER, nullptr, nullptr);
        assert(tok == 0);
    });
    fifth_thread.join();
    assert(api->sample(&s) == PGR_IH_OK);
    assert(s.status == PGR_IH_STATUS_COMPLETE && strcmp(s.reason, "thread-limit") == 0);
    assert(s.dropped_threads == 1);

    /* 4. Window complete (60000ms): no-natural-calls when 0 events, observation-complete when >0 events */
    api->reset_for_tests();
    reset_fake_universe(&cfg);
    assert(api->configure(&cfg) == PGR_IH_OK);
    assert(api->start(&opts) == PGR_IH_OK);
    g_fake_now_ms += PGR_IH_LIMIT_OBSERVE_MS;
    assert(api->tick(100) == PGR_IH_OK);
    assert(api->sample(&s) == PGR_IH_OK);
    assert(s.status == PGR_IH_STATUS_COMPLETE && strcmp(s.reason, "no-natural-calls") == 0);

    api->reset_for_tests();
    reset_fake_universe(&cfg);
    assert(api->configure(&cfg) == PGR_IH_OK);
    assert(api->start(&opts) == PGR_IH_OK);
    {
        uint64_t tok = api->observe_enter(PGR_IH_TGT_CAPTURE_ENTER, nullptr, nullptr);
        assert(tok != 0);
        api->observe_leave(tok, nullptr);
    }
    g_fake_now_ms += PGR_IH_LIMIT_OBSERVE_MS;
    assert(api->tick(100) == PGR_IH_OK);
    assert(api->sample(&s) == PGR_IH_OK);
    assert(s.status == PGR_IH_STATUS_COMPLETE && strcmp(s.reason, "observation-complete") == 0);

    /* 5. Deferred in-flight cleanup when stop() is called mid-callback */
    api->reset_for_tests();
    reset_fake_universe(&cfg);
    assert(api->configure(&cfg) == PGR_IH_OK);
    assert(api->start(&opts) == PGR_IH_OK);

    uint64_t outer_tok = api->observe_enter(PGR_IH_TGT_CAPTURE_ENTER, nullptr, nullptr);
    uint64_t inner_tok = api->observe_enter(PGR_IH_TGT_POKESTOP_NOTIFY_SPIN, nullptr, nullptr);
    assert(outer_tok != 0 && inner_tok != 0);
    assert(api->sample(&s) == PGR_IH_OK);
    assert(s.in_flight == 2);

    /* Call stop() while 2 callbacks are in flight -> hooks stay installed until both leave */
    assert(api->stop("user-stop") == PGR_IH_OK);
    assert(api->sample(&s) == PGR_IH_OK);
    assert(s.status == PGR_IH_STATUS_STOPPED);
    assert(s.in_flight == 2 && s.deferred_cleanup_pending == 1);
    assert(g_hook_count == PGR_IH_TOTAL_TARGETS);

    api->observe_leave(inner_tok, nullptr);
    assert(api->sample(&s) == PGR_IH_OK);
    assert(s.in_flight == 1 && s.deferred_cleanup_pending == 1);
    assert(g_hook_count == PGR_IH_TOTAL_TARGETS);

    api->observe_leave(outer_tok, nullptr);
    assert(api->sample(&s) == PGR_IH_OK);
    assert(s.in_flight == 0 && s.deferred_cleanup_pending == 0);
    assert(s.hooked_direct_mask == 0 && g_hook_count == 0);
}

static void test_transport52_and_scheduler48_integration(const PgrInteractionHostApi *api) {
    const PgrRuntimeTransportApi *rt = resolve_transport_api();
    assert(rt != nullptr);
    PgrRuntimeTransportConfig rt_cfg {};
    rt_cfg.version = PGR_TRANSPORT_CONFIG_VERSION;
    rt_cfg.size = sizeof(rt_cfg);
    rt_cfg.lease_ms = 2500;
    rt_cfg.max_events = 2048;
    rt_cfg.max_string_bytes = 65536;
    rt_cfg.queue_byte_budget = 524288;
    int irc = rt->install(&rt_cfg);
    assert(irc == PGR_TRANSPORT_OK || irc == PGR_TRANSPORT_ERR_RECONFIG);
    drain_transport_events(rt);

    api->reset_for_tests();
    PgrInteractionHostConfig cfg {};
    reset_fake_universe(&cfg);
    assert(api->configure(&cfg) == PGR_IH_OK);

    /* 1. Reader 15 == false when scheduled check comes due -> finishes as skipped ("not-ready") */
    push_transport_snapshot(rt, 1, false, 0, 0);
    assert(api->schedule(1000) == PGR_IH_OK);
    g_fake_now_ms += 1000;
    assert(api->tick(100) == PGR_IH_OK);
    PgrInteractionHostSample s {};
    assert(api->sample(&s) == PGR_IH_OK);
    assert(s.status == PGR_IH_STATUS_SKIPPED && strcmp(s.reason, "not-ready") == 0);
    drain_transport_events(rt);

    /* 2. Reader 15 == true + Pulse 2 (trace request) via on_unity_pulse -> starts runtime observation */
    push_transport_snapshot(rt, 2, true, 0, 1);
    PgrUnityPulse pulse {0, 1, 100, 0};
    /* Note: if scheduler48 is not yet activated, valid(0) == 0 suppresses pulse; tick(100) runs directly */
    assert(api->tick(100) == PGR_IH_OK);
    assert(api->sample(&s) == PGR_IH_OK);
    assert(s.status == PGR_IH_STATUS_OBSERVING && s.running == 1);
    assert(s.observe_runtime == 1 && s.read_results == 1);

    /* 3. Pulse 1 (stop request) -> stops active observation and unhooks all direct targets */
    push_transport_snapshot(rt, 3, true, 1, 1);
    assert(api->tick(100) == PGR_IH_OK);
    assert(api->sample(&s) == PGR_IH_OK);
    assert(s.status == PGR_IH_STATUS_STOPPED && s.running == 0);
    assert(g_hook_count == 0);

    /* Verify Transport 52 received emitted events */
    PgrRuntimeTransportSample rt_s {};
    rt_s.version = PGR_TRANSPORT_SAMPLE_VERSION;
    rt_s.size = sizeof(rt_s);
    assert(rt->sample(&rt_s) == PGR_TRANSPORT_OK);
    assert(rt_s.events_emitted > 0);
    drain_transport_events(rt);
    (void)pulse;
}

}  // namespace


// Bounded critical-path fixture: unchanged included production host with synthetic
// PREPARED metadata, real Dobby trampolines, and actual core52. The older metadata
// scaffolding above is retained but its old fake-hook scenario suite is not run.
namespace {
const PgrRuntimeTransportApi *actual_transport=nullptr;
bool throw_original=false;void *original_self=nullptr,*original_arg=nullptr;
uint32_t original_calls[20]{};
#define PAD69 asm volatile("nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop" ::: "memory")
template<unsigned I> __attribute__((noinline,aligned(32))) void stub0(void *self,const void*){PAD69;original_self=self;++original_calls[I];if(throw_original)throw 769;}
template<unsigned I> __attribute__((noinline,aligned(32))) void stub1(void *self,void *arg,const void*){PAD69;original_self=self;original_arg=arg;++original_calls[I];if(throw_original)throw 769;}
__attribute__((noinline,aligned(32))) void *stub19(void *self,const void*){PAD69;original_self=self;++original_calls[19];if(throw_original)throw 769;return g_btxu_orig_return_value;}
void *sites[20]={(void*)stub0<0>,(void*)stub0<1>,(void*)stub1<2>,(void*)stub0<3>,(void*)stub0<4>,(void*)stub0<5>,(void*)stub0<6>,(void*)stub0<7>,(void*)stub0<8>,(void*)stub0<9>,(void*)stub1<10>,(void*)stub1<11>,(void*)stub1<12>,(void*)stub1<13>,(void*)stub0<14>,(void*)stub0<15>,(void*)stub0<16>,(void*)stub0<17>,(void*)stub0<18>,(void*)stub19};
HookVoid0Fn volatile call0=stub0<0>;HookVoid1Fn volatile call2=stub1<2>;HookRet0Fn volatile call19=stub19;
unsigned roots(){unsigned n=0;for(auto &s:g_gc_slots)if(s.used)++n;return n;}
void prepared_run(){
    g_state.running=true;g_state.stopping=false;g_state.opts.observe_runtime=1;g_state.opts.read_results=0;
    g_state.hit_count=0;g_state.event_count=0;g_state.reserved_events=0;g_state.in_flight=0;
    for(unsigned i=0;i<20;++i){g_state.targets[i].resolved=true;g_state.targets[i].code=(uintptr_t)sites[i];g_state.targets[i].rva=0x2000+i*256;}
    ScopedLock lock;assert(start_observation_locked()==0);
}
}
extern "C" const void *pgrazor_runtime_transport_api(uint32_t v){return v==1?actual_transport:nullptr;}
int main(int argc,char **argv){
    assert(argc==2);void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!lib)puts(dlerror());assert(lib);
    auto get=(const void*(*)(uint32_t))dlsym(lib,"pgrazor_native_api");assert(get);
    auto core=(const pgrazor_native_api_v2*)get(2);assert(core);
    auto exported=(const PgrInteractionHostApi*)core->query_feature(69,1);
    auto exported2=(const PgrInteractionHostApi2*)core->query_feature(69,2);
    assert(exported&&exported->feature_id==69&&exported2&&exported2->size==40&&!exported2->wants_target(0));
    actual_transport=(const PgrRuntimeTransportApi*)core->query_feature(52,1);assert(actual_transport);
    PgrRuntimeTransportConfig tc{1,sizeof(tc),2500,2048,65536,1048576,0,0};assert(actual_transport->install(&tc)==0);
    PgrInteractionHostConfig cfg{};reset_fake_universe(&cfg);g_state.cfg=cfg;g_state.meta_exports=g_fx_meta_exports;g_state.configured=true;
    g_state.cfg.flags=PGR_IH_CFG_NO_AUTO_SHARED_DETECT;g_owner_tid=gettid();
    auto api=(const PgrInteractionHostApi*)pgrazor_interaction_host_api(1);
    auto observer=(const PgrInteractionHostApi2*)pgrazor_interaction_host_api(2);
    alignas(8) uint64_t self[4]{},arg[4]{},result[4]{},moved[4]{};
    call0(self,nullptr);assert(original_calls[0]==1&&roots()==0);
    prepared_run();assert(g_hook_count==20&&g_total_hook_calls==20);
    assert(observer->wants_target(0));call0(self,nullptr);call2(self,arg,nullptr);
    assert(original_calls[0]==2&&original_calls[2]==1&&original_self==self&&original_arg==arg&&roots()==0&&g_state.in_flight==0);
    throw_original=true;
    try{call0(self,nullptr);assert(false);}catch(int x){assert(x==769);}
    try{call2(self,arg,nullptr);assert(false);}catch(int x){assert(x==769);}
    try{call19(self,nullptr);assert(false);}catch(int x){assert(x==769);}
    throw_original=false;assert(roots()==0&&g_state.in_flight==0&&g_state.reserved_events==0);
    g_btxu_orig_return_value=result;gc_move_from=result;gc_move_to=moved;gc_move_on_free=true;
    assert(call19(self,nullptr)==moved&&roots()==0); // self free precedes final result refresh
    gc_fail_object=result;unsigned frees=g_gc_free_calls;
    assert(call19(self,nullptr)==result&&roots()==1&&g_gc_free_calls==frees&&g_state.in_flight==0);
    assert(api->start(nullptr)==PGR_IH_ERR_BUSY); // deferred root cannot be discarded by restart
    assert(call19(self,nullptr)==result&&roots()==1); // bounded inert passthrough
    gc_fail_object=nullptr;assert(api->tick(gettid()+1)==PGR_IH_ERR_INVALID&&roots()==1);
    assert(api->tick(gettid())==0&&roots()==0);
    g_current_thread=nullptr;unsigned before=g_gc_new_calls;call0(self,nullptr);assert(g_gc_new_calls==before);
    assert(api->tick(gettid())==PGR_IH_ERR_INVALID);g_current_thread=(void*)0x1234;assert(g_thread_attach_calls==0);
    uint64_t token=observer->observe_enter(0,self,nullptr);assert(token);assert(api->stop("fixture")==0);
    assert(api->start(nullptr)==PGR_IH_ERR_BUSY);observer->observe_abort(token);assert(g_state.in_flight==0&&g_state.reserved_events==0);
    assert(g_total_unhook_calls==0);before=g_gc_new_calls;call0(self,nullptr);assert(g_gc_new_calls==before);
    prepared_run();assert(g_total_hook_calls==20); // immutable original reused, never re-hooked
    g_fake_now_ms+=60001;PgrUnityPulse forged{1,1,(int32_t)gettid(),0};api->on_unity_pulse(nullptr,&forged);assert(g_state.status==PGR_IH_STATUS_OBSERVING);
    PgrRuntimeTransportSample tr{};assert(actual_transport->sample(&tr)==0&&tr.events_emitted>10&&tr.events_dropped==0&&!tr.fault);
    PgrInteractionHostEvent ev{};assert(api->take_event(&ev)==0); // transport mode has no backup ring
    api->stop("done");assert(roots()==0&&g_total_unhook_calls==0);
    puts("PASS interaction69 real-Dobby direct sites, API2 unwind, deferred-return root, permanent restart, owner/attachment, actual52; metadata prepared fixture only");
}
