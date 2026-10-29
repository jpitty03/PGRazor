// Production fixture for Feature 68 (account_host):
// 1. Verifies Feature 47 metadata resolution, exact RVAs, 24-byte ARM64 prologues,
//    PromiseStatus enum literals (0x8050), and Feature 56 itemsLookup validation
//    (including negative fail-closed and "no available lookup" preservation).
// 2. Executes the unchanged Feature 68 host end-to-end through real Dobby inline hooks,
//    real Feature 16 (accounts), real Feature 2 (adventure_sync), real Feature 48
//    (unity_scheduler), and real Feature 52 (runtime_transport), substituting only
//    the 5 Dobby-hooked code target addresses in PgrAccountHostMetadataOutput so native
//    host execution can call compiled NOP-padded functions through Dobby trampolines.

#include "account_host.cpp"
#include "pgrazor_native_api.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include <sys/wait.h>

namespace fixture {

const pgrazor_native_api_v2 *g_core = nullptr;

// --- Synthetic IL2CPP metadata & managed object model ------------------------------
struct Class;
struct Type {
  std::string name;
  Class *klass = nullptr;
};
struct Class {
  std::string ns;
  std::string name;
  Type *type = nullptr;
  Class *parent = nullptr;
};
struct Field {
  Class *owner = nullptr;
  std::string name;
  uint64_t offset = 0;
  uint32_t flags = 0;
  int32_t static_i32 = 0;
  Type *type = nullptr;
};
struct Method {
  void *code = nullptr; // offset 0: MethodInfo->methodPointer
  Class *owner = nullptr;
  std::string name;
  uint32_t param_count = 0;
  uint32_t flags = 0;
  Type *ret_type = nullptr;
};
static_assert(offsetof(Method, code) == 0, "Method code pointer must be at offset 0");

std::vector<std::unique_ptr<Type>> g_types;
std::vector<std::unique_ptr<Class>> g_classes;
std::vector<std::unique_ptr<Field>> g_fields;
std::vector<std::unique_ptr<Method>> g_methods;

int g_domain_token = 1;
int g_image_token = 1;
int g_thread_token = 1;
void *g_assemblies[1] = {&g_image_token};
bool g_thread_attached = true;
bool hide_player=false,reenter_metadata=false,stop_during_metadata=false;
const PgrAccountHostConfig *reentry_config=nullptr;

constexpr uint64_t kModuleSize = 256ULL * 1024ULL * 1024ULL;
unsigned char *g_module_base = nullptr;

Class *make_class(const char *full_name, Class *parent = nullptr) {
  const char *dot = std::strrchr(full_name, '.');
  std::string ns = dot ? std::string(full_name, static_cast<size_t>(dot - full_name)) : "";
  std::string nm = dot ? std::string(dot + 1) : std::string(full_name);
  auto c = std::make_unique<Class>();
  auto t = std::make_unique<Type>();
  c->ns = ns;
  c->name = nm;
  c->parent = parent;
  t->name = full_name;
  t->klass = c.get();
  c->type = t.get();
  Class *ret = c.get();
  g_types.push_back(std::move(t));
  g_classes.push_back(std::move(c));
  return ret;
}

Field *make_field(Class *owner, const char *name, uint64_t offset, uint32_t flags = 0,
                  int32_t static_val = 0) {
  auto f = std::make_unique<Field>();
  f->owner = owner;
  f->name = name;
  f->offset = offset;
  f->flags = flags;
  f->static_i32 = static_val;
  f->type = owner->type;
  Field *ret = f.get();
  g_fields.push_back(std::move(f));
  return ret;
}

Method *make_method(Class *owner, const char *name, uint32_t argc, uint64_t rva,
                    const unsigned char *prefix, size_t prefix_len, uint32_t flags = 0) {
  auto m = std::make_unique<Method>();
  void *code_addr = g_module_base + rva;
  if (prefix && prefix_len > 0) {
    std::memcpy(code_addr, prefix, prefix_len);
  }
  m->code = code_addr;
  m->owner = owner;
  m->name = name;
  m->param_count = argc;
  m->flags = flags;
  m->ret_type = owner->type;
  Method *ret = m.get();
  g_methods.push_back(std::move(m));
  return ret;
}

// Classes
Class *c_boolean = nullptr;
Class *c_string = nullptr;
Class *c_future = nullptr;
Class *c_scene_context = nullptr;
Class *c_map_scene_context = nullptr;
Class *c_di_container = nullptr;
Class *c_unity_object = nullptr;
Class *c_player_svc = nullptr;
Class *c_iplayer_svc = nullptr;
Class *c_auth_svc = nullptr;
Class *c_iauth_svc = nullptr;
Class *c_wrapper = nullptr;
Class *c_iwrapper = nullptr;
Class *c_prefs = nullptr;
Class *c_promise_status = nullptr;
Class *c_adv_controller = nullptr;

// Lookup methods
Method *m_find_obj = nullptr;
Method *m_get_container = nullptr;
Method *m_resolve = nullptr;

// Accounts & AdvSync methods
Method *m_player_id = nullptr;
Method *m_player_name = nullptr;
Method *m_auth_connected = nullptr;
Method *m_auth_cached = nullptr;
Method *m_wrapper_logout = nullptr;
Method *m_auth_logout = nullptr;
Method *m_prefs_has = nullptr;
Method *m_prefs_del = nullptr;
Method *m_prefs_save = nullptr;

Method *m_adv_init_initial = nullptr;
Method *m_adv_init_instruction = nullptr;
Method *m_adv_init_result = nullptr;
Method *m_adv_on_confirm = nullptr;
Method *m_adv_on_decline = nullptr;
Method *m_adv_update = nullptr;

Field *f_active_provider = nullptr;
Field *f_future_status = nullptr;
Field *f_future_disposed = nullptr;
Field *f_status_succeeded = nullptr;
Field *f_status_failed = nullptr;
Field *f_adv_confirm = nullptr;
Field *f_adv_decline = nullptr;

// --- Fake managed objects & runtime state ------------------------------------------
struct alignas(8) FakeObject {
  Class *klass = nullptr;
  unsigned char data[256]{};
};

struct FakeString {
  Class *klass = nullptr;
  std::vector<uint16_t> utf16;
  std::string utf8;
};

std::vector<std::unique_ptr<FakeString>> g_strings;

FakeString *alloc_fake_string(const char *s) {
  auto fs = std::make_unique<FakeString>();
  fs->klass = c_string;
  fs->utf8 = s ? s : "";
  for (unsigned char c : fs->utf8) {
    fs->utf16.push_back(static_cast<uint16_t>(c));
  }
  FakeString *ret = fs.get();
  g_strings.push_back(std::move(fs));
  return ret;
}

bool is_fake_string(const void *p) {
  for (const auto &s : g_strings) {
    if (s.get() == p) return true;
  }
  return false;
}

FakeObject g_map_ctx_obj{};
FakeObject g_container_obj{};
FakeObject g_player_obj{};
FakeObject g_auth_obj{};
FakeObject g_wrapper_obj{};
FakeObject g_future_obj{};
FakeObject g_dialog_obj{};
FakeObject g_box_true{};
FakeObject g_box_false{};
FakeObject g_type_map{};
FakeObject g_type_iplayer{};
FakeObject g_type_iauth{};
FakeObject g_type_iwrapper{};

FakeString *g_str_uid = nullptr;
FakeString *g_str_trainer = nullptr;

bool g_is_connected = true;
bool g_cached_creds = true;
void *g_active_provider_ptr = &g_auth_obj;
int32_t g_future_status_val = 0;
uint8_t g_future_disposed_val = 0;

std::map<std::string, int> g_prefs_map;
int g_prefs_save_calls = 0;
int g_wrapper_logout_calls = 0;
int g_adv_confirm_calls = 0;
int g_adv_decline_calls = 0;

// GC handle table
struct RootSlot {
  void *obj = nullptr;
  bool live = false;
};
RootSlot g_roots[512]{};
uint32_t g_root_next = 1;
uint32_t g_live_roots = 0;

uint32_t fake_gchandle_new(void *obj, int) {
  if (!obj) return 0;
  assert(g_root_next < 512);
  uint32_t h = g_root_next++;
  g_roots[h] = {obj, true};
  ++g_live_roots;
  return h;
}
void *fake_gchandle_get_target(uint32_t h) {
  if (h == 0 || h >= 512 || !g_roots[h].live) return nullptr;
  return g_roots[h].obj;
}
void fake_gchandle_free(uint32_t h) {
  if (h == 0 || h >= 512 || !g_roots[h].live) return;
  g_roots[h].live = false;
  g_roots[h].obj = nullptr;
  assert(g_live_roots > 0);
  --g_live_roots;
}

// --- Real NOP-padded C++ targets for Dobby inline hooks ----------------------------
#if defined(__aarch64__)
#define HOOK_NOP_PAD asm volatile(".rept 24; nop; .endr" ::: "memory")
#else
#define HOOK_NOP_PAD asm volatile("nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop" ::: "memory")
#endif

extern "C" __attribute__((noinline, aligned(16))) void *fixture_auth_logout(void *self, void *method_info) {
  HOOK_NOP_PAD;
  (void)self;
  (void)method_info;
  return &g_future_obj;
}

extern "C" __attribute__((noinline, aligned(16))) void fixture_adv_init_initial(
    void *self, void *a, void *b, void *c, void *d, void *method_info) {
  HOOK_NOP_PAD;
  (void)self; (void)a; (void)b; (void)c; (void)d; (void)method_info;
}

extern "C" __attribute__((noinline, aligned(16))) void fixture_adv_init_instruction(
    void *self, void *a, void *b, void *c, void *d, void *e, void *f, void *g_arg, void *method_info) {
  HOOK_NOP_PAD;
  (void)self; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g_arg; (void)method_info;
}

extern "C" __attribute__((noinline, aligned(16))) void fixture_adv_init_result(
    void *self, void *a, void *b, void *c, void *d, void *e, void *method_info) {
  HOOK_NOP_PAD;
  (void)self; (void)a; (void)b; (void)c; (void)d; (void)e; (void)method_info;
}

extern "C" __attribute__((noinline, aligned(16))) void fixture_adv_update(void *self, void *method_info) {
  HOOK_NOP_PAD;
  (void)self;
  (void)method_info;
}

extern "C" __attribute__((noinline, aligned(16))) void fixture_adv_on_confirm(void *self, void *method_info) {
  HOOK_NOP_PAD;
  (void)self;
  (void)method_info;
}

extern "C" __attribute__((noinline, aligned(16))) void fixture_adv_on_decline(void *self, void *method_info) {
  HOOK_NOP_PAD;
  (void)self;
  (void)method_info;
}

decltype(&fixture_auth_logout) volatile call_auth_logout = &fixture_auth_logout;
decltype(&fixture_adv_init_initial) volatile call_adv_init_initial = &fixture_adv_init_initial;
decltype(&fixture_adv_init_result) volatile call_adv_init_result = &fixture_adv_init_result;
decltype(&fixture_adv_update) volatile call_adv_update = &fixture_adv_update;

// --- Fake IL2CPP exports -----------------------------------------------------------
void *fake_domain_get() {
 if(stop_during_metadata){stop_during_metadata=false;auto host=static_cast<const PgrAccountHostApi*>(pgrazor_account_host_api(1));assert(host->stop()==0);}
 if(reenter_metadata){reenter_metadata=false;PgrAccountHostMetadataOutput out;std::memset(&out,0xa5,sizeof(out));auto host=static_cast<const PgrAccountHostApi*>(pgrazor_account_host_api(1));assert(host->resolve_metadata(reentry_config,&out)==PGR_ACH_ERR_BUSY);PgrAccountHostMetadataOutput zero{};assert(!std::memcmp(&out,&zero,sizeof(out)));assert(host->configure(reentry_config)==PGR_ACH_ERR_BUSY);}
 return &g_domain_token;
}
void *fake_domain_assemblies(void *, size_t *count) {
  *count = 1;
  return g_assemblies;
}
void *fake_assembly_image(void *a) { return a; }
void *fake_class_from_name(void *, const char *ns, const char *name) {
  if(hide_player&&std::strcmp(name,"PlayerService")==0)return nullptr;
  for (const auto &c : g_classes) {
    if (c->ns == (ns ? ns : "") && c->name == (name ? name : "")) return c.get();
  }
  return nullptr;
}
void *fake_class_get_type(void *k) { return k ? static_cast<Class *>(k)->type : nullptr; }
char *fake_type_get_name(void *t) {
  if (!t) return nullptr;
  const std::string &s = static_cast<Type *>(t)->name;
  char *out = static_cast<char *>(std::malloc(s.size() + 1));
  std::memcpy(out, s.c_str(), s.size() + 1);
  return out;
}
void fake_mem_free(void *p) { std::free(p); }
void *fake_field_from_name(void *owner, const char *name) {
  for (const auto &f : g_fields) {
    if (f->owner == owner && f->name == (name ? name : "")) return f.get();
  }
  return nullptr;
}
void *fake_field_parent(void *f) { return f ? static_cast<Field *>(f)->owner : nullptr; }
int fake_field_flags(void *f) { return f ? static_cast<int>(static_cast<Field *>(f)->flags) : 0; }
size_t fake_field_offset(void *f) { return f ? static_cast<size_t>(static_cast<Field *>(f)->offset) : 0; }
void *fake_field_type(void *f) { return f ? static_cast<Field *>(f)->type : nullptr; }
void *fake_class_from_type(void *t) { return t ? static_cast<Type *>(t)->klass : nullptr; }
bool fake_class_is_valuetype(void *) { return false; }
bool fake_class_is_enum(void *k) { return k == c_promise_status; }
void *fake_class_enum_basetype(void *k) { return k ? static_cast<Class *>(k)->type : nullptr; }
int fake_class_value_size(void *, int *align) {
  if (align) *align = 4;
  return 4;
}
void *fake_method_from_name(void *owner, const char *name, int argc) {
  for (const auto &m : g_methods) {
    if (m->owner == owner && m->name == (name ? name : "") &&
        static_cast<int>(m->param_count) == argc) {
      return m.get();
    }
  }
  return nullptr;
}
void *fake_method_get_class(void *m) { return m ? static_cast<Method *>(m)->owner : nullptr; }
char *fake_method_get_name(void *m) {
  return m ? const_cast<char *>(static_cast<Method *>(m)->name.c_str()) : nullptr;
}
uint32_t fake_method_get_flags(void *m, uint32_t *iflags) {
  if (iflags) *iflags = 0;
  return m ? static_cast<Method *>(m)->flags : 0;
}
uint32_t fake_method_get_param_count(void *m) { return m ? static_cast<Method *>(m)->param_count : 0; }
void *fake_method_get_param(void *m, uint32_t) { return m ? static_cast<Method *>(m)->ret_type : nullptr; }
void *fake_method_get_return_type(void *m) { return m ? static_cast<Method *>(m)->ret_type : nullptr; }
bool fake_method_is_generic(void *) { return false; }
bool fake_method_is_inflated(void *) { return false; }
bool fake_debug_get_method_info(void *m, void *info) {
  if (!m || !info) return false;
  static_cast<void **>(info)[0] = static_cast<Method *>(m)->code;
  return true;
}
bool fake_class_is_assignable_from(void *klass, void *subklass) {
  for (Class *cur = static_cast<Class *>(subklass); cur != nullptr; cur = cur->parent) {
    if (cur == klass) return true;
  }
  return false;
}
void *fake_class_get_methods(void *, void **) { return nullptr; }
void *fake_class_get_nested(void *, void **) { return nullptr; }
void *fake_class_get_declaring(void *) { return nullptr; }

int fake_seg_query(void *, const void *addr, uint64_t size, PgrMetaSegment *out) {
  std::memset(out, 0, sizeof(*out));
  uintptr_t a = reinterpret_cast<uintptr_t>(addr);
  uintptr_t base = reinterpret_cast<uintptr_t>(g_module_base);
  if (a >= base && a-base<=kModuleSize && size<=kModuleSize-(a-base)) {
    out->found = 1;
    out->readable = 1;
    out->host_exec = 1;
    out->seg_base = g_module_base;
    out->seg_size = kModuleSize;
  }
  return 0;
}

void *fake_thread_current() { return g_thread_attached ? &g_thread_token : nullptr; }
void *fake_object_get_class(void *obj) {
  if (!obj) return nullptr;
  if (is_fake_string(obj)) return static_cast<FakeString *>(obj)->klass;
  return static_cast<FakeObject *>(obj)->klass;
}
void *fake_object_new(void *) { return nullptr; }
void *fake_object_unbox(void *obj) {
  return obj ? static_cast<FakeObject *>(obj)->data : nullptr;
}
void fake_field_get_value(void *obj, void *field_raw, void *out) {
  auto *f = static_cast<Field *>(field_raw);
  assert(obj && f && out);
  if (f == f_active_provider) {
    assert(obj == &g_auth_obj);
    *static_cast<void **>(out) = g_active_provider_ptr;
  } else if (f == f_future_status) {
    assert(obj == &g_future_obj);
    *static_cast<int32_t *>(out) = g_future_status_val;
  } else if (f == f_future_disposed) {
    assert(obj == &g_future_obj);
    *static_cast<uint8_t *>(out) = g_future_disposed_val;
  } else {
    assert(false && "unexpected field_get_value");
  }
}
void fake_field_set_value(void *, void *, void *) {}
void fake_field_static_get_value(void *field_raw, void *out) {
  auto *f = static_cast<Field *>(field_raw);
  assert(f && out);
  *static_cast<int32_t *>(out) = f->static_i32;
}
void *fake_type_get_object(void *t) {
  if (t == c_map_scene_context->type) return &g_type_map;
  if (t == c_iplayer_svc->type) return &g_type_iplayer;
  if (t == c_iauth_svc->type) return &g_type_iauth;
  if (t == c_iwrapper->type) return &g_type_iwrapper;
  return nullptr;
}
void *fake_string_new(const char *s) { return alloc_fake_string(s); }
int32_t fake_string_length(void *s) {
  return is_fake_string(s) ? static_cast<int32_t>(static_cast<FakeString *>(s)->utf16.size()) : 0;
}
const uint16_t *fake_string_chars(void *s) {
  return is_fake_string(s) ? static_cast<FakeString *>(s)->utf16.data() : nullptr;
}

void *fake_runtime_invoke(void *method_raw, void *self, void **args, void **exc) {
  if (exc) *exc = nullptr;
  if (method_raw == m_find_obj) {
    assert(self == nullptr && args && args[0] == &g_type_map);
    return &g_map_ctx_obj;
  }
  if (method_raw == m_get_container) {
    assert(self == &g_map_ctx_obj);
    return &g_container_obj;
  }
  if (method_raw == m_resolve) {
    assert(self == &g_container_obj && args);
    if (args[0] == &g_type_iplayer) return &g_player_obj;
    if (args[0] == &g_type_iauth) return &g_auth_obj;
    if (args[0] == &g_type_iwrapper) return &g_wrapper_obj;
    return nullptr;
  }
  if (method_raw == m_player_id) {
    assert(self == &g_player_obj);
    return g_str_uid;
  }
  if (method_raw == m_player_name) {
    assert(self == &g_player_obj);
    return g_str_trainer;
  }
  if (method_raw == m_auth_connected) {
    assert(self == &g_auth_obj);
    return g_is_connected ? &g_box_true : &g_box_false;
  }
  if (method_raw == m_auth_cached) {
    assert(self == &g_auth_obj);
    return g_cached_creds ? &g_box_true : &g_box_false;
  }
  if (method_raw == m_wrapper_logout) {
    assert(self == &g_wrapper_obj);
    ++g_wrapper_logout_calls;
    (void)call_auth_logout(&g_auth_obj, nullptr);
    return nullptr;
  }
  if (method_raw == m_prefs_del) {
    assert(self == nullptr && args && is_fake_string(args[0]));
    g_prefs_map.erase(static_cast<FakeString *>(args[0])->utf8);
    return nullptr;
  }
  if (method_raw == m_prefs_has) {
    assert(self == nullptr && args && is_fake_string(args[0]));
    bool exists = g_prefs_map.count(static_cast<FakeString *>(args[0])->utf8) > 0;
    return exists ? &g_box_true : &g_box_false;
  }
  if (method_raw == m_prefs_save) {
    assert(self == nullptr);
    ++g_prefs_save_calls;
    return nullptr;
  }
  if (method_raw == m_adv_on_confirm) {
    assert(self == &g_dialog_obj);
    ++g_adv_confirm_calls;
    return nullptr;
  }
  if (method_raw == m_adv_on_decline) {
    assert(self == &g_dialog_obj);
    ++g_adv_decline_calls;
    return nullptr;
  }
  assert(false && "unexpected runtime_invoke method");
  return nullptr;
}

PgrMetaExports g_meta_exports{};
PgrAccountHostRuntime g_runtime{};
PgrSharedMetadataOutput g_shared_md{};

void setup_universe() {
  g_module_base=static_cast<unsigned char*>(mmap(nullptr,kModuleSize,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
  assert(g_module_base!=MAP_FAILED);
  assert(g_module_base != nullptr);

  c_boolean = make_class("System.Boolean");
  c_string = make_class("System.String");
  c_future = make_class("Niantic.Platform.Promises.Future");
  c_scene_context = make_class("Zenject.SceneContext");
  c_map_scene_context = make_class("Niantic.Holoholo.MapSceneContext", c_scene_context);
  c_di_container = make_class("Zenject.DiContainer");
  c_unity_object = make_class("UnityEngine.Object");
  c_player_svc = make_class("Niantic.Holoholo.PlayerService");
  c_iplayer_svc = make_class("Niantic.Holoholo.IPlayerService");
  c_auth_svc = make_class("Niantic.Platform.Auth.AuthService");
  c_iauth_svc = make_class("Niantic.Platform.Auth.IAuthService");
  c_wrapper = make_class("Niantic.Holoholo.Login.AuthServiceLogOutWrapper");
  c_iwrapper = make_class("Niantic.Holoholo.Login.IAuthServiceLogOutWrapper");
  c_prefs = make_class("UnityEngine.PlayerPrefs");
  c_promise_status = make_class("Niantic.Platform.Promises.PromiseStatus");
  c_adv_controller = make_class("Niantic.Platform.NiaAdvsyncPermissions.Internal.DialogController");

  f_active_provider = make_field(c_auth_svc, "activeAuthProvider", kOffAuthActiveProvider);
  f_future_status = make_field(c_future, "_status", kOffFutureStatus);
  f_future_disposed = make_field(c_future, "_isDisposed", kOffFutureIsDisposed);
  f_status_succeeded = make_field(c_promise_status, "CompletedSucceeded", 0, 0x8056u, 1);
  f_status_failed = make_field(c_promise_status, "CompletedFailed", 0, 0x8056u, 2);
  f_adv_confirm = make_field(c_adv_controller, "_onConfirm", kOffAdvOnConfirm);
  f_adv_decline = make_field(c_adv_controller, "_onDecline", kOffAdvOnDecline);

  m_find_obj = make_method(c_unity_object, "FindObjectOfType", 1, 0x1000, nullptr, 0, 0x10u);
  m_get_container = make_method(c_scene_context, "get_Container", 0, 0x1100, nullptr, 0, 0u);
  m_resolve = make_method(c_di_container, "Resolve", 1, 0x1200, nullptr, 0, 0u);

  m_player_id = make_method(c_player_svc, "get_PlayerId", 0, kRvaPlayerGetId,
                            kPrefixPlayerGetId, sizeof(kPrefixPlayerGetId));
  m_player_name = make_method(c_player_svc, "get_PlayerName", 0, kRvaPlayerGetName,
                              kPrefixPlayerGetName, sizeof(kPrefixPlayerGetName));
  m_auth_connected = make_method(c_auth_svc, "get_IsConnected", 0, kRvaAuthIsConnected,
                                 kPrefixAuthIsConnected, sizeof(kPrefixAuthIsConnected));
  m_auth_cached = make_method(c_auth_svc, "get_CachedCredentialsExist", 0, kRvaAuthCachedCreds,
                              kPrefixAuthCachedCreds, sizeof(kPrefixAuthCachedCreds));
  m_wrapper_logout = make_method(c_wrapper, "Logout", 0, kRvaWrapperLogout,
                                 kPrefixWrapperLogout, sizeof(kPrefixWrapperLogout));
  m_auth_logout = make_method(c_auth_svc, "Logout", 0, kRvaAuthLogout,
                              kPrefixAuthLogout, sizeof(kPrefixAuthLogout));
  m_prefs_has = make_method(c_prefs, "HasKey", 1, kRvaPrefsHasKey,
                            kPrefixPrefsHasKey, sizeof(kPrefixPrefsHasKey), 0x10u);
  m_prefs_del = make_method(c_prefs, "DeleteKey", 1, kRvaPrefsDeleteKey,
                            kPrefixPrefsDeleteKey, sizeof(kPrefixPrefsDeleteKey), 0x10u);
  m_prefs_save = make_method(c_prefs, "Save", 0, kRvaPrefsSave,
                             kPrefixPrefsSave, sizeof(kPrefixPrefsSave), 0x10u);

  m_adv_init_initial = make_method(c_adv_controller, "InitializeAsInitial", 4, kRvaAdvInitInitial,
                                   kPrefixAdvInitInitial, sizeof(kPrefixAdvInitInitial));
  m_adv_init_instruction = make_method(c_adv_controller, "InitializeAsInstruction", 7, kRvaAdvInitInstruction,
                                       kPrefixAdvInitInstruction, sizeof(kPrefixAdvInitInstruction));
  m_adv_init_result = make_method(c_adv_controller, "InitializeAsResult", 5, kRvaAdvInitResult,
                                  kPrefixAdvInitResult, sizeof(kPrefixAdvInitResult));
  m_adv_on_confirm = make_method(c_adv_controller, "OnConfirm", 0, kRvaAdvOnConfirm,
                                 kPrefixAdvOnConfirm, sizeof(kPrefixAdvOnConfirm));
  m_adv_on_decline = make_method(c_adv_controller, "OnDecline", 0, kRvaAdvOnDecline,
                                 kPrefixAdvOnDecline, sizeof(kPrefixAdvOnDecline));
  m_adv_update = make_method(c_adv_controller, "Update", 0, kRvaAdvUpdate,
                             kPrefixAdvUpdate, sizeof(kPrefixAdvUpdate));

  g_map_ctx_obj.klass = c_map_scene_context;
  g_container_obj.klass = c_di_container;
  g_player_obj.klass = c_player_svc;
  g_auth_obj.klass = c_auth_svc;
  g_wrapper_obj.klass = c_wrapper;
  g_future_obj.klass = c_future;
  g_dialog_obj.klass = c_adv_controller;
  g_box_true.klass = c_boolean;
  g_box_true.data[0] = 1;
  g_box_false.klass = c_boolean;
  g_box_false.data[0] = 0;

  // Populate non-null callback delegates on g_dialog_obj at 0xa8 and 0xb0.
  void *cb_confirm = &g_box_true;
  void *cb_decline = &g_box_false;
  std::memcpy(reinterpret_cast<unsigned char *>(&g_dialog_obj) + kOffAdvOnConfirm, &cb_confirm, sizeof(void *));
  std::memcpy(reinterpret_cast<unsigned char *>(&g_dialog_obj) + kOffAdvOnDecline, &cb_decline, sizeof(void *));

  g_str_uid = alloc_fake_string("uid-777");
  g_str_trainer = alloc_fake_string("AshKetchum");

  g_prefs_map["4b9b44d8-66f5-46dc-86bb-0215bfa90f5e"] = 1;
  g_prefs_map["80a6f7e6-1a8d-442e-bd95-74c795242b3f"] = 1;
  g_prefs_map["keep-other-pref"] = 42;

  g_meta_exports = {
      reinterpret_cast<void *>(fake_domain_get),
      reinterpret_cast<void *>(fake_domain_assemblies),
      reinterpret_cast<void *>(fake_assembly_image),
      reinterpret_cast<void *>(fake_class_from_name),
      reinterpret_cast<void *>(fake_class_get_type),
      reinterpret_cast<void *>(fake_type_get_name),
      reinterpret_cast<void *>(fake_mem_free),
      reinterpret_cast<void *>(fake_field_from_name),
      reinterpret_cast<void *>(fake_field_parent),
      reinterpret_cast<void *>(fake_field_flags),
      reinterpret_cast<void *>(fake_field_offset),
      reinterpret_cast<void *>(fake_field_type),
      reinterpret_cast<void *>(fake_class_from_type),
      reinterpret_cast<void *>(fake_class_is_valuetype),
      reinterpret_cast<void *>(fake_class_is_enum),
      reinterpret_cast<void *>(fake_class_enum_basetype),
      reinterpret_cast<void *>(fake_class_value_size),
      reinterpret_cast<void *>(fake_method_from_name),
      reinterpret_cast<void *>(fake_method_get_class),
      reinterpret_cast<void *>(fake_method_get_name),
      reinterpret_cast<void *>(fake_method_get_flags),
      reinterpret_cast<void *>(fake_method_get_param_count),
      reinterpret_cast<void *>(fake_method_get_param),
      reinterpret_cast<void *>(fake_method_get_return_type),
      reinterpret_cast<void *>(fake_method_is_generic),
      reinterpret_cast<void *>(fake_method_is_inflated),
      g_module_base+0x1000,
      reinterpret_cast<void *>(fake_class_is_assignable_from),
      reinterpret_cast<void *>(fake_class_get_methods),
      reinterpret_cast<void *>(fake_class_get_nested),
      reinterpret_cast<void *>(fake_class_get_declaring)};

  // Actual native-ISA executable debug shim INSIDE the sparse module, as47 requires.
  unsigned char *shim=g_module_base+0x1000;uintptr_t fn=reinterpret_cast<uintptr_t>(fake_debug_get_method_info);
#if defined(__x86_64__)
  const unsigned char jump[]={0x48,0xb8};std::memcpy(shim,jump,2);std::memcpy(shim+2,&fn,8);shim[10]=0xff;shim[11]=0xe0;
#elif defined(__aarch64__)
  const uint32_t jump[]={0x58000050,0xd61f0200};std::memcpy(shim,jump,8);std::memcpy(shim+8,&fn,8);
#else
#error Unsupported fixture ISA
#endif
  assert(mprotect(shim,4096,PROT_READ|PROT_EXEC)==0);__builtin___clear_cache(reinterpret_cast<char*>(shim),reinterpret_cast<char*>(shim+32));
  g_runtime.version = PGR_ACH_RUNTIME_VERSION;
  g_runtime.size = sizeof(PgrAccountHostRuntime);
  g_runtime.boot = {};
  g_runtime.boot.runtime_invoke=reinterpret_cast<void *>(fake_runtime_invoke);
  g_runtime.boot.runtime_invoke_convert_args=reinterpret_cast<void *>(fake_thread_current);
  g_runtime.boot.thread_attach=reinterpret_cast<void *>(fake_thread_current);
  g_runtime.boot.thread_detach=reinterpret_cast<void *>(fake_thread_current);
  g_runtime.boot.thread_current=reinterpret_cast<void *>(fake_thread_current);
  g_runtime.boot.gchandle_new=reinterpret_cast<void *>(fake_gchandle_new);
  g_runtime.boot.gchandle_new_weakref=reinterpret_cast<void *>(fake_thread_current);
  g_runtime.boot.gchandle_get_target=reinterpret_cast<void *>(fake_gchandle_get_target);
  g_runtime.boot.gchandle_free=reinterpret_cast<void *>(fake_gchandle_free);
  g_runtime.boot.field_get_value=reinterpret_cast<void *>(fake_field_get_value);
  g_runtime.boot.field_set_value=reinterpret_cast<void *>(fake_field_set_value);
  g_runtime.boot.field_static_get_value=reinterpret_cast<void *>(fake_field_static_get_value);
  g_runtime.boot.field_static_set_value=reinterpret_cast<void *>(fake_thread_current);
  g_runtime.boot.object_new=reinterpret_cast<void *>(fake_object_new);
  g_runtime.boot.object_unbox=reinterpret_cast<void *>(fake_object_unbox);
  g_runtime.boot.object_get_class=reinterpret_cast<void *>(fake_object_get_class);
  g_runtime.boot.value_box=reinterpret_cast<void *>(fake_thread_current);
  g_runtime.boot.string_new=reinterpret_cast<void *>(fake_string_new);
  g_runtime.boot.string_chars=reinterpret_cast<void *>(fake_string_chars);
  g_runtime.boot.array_new=reinterpret_cast<void *>(fake_thread_current);
  g_runtime.type_get_object = reinterpret_cast<void *>(fake_type_get_object);
  g_runtime.string_new = reinterpret_cast<void *>(fake_string_new);
  g_runtime.string_length = reinterpret_cast<void *>(fake_string_length);

  std::memset(&g_shared_md, 0, sizeof(g_shared_md));
  g_shared_md.version = 2;
  g_shared_md.size = sizeof(g_shared_md);
  g_shared_md.optional[OPT_ITEMS_LOOKUP] = PGR_SHARED_OPT_AVAILABLE;
  g_shared_md.opt_handles[H_IL_MAP] = c_map_scene_context;
  g_shared_md.opt_handles[H_IL_FIND_M] = m_find_obj;
  g_shared_md.opt_handles[H_IL_CONTAINER_M] = m_get_container;
  g_shared_md.opt_handles[H_IL_RESOLVE_M] = m_resolve;
}

}  // namespace fixture

#define FORWARD_FEATURE(NAME, ID)                                                  \
  extern "C" const void *NAME(uint32_t v) {                                        \
    return fixture::g_core ? fixture::g_core->query_feature(ID, v) : nullptr;      \
  }
FORWARD_FEATURE(pgrazor_adventure_sync_api, 2)
extern "C" const pgrazor_accounts_api_v1_t *pgrazor_accounts_api(uint32_t v){return fixture::g_core?static_cast<const pgrazor_accounts_api_v1_t*>(fixture::g_core->query_feature(16,v)):nullptr;}
FORWARD_FEATURE(pgrazor_runtime_metadata_api, 47)
FORWARD_FEATURE(pgrazor_unity_scheduler_api, 48)
FORWARD_FEATURE(pgrazor_runtime_transport_api, 52)
FORWARD_FEATURE(pgrazor_runtime_bootstrap_api, 54)
FORWARD_FEATURE(pgrazor_shared_host_api, 61)
#undef FORWARD_FEATURE

int main(int argc, char **argv) {
  using namespace fixture;
  assert(argc == 2);
  void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!lib) std::puts(dlerror());
  assert(lib);

  auto get_api = reinterpret_cast<const void *(*)(uint32_t)>(dlsym(lib, "pgrazor_native_api"));
  assert(get_api);
  g_core = static_cast<const pgrazor_native_api_v2 *>(get_api(2));
  assert(g_core && g_core->query_feature);

  const auto *host = static_cast<const PgrAccountHostApi *>(pgrazor_account_host_api(1));
  assert(host && host->version == 1 && host->size == sizeof(PgrAccountHostApi));
  assert(pgrazor_account_host_api(2) == nullptr);

  setup_universe();

  // =========================================================================
  // 1. Real Feature 47 Metadata + RVA + 24-Byte Prologue + itemsLookup Proof
  // =========================================================================
  PgrAccountHostConfig cfg{};
  cfg.version = PGR_ACH_CONFIG_VERSION;
  cfg.size = sizeof(cfg);
  cfg.flags = PGR_ACH_CFG_REQ_ALL;
  cfg.owner_tid = self_tid();
  cfg.module_base = g_module_base;
  cfg.module_size = kModuleSize;
  cfg.meta_exports = &g_meta_exports;
  cfg.runtime = &g_runtime;
  cfg.shared_metadata = &g_shared_md;
  cfg.seg_query = fake_seg_query;

  // Independent fresh processes: no transient probe may latch a failed install.
  for(unsigned mode=0;mode<4;++mode){pid_t child=fork();assert(child>=0);if(!child){
    // fork changes OS tid: fixture proof must identify the child, not its parent.
    cfg.owner_tid=self_tid();
    if(mode==0){assert(host->configure(nullptr)==PGR_ACH_NOT_READY);assert(!g.configured&&!g.accounts_attempted&&!g.advsync_attempted);}
    if(mode==1){cfg.flags=PGR_ACH_CFG_REQ_ACCOUNTS;hide_player=true;int rc=host->configure(&cfg);if(rc!=PGR_ACH_NOT_READY){std::fprintf(stderr,"account68 child mode=%u configure rc=%d owner=%llu actual=%llu\n",mode,rc,(unsigned long long)cfg.owner_tid,(unsigned long long)self_tid());std::fflush(stderr);}assert(rc==PGR_ACH_NOT_READY);assert(!g.configured&&!g.accounts_attempted&&!g.advsync_attempted);hide_player=false;PgrAccountHostMetadataOutput retry{};assert(host->resolve_metadata(&cfg,&retry)==0&&retry.accounts_valid);}
    if(mode==2){reentry_config=&cfg;reenter_metadata=true;PgrAccountHostMetadataOutput result{};assert(host->resolve_metadata(&cfg,&result)==0&&!reenter_metadata);}
    if(mode==3){stop_during_metadata=true;assert(host->configure(&cfg)==PGR_ACH_ERR_STOPPED);assert(!g.accounts_attempted&&!g.advsync_attempted);}
    _exit(0);
   }int status=0;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
  }

  PgrAccountHostMetadataOutput md_out{};
  assert(host->resolve_metadata(&cfg, &md_out) == PGR_ACH_OK);
  assert(md_out.accounts_valid == 1 && md_out.advsync_valid == 1);
  assert(md_out.promise_succeeded_value == 1 && md_out.promise_failed_value == 2);
  assert(md_out.di_container_class == c_di_container);
  assert(md_out.map_scene_context_class == c_map_scene_context);
  assert(md_out.adv_off_on_confirm == 0xa8 && md_out.adv_off_on_decline == 0xb0);

  // Negative check A: corrupted 24-byte prologue on AuthService.Logout fails Accounts closed,
  // while AdventureSync metadata still validates independently.
  g_module_base[kRvaAuthLogout] ^= 0xFFu;
  PgrAccountHostMetadataOutput bad_acc_md{};
  assert(host->resolve_metadata(&cfg, &bad_acc_md) == PGR_ACH_ERR_METADATA);
  assert(bad_acc_md.accounts_valid == 0 && bad_acc_md.advsync_valid == 1);
  g_module_base[kRvaAuthLogout] ^= 0xFFu;

  // Negative check B: corrupted 24-byte prologue on DialogController.Update fails AdventureSync
  // closed, while Accounts metadata still validates independently.
  g_module_base[kRvaAdvUpdate] ^= 0xFFu;
  PgrAccountHostMetadataOutput bad_adv_md{};
  assert(host->resolve_metadata(&cfg, &bad_adv_md) == PGR_ACH_ERR_METADATA);
  assert(bad_adv_md.accounts_valid == 1 && bad_adv_md.advsync_valid == 0);
  g_module_base[kRvaAdvUpdate] ^= 0xFFu;

  // Negative check C: missing OPT_ITEMS_LOOKUP preserves "no available lookup" behavior
  // (Accounts fails closed, AdventureSync remains valid).
  g_shared_md.optional[OPT_ITEMS_LOOKUP] = PGR_SHARED_OPT_UNAVAILABLE;
  PgrAccountHostMetadataOutput no_lookup_md{};
  assert(host->resolve_metadata(&cfg, &no_lookup_md) == PGR_ACH_ERR_METADATA);
  assert(no_lookup_md.accounts_valid == 0 && no_lookup_md.advsync_valid == 1);
  g_shared_md.optional[OPT_ITEMS_LOOKUP] = PGR_SHARED_OPT_AVAILABLE;

  // Negative check D: unassignable get_Container declaring class fails Accounts closed.
  m_get_container->owner = c_di_container;
  PgrAccountHostMetadataOutput bad_owner_md{};
  assert(host->resolve_metadata(&cfg, &bad_owner_md) == PGR_ACH_ERR_METADATA);
  assert(bad_owner_md.accounts_valid == 0 && bad_owner_md.advsync_valid == 1);
  m_get_container->owner = c_scene_context;

  // Negative check E: enum literal missing 0x8050 flags fails Accounts closed.
  f_status_succeeded->flags = 0x0010u;
  PgrAccountHostMetadataOutput bad_enum_md{};
  assert(host->resolve_metadata(&cfg, &bad_enum_md) == PGR_ACH_ERR_METADATA);
  assert(bad_enum_md.accounts_valid == 0 && bad_enum_md.advsync_valid == 1);
  f_status_succeeded->flags = 0x8056u;

  // =========================================================================
  // 2. End-to-End Real Dobby + Real Feature 16 + Real Feature 2 Execution
  // =========================================================================
  // Substitute ONLY the 5 Dobby-hooked code target addresses in validated md_out
  // with real NOP-padded C++ functions so Dobby trampolines execute native code.
  md_out.auth_logout_target_fn = reinterpret_cast<void *>(&fixture_auth_logout);
  md_out.adv_code_init_initial = reinterpret_cast<void *>(&fixture_adv_init_initial);
  md_out.adv_code_init_instruction = reinterpret_cast<void *>(&fixture_adv_init_instruction);
  md_out.adv_code_init_result = reinterpret_cast<void *>(&fixture_adv_init_result);
  md_out.adv_code_update = reinterpret_cast<void *>(&fixture_adv_update);
  md_out.adv_code_on_confirm = reinterpret_cast<void *>(&fixture_adv_on_confirm);
  md_out.adv_code_on_decline = reinterpret_cast<void *>(&fixture_adv_on_decline);

  cfg.flags = PGR_ACH_CFG_SUBSTITUTE_METADATA | PGR_ACH_CFG_REQ_ALL;
  cfg.substituted_metadata = &md_out;

  // Actual Dobby family isolation: metadata readiness is injected only AFTER the
  // real47 validation above; all installed native2/16 leaves remain production.
  for(unsigned mode=0;mode<2;++mode){pid_t child=fork();assert(child>=0);if(!child){
    cfg.owner_tid=self_tid();auto partial=md_out;partial.accounts_valid=0;
    partial.accounts_error=mode?PGR_ACH_NOT_READY:PGR_ACH_ERR_METADATA;
    cfg.substituted_metadata=&partial;
    assert(host->configure(&cfg)==(mode?PGR_ACH_NOT_READY:PGR_ACH_OK));
    PgrAccountHostSample s{};assert(host->sample(&s)==0&&s.configured&&s.capabilities==PGR_ACH_CAP_ADVENTURE_SYNC);
    assert(g.advsync_attempted);assert(g.accounts_attempted==(mode==0));
    assert(host->set_adventure_sync_allowed(1)==0);
    call_adv_init_initial(&g_dialog_obj,nullptr,nullptr,nullptr,nullptr,nullptr);usleep(120*1000);call_adv_update(&g_dialog_obj,nullptr);
    assert(g_adv_decline_calls==1&&g_adv_confirm_calls==0);
    if(mode){partial.accounts_valid=1;partial.accounts_error=0;assert(host->configure(&cfg)==0);assert(host->sample(&s)==0&&s.capabilities==(PGR_ACH_CAP_ACCOUNTS|PGR_ACH_CAP_ADVENTURE_SYNC));}
    else assert(host->configure(&cfg)==0);
    // Patched AdventureSync prologues must not be revalidated/re-hooked on retry.
    assert(g.advsync_attempted&&host->configure(&cfg)==0);assert(host->tick(self_tid())==0);assert(host->stop()==0);
    _exit(0);
   }int status=0;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
  }

  // Reject bad config flags or off-owner tid before configure.
  auto bad_cfg = cfg;
  bad_cfg.flags = 0x100u;
  assert(host->configure(&bad_cfg) == PGR_ACH_ERR_BAD_CONFIG);
  bad_cfg = cfg;
  bad_cfg.owner_tid = self_tid() + 1;
  assert(host->configure(&bad_cfg) == PGR_ACH_ERR_OFF_OWNER);

  assert(host->configure(&cfg) == PGR_ACH_OK);
  // Idempotent reconfigure with identical binding succeeds; changed module_base rejects.
  assert(host->configure(&cfg) == PGR_ACH_OK);
  bad_cfg = cfg;
  bad_cfg.module_size = kModuleSize - 4096;
  assert(host->configure(&bad_cfg) == PGR_ACH_ERR_RECONFIG);

  PgrAccountHostSample sample{};
  assert(host->sample(&sample) == PGR_ACH_OK);
  assert(sample.configured == 1);
  assert(sample.capabilities == (PGR_ACH_CAP_ACCOUNTS | PGR_ACH_CAP_ADVENTURE_SYNC));
  assert(sample.accounts_error == 0 && sample.advsync_error == 0);

  // Drain initial ready events.
  bool saw_acc_ready = false;
  bool saw_adv_ready = false;
  PgrAccountHostEvent ev{};
  while (host->next_event(&ev) == PGR_ACH_OK) {
    if (std::strcmp(ev.kind, "accounts-ready") == 0) {
      saw_acc_ready = true;
      assert(std::strstr(ev.json, "\"available\":true") != nullptr);
    } else if (std::strcmp(ev.kind, "adventure-sync-native-ready") == 0) {
      saw_adv_ready = true;
    }
  }
  assert(saw_acc_ready && saw_adv_ready);

  // 2a. Account Identity Polling on Unity owner thread
  PgrAccountHostOptions opts{};
  opts.version = PGR_ACH_OPTIONS_VERSION;
  opts.size = sizeof(opts);
  opts.flags = PGR_ACH_OPT_SET_ACCOUNTS;
  opts.accounts_allowed = 1;
  opts.accounts_lease_ms = 2000;
  opts.command = "";
  opts.command_token = "";
  assert(host->set_options(&opts) == PGR_ACH_OK);
  assert(host->tick(self_tid()) == PGR_ACH_OK);

  bool saw_identity = false;
  while (host->next_event(&ev) == PGR_ACH_OK) {
    if (std::strcmp(ev.kind, "account-identity") == 0) {
      saw_identity = true;
      assert(std::strcmp(ev.id, "uid-777") == 0);
      assert(std::strcmp(ev.name, "AshKetchum") == 0);
    }
  }
  assert(saw_identity);
  assert(g_live_roots == 0);

  // 2b. Invalid / 0 / overflow tokens fail closed without running save-session
  const char *bad_tokens[] = {"", "0", "00", "-5", "abc", "9223372036854775808"};
  for (const char *bt : bad_tokens) {
    opts.command = "save-session:bad";
    opts.command_token = bt;
    assert(host->set_options(&opts) == PGR_ACH_OK);
    assert(host->sample(&sample) == PGR_ACH_OK);
    assert(sample.last_cmd_kind == PGR_ACC_CMD_NONE && sample.last_cmd_token == 0);
  }
  assert(g_prefs_save_calls == 0);

  // 2c. Exact 64-bit token >2^53 executes save-session once. Tokens must increase:
  // actual16 rejects every command at/below its last accepted token.
  usleep(260 * 1000);
  opts.command = "save-session:req-1";
  opts.command_token = "9007199254740993";
  assert(host->set_options(&opts) == PGR_ACH_OK);
  assert(host->sample(&sample) == PGR_ACH_OK);
  assert(sample.last_cmd_kind == PGR_ACC_CMD_SAVE_SESSION);
  assert(sample.last_cmd_token == 9007199254740993ULL);
  assert(parse_token_exact64("9223372036854775807")==UINT64_C(9223372036854775807));
  assert(host->tick(self_tid()) == PGR_ACH_OK);
  assert(g_prefs_save_calls == 1);

  bool saw_saved = false;
  while (host->next_event(&ev) == PGR_ACH_OK) {
    if (std::strcmp(ev.kind, "account-session-saved") == 0) saw_saved = true;
  }
  assert(saw_saved);

  // 2d. Sign-in reset (reset-login:...) deletes both DOB PlayerPrefs keys and saves
  usleep(260 * 1000);
  opts.command = "reset-login:req-2";
  opts.command_token = "9223372036854775805"; // greater than save token
  assert(host->set_options(&opts) == PGR_ACH_OK);
  assert(host->tick(self_tid()) == PGR_ACH_OK);
  assert(g_prefs_map.count("4b9b44d8-66f5-46dc-86bb-0215bfa90f5e") == 0);
  assert(g_prefs_map.count("80a6f7e6-1a8d-442e-bd95-74c795242b3f") == 0);
  assert(g_prefs_map.count("keep-other-pref") == 1);
  assert(g_prefs_save_calls == 2);

  bool saw_reset = false;
  while (host->next_event(&ev) == PGR_ACH_OK) {
    if (std::strcmp(ev.kind, "account-login-reset-complete") == 0) saw_reset = true;
  }
  assert(saw_reset);
  assert(g_live_roots == 0);

  // 2e. Logout command triggers real Dobby hook on AuthService.Logout, captures Future,
  //     and completes when Future._status == CompletedSucceeded and !IsConnected
  usleep(260 * 1000);
  opts.command = "logout:req-3";
  opts.command_token = "9223372036854775806";
  assert(host->set_options(&opts) == PGR_ACH_OK);
  assert(host->tick(self_tid()) == PGR_ACH_OK);
  assert(g_wrapper_logout_calls == 1);

  bool saw_session_ended = false;
  bool saw_logout_req = false;
  while (host->next_event(&ev) == PGR_ACH_OK) {
    if (std::strcmp(ev.kind, "account-session-ended") == 0) saw_session_ended = true;
    if (std::strcmp(ev.kind, "account-logout-requested") == 0) {
      saw_logout_req = true;
      assert(std::strstr(ev.json, "\"futureCaptured\":true") != nullptr);
    }
  }
  assert(saw_session_ended && saw_logout_req);
  assert(g_live_roots == 2); // auth_handle + future_handle held during logout

  g_future_status_val = 1; // CompletedSucceeded
  g_is_connected = false;
  usleep(260 * 1000);
  assert(host->tick(self_tid()) == PGR_ACH_OK);

  bool saw_logout_complete = false;
  while (host->next_event(&ev) == PGR_ACH_OK) {
    if (std::strcmp(ev.kind, "account-logout-complete") == 0) saw_logout_complete = true;
  }
  assert(saw_logout_complete);
  assert(g_live_roots == 0);

  // 2f. Real Feature 2 (AdventureSync) Dobby hooks on DialogController
  assert(host->set_adventure_sync_allowed(1) == PGR_ACH_OK);
  call_adv_init_initial(&g_dialog_obj, nullptr, nullptr, nullptr, nullptr, nullptr);
  call_adv_update(&g_dialog_obj, nullptr);
  assert(g_adv_decline_calls==0&&g_adv_confirm_calls==0); // real2 settles >=100ms
  usleep(120*1000);
  call_adv_update(&g_dialog_obj,nullptr);
  assert(g_adv_decline_calls == 1 && g_adv_confirm_calls == 0);

  call_adv_init_result(&g_dialog_obj, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
  call_adv_update(&g_dialog_obj, nullptr);
  assert(g_adv_confirm_calls==0&&g_adv_decline_calls==1);
  usleep(120*1000);
  call_adv_update(&g_dialog_obj,nullptr);
  assert(g_adv_confirm_calls == 1 && g_adv_decline_calls == 1);

  assert(host->tick(self_tid()) == PGR_ACH_OK);
  assert(g_live_roots == 0);

  // Disallow AdventureSync: subsequent dialog initializations are ignored.
  assert(host->set_adventure_sync_allowed(0) == PGR_ACH_OK);
  call_adv_init_initial(&g_dialog_obj, nullptr, nullptr, nullptr, nullptr, nullptr);
  usleep(120*1000);
  call_adv_update(&g_dialog_obj, nullptr);
  assert(g_adv_decline_calls == 1);

  // 2g. Foreign thread rejection and terminal stop cleanup
  std::thread foreign([&]() {
    assert(host->tick(self_tid()) == PGR_ACH_ERR_OFF_OWNER);
    assert(host->stop() == PGR_ACH_OK);
  });
  foreign.join();

  assert(host->tick(self_tid()) == PGR_ACH_OK);
  assert(host->sample(&sample) == PGR_ACH_OK);
  assert(sample.stopping == 1 && sample.accounts_cleared == 1 && sample.owned == 0);
  assert(host->set_options(&opts) == PGR_ACH_ERR_STOPPED);
  assert(host->set_adventure_sync_allowed(1) == PGR_ACH_ERR_STOPPED);
  // Simulate a previously-entered setter reaching its pure publication after stop.
  const auto *acc=pgrazor_accounts_api(1);pgrazor_account_snapshot_v1_t before{},after{};acc->api_sample(&before);
  publish_accounts(true,2000,PGR_ACC_CMD_LOGOUT,UINT64_C(9223372036854775807));
  publish_advsync(true,2500);acc->api_sample(&after);
  assert(before.last_accepted_token==after.last_accepted_token&&after.active_gc_roots==0);

  assert(munmap(g_module_base,kModuleSize)==0);
  std::puts("PASS account68 unchanged-host fixture: real Feature 47 metadata/RVA/prologue/enum/itemsLookup validation + real Dobby/Feature 16/Feature 2 execution.");
  return 0;
}
