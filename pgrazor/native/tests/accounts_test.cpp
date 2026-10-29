#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../accounts.h"

static_assert(sizeof(void*) == 8, "LP64 test target required");
static_assert(sizeof(pgrazor_accounts_config_v1_t) == 328, "LP64 config size");
static_assert(sizeof(pgrazor_account_event_v1_t) == 1056, "LP64 event size");
static_assert(sizeof(pgrazor_account_snapshot_v1_t) == 1080, "LP64 snapshot size");
static_assert(sizeof(pgrazor_accounts_api_v1_t) == 56, "LP64 api table size");

namespace {
void* g_hook_target = nullptr;
void* g_hook_replacement = nullptr;
int g_hook_install_rc = 0;
int g_hook_install_calls = 0;
}  // namespace

#if PGR_ACCOUNTS_REAL_DOBBY
// Real-Dobby mode keeps scenario counters and forced-install errors, then delegates
// through the explicit C-to-C++ test bridge to the production hook engine.
extern "C" int pgrazor_test_real_hook(void* target, void* replacement, void** original);
extern "C" int pgrazor_hook(void* target, void* replacement, void** original) {
  ++g_hook_install_calls;
  if (g_hook_install_rc != 0) {
    return g_hook_install_rc;
  }
  g_hook_target = target;
  g_hook_replacement = replacement;
  return pgrazor_test_real_hook(target, replacement, original);
}
#else
// Stub mode (default): provide pgrazor_hook directly; no Dobby, no real patching.
extern "C" int pgrazor_hook(void* target, void* replacement, void** original) {
  ++g_hook_install_calls;
  if (g_hook_install_rc != 0) {
    return g_hook_install_rc;
  }
  g_hook_target = target;
  g_hook_replacement = replacement;
  if (original) {
    *original = target;
  }
  return 0;
}
#endif

#include "../accounts.cpp"

namespace {

struct FakeClass {
  const char* name;
};
struct FakeType {
  Il2CppClass* klass;
};
struct FakeField {
  const char* name;
};
struct FakeMethod {
  const char* name;
};

struct FakeObject {
  Il2CppClass* klass = nullptr;
  bool bool_val = false;
  std::u16string str_val;
  void* active_provider = nullptr;
  uint8_t future_disposed = 0;
  int32_t future_status = 0;
};

FakeClass kClsBoolean{"System.Boolean"};
FakeClass kClsString{"System.String"};
FakeClass kClsFuture{"Future"};
FakeClass kClsMapCtx{"MapSceneContext"};
FakeClass kClsContainer{"DiContainer"};
FakeClass kClsPlayer{"PlayerService"};
FakeClass kClsPlayerContract{"IPlayerService"};
FakeClass kClsAuth{"AuthService"};
FakeClass kClsAuthContract{"IAuthService"};
FakeClass kClsWrapper{"AuthServiceLogOutWrapper"};
FakeClass kClsWrapperContract{"IAuthServiceLogOutWrapper"};
FakeClass kClsPrefs{"PlayerPrefs"};
FakeClass kClsType{"System.RuntimeType"};

FakeType kTypeMap{reinterpret_cast<Il2CppClass*>(&kClsMapCtx)};
FakeType kTypePlayer{reinterpret_cast<Il2CppClass*>(&kClsPlayerContract)};
FakeType kTypeAuth{reinterpret_cast<Il2CppClass*>(&kClsAuthContract)};
FakeType kTypeWrapper{reinterpret_cast<Il2CppClass*>(&kClsWrapperContract)};

FakeField kFldActiveProvider{"activeAuthProvider"};
FakeField kFldFutureStatus{"_status"};
FakeField kFldFutureDisposed{"_isDisposed"};

FakeMethod kMthFindObj{"FindObjectOfType"};
FakeMethod kMthGetContainer{"get_Container"};
FakeMethod kMthResolve{"Resolve"};
FakeMethod kMthGetPlayerId{"get_PlayerId"};
FakeMethod kMthGetPlayerName{"get_PlayerName"};
FakeMethod kMthGetConnected{"get_IsConnected"};
FakeMethod kMthGetCached{"get_CachedCredentialsExist"};
FakeMethod kMthWrapperLogout{"Wrapper.Logout"};
FakeMethod kMthAuthLogout{"Auth.Logout"};
FakeMethod kMthPrefsHasKey{"PlayerPrefs.HasKey"};
FakeMethod kMthPrefsDeleteKey{"PlayerPrefs.DeleteKey"};
FakeMethod kMthPrefsSave{"PlayerPrefs.Save"};

struct FakeRuntime {
  bool prefs_save_throws = false;
  bool first_delete_throws = false;
  FakeObject type_map_obj{reinterpret_cast<Il2CppClass*>(&kClsType)};
  FakeObject type_player_obj{reinterpret_cast<Il2CppClass*>(&kClsType)};
  FakeObject type_auth_obj{reinterpret_cast<Il2CppClass*>(&kClsType)};
  FakeObject type_wrapper_obj{reinterpret_cast<Il2CppClass*>(&kClsType)};

  FakeObject map_ctx_obj{reinterpret_cast<Il2CppClass*>(&kClsMapCtx)};
  FakeObject container_obj{reinterpret_cast<Il2CppClass*>(&kClsContainer)};
  FakeObject player_obj{reinterpret_cast<Il2CppClass*>(&kClsPlayer)};
  FakeObject auth_obj{reinterpret_cast<Il2CppClass*>(&kClsAuth)};
  FakeObject wrapper_obj{reinterpret_cast<Il2CppClass*>(&kClsWrapper)};
  FakeObject future_obj{reinterpret_cast<Il2CppClass*>(&kClsFuture)};
  FakeObject exc_obj{reinterpret_cast<Il2CppClass*>(&kClsString)};

  FakeObject bool_true{reinterpret_cast<Il2CppClass*>(&kClsBoolean), true};
  FakeObject bool_false{reinterpret_cast<Il2CppClass*>(&kClsBoolean), false};
  FakeObject player_id_str{reinterpret_cast<Il2CppClass*>(&kClsString), false, u"player-42"};
  FakeObject player_name_str{reinterpret_cast<Il2CppClass*>(&kClsString), false, u"TrainerAsh"};

  std::vector<FakeObject*> allocated_strings;
  struct HandleSlot {
    Il2CppObject* obj = nullptr;
    bool alive = false;
  };
  std::vector<HandleSlot> handles;
  size_t live_handle_count = 0;

  bool is_connected = true;
  bool has_cached_creds = false;
  bool wrapper_throws = false;
  bool chain_auth_logout_inside_wrapper = true;
  int orig_auth_logout_calls = 0;
  int wrapper_logout_calls = 0;
  int prefs_save_calls = 0;
  bool has_key1 = true;
  bool has_key2 = true;
  std::vector<std::string> deleted_keys;
  std::vector<std::string> checked_keys;

  void (*on_get_player_name)() = nullptr;
  void (*on_wrapper_logout)() = nullptr;
};

FakeRuntime g_rt;

std::string u16_to_ascii(const std::u16string& s) {
  std::string out;
  out.reserve(s.size());
  for (char16_t c : s) out.push_back(static_cast<char>(c));
  return out;
}

#if PGR_ACCOUNTS_REAL_DOBBY
// Dobby rewrites this function's entry in place, so it must not be inlined and must
// own enough relocatable prologue bytes for the inline patch + trampoline.
__attribute__((noinline))
#endif
Il2CppObject* fake_orig_auth_logout(Il2CppObject* self, const MethodInfo* /*method*/) {
#if PGR_ACCOUNTS_REAL_DOBBY
  // NOP padding reserves patchable prologue space on both arm64 and x86-64.
  asm volatile("nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop" ::: "memory");
#endif
  assert(self == reinterpret_cast<Il2CppObject*>(&g_rt.auth_obj));
  ++g_rt.orig_auth_logout_calls;
  return reinterpret_cast<Il2CppObject*>(&g_rt.future_obj);
}

// Simulate the game invoking AuthService.Logout. Stub mode calls the recorded
// replacement (hook_auth_logout) directly. Real-Dobby mode calls the physically
// patched original target through a volatile function pointer so the compiler
// cannot inline or devirtualize past the patch site, exercising the installed
// inline hook and its trampoline.
Il2CppObject* invoke_installed_auth_logout(Il2CppObject* self, const MethodInfo* method) {
#if PGR_ACCOUNTS_REAL_DOBBY
  AuthLogoutFn volatile fn = reinterpret_cast<AuthLogoutFn>(g_hook_target);
#else
  AuthLogoutFn volatile fn = reinterpret_cast<AuthLogoutFn>(g_hook_replacement);
#endif
  return fn(self, method);
}

uintptr_t fake_gchandle_new(Il2CppObject* obj, bool /*pinned*/) {
  if (!obj) return 0;
  g_rt.handles.push_back({obj, true});
  ++g_rt.live_handle_count;
  return static_cast<uintptr_t>(g_rt.handles.size());
}

Il2CppObject* fake_gchandle_get_target(uintptr_t h) {
  if (h == 0 || h > g_rt.handles.size()) return nullptr;
  const auto& slot = g_rt.handles[h - 1];
  assert(slot.alive && "Use-after-free of GC handle");
  return slot.obj;
}

void fake_gchandle_free(uintptr_t h) {
  assert(h != 0 && h <= g_rt.handles.size());
  auto& slot = g_rt.handles[h - 1];
  assert(slot.alive && "Double-free of GC handle");
  slot.alive = false;
  slot.obj = nullptr;
  --g_rt.live_handle_count;
}

Il2CppClass* fake_object_get_class(Il2CppObject* obj) {
  return obj ? reinterpret_cast<FakeObject*>(obj)->klass : nullptr;
}

void* fake_object_unbox(Il2CppObject* obj) {
  return obj ? &reinterpret_cast<FakeObject*>(obj)->bool_val : nullptr;
}

const Il2CppType* fake_class_get_type(Il2CppClass* k) {
  if (k == reinterpret_cast<Il2CppClass*>(&kClsMapCtx)) return reinterpret_cast<const Il2CppType*>(&kTypeMap);
  if (k == reinterpret_cast<Il2CppClass*>(&kClsPlayerContract)) return reinterpret_cast<const Il2CppType*>(&kTypePlayer);
  if (k == reinterpret_cast<Il2CppClass*>(&kClsAuthContract)) return reinterpret_cast<const Il2CppType*>(&kTypeAuth);
  if (k == reinterpret_cast<Il2CppClass*>(&kClsWrapperContract)) return reinterpret_cast<const Il2CppType*>(&kTypeWrapper);
  return nullptr;
}

Il2CppObject* fake_type_get_object(const Il2CppType* t) {
  if (t == reinterpret_cast<const Il2CppType*>(&kTypeMap)) return reinterpret_cast<Il2CppObject*>(&g_rt.type_map_obj);
  if (t == reinterpret_cast<const Il2CppType*>(&kTypePlayer)) return reinterpret_cast<Il2CppObject*>(&g_rt.type_player_obj);
  if (t == reinterpret_cast<const Il2CppType*>(&kTypeAuth)) return reinterpret_cast<Il2CppObject*>(&g_rt.type_auth_obj);
  if (t == reinterpret_cast<const Il2CppType*>(&kTypeWrapper)) return reinterpret_cast<Il2CppObject*>(&g_rt.type_wrapper_obj);
  return nullptr;
}

Il2CppString* fake_string_new(const char* s) {
  if (!s) return nullptr;
  auto* fo = new FakeObject();
  fo->klass = reinterpret_cast<Il2CppClass*>(&kClsString);
  for (const char* p = s; *p; ++p) fo->str_val.push_back(static_cast<char16_t>(*p));
  g_rt.allocated_strings.push_back(fo);
  return reinterpret_cast<Il2CppString*>(fo);
}

int32_t fake_string_length(Il2CppString* s) {
  return s ? static_cast<int32_t>(reinterpret_cast<FakeObject*>(s)->str_val.size()) : 0;
}

const uint16_t* fake_string_chars(Il2CppString* s) {
  return s ? reinterpret_cast<const uint16_t*>(reinterpret_cast<FakeObject*>(s)->str_val.data()) : nullptr;
}

void fake_field_get_value(Il2CppObject* obj, FieldInfo* field, void* out) {
  auto* fo = reinterpret_cast<FakeObject*>(obj);
  if (field == reinterpret_cast<FieldInfo*>(&kFldActiveProvider)) {
    *reinterpret_cast<void**>(out) = fo->active_provider;
  } else if (field == reinterpret_cast<FieldInfo*>(&kFldFutureDisposed)) {
    *reinterpret_cast<uint8_t*>(out) = fo->future_disposed;
  } else if (field == reinterpret_cast<FieldInfo*>(&kFldFutureStatus)) {
    *reinterpret_cast<int32_t*>(out) = fo->future_status;
  }
}

Il2CppObject* fake_runtime_invoke(const MethodInfo* method, void* obj, void** params, Il2CppObject** exc) {
  if (exc) *exc = nullptr;
  if (method == reinterpret_cast<const MethodInfo*>(&kMthFindObj)) {
    assert(obj == nullptr);
    assert(params && params[0] == &g_rt.type_map_obj);
    return reinterpret_cast<Il2CppObject*>(&g_rt.map_ctx_obj);
  }
  if (method == reinterpret_cast<const MethodInfo*>(&kMthGetContainer)) {
    assert(obj == &g_rt.map_ctx_obj);
    return reinterpret_cast<Il2CppObject*>(&g_rt.container_obj);
  }
  if (method == reinterpret_cast<const MethodInfo*>(&kMthResolve)) {
    assert(obj == &g_rt.container_obj && params != nullptr);
    if (params[0] == &g_rt.type_auth_obj) return reinterpret_cast<Il2CppObject*>(&g_rt.auth_obj);
    if (params[0] == &g_rt.type_player_obj) return reinterpret_cast<Il2CppObject*>(&g_rt.player_obj);
    if (params[0] == &g_rt.type_wrapper_obj) return reinterpret_cast<Il2CppObject*>(&g_rt.wrapper_obj);
    return nullptr;
  }
  if (method == reinterpret_cast<const MethodInfo*>(&kMthGetConnected)) {
    assert(obj == &g_rt.auth_obj);
    return reinterpret_cast<Il2CppObject*>(g_rt.is_connected ? &g_rt.bool_true : &g_rt.bool_false);
  }
  if (method == reinterpret_cast<const MethodInfo*>(&kMthGetCached)) {
    assert(obj == &g_rt.auth_obj);
    return reinterpret_cast<Il2CppObject*>(g_rt.has_cached_creds ? &g_rt.bool_true : &g_rt.bool_false);
  }
  if (method == reinterpret_cast<const MethodInfo*>(&kMthGetPlayerId)) {
    assert(obj == &g_rt.player_obj);
    return reinterpret_cast<Il2CppObject*>(&g_rt.player_id_str);
  }
  if (method == reinterpret_cast<const MethodInfo*>(&kMthGetPlayerName)) {
    assert(obj == &g_rt.player_obj);
    if (g_rt.on_get_player_name) g_rt.on_get_player_name();
    return reinterpret_cast<Il2CppObject*>(&g_rt.player_name_str);
  }
  if (method == reinterpret_cast<const MethodInfo*>(&kMthPrefsSave)) {
    assert(obj == nullptr);
    ++g_rt.prefs_save_calls;
    if(g_rt.prefs_save_throws && exc) *exc=reinterpret_cast<Il2CppObject*>(&g_rt.exc_obj);
    return nullptr;
  }
  if (method == reinterpret_cast<const MethodInfo*>(&kMthPrefsDeleteKey)) {
    assert(obj == nullptr && params && params[0]);
    std::string k = u16_to_ascii(reinterpret_cast<FakeObject*>(params[0])->str_val);
    g_rt.deleted_keys.push_back(k);
    if (k == kDobKey1) g_rt.has_key1 = false;
    if (k == kDobKey2) g_rt.has_key2 = false;
    if (g_rt.first_delete_throws && g_rt.deleted_keys.size()==1 && exc) *exc=reinterpret_cast<Il2CppObject*>(&g_rt.exc_obj);
    return nullptr;
  }
  if (method == reinterpret_cast<const MethodInfo*>(&kMthPrefsHasKey)) {
    assert(obj == nullptr && params && params[0]);
    std::string k = u16_to_ascii(reinterpret_cast<FakeObject*>(params[0])->str_val);
    g_rt.checked_keys.push_back(k);
    bool exists = (k == kDobKey1 && g_rt.has_key1) || (k == kDobKey2 && g_rt.has_key2);
    return reinterpret_cast<Il2CppObject*>(exists ? &g_rt.bool_true : &g_rt.bool_false);
  }
  if (method == reinterpret_cast<const MethodInfo*>(&kMthWrapperLogout)) {
    assert(obj == &g_rt.wrapper_obj);
    ++g_rt.wrapper_logout_calls;
    if (g_rt.chain_auth_logout_inside_wrapper && g_hook_replacement) {
      auto fn = reinterpret_cast<AuthLogoutFn>(g_hook_replacement);
      fn(reinterpret_cast<Il2CppObject*>(&g_rt.auth_obj),
         reinterpret_cast<const MethodInfo*>(&kMthAuthLogout));
    }
    if (g_rt.on_wrapper_logout) g_rt.on_wrapper_logout();
    if (g_rt.wrapper_throws) {
      if (exc) *exc = reinterpret_cast<Il2CppObject*>(&g_rt.exc_obj);
      return nullptr;
    }
    return nullptr;
  }
  return nullptr;
}

pgrazor_accounts_config_v1_t make_valid_config() {
  pgrazor_accounts_config_v1_t c{};
  c.runtime_invoke = &fake_runtime_invoke;
  c.gchandle_new = &fake_gchandle_new;
  c.gchandle_get_target = &fake_gchandle_get_target;
  c.gchandle_free = &fake_gchandle_free;
  c.object_get_class = &fake_object_get_class;
  c.object_unbox = &fake_object_unbox;
  c.class_get_type = &fake_class_get_type;
  c.type_get_object = &fake_type_get_object;
  c.string_new = &fake_string_new;
  c.string_length = &fake_string_length;
  c.string_chars = &fake_string_chars;
  c.field_get_value = &fake_field_get_value;

  c.boolean_class = reinterpret_cast<Il2CppClass*>(&kClsBoolean);
  c.string_class = reinterpret_cast<Il2CppClass*>(&kClsString);
  c.future_class = reinterpret_cast<Il2CppClass*>(&kClsFuture);
  c.map_scene_context_class = reinterpret_cast<Il2CppClass*>(&kClsMapCtx);
  c.di_container_class = reinterpret_cast<Il2CppClass*>(&kClsContainer);
  c.player_class = reinterpret_cast<Il2CppClass*>(&kClsPlayer);
  c.player_contract_class = reinterpret_cast<Il2CppClass*>(&kClsPlayerContract);
  c.auth_class = reinterpret_cast<Il2CppClass*>(&kClsAuth);
  c.auth_contract_class = reinterpret_cast<Il2CppClass*>(&kClsAuthContract);
  c.wrapper_class = reinterpret_cast<Il2CppClass*>(&kClsWrapper);
  c.wrapper_contract_class = reinterpret_cast<Il2CppClass*>(&kClsWrapperContract);
  c.prefs_class = reinterpret_cast<Il2CppClass*>(&kClsPrefs);

  c.auth_active_provider_field = reinterpret_cast<FieldInfo*>(&kFldActiveProvider);
  c.future_status_field = reinterpret_cast<FieldInfo*>(&kFldFutureStatus);
  c.future_disposed_field = reinterpret_cast<FieldInfo*>(&kFldFutureDisposed);

  c.find_object_of_type_method = reinterpret_cast<const MethodInfo*>(&kMthFindObj);
  c.map_get_container_method = reinterpret_cast<const MethodInfo*>(&kMthGetContainer);
  c.container_resolve_method = reinterpret_cast<const MethodInfo*>(&kMthResolve);
  c.player_get_id_method = reinterpret_cast<const MethodInfo*>(&kMthGetPlayerId);
  c.player_get_name_method = reinterpret_cast<const MethodInfo*>(&kMthGetPlayerName);
  c.auth_get_connected_method = reinterpret_cast<const MethodInfo*>(&kMthGetConnected);
  c.auth_get_cached_method = reinterpret_cast<const MethodInfo*>(&kMthGetCached);
  c.wrapper_logout_method = reinterpret_cast<const MethodInfo*>(&kMthWrapperLogout);
  c.auth_logout_method = reinterpret_cast<const MethodInfo*>(&kMthAuthLogout);
  c.prefs_has_key_method = reinterpret_cast<const MethodInfo*>(&kMthPrefsHasKey);
  c.prefs_delete_key_method = reinterpret_cast<const MethodInfo*>(&kMthPrefsDeleteKey);
  c.prefs_save_method = reinterpret_cast<const MethodInfo*>(&kMthPrefsSave);

  c.auth_logout_target_fn = reinterpret_cast<void*>(&fake_orig_auth_logout);
  c.promise_succeeded_value = 1;
  c.promise_failed_value = 2;
  return c;
}

bool run_isolated(const char* name, void (*fn)()) {
  std::fflush(stdout);
  pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    fn();
    std::_Exit(0);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  return ok;
}

void test_monotonic_tokens_and_init() {
  const auto* api = pgrazor_accounts_api(PGR_ACCOUNTS_API_VERSION);
  assert(api && api->version == 1 && api->size == sizeof(pgrazor_accounts_api_v1_t));
  auto cfg = make_valid_config();
  assert(api->api_init(&cfg));

  pgrazor_account_event_v1_t ev[8]{};
  assert(api->api_drain(ev, 8) == 1 && ev[0].kind == PGR_ACC_EVT_READY && ev[0].available == 1);

  const int32_t tid = current_os_tid();
  assert(api->api_update(true, 2000, PGR_ACC_CMD_SAVE_SESSION, 10));
  api->api_tick(0, tid);
  assert(g_rt.prefs_save_calls == 1);

  pgrazor_account_snapshot_v1_t snap{};
  api->api_sample(&snap);
  assert(snap.last_accepted_token == 10 && snap.last_consumed_token == 10);

  // Duplicate token 10 and older token 5 must be rejected.
  assert(api->api_update(true, 2000, PGR_ACC_CMD_SAVE_SESSION, 10));
  usleep(260000);
  api->api_tick(0, tid);
  assert(api->api_update(true, 2000, PGR_ACC_CMD_SAVE_SESSION, 5));
  usleep(260000);
  api->api_tick(0, tid);
  assert(g_rt.prefs_save_calls == 1);

  // High-water mark at submission: token 20 followed by token 15 before tick keeps token 20.
  assert(api->api_update(true, 2000, PGR_ACC_CMD_SAVE_SESSION, 20));
  assert(api->api_update(true, 2000, PGR_ACC_CMD_RESET_LOGIN, 15));
  usleep(260000);
  api->api_tick(0, tid);
  assert(g_rt.prefs_save_calls == 2 && g_rt.deleted_keys.empty());
}

void test_config_rebind_rejected() {
  const auto* api = pgrazor_accounts_api(PGR_ACCOUNTS_API_VERSION);
  auto cfg1 = make_valid_config();
  assert(api->api_init(&cfg1));
  assert(api->api_init(&cfg1));
  auto cfg2 = cfg1;
  cfg2.promise_succeeded_value = 99;
  assert(!api->api_init(&cfg2));
  assert(g_hook_install_calls == 1);
}

void test_wrong_thread_cannot_steal_owner() {
  const auto* api = pgrazor_accounts_api(PGR_ACCOUNTS_API_VERSION);
  auto cfg = make_valid_config();
  assert(api->api_init(&cfg));

  const int32_t main_tid = current_os_tid();
  // Forged tid from main thread must be rejected.
  api->api_tick(0, main_tid + 123);
  pgrazor_account_snapshot_v1_t snap{};
  api->api_sample(&snap);
  assert(snap.owner_tid == 0);

  // Legitimate owner binds main_tid.
  api->api_tick(0, main_tid);
  api->api_sample(&snap);
  assert(snap.owner_tid == main_tid);

  // Worker thread supplying its own tid cannot steal owner or clear.
  std::thread worker([&]() {
    const int32_t wtid = current_os_tid();
    assert(api->api_update(true, 2000, PGR_ACC_CMD_SAVE_SESSION, 1));
    usleep(260000);
    api->api_tick(0, wtid);
    assert(!api->api_clear());
  });
  worker.join();
  assert(g_rt.prefs_save_calls == 0);
  api->api_sample(&snap);
  assert(snap.owner_tid == main_tid);
}

void test_save_session_once() {
  const auto* api = pgrazor_accounts_api(PGR_ACCOUNTS_API_VERSION);
  auto cfg = make_valid_config();
  assert(api->api_init(&cfg));
  pgrazor_account_event_v1_t ev[8]{};
  api->api_drain(ev, 8);

  const int32_t tid = current_os_tid();
  assert(api->api_update(true, 2000, PGR_ACC_CMD_SAVE_SESSION, 1));
  api->api_tick(0, tid);
  usleep(260000);
  api->api_tick(0, tid);
  assert(g_rt.prefs_save_calls == 1);
  size_t n = api->api_drain(ev, 8);
  assert(n >= 1 && ev[0].kind == PGR_ACC_EVT_SESSION_SAVED);
}

void test_reset_login_exact_keys() {
  const auto* api = pgrazor_accounts_api(PGR_ACCOUNTS_API_VERSION);
  auto cfg = make_valid_config();
  assert(api->api_init(&cfg));
  pgrazor_account_event_v1_t ev[8]{};
  api->api_drain(ev, 8);

  const int32_t tid = current_os_tid();
  assert(api->api_update(true, 2000, PGR_ACC_CMD_RESET_LOGIN, 1));
  api->api_tick(0, tid);

  assert(g_rt.deleted_keys.size() == 2);
  assert(g_rt.deleted_keys[0] == kDobKey1);
  assert(g_rt.deleted_keys[1] == kDobKey2);
  assert(g_rt.checked_keys.size() == 2);
  assert(!g_rt.has_key1 && !g_rt.has_key2);
  assert(g_rt.prefs_save_calls == 1);

  size_t n = api->api_drain(ev, 8);
  assert(n == 1 && ev[0].kind == PGR_ACC_EVT_LOGIN_RESET_COMPLETE);
  assert(g_rt.live_handle_count == 0);
}

void test_revocation_and_reenable_during_getter_stops_stale_mutation() {
  const auto* api = pgrazor_accounts_api(PGR_ACCOUNTS_API_VERSION);
  auto cfg = make_valid_config();
  assert(api->api_init(&cfg));
  pgrazor_account_event_v1_t ev[8]{};
  api->api_drain(ev, 8);

  g_rt.on_get_player_name = []() {
    const auto* a = pgrazor_accounts_api(PGR_ACCOUNTS_API_VERSION);
    assert(a->api_update(false, 0, PGR_ACC_CMD_NONE, 0));
    assert(a->api_update(true, 2000, PGR_ACC_CMD_LOGOUT, 1));
  };

  const int32_t tid = current_os_tid();
  assert(api->api_update(true, 2000, PGR_ACC_CMD_LOGOUT, 1));
  api->api_tick(0, tid);

  assert(g_rt.wrapper_logout_calls == 0);
  size_t n = api->api_drain(ev, 8);
  assert(n == 1 && ev[0].kind == PGR_ACC_EVT_LOGOUT_FAILED &&
         ev[0].reason == PGR_ACC_REASON_LEASE_EXPIRED);
  assert(g_rt.live_handle_count == 0);
}

void test_reentrant_clear_and_nested_tick_protect_active_roots() {
  const auto* api = pgrazor_accounts_api(PGR_ACCOUNTS_API_VERSION);
  auto cfg = make_valid_config();
  assert(api->api_init(&cfg));

  g_rt.on_wrapper_logout = []() {
    const auto* a = pgrazor_accounts_api(PGR_ACCOUNTS_API_VERSION);
    pgrazor_account_snapshot_v1_t mid{};
    a->api_sample(&mid);
    assert(mid.active_depth == 1);
    assert(mid.active_gc_roots > 0);
    const size_t roots_before = g_rt.live_handle_count;

    // Reentrant clear schedules deferred clear without freeing active roots mid-frame.
    assert(a->api_clear());
    // Nested tick while active_depth > 0 must be rejected and not free active roots.
    a->api_tick(0, current_os_tid());
    assert(g_rt.live_handle_count == roots_before);
  };

  const int32_t tid = current_os_tid();
  assert(api->api_update(true, 2000, PGR_ACC_CMD_LOGOUT, 1));
  api->api_tick(0, tid);

  pgrazor_account_snapshot_v1_t post{};
  api->api_sample(&post);
  assert(post.active_depth == 0);
  assert(post.active_gc_roots == 0);
  assert(g_rt.live_handle_count == 0);
}

void test_no_root_leak_after_clear() {
  const auto* api = pgrazor_accounts_api(PGR_ACCOUNTS_API_VERSION);
  auto cfg = make_valid_config();
  assert(api->api_init(&cfg));

  const int32_t tid = current_os_tid();
  assert(api->api_update(true, 2000, PGR_ACC_CMD_LOGOUT, 1));
  api->api_tick(0, tid);

  pgrazor_account_snapshot_v1_t snap{};
  api->api_sample(&snap);
  assert(snap.logging_out == 1);
  assert(snap.active_depth == 0);
  assert(snap.active_gc_roots == 2);
  assert(g_rt.live_handle_count == 2);

  assert(api->api_clear());
  api->api_sample(&snap);
  assert(snap.logging_out == 0);
  assert(snap.active_depth == 0);
  assert(snap.active_gc_roots == 0);
  assert(g_rt.live_handle_count == 0);
}

void test_identity_snapshots_and_session_ended() {
  const auto* api = pgrazor_accounts_api(PGR_ACCOUNTS_API_VERSION);
  auto cfg = make_valid_config();
  assert(api->api_init(&cfg));
  pgrazor_account_event_v1_t ev[8]{};
  api->api_drain(ev, 8);

  const int32_t tid = current_os_tid();
  assert(api->api_update(true, 2000, PGR_ACC_CMD_NONE, 0));
  api->api_tick(0, tid);

  pgrazor_account_snapshot_v1_t snap{};
  api->api_sample(&snap);
  assert(snap.has_identity == 1);
  assert(std::u16string(reinterpret_cast<const char16_t*>(snap.id), snap.id_len) == u"player-42");
  assert(std::u16string(reinterpret_cast<const char16_t*>(snap.name), snap.name_len) == u"TrainerAsh");

  size_t n = api->api_drain(ev, 8);
  assert(n == 1 && ev[0].kind == PGR_ACC_EVT_IDENTITY);

  // External game logout hits hook directly; must chain original once, clear identity, and not capture future.
  Il2CppObject* ret = invoke_installed_auth_logout(
      reinterpret_cast<Il2CppObject*>(&g_rt.auth_obj),
      reinterpret_cast<const MethodInfo*>(&kMthAuthLogout));
  assert(ret == reinterpret_cast<Il2CppObject*>(&g_rt.future_obj));
  assert(g_rt.orig_auth_logout_calls == 1);
  api->api_sample(&snap);
  assert(snap.has_identity == 0);
  assert(snap.active_gc_roots == 0);
  n = api->api_drain(ev, 8);
  assert(n == 1 && ev[0].kind == PGR_ACC_EVT_SESSION_ENDED);
}

void test_uncertainty_stops_replay_on_new_token_after_invoked_logout_fails() {
  const auto* api = pgrazor_accounts_api(PGR_ACCOUNTS_API_VERSION);
  auto cfg = make_valid_config();
  assert(api->api_init(&cfg));
  pgrazor_account_event_v1_t ev[8]{};
  api->api_drain(ev, 8);

  g_rt.wrapper_throws = true;
  const int32_t tid = current_os_tid();
  assert(api->api_update(true, 2000, PGR_ACC_CMD_LOGOUT, 1));
  api->api_tick(0, tid);
  assert(g_rt.wrapper_logout_calls == 1);

  // Wrapper.Logout was invoked and threw an exception (uncertain state). Subsequent command
  // with a new token must be blocked rather than replaying or mutating uncertain state.
  usleep(260000);
  assert(api->api_update(true, 2000, PGR_ACC_CMD_SAVE_SESSION, 2));
  api->api_tick(0, tid);
  assert(g_rt.prefs_save_calls == 0);
}

void test_partial_reset_blocks_new_command() {
  const auto* api=pgrazor_accounts_api(1); auto cfg=make_valid_config();
  assert(api->api_init(&cfg));
  g_rt.first_delete_throws=true;
  assert(api->api_update(true,2000,PGR_ACC_CMD_RESET_LOGIN,1));
  api->api_tick(0,current_os_tid());
  assert(g_rt.deleted_keys.size()==1 && !g_rt.has_key1 && g_rt.has_key2);
  usleep(260000);
  assert(api->api_update(true,2000,PGR_ACC_CMD_SAVE_SESSION,2));
  api->api_tick(0,current_os_tid());
  assert(g_rt.prefs_save_calls==0);
}
void test_failed_save_blocks_retry() {
  const auto* api=pgrazor_accounts_api(1); auto cfg=make_valid_config();
  assert(api->api_init(&cfg)); g_rt.prefs_save_throws=true;
  assert(api->api_update(true,2000,PGR_ACC_CMD_SAVE_SESSION,1));
  api->api_tick(0,current_os_tid());
  assert(g_rt.prefs_save_calls==1);
  usleep(260000);
  assert(api->api_update(true,2000,PGR_ACC_CMD_SAVE_SESSION,2));
  api->api_tick(0,current_os_tid());
  assert(g_rt.prefs_save_calls==1);
}

void test_bounded_queue_overflow_fails_closed() {
  const auto* api = pgrazor_accounts_api(PGR_ACCOUNTS_API_VERSION);
  auto cfg = make_valid_config();
  assert(api->api_init(&cfg));

  const int32_t tid = current_os_tid();
  api->api_tick(0, tid);  // Binds owner_tid

  for (size_t i = 0; i < PGR_ACCOUNTS_EVENT_QUEUE_CAP + 8; ++i) {
    invoke_installed_auth_logout(reinterpret_cast<Il2CppObject*>(&g_rt.auth_obj),
                                 reinterpret_cast<const MethodInfo*>(&kMthAuthLogout));
  }

  pgrazor_account_snapshot_v1_t snap{};
  api->api_sample(&snap);
  assert(snap.queue_overflow_latched == 1);
  assert(!api->api_update(true, 2000, PGR_ACC_CMD_SAVE_SESSION, 99));
  assert(!api->api_init(&cfg));

  pgrazor_account_event_v1_t events[PGR_ACCOUNTS_EVENT_QUEUE_CAP + 4]{};
  size_t drained = api->api_drain(events, PGR_ACCOUNTS_EVENT_QUEUE_CAP + 4);
  assert(drained == PGR_ACCOUNTS_EVENT_QUEUE_CAP);
  for (size_t i = 0; i < drained; ++i) {
    assert(events[i].seq == i + 1);
  }
}

}  // namespace

int main() {
  int failed = 0;
  if (!run_isolated("partial_reset_blocks_new_command", &test_partial_reset_blocks_new_command)) ++failed;
  if (!run_isolated("failed_save_blocks_retry", &test_failed_save_blocks_retry)) ++failed;
  if (!run_isolated("monotonic_tokens_and_init", &test_monotonic_tokens_and_init)) ++failed;
  if (!run_isolated("config_rebind_rejected", &test_config_rebind_rejected)) ++failed;
  if (!run_isolated("wrong_thread_cannot_steal_owner", &test_wrong_thread_cannot_steal_owner)) ++failed;
  if (!run_isolated("save_session_once", &test_save_session_once)) ++failed;
  if (!run_isolated("reset_login_exact_keys", &test_reset_login_exact_keys)) ++failed;
  if (!run_isolated("revocation_and_reenable_during_getter", &test_revocation_and_reenable_during_getter_stops_stale_mutation)) ++failed;
  if (!run_isolated("reentrant_clear_and_nested_tick_protect_roots", &test_reentrant_clear_and_nested_tick_protect_active_roots)) ++failed;
  if (!run_isolated("no_root_leak_after_clear", &test_no_root_leak_after_clear)) ++failed;
  if (!run_isolated("identity_snapshots_and_session_ended", &test_identity_snapshots_and_session_ended)) ++failed;
  if (!run_isolated("uncertainty_stops_replay_on_new_token", &test_uncertainty_stops_replay_on_new_token_after_invoked_logout_fails)) ++failed;
  if (!run_isolated("bounded_queue_overflow_fails_closed", &test_bounded_queue_overflow_fails_closed)) ++failed;
  return failed == 0 ? 0 : 1;
}
