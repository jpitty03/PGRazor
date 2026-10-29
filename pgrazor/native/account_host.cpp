#include "account_host.h"

#include "runtime_transport.h"
#include "shared_host.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <string>
#include <time.h>
#include <unistd.h>

namespace {

using namespace pgr_shared_meta;

// --- Disassembly-pinned RVAs and 24-byte ARM64 prologues (exact match with JS) ---
constexpr uint64_t kRvaPlayerGetId        = 178458668ULL; // 0xaa2d3ac
constexpr uint64_t kRvaPlayerGetName      = 178459312ULL; // 0xaa2d630
constexpr uint64_t kRvaAuthIsConnected    = 204755248ULL; // 0xc341530
constexpr uint64_t kRvaAuthCachedCreds    = 204755468ULL; // 0xc34160c
constexpr uint64_t kRvaWrapperLogout      = 175973768ULL; // 0xa7ce988
constexpr uint64_t kRvaAuthLogout         = 204773516ULL; // 0xc345c8c
constexpr uint64_t kRvaPrefsHasKey        = 228907924ULL; // 0xda49f94
constexpr uint64_t kRvaPrefsDeleteKey     = 228908396ULL; // 0xda4a16c
constexpr uint64_t kRvaPrefsSave          = 228908844ULL; // 0xda4a32c

constexpr uint64_t kRvaAdvInitInitial     = 202585836ULL; // 0xc12faec
constexpr uint64_t kRvaAdvInitInstruction = 202588024ULL; // 0xc130378
constexpr uint64_t kRvaAdvInitResult      = 202592616ULL; // 0xc131568
constexpr uint64_t kRvaAdvOnConfirm       = 202597316ULL; // 0xc1327c4
constexpr uint64_t kRvaAdvOnDecline       = 202597344ULL; // 0xc1327e0
constexpr uint64_t kRvaAdvUpdate          = 202597372ULL; // 0xc1327fc

constexpr unsigned char kPrefixPlayerGetId[24] = {
    0x08, 0x84, 0x40, 0xf9, 0x88, 0x00, 0x00, 0xb4, 0x00, 0x79, 0x40, 0xf9,
    0x40, 0x00, 0x00, 0xb4, 0xc0, 0x03, 0x5f, 0xd6, 0xe8, 0x35, 0x02, 0xd0};
constexpr unsigned char kPrefixPlayerGetName[24] = {
    0x08, 0x84, 0x40, 0xf9, 0x88, 0x00, 0x00, 0xb4, 0x00, 0x0d, 0x40, 0xf9,
    0x40, 0x00, 0x00, 0xb4, 0xc0, 0x03, 0x5f, 0xd6, 0xe8, 0x35, 0x02, 0xd0};
constexpr unsigned char kPrefixAuthIsConnected[24] = {
    0x00, 0x60, 0x40, 0x39, 0xc0, 0x03, 0x5f, 0xd6, 0x01, 0x60, 0x00, 0x39,
    0xc0, 0x03, 0x5f, 0xd6, 0xfe, 0x57, 0xbe, 0xa9, 0xf4, 0x4f, 0x01, 0xa9};
constexpr unsigned char kPrefixAuthCachedCreds[24] = {
    0xfe, 0x0f, 0x1f, 0xf8, 0x0a, 0x00, 0x00, 0x94, 0xc0, 0x00, 0x00, 0xb4,
    0x08, 0x10, 0x40, 0xb9, 0x88, 0x00, 0x00, 0x34, 0x20, 0x00, 0x80, 0x52};
constexpr unsigned char kPrefixWrapperLogout[24] = {
    0xfe, 0x57, 0xbe, 0xa9, 0xf4, 0x4f, 0x01, 0xa9, 0xd4, 0x47, 0x02, 0xf0,
    0xf3, 0x03, 0x00, 0xaa, 0x88, 0xbe, 0x66, 0x39, 0xa8, 0x01, 0x00, 0x37};
constexpr unsigned char kPrefixAuthLogout[24] = {
    0xfe, 0x0f, 0x1c, 0xf8, 0xf8, 0x5f, 0x01, 0xa9, 0xf6, 0x57, 0x02, 0xa9,
    0xf4, 0x4f, 0x03, 0xa9, 0x93, 0x6c, 0x01, 0x90, 0xf4, 0x03, 0x00, 0xaa};
constexpr unsigned char kPrefixPrefsHasKey[24] = {
    0xff, 0xc3, 0x00, 0xd1, 0xfe, 0x57, 0x01, 0xa9, 0xf4, 0x4f, 0x02, 0xa9,
    0xd4, 0xb4, 0x00, 0xd0, 0xf3, 0x03, 0x00, 0xaa, 0x88, 0xb2, 0x55, 0x39};
constexpr unsigned char kPrefixPrefsDeleteKey[24] = {
    0xff, 0xc3, 0x00, 0xd1, 0xfe, 0x57, 0x01, 0xa9, 0xf4, 0x4f, 0x02, 0xa9,
    0xd4, 0xb4, 0x00, 0xd0, 0xf3, 0x03, 0x00, 0xaa, 0x88, 0xb6, 0x55, 0x39};
constexpr unsigned char kPrefixPrefsSave[24] = {
    0xfe, 0x4f, 0xbf, 0xa9, 0xd3, 0xb4, 0x00, 0xd0, 0x60, 0xba, 0x42, 0xf9,
    0x60, 0x00, 0x00, 0xb4, 0xfe, 0x4f, 0xc1, 0xa8, 0x00, 0x00, 0x1f, 0xd6};

constexpr unsigned char kPrefixAdvInitInitial[24] = {
    0xfe, 0x0f, 0x1a, 0xf8, 0xfc, 0x6f, 0x01, 0xa9, 0xfa, 0x67, 0x02, 0xa9,
    0xf8, 0x5f, 0x03, 0xa9, 0xf6, 0x57, 0x04, 0xa9, 0xf4, 0x4f, 0x05, 0xa9};
constexpr unsigned char kPrefixAdvInitInstruction[24] = {
    0xfd, 0x7b, 0xba, 0xa9, 0xfc, 0x6f, 0x01, 0xa9, 0xfa, 0x67, 0x02, 0xa9,
    0xf8, 0x5f, 0x03, 0xa9, 0xf6, 0x57, 0x04, 0xa9, 0xf4, 0x4f, 0x05, 0xa9};
constexpr unsigned char kPrefixAdvInitResult[24] = {
    0xff, 0xc3, 0x01, 0xd1, 0xfd, 0x7b, 0x01, 0xa9, 0xfc, 0x6f, 0x02, 0xa9,
    0xfa, 0x67, 0x03, 0xa9, 0xf8, 0x5f, 0x04, 0xa9, 0xf6, 0x57, 0x05, 0xa9};
constexpr unsigned char kPrefixAdvOnConfirm[24] = {
    0x08, 0x54, 0x40, 0xf9, 0xa8, 0x00, 0x00, 0xb4, 0x00, 0x21, 0x40, 0xf9,
    0x01, 0x15, 0x40, 0xf9, 0x02, 0x0d, 0x40, 0xf9, 0x40, 0x00, 0x1f, 0xd6};
constexpr unsigned char kPrefixAdvOnDecline[24] = {
    0x08, 0x58, 0x40, 0xf9, 0xa8, 0x00, 0x00, 0xb4, 0x00, 0x21, 0x40, 0xf9,
    0x01, 0x15, 0x40, 0xf9, 0x02, 0x0d, 0x40, 0xf9, 0x40, 0x00, 0x1f, 0xd6};
constexpr unsigned char kPrefixAdvUpdate[24] = {
    0xfe, 0x0f, 0x1e, 0xf8, 0xf4, 0x4f, 0x01, 0xa9, 0x94, 0x7d, 0x01, 0xf0,
    0xf3, 0x03, 0x00, 0xaa, 0x88, 0xca, 0x46, 0xf9, 0xa8, 0x00, 0x00, 0xb4};

constexpr uint64_t kOffAuthActiveProvider = 0x98ULL;
constexpr uint64_t kOffFutureStatus       = 0x40ULL;
constexpr uint64_t kOffFutureIsDisposed   = 0x30ULL;
constexpr uint64_t kOffAdvOnConfirm       = 0xa8ULL;
constexpr uint64_t kOffAdvOnDecline       = 0xb0ULL;

constexpr uint64_t kMaxTokenI64 = 9223372036854775807ULL;

const char *const kAccountsReasonStrings[] = {"","cleared-auth-state","session-save-unavailable","login-reset-unavailable","Error: login-reset-not-saved","Error: logout-wrapper","Error: logout-auth-invalid","Error: logout-future-invalid","Error: logout-failed","Error: logout-timeout","Error: missing-auth-state","Error: invocation-exception","Error: lease-expired","Error: reentrant-abort","Error: queue-overflow"};
constexpr size_t kAccountsReasonCount = sizeof(kAccountsReasonStrings) / sizeof(kAccountsReasonStrings[0]);

// --- Feature getters ---------------------------------------------------------------
const pgrazor_accounts_api_v1_t *accounts_api() {
  const auto *p = static_cast<const pgrazor_accounts_api_v1_t *>(pgrazor_accounts_api(1));
  return (p && p->version == 1 && p->size == sizeof(pgrazor_accounts_api_v1_t)) ? p : nullptr;
}

const pgrazor_advsync_api_v1 *advsync_api() {
  const auto *p = static_cast<const pgrazor_advsync_api_v1 *>(pgrazor_adventure_sync_api(1));
  return (p && p->version == 1 && p->size == sizeof(pgrazor_advsync_api_v1)) ? p : nullptr;
}

const PgrMetaApi *meta_api() {
  const auto *p = static_cast<const PgrMetaApi *>(pgrazor_runtime_metadata_api(1));
  return (p && p->version == 1 && p->size == sizeof(PgrMetaApi)) ? p : nullptr;
}

const PgrBootApi *boot_api() {
  const auto *p = static_cast<const PgrBootApi *>(pgrazor_runtime_bootstrap_api(1));
  return (p && p->version == 1 && p->size == sizeof(PgrBootApi)) ? p : nullptr;
}

const PgrRuntimeTransportApi *transport_api() {
  const auto *p = static_cast<const PgrRuntimeTransportApi *>(pgrazor_runtime_transport_api(1));
  return (p && p->version == 1 && p->size == sizeof(PgrRuntimeTransportApi)) ? p : nullptr;
}

const PgrSharedHostApi *shared_host_api() {
  const auto *p = static_cast<const PgrSharedHostApi *>(pgrazor_shared_host_api(1));
  return (p && p->version == 1 && p->size == sizeof(PgrSharedHostApi)) ? p : nullptr;
}

const PgrUnitySchedulerApi *scheduler_api2() {
  const auto *p = static_cast<const PgrUnitySchedulerApi *>(pgrazor_unity_scheduler_api(2));
  return (p && p->version == 2 && p->size == sizeof(PgrUnitySchedulerApi)) ? p : nullptr;
}

const PgrUnitySchedulerApi3 *scheduler_api3() {
  const auto *p = static_cast<const PgrUnitySchedulerApi3 *>(pgrazor_unity_scheduler_api(3));
  return (p && p->version == 3 && p->size == sizeof(PgrUnitySchedulerApi3)) ? p : nullptr;
}

// --- OS / clock / string helpers ---------------------------------------------------
uint64_t self_tid() {
  return static_cast<uint64_t>(static_cast<pid_t>(gettid()));
}

uint64_t clock_boottime_ms() {
  struct timespec ts {};
  if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0 || ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L)
    return 0;
  return static_cast<uint64_t>(ts.tv_sec) * 1000ULL + static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
}

void copy_cstr(char *dst, size_t cap, const char *src) {
  if (!dst || cap == 0) return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  size_t n = strnlen(src, cap - 1);
  std::memcpy(dst, src, n);
  dst[n] = '\0';
}

void append_utf8_cp(std::string *out, uint32_t cp) {
  if (cp <= 0x7Fu) {
    out->push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FFu) {
    out->push_back(static_cast<char>(0xC0u | (cp >> 6)));
    out->push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else if (cp <= 0xFFFFu) {
    out->push_back(static_cast<char>(0xE0u | (cp >> 12)));
    out->push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out->push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else if (cp <= 0x10FFFFu) {
    out->push_back(static_cast<char>(0xF0u | (cp >> 18)));
    out->push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
    out->push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out->push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  }
}

bool bounded_utf16_to_utf8(const uint16_t *chars, uint32_t len, std::string *out) {
  out->clear();
  if (!chars || len < 1 || len > PGR_ACCOUNTS_MAX_UTF16) return false;
  out->reserve(len);
  for (uint32_t i = 0; i < len; ++i) {
    uint32_t w1 = chars[i];
    if (w1 >= 0xD800u && w1 <= 0xDBFFu && i + 1 < len) {
      uint32_t w2 = chars[i + 1];
      if (w2 >= 0xDC00u && w2 <= 0xDFFFu) {
        uint32_t cp = 0x10000u + (((w1 - 0xD800u) << 10) | (w2 - 0xDC00u));
        append_utf8_cp(out, cp);
        ++i;
        continue;
      }
    }
    if (w1 >= 0xD800u && w1 <= 0xDFFFu) {
      append_utf8_cp(out, 0xFFFDu);
    } else {
      append_utf8_cp(out, w1);
    }
  }
  return !out->empty();
}

std::string json_quote(const std::string &s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20u) {
          char b[8];
          std::snprintf(b, sizeof(b), "\\u%04x", static_cast<unsigned>(c));
          out += b;
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  out.push_back('"');
  return out;
}

// --- Exact 64-bit decimal token parser & command classifier ------------------------
uint64_t parse_token_exact64(const char *raw) {
  if (!raw || raw[0] == '\0') return 0;
  uint64_t acc = 0;
  for (size_t i = 0; raw[i] != '\0'; ++i) {
    if (i >= PGR_ACH_TOKEN_MAX_BYTES) return 0;
    char c = raw[i];
    if (c < '0' || c > '9') return 0;
    uint64_t d = static_cast<uint64_t>(c - '0');
    if (acc > (kMaxTokenI64 - d) / 10ULL) return 0;
    acc = acc * 10ULL + d;
  }
  return acc; // 0 if "0" or "00...", otherwise 1..9223372036854775807
}

uint32_t classify_command(const char *cmd) {
  if (!cmd || cmd[0] == '\0') return PGR_ACC_CMD_NONE;
  size_t len = strnlen(cmd, PGR_ACH_CMD_MAX_BYTES + 1);
  if (len == 0 || len > PGR_ACH_CMD_MAX_BYTES) return PGR_ACC_CMD_NONE;
  if (std::strncmp(cmd, "save-session:", 13) == 0) return PGR_ACC_CMD_SAVE_SESSION;
  if (std::strncmp(cmd, "reset-login:", 12) == 0) return PGR_ACC_CMD_RESET_LOGIN;
  return PGR_ACC_CMD_LOGOUT;
}

// --- Host state --------------------------------------------------------------------
struct HostState {
  std::mutex policy_mu;
  std::mutex event_mu;

  std::atomic<bool> configured{false};
  std::atomic<bool> stopping{false};
  std::atomic<bool> tick_busy{false};
  std::atomic<bool> accounts_cleared{false};
  std::atomic<uint32_t> caps{0};
  std::atomic<int32_t> accounts_error{0};
  std::atomic<int32_t> advsync_error{0};
  std::atomic<uint64_t> owner_tid{0};
  std::atomic<uint64_t> ticks{0};

  bool accounts_attempted = false;
  bool advsync_attempted = false;
  bool accounts_ready_emitted = false;
  uint32_t cfg_flags = 0;
  const void *module_base = nullptr;
  uint64_t module_size = 0;
  PgrMetaExports meta_exports{};
  PgrAccountHostRuntime runtime{};

  // Accounts policy state (protected by policy_mu)
  uint64_t acc_until_ms = 0;
  uint32_t acc_cmd_kind = PGR_ACC_CMD_NONE;
  uint64_t acc_cmd_token = 0;

  // AdventureSync policy state (protected by policy_mu)
  bool adv_allowed = false;

  // Pull event ring (protected by event_mu)
  PgrAccountHostEvent events[PGR_ACH_EVENT_QUEUE_MAX]{};
  uint32_t event_head = 0;
  uint32_t event_count = 0;
  uint64_t event_seq = 0;
};

HostState g;
std::atomic<bool> config_busy{false};
struct ConfigGuard {bool acquired;ConfigGuard():acquired(!config_busy.exchange(true)){}~ConfigGuard(){if(acquired)config_busy=false;}};
thread_local int metadata_retry_status=PGR_ACH_OK;
int metadata_status(int rc,bool missing_class=false){
 if(rc==PGR_MD_ERR_BUSY)metadata_retry_status=PGR_ACH_ERR_BUSY;
 else if(missing_class&&rc==PGR_MD_ERR_NOT_FOUND&&metadata_retry_status==PGR_ACH_OK)metadata_retry_status=PGR_ACH_NOT_READY;
 return metadata_retry_status!=PGR_ACH_OK?metadata_retry_status:PGR_ACH_ERR_METADATA;
}

bool is_attached_owner(uint64_t tid) {
  uint64_t expected = g.owner_tid.load(std::memory_order_acquire);
  if (tid == 0 || expected == 0 || tid != expected || tid != self_tid()) return false;
  if (!g.runtime.boot.thread_current) return false;
  auto fn = reinterpret_cast<void *(*)()>(g.runtime.boot.thread_current);
  return fn() != nullptr;
}

bool emit_transport_action(uint32_t opcode, const int64_t *scalars, uint32_t nscalars,
                           const PgrRuntimeTransportString *strings, uint32_t nstrings) {
  if (!(g.cfg_flags & PGR_ACH_CFG_USE_TRANSPORT)) return true;
  const auto *tr = transport_api();
  if (!tr || !tr->emit) {
    g.stopping.store(true, std::memory_order_release);
    return false;
  }
  if (tr->emit(opcode, scalars, nscalars, strings, nstrings) != PGR_TRANSPORT_OK) {
    g.stopping.store(true, std::memory_order_release);
    return false;
  }
  return true;
}

void emit_transport_event_json(const std::string &json) {
  if (!(g.cfg_flags & PGR_ACH_CFG_USE_TRANSPORT)) return;
  const char channel[] = "quick-close";
  PgrRuntimeTransportString strs[2] = {
      {channel, static_cast<uint32_t>(sizeof(channel) - 1)},
      {json.data(), static_cast<uint32_t>(json.size())}};
  emit_transport_action(19u, nullptr, 0, strs, 2u);
}

void push_host_event(uint32_t kind_code, uint32_t reason_code, const char *kind_name,
                     const std::string &body_fields, const char *id_str = nullptr,
                     const char *name_str = nullptr, const char *reason_str = nullptr,
                     bool emit_transport_diag = true) {
  uint64_t now = clock_boottime_ms();
  std::string json = "{\"kind\":" + json_quote(kind_name ? kind_name : "") +
                     ",\"boottimeMs\":" + std::to_string(now) +
                     (body_fields.empty() ? "" : "," + body_fields) + "}";

  if (emit_transport_diag && (g.cfg_flags & PGR_ACH_CFG_USE_TRANSPORT)) {
    emit_transport_event_json(json);
  }
  // Compiled52 is the selected sink. Keeping a second undrained copy would
  // eventually stop account management even while Java drains52 successfully.
  if(g.cfg_flags & PGR_ACH_CFG_USE_TRANSPORT)return;

  std::lock_guard<std::mutex> lk(g.event_mu);
  if (g.event_count >= PGR_ACH_EVENT_QUEUE_MAX) {
    g.stopping.store(true, std::memory_order_release);
    return;
  }
  PgrAccountHostEvent &ev = g.events[(g.event_head + g.event_count++) % PGR_ACH_EVENT_QUEUE_MAX];
  std::memset(&ev, 0, sizeof(ev));
  ev.version = PGR_ACH_EVENT_VERSION;
  ev.size = sizeof(PgrAccountHostEvent);
  ev.kind_code = kind_code;
  ev.reason_code = reason_code;
  ev.sequence = ++g.event_seq;
  copy_cstr(ev.kind, sizeof(ev.kind), kind_name);
  copy_cstr(ev.id, sizeof(ev.id), id_str);
  copy_cstr(ev.name, sizeof(ev.name), name_str);
  copy_cstr(ev.reason, sizeof(ev.reason), reason_str);
  copy_cstr(ev.json, sizeof(ev.json), json.c_str());
}

void emit_accounts_ready_once(bool available) {
  if (g.accounts_ready_emitted) return;
  g.accounts_ready_emitted = true;
  int64_t val = available ? 1 : 0;
  emit_transport_action(1u, &val, 1u, nullptr, 0u);
  push_host_event(PGR_ACH_EVT_ACCOUNTS_READY, available ? 1u : 0u, "accounts-ready",
                  std::string("\"available\":") + (available ? "true" : "false"));
}

int drain_accounts_events() {
  const auto *acc = accounts_api();
  if (!acc || !(g.caps.load(std::memory_order_acquire) & PGR_ACH_CAP_ACCOUNTS)) return PGR_ACH_OK;

  pgrazor_account_event_v1_t batch[PGR_ACCOUNTS_EVENT_QUEUE_CAP]{};
  for (int pass = 0; pass < 4; ++pass) {
    uint32_t n = acc->api_drain(batch, PGR_ACCOUNTS_EVENT_QUEUE_CAP);
    if (n == 0) break;
    if (n > PGR_ACCOUNTS_EVENT_QUEUE_CAP) {
      g.accounts_error.store(PGR_ACH_ERR_INSTALL, std::memory_order_release);
      g.stopping.store(true, std::memory_order_release);
      return PGR_ACH_ERR_INSTALL;
    }

    for (uint32_t i = 0; i < n; ++i) {
      const auto &ev = batch[i];
      const char *rtext = (ev.reason < kAccountsReasonCount) ? kAccountsReasonStrings[ev.reason] : "";

      switch (ev.kind) {
        case PGR_ACC_EVT_READY: {
          emit_accounts_ready_once(ev.available != 0);
          break;
        }
        case PGR_ACC_EVT_SESSION_ENDED: {
          emit_transport_action(2u, nullptr, 0u, nullptr, 0u);
          push_host_event(PGR_ACH_EVT_SESSION_ENDED, ev.reason, "account-session-ended", "");
          break;
        }
        case PGR_ACC_EVT_LOGOUT_REQUESTED: {
          bool cap = (ev.future_captured != 0);
          push_host_event(PGR_ACH_EVT_LOGOUT_REQUESTED, cap ? 1u : 0u, "account-logout-requested",
                          std::string("\"futureCaptured\":") + (cap ? "true" : "false"));
          break;
        }
        case PGR_ACC_EVT_LOGOUT_COMPLETE: {
          emit_transport_action(3u, nullptr, 0u, nullptr, 0u);
          std::string body = (ev.reason == PGR_ACC_REASON_CLEARED_AUTH_STATE)
                                 ? "\"confirmation\":\"cleared-auth-state\""
                                 : "";
          push_host_event(PGR_ACH_EVT_LOGOUT_COMPLETE, ev.reason, "account-logout-complete", body,
                          nullptr, nullptr,
                          ev.reason == PGR_ACC_REASON_CLEARED_AUTH_STATE ? "cleared-auth-state" : "");
          break;
        }
        case PGR_ACC_EVT_LOGOUT_FAILED: {
          emit_transport_action(4u, nullptr, 0u, nullptr, 0u);
          std::string body = (rtext && rtext[0] != '\0') ? ("\"reason\":" + json_quote(rtext)) : "";
          push_host_event(PGR_ACH_EVT_LOGOUT_FAILED, ev.reason, "account-logout-failed", body,
                          nullptr, nullptr, rtext);
          break;
        }
        case PGR_ACC_EVT_SESSION_SAVED: {
          emit_transport_action(6u, nullptr, 0u, nullptr, 0u);
          push_host_event(PGR_ACH_EVT_SESSION_SAVED, ev.reason, "account-session-saved", "");
          break;
        }
        case PGR_ACC_EVT_LOGIN_RESET_COMPLETE: {
          emit_transport_action(5u, nullptr, 0u, nullptr, 0u);
          push_host_event(PGR_ACH_EVT_LOGIN_RESET_COMPLETE, ev.reason, "account-login-reset-complete", "");
          break;
        }
        case PGR_ACC_EVT_IDENTITY: {
          std::string id_utf8, name_utf8;
          // Never synthesize account identity: require non-empty bounded UTF-16 id and name.
          if (bounded_utf16_to_utf8(ev.id, ev.id_len, &id_utf8) &&
              bounded_utf16_to_utf8(ev.name, ev.name_len, &name_utf8)) {
            if (g.cfg_flags & PGR_ACH_CFG_USE_TRANSPORT) {
              PgrRuntimeTransportString strs[2] = {
                  {id_utf8.data(), static_cast<uint32_t>(id_utf8.size())},
                  {name_utf8.data(), static_cast<uint32_t>(name_utf8.size())}};
              emit_transport_action(0u, nullptr, 0u, strs, 2u);
            }
            std::string body = "\"id\":" + json_quote(id_utf8) + ",\"name\":" + json_quote(name_utf8);
            // Note: emit_transport_diag = false so account identity is never written to diagnostic opcode 19 logs.
            push_host_event(PGR_ACH_EVT_IDENTITY, 0u, "account-identity", body,
                            id_utf8.c_str(), name_utf8.c_str(), nullptr, false);
          }
          break;
        }
        case PGR_ACC_EVT_READ_ERROR: {
          const char *msg = (rtext && rtext[0] != '\0') ? rtext : "Error: read-error";
          std::string body = "\"reason\":" + json_quote(msg);
          push_host_event(PGR_ACH_EVT_ACCOUNTS_READ_ERROR, ev.reason, "accounts-read-error", body,
                          nullptr, nullptr, msg);
          break;
        }
        default:
          break;
      }
    }
    if (n < PGR_ACCOUNTS_EVENT_QUEUE_CAP) break;
  }
  return PGR_ACH_OK;
}

// --- Metadata resolution & validation via Feature 47 -------------------------------
struct EffectiveConfig {
  uint32_t flags = 0;
  uint64_t owner_tid = 0;
  const void *module_base = nullptr;
  uint64_t module_size = 0;
  PgrMetaExports meta{};
  PgrAccountHostRuntime runtime{};
  bool has_lookup = false;
  bool lookup_known = false;
  PgrAccountHostItemsLookup lookup{};
  const PgrAccountHostMetadataOutput *substituted = nullptr;
  pgr_meta_seg_fn seg_query = nullptr;
  void *seg_ctx = nullptr;
};

bool check_all_pointers_non_null(const void *const *slots, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (slots[i] == nullptr) return false;
  }
  return true;
}

int build_effective_config(const PgrAccountHostConfig *cfg, EffectiveConfig *out) {
  *out = EffectiveConfig{};

  const bool use_boot = (cfg == nullptr) || ((cfg->flags & PGR_ACH_CFG_USE_BOOTSTRAP) != 0);
  if (cfg != nullptr) {
    if (cfg->version != PGR_ACH_CONFIG_VERSION || cfg->size != sizeof(PgrAccountHostConfig) ||
        cfg->reserved != 0 || (cfg->flags & ~PGR_ACH_CFG_KNOWN_MASK) != 0) {
      return PGR_ACH_ERR_BAD_CONFIG;
    }
    out->flags = cfg->flags;
    out->owner_tid = cfg->owner_tid;
    out->seg_query = cfg->seg_query;
    out->seg_ctx = cfg->seg_ctx;
    out->substituted = cfg->substituted_metadata;
  } else {
    out->flags = PGR_ACH_CFG_USE_TRANSPORT | PGR_ACH_CFG_USE_BOOTSTRAP | PGR_ACH_CFG_REQ_ALL;
    out->owner_tid = self_tid();
  }

  if ((out->flags & PGR_ACH_CFG_REQ_ALL) == 0) {
    out->flags |= PGR_ACH_CFG_REQ_ALL;
  }

  if (out->owner_tid == 0 || out->owner_tid != self_tid()) {
    return PGR_ACH_ERR_OFF_OWNER;
  }

  if (use_boot) {
    const auto *boot = boot_api();
    PgrBootBinding b{};
    if (!boot || !boot->snapshot)return PGR_ACH_ERR_BAD_CONFIG;
    int status=boot->snapshot(&b);
    if(status==PGR_BOOT_NOT_READY)return PGR_ACH_NOT_READY;
    if(status==PGR_BOOT_ERR_BUSY)return PGR_ACH_ERR_BUSY;
    if(status!=PGR_BOOT_OK)return PGR_ACH_ERR_BAD_CONFIG;
    out->module_base = b.module_base;
    out->module_size = b.module_size;
    out->meta = b.meta;
    out->runtime.version = PGR_ACH_RUNTIME_VERSION;
    out->runtime.size = sizeof(PgrAccountHostRuntime);
    out->runtime.boot = b.runtime;
    if (b.handle != nullptr) {
      out->runtime.type_get_object = dlsym(b.handle, "il2cpp_type_get_object");
      out->runtime.string_new = dlsym(b.handle, "il2cpp_string_new");
      out->runtime.string_length = dlsym(b.handle, "il2cpp_string_length");
    }
    if (cfg && cfg->runtime) {
      if (cfg->runtime->version != PGR_ACH_RUNTIME_VERSION || cfg->runtime->size != sizeof(PgrAccountHostRuntime))
        return PGR_ACH_ERR_BAD_CONFIG;
      if (!out->runtime.type_get_object) out->runtime.type_get_object = cfg->runtime->type_get_object;
      if (!out->runtime.string_new) out->runtime.string_new = cfg->runtime->string_new;
      if (!out->runtime.string_length) out->runtime.string_length = cfg->runtime->string_length;
    }
  } else {
    if (!cfg->module_base || cfg->module_size == 0 || !cfg->meta_exports || !cfg->runtime) {
      return PGR_ACH_ERR_BAD_CONFIG;
    }
    if (cfg->runtime->version != PGR_ACH_RUNTIME_VERSION || cfg->runtime->size != sizeof(PgrAccountHostRuntime)) {
      return PGR_ACH_ERR_BAD_CONFIG;
    }
    out->module_base = cfg->module_base;
    out->module_size = cfg->module_size;
    out->meta = *cfg->meta_exports;
    out->runtime = *cfg->runtime;
  }

  // Validate all runtime exports and thread attachment on owner_tid.
  const void *const *boot_slots = reinterpret_cast<const void *const *>(&out->runtime.boot);
  if (!check_all_pointers_non_null(boot_slots, sizeof(PgrBootRuntimeExports) / sizeof(void *)) ||
      !out->runtime.type_get_object || !out->runtime.string_new || !out->runtime.string_length) {
    return PGR_ACH_ERR_BAD_CONFIG;
  }
  auto thread_cur = reinterpret_cast<void *(*)()>(out->runtime.boot.thread_current);
  if (thread_cur() == nullptr) {
    return PGR_ACH_ERR_OFF_OWNER;
  }

  const bool has_substituted =
      ((out->flags & PGR_ACH_CFG_SUBSTITUTE_METADATA) != 0) && (out->substituted != nullptr);
  if ((out->flags & PGR_ACH_CFG_SUBSTITUTE_METADATA) != 0) {
    if (!out->substituted || out->substituted->version != PGR_ACH_META_OUT_VERSION ||
        out->substituted->size != sizeof(PgrAccountHostMetadataOutput)) {
      return PGR_ACH_ERR_BAD_CONFIG;
    }
  }
  if (!has_substituted) {
    const void *const *meta_slots = reinterpret_cast<const void *const *>(&out->meta);
    if (!check_all_pointers_non_null(meta_slots, sizeof(PgrMetaExports) / sizeof(void *))) {
      return PGR_ACH_ERR_BAD_CONFIG;
    }
  }

  // Extract itemsLookup from shared_metadata (56), items_lookup struct, or shared_host (61) getter.
  const PgrSharedMetadataOutput *sh_md = nullptr;
  if (cfg && cfg->shared_metadata) {
    if (cfg->shared_metadata->version != 2 || cfg->shared_metadata->size != sizeof(PgrSharedMetadataOutput)) {
      return PGR_ACH_ERR_BAD_CONFIG;
    }
    sh_md = cfg->shared_metadata;
  } else if (cfg && cfg->items_lookup) {
    out->lookup_known=true;
    if (cfg->items_lookup->version != PGR_ACH_LOOKUP_VERSION ||
        cfg->items_lookup->size != sizeof(PgrAccountHostItemsLookup)) {
      return PGR_ACH_ERR_BAD_CONFIG;
    }
    out->lookup = *cfg->items_lookup;
    out->has_lookup = (out->lookup.map_scene_context_class != nullptr &&
                       out->lookup.find_object_of_type_method != nullptr &&
                       out->lookup.map_get_container_method != nullptr &&
                       out->lookup.container_resolve_method != nullptr);
  } else if (const auto *sh = shared_host_api()) {
    if (sh->metadata) {
      const auto *m = sh->metadata();
      if (m && m->version == 2 && m->size == sizeof(PgrSharedMetadataOutput)) {
        sh_md = m;
      }
    }
  }

  if (sh_md != nullptr) {
    out->lookup_known=true;
    if (sh_md->optional[OPT_ITEMS_LOOKUP] == PGR_SHARED_OPT_AVAILABLE) {
      out->lookup.version = PGR_ACH_LOOKUP_VERSION;
      out->lookup.size = sizeof(PgrAccountHostItemsLookup);
      out->lookup.map_scene_context_class = const_cast<void *>(sh_md->opt_handles[H_IL_MAP]);
      out->lookup.find_object_of_type_method = const_cast<void *>(sh_md->opt_handles[H_IL_FIND_M]);
      out->lookup.map_get_container_method = const_cast<void *>(sh_md->opt_handles[H_IL_CONTAINER_M]);
      out->lookup.container_resolve_method = const_cast<void *>(sh_md->opt_handles[H_IL_RESOLVE_M]);
      out->has_lookup = (out->lookup.map_scene_context_class != nullptr &&
                         out->lookup.find_object_of_type_method != nullptr &&
                         out->lookup.map_get_container_method != nullptr &&
                         out->lookup.container_resolve_method != nullptr);
    }
  }

  return PGR_ACH_OK;
}

int resolve_class_47(const PgrMetaApi *md, const char *full_name, void **out_klass) {
  *out_klass = nullptr;
  const char *dot = std::strrchr(full_name, '.');
  std::string ns = dot ? std::string(full_name, static_cast<size_t>(dot - full_name)) : std::string();
  const char *name = dot ? (dot + 1) : full_name;

  PgrMetaClassQuery q{};
  q.version = PGR_MD_QUERY_VERSION;
  q.size = sizeof(q);
  q.name_space = ns.c_str();
  q.name = name;
  q.canonical = full_name;
  q.canonical_match = PGR_MD_MATCH_EXACT;

  PgrMetaClassResult r{};
  int rc = md->resolve_class(&q, &r);
  if (rc != PGR_MD_OK || !r.klass) return metadata_status(rc,true);
  *out_klass = const_cast<void *>(r.klass);
  return PGR_ACH_OK;
}

int validate_field_47(const PgrMetaApi *md, const void *owner, const char *name, uint64_t offset,
                      void **out_field) {
  if (out_field) *out_field = nullptr;
  PgrMetaFieldQuery q{};
  q.version = PGR_MD_QUERY_VERSION;
  q.size = sizeof(q);
  q.owner = owner;
  q.name = name;
  q.offset = offset;
  q.required_mask = 0;
  q.forbidden_mask = 0;
  q.kind = PGR_MD_FIELD_ANY;
  q.value_size = 0;
  q.type_name = nullptr;
  q.type_match = PGR_MD_MATCH_ANY;

  PgrMetaFieldResult r{};
  int rc = md->validate_field(&q, &r);
  if (rc != PGR_MD_OK || !r.field || r.offset != offset) return metadata_status(rc);
  if (out_field) *out_field = const_cast<void *>(r.field);
  return PGR_ACH_OK;
}

int validate_method_47(const PgrMetaApi *md, const EffectiveConfig &ec, const void *owner,
                       const char *name, uint32_t param_count, bool allow_static,
                       uint64_t rva, const unsigned char *prefix, uint32_t prefix_len,
                       void **out_method, void **out_code) {
  if (out_method) *out_method = nullptr;
  if (out_code) *out_code = nullptr;
  if (!owner || !name || !prefix || prefix_len == 0) return PGR_ACH_ERR_METADATA;

  const uintptr_t base = reinterpret_cast<uintptr_t>(ec.module_base);
  if (rva > ec.module_size || prefix_len > ec.module_size - rva || base > UINTPTR_MAX - rva) {
    return PGR_ACH_ERR_METADATA;
  }
  const void *expected_code = reinterpret_cast<const void *>(base + rva);

  using MethodFromNameFn = void *(*)(const void *, const char *, int);
  using MethodFlagsFn = uint32_t (*)(const void *, uint32_t *);
  auto fn_from_name = reinterpret_cast<MethodFromNameFn>(ec.meta.class_get_method_from_name);
  auto fn_flags = reinterpret_cast<MethodFlagsFn>(ec.meta.method_get_flags);

  void *raw_m = fn_from_name(owner, name, static_cast<int>(param_count));
  if (!raw_m) return PGR_ACH_ERR_METADATA;

  // Verify MethodInfo->methodPointer at offset 0 matches module_base + rva (exact JS parity).
  const void *first_word_code = nullptr;
  std::memcpy(&first_word_code, raw_m, sizeof(first_word_code));
  if (first_word_code != expected_code) return PGR_ACH_ERR_METADATA;

  uint32_t iflags = 0;
  uint32_t mflags = fn_flags(raw_m, &iflags);
  bool is_static = (mflags & 0x10u) != 0;
  if (!allow_static && is_static) return PGR_ACH_ERR_METADATA;

  PgrMetaMethodQuery q{};
  q.version = PGR_MD_QUERY_VERSION;
  q.size = sizeof(q);
  q.owner = owner;
  q.name = name;
  q.return_type = nullptr;
  q.return_match = PGR_MD_MATCH_ANY;
  q.param_count = param_count;
  q.is_static = is_static ? 1u : 0u;
  q.allow_inflated = 0u;
  q.expected_code = expected_code;
  q.prologue = prefix;
  q.prologue_len = prefix_len;

  PgrMetaMethodResult r{};
  int rc = md->validate_method(&q, &r);
  if (rc != PGR_MD_OK || r.method != raw_m || r.code != expected_code) {
    return metadata_status(rc);
  }

  // Also run explicit Feature 47 validate_code check on the 24-byte prologue.
  PgrMetaCodeQuery cq{};
  cq.version = PGR_MD_QUERY_VERSION;
  cq.size = sizeof(cq);
  cq.address = expected_code;
  cq.length = prefix_len;
  cq.prologue = prefix;
  cq.prologue_len = prefix_len;
  rc=md->validate_code(&cq);
  if (rc != PGR_MD_OK) {
    return metadata_status(rc);
  }

  if (out_method) *out_method = const_cast<void *>(r.method);
  if (out_code) *out_code = const_cast<void *>(expected_code);
  return PGR_ACH_OK;
}

int validate_enum_literal(const EffectiveConfig &ec, const void *enum_class, const char *name,
                          int32_t *out_val) {
  if (!enum_class || !name || !out_val) return PGR_ACH_ERR_METADATA;
  using FieldFromNameFn = void *(*)(const void *, const char *);
  using FieldParentFn = void *(*)(const void *);
  using FieldFlagsFn = int (*)(const void *);
  using StaticGetFn = void (*)(const void *, void *);

  auto fn_field = reinterpret_cast<FieldFromNameFn>(ec.meta.class_get_field_from_name);
  auto fn_parent = reinterpret_cast<FieldParentFn>(ec.meta.field_get_parent);
  auto fn_flags = reinterpret_cast<FieldFlagsFn>(ec.meta.field_get_flags);
  auto fn_static_get = reinterpret_cast<StaticGetFn>(ec.runtime.boot.field_static_get_value);

  void *f = fn_field(enum_class, name);
  if (!f || fn_parent(f) != enum_class) return PGR_ACH_ERR_METADATA;
  if ((static_cast<uint32_t>(fn_flags(f)) & 0x8050u) != 0x8050u) return PGR_ACH_ERR_METADATA;
  int32_t v = 0;
  fn_static_get(f, &v);
  *out_val = v;
  return PGR_ACH_OK;
}

int resolve_accounts_metadata(const PgrMetaApi *md, const EffectiveConfig &ec,
                              PgrAccountHostMetadataOutput *out) {
  out->accounts_valid = 0;
  out->accounts_error = PGR_ACH_ERR_METADATA;

  // Preserve "no available lookup" behavior: if OPT_ITEMS_LOOKUP is missing, fail Accounts closed.
  if (!ec.has_lookup || !ec.lookup.map_scene_context_class || !ec.lookup.find_object_of_type_method ||
      !ec.lookup.map_get_container_method || !ec.lookup.container_resolve_method) {
    return ec.lookup_known?PGR_ACH_ERR_METADATA:PGR_ACH_NOT_READY;
  }

  using MethodClassFn = void *(*)(const void *);
  using AssignableFn = bool (*)(const void *, const void *);
  auto fn_method_class = reinterpret_cast<MethodClassFn>(ec.meta.method_get_class);
  auto fn_assignable = reinterpret_cast<AssignableFn>(ec.meta.class_is_assignable_from);

  void *di_container_class = fn_method_class(ec.lookup.container_resolve_method);
  void *map_container_declaring = fn_method_class(ec.lookup.map_get_container_method);
  if (!di_container_class || !map_container_declaring ||
      !fn_assignable(map_container_declaring, ec.lookup.map_scene_context_class)) {
    return PGR_ACH_ERR_METADATA;
  }

  void *status_class = nullptr;
  if (resolve_class_47(md, "System.Boolean", &out->boolean_class) != PGR_ACH_OK ||
      resolve_class_47(md, "System.String", &out->string_class) != PGR_ACH_OK ||
      resolve_class_47(md, "Niantic.Platform.Promises.Future", &out->future_class) != PGR_ACH_OK ||
      resolve_class_47(md, "Niantic.Holoholo.PlayerService", &out->player_service_class) != PGR_ACH_OK ||
      resolve_class_47(md, "Niantic.Holoholo.IPlayerService", &out->iplayer_service_class) != PGR_ACH_OK ||
      resolve_class_47(md, "Niantic.Platform.Auth.AuthService", &out->auth_service_class) != PGR_ACH_OK ||
      resolve_class_47(md, "Niantic.Platform.Auth.IAuthService", &out->iauth_service_class) != PGR_ACH_OK ||
      resolve_class_47(md, "Niantic.Holoholo.Login.AuthServiceLogOutWrapper", &out->logout_wrapper_class) != PGR_ACH_OK ||
      resolve_class_47(md, "Niantic.Holoholo.Login.IAuthServiceLogOutWrapper", &out->ilogout_wrapper_class) != PGR_ACH_OK ||
      resolve_class_47(md, "UnityEngine.PlayerPrefs", &out->player_prefs_class) != PGR_ACH_OK ||
      resolve_class_47(md, "Niantic.Platform.Promises.PromiseStatus", &status_class) != PGR_ACH_OK) {
    return PGR_ACH_ERR_METADATA;
  }

  if (validate_field_47(md, out->auth_service_class, "activeAuthProvider", kOffAuthActiveProvider,
                        &out->auth_active_provider_field) != PGR_ACH_OK ||
      validate_field_47(md, out->future_class, "_status", kOffFutureStatus,
                        &out->future_status_field) != PGR_ACH_OK ||
      validate_field_47(md, out->future_class, "_isDisposed", kOffFutureIsDisposed,
                        &out->future_is_disposed_field) != PGR_ACH_OK) {
    return PGR_ACH_ERR_METADATA;
  }

  if (validate_enum_literal(ec, status_class, "CompletedSucceeded", &out->promise_succeeded_value) != PGR_ACH_OK ||
      validate_enum_literal(ec, status_class, "CompletedFailed", &out->promise_failed_value) != PGR_ACH_OK) {
    return PGR_ACH_ERR_METADATA;
  }

  if (validate_method_47(md, ec, out->player_service_class, "get_PlayerId", 0, false,
                         kRvaPlayerGetId, kPrefixPlayerGetId, sizeof(kPrefixPlayerGetId),
                         &out->player_get_id_method, nullptr) != PGR_ACH_OK ||
      validate_method_47(md, ec, out->player_service_class, "get_PlayerName", 0, false,
                         kRvaPlayerGetName, kPrefixPlayerGetName, sizeof(kPrefixPlayerGetName),
                         &out->player_get_name_method, nullptr) != PGR_ACH_OK ||
      validate_method_47(md, ec, out->auth_service_class, "get_IsConnected", 0, false,
                         kRvaAuthIsConnected, kPrefixAuthIsConnected, sizeof(kPrefixAuthIsConnected),
                         &out->auth_get_is_connected_method, nullptr) != PGR_ACH_OK ||
      validate_method_47(md, ec, out->auth_service_class, "get_CachedCredentialsExist", 0, false,
                         kRvaAuthCachedCreds, kPrefixAuthCachedCreds, sizeof(kPrefixAuthCachedCreds),
                         &out->auth_get_cached_credentials_method, nullptr) != PGR_ACH_OK ||
      validate_method_47(md, ec, out->logout_wrapper_class, "Logout", 0, false,
                         kRvaWrapperLogout, kPrefixWrapperLogout, sizeof(kPrefixWrapperLogout),
                         &out->wrapper_logout_method, nullptr) != PGR_ACH_OK ||
      validate_method_47(md, ec, out->auth_service_class, "Logout", 0, false,
                         kRvaAuthLogout, kPrefixAuthLogout, sizeof(kPrefixAuthLogout),
                         &out->auth_logout_method, &out->auth_logout_target_fn) != PGR_ACH_OK ||
      validate_method_47(md, ec, out->player_prefs_class, "HasKey", 1, true,
                         kRvaPrefsHasKey, kPrefixPrefsHasKey, sizeof(kPrefixPrefsHasKey),
                         &out->prefs_has_key_method, nullptr) != PGR_ACH_OK ||
      validate_method_47(md, ec, out->player_prefs_class, "DeleteKey", 1, true,
                         kRvaPrefsDeleteKey, kPrefixPrefsDeleteKey, sizeof(kPrefixPrefsDeleteKey),
                         &out->prefs_delete_key_method, nullptr) != PGR_ACH_OK ||
      validate_method_47(md, ec, out->player_prefs_class, "Save", 0, true,
                         kRvaPrefsSave, kPrefixPrefsSave, sizeof(kPrefixPrefsSave),
                         &out->prefs_save_method, nullptr) != PGR_ACH_OK) {
    return PGR_ACH_ERR_METADATA;
  }

  out->map_scene_context_class = ec.lookup.map_scene_context_class;
  out->di_container_class = di_container_class;
  out->find_object_of_type_method = ec.lookup.find_object_of_type_method;
  out->map_get_container_method = ec.lookup.map_get_container_method;
  out->container_resolve_method = ec.lookup.container_resolve_method;

  out->accounts_valid = 1u;
  out->accounts_error = PGR_ACH_OK;
  return PGR_ACH_OK;
}

int resolve_advsync_metadata(const PgrMetaApi *md, const EffectiveConfig &ec,
                             PgrAccountHostMetadataOutput *out) {
  out->advsync_valid = 0;
  out->advsync_error = PGR_ACH_ERR_METADATA;

  if (resolve_class_47(md, "Niantic.Platform.NiaAdvsyncPermissions.Internal.DialogController",
                       &out->adv_controller_class) != PGR_ACH_OK) {
    return PGR_ACH_ERR_METADATA;
  }

  if (validate_field_47(md, out->adv_controller_class, "_onConfirm", kOffAdvOnConfirm, nullptr) != PGR_ACH_OK ||
      validate_field_47(md, out->adv_controller_class, "_onDecline", kOffAdvOnDecline, nullptr) != PGR_ACH_OK) {
    return PGR_ACH_ERR_METADATA;
  }

  void *m_init_initial = nullptr;
  void *m_init_instruction = nullptr;
  void *m_init_result = nullptr;
  void *m_update = nullptr;

  if (validate_method_47(md, ec, out->adv_controller_class, "InitializeAsInitial", 4, false,
                         kRvaAdvInitInitial, kPrefixAdvInitInitial, sizeof(kPrefixAdvInitInitial),
                         &m_init_initial, &out->adv_code_init_initial) != PGR_ACH_OK ||
      validate_method_47(md, ec, out->adv_controller_class, "InitializeAsInstruction", 7, false,
                         kRvaAdvInitInstruction, kPrefixAdvInitInstruction, sizeof(kPrefixAdvInitInstruction),
                         &m_init_instruction, &out->adv_code_init_instruction) != PGR_ACH_OK ||
      validate_method_47(md, ec, out->adv_controller_class, "InitializeAsResult", 5, false,
                         kRvaAdvInitResult, kPrefixAdvInitResult, sizeof(kPrefixAdvInitResult),
                         &m_init_result, &out->adv_code_init_result) != PGR_ACH_OK ||
      validate_method_47(md, ec, out->adv_controller_class, "OnConfirm", 0, false,
                         kRvaAdvOnConfirm, kPrefixAdvOnConfirm, sizeof(kPrefixAdvOnConfirm),
                         &out->adv_method_on_confirm, &out->adv_code_on_confirm) != PGR_ACH_OK ||
      validate_method_47(md, ec, out->adv_controller_class, "OnDecline", 0, false,
                         kRvaAdvOnDecline, kPrefixAdvOnDecline, sizeof(kPrefixAdvOnDecline),
                         &out->adv_method_on_decline, &out->adv_code_on_decline) != PGR_ACH_OK ||
      validate_method_47(md, ec, out->adv_controller_class, "Update", 0, false,
                         kRvaAdvUpdate, kPrefixAdvUpdate, sizeof(kPrefixAdvUpdate),
                         &m_update, &out->adv_code_update) != PGR_ACH_OK) {
    return PGR_ACH_ERR_METADATA;
  }

  out->adv_off_on_confirm = kOffAdvOnConfirm;
  out->adv_off_on_decline = kOffAdvOnDecline;
  out->advsync_valid = 1u;
  out->advsync_error = PGR_ACH_OK;
  return PGR_ACH_OK;
}

int resolve_metadata_internal(const EffectiveConfig &ec, PgrAccountHostMetadataOutput *out) {
  metadata_retry_status=PGR_ACH_OK;
  std::memset(out, 0, sizeof(*out));
  out->version = PGR_ACH_META_OUT_VERSION;
  out->size = sizeof(PgrAccountHostMetadataOutput);

  if ((ec.flags & PGR_ACH_CFG_SUBSTITUTE_METADATA) != 0 && ec.substituted != nullptr) {
    *out = *ec.substituted;
    return PGR_ACH_OK;
  }

  const auto *md = meta_api();
  if (!md) {
    out->accounts_error = PGR_ACH_ERR_METADATA;
    out->advsync_error = PGR_ACH_ERR_METADATA;
    return PGR_ACH_ERR_METADATA;
  }

  PgrMetaBindConfig bind_cfg{};
  bind_cfg.version = PGR_MD_CONFIG_VERSION;
  bind_cfg.size = sizeof(bind_cfg);
  bind_cfg.exports = &ec.meta;
  bind_cfg.module_base = ec.module_base;
  bind_cfg.module_size = ec.module_size;
  bind_cfg.seg_query = ec.seg_query;
  bind_cfg.seg_ctx = ec.seg_ctx;

  int bind_status=md->bind(&bind_cfg);
  if (bind_status != PGR_MD_OK) {
    int status=metadata_status(bind_status);out->accounts_error=status;out->advsync_error=status;return status;
  }

  int acc_rc = PGR_ACH_OK;
  int adv_rc = PGR_ACH_OK;
  if (ec.flags & PGR_ACH_CFG_REQ_ACCOUNTS) {
    metadata_retry_status=PGR_ACH_OK;
    acc_rc = resolve_accounts_metadata(md, ec, out);
    if(metadata_retry_status!=PGR_ACH_OK)acc_rc=metadata_retry_status;
    out->accounts_error=acc_rc;
  }
  if (ec.flags & PGR_ACH_CFG_REQ_ADVSYNC) {
    metadata_retry_status=PGR_ACH_OK;
    adv_rc = resolve_advsync_metadata(md, ec, out);
    if(metadata_retry_status!=PGR_ACH_OK)adv_rc=metadata_retry_status;
    out->advsync_error=adv_rc;
  }

  if(acc_rc==PGR_ACH_ERR_BUSY||adv_rc==PGR_ACH_ERR_BUSY)return PGR_ACH_ERR_BUSY;
  if(acc_rc==PGR_ACH_NOT_READY||adv_rc==PGR_ACH_NOT_READY)return PGR_ACH_NOT_READY;
  if ((ec.flags & PGR_ACH_CFG_REQ_ACCOUNTS) && acc_rc != PGR_ACH_OK) return acc_rc;
  if ((ec.flags & PGR_ACH_CFG_REQ_ADVSYNC) && adv_rc != PGR_ACH_OK) return adv_rc;
  return PGR_ACH_OK;
}

bool validate_accounts_metadata_pointers(const PgrAccountHostMetadataOutput &m) {
  if (!m.accounts_valid) return false;
  const void *const ptrs[] = {
      m.boolean_class, m.string_class, m.future_class, m.map_scene_context_class,
      m.di_container_class, m.player_service_class, m.iplayer_service_class,
      m.auth_service_class, m.iauth_service_class, m.logout_wrapper_class,
      m.ilogout_wrapper_class, m.player_prefs_class, m.auth_active_provider_field,
      m.future_status_field, m.future_is_disposed_field, m.find_object_of_type_method,
      m.map_get_container_method, m.container_resolve_method, m.player_get_id_method,
      m.player_get_name_method, m.auth_get_is_connected_method,
      m.auth_get_cached_credentials_method, m.wrapper_logout_method, m.auth_logout_method,
      m.prefs_has_key_method, m.prefs_delete_key_method, m.prefs_save_method,
      m.auth_logout_target_fn};
  return check_all_pointers_non_null(ptrs, sizeof(ptrs) / sizeof(ptrs[0]));
}

bool validate_advsync_metadata_pointers(const PgrAccountHostMetadataOutput &m) {
  if (!m.advsync_valid) return false;
  const void *const ptrs[] = {
      m.adv_controller_class, m.adv_method_on_confirm, m.adv_method_on_decline,
      m.adv_code_update, m.adv_code_on_confirm, m.adv_code_on_decline,
      m.adv_code_init_initial, m.adv_code_init_instruction, m.adv_code_init_result};
  return check_all_pointers_non_null(ptrs, sizeof(ptrs) / sizeof(ptrs[0])) &&
         m.adv_off_on_confirm == kOffAdvOnConfirm &&
         m.adv_off_on_decline == kOffAdvOnDecline;
}

// --- API functions -----------------------------------------------------------------
int api_resolve_metadata(const PgrAccountHostConfig *config, PgrAccountHostMetadataOutput *out) {
  if (!out) return PGR_ACH_ERR_BAD_CONFIG;
  std::memset(out,0,sizeof(*out));
  ConfigGuard guard;if(!guard.acquired)return PGR_ACH_ERR_BUSY;
  EffectiveConfig ec{};
  int rc = build_effective_config(config, &ec);
  if (rc != PGR_ACH_OK) return rc;
  return resolve_metadata_internal(ec, out);
}

int api_configure(const PgrAccountHostConfig *config) {
  ConfigGuard guard;if(!guard.acquired)return PGR_ACH_ERR_BUSY;
  if (g.stopping.load(std::memory_order_acquire)) return PGR_ACH_ERR_STOPPED;

  EffectiveConfig ec{};
  int rc = build_effective_config(config, &ec);
  if (rc != PGR_ACH_OK) return rc;

  if (g.stopping.load(std::memory_order_acquire)) return PGR_ACH_ERR_STOPPED;

  const uint32_t base_flags_mask = ~(PGR_ACH_CFG_REQ_ALL);
  if (g.configured.load(std::memory_order_acquire)) {
    if (g.owner_tid.load(std::memory_order_acquire) != ec.owner_tid ||
        g.module_base != ec.module_base || g.module_size != ec.module_size ||
        std::memcmp(&g.runtime,&ec.runtime,sizeof(g.runtime)) ||
        std::memcmp(&g.meta_exports,&ec.meta,sizeof(g.meta_exports)) ||
        (g.cfg_flags & base_flags_mask) != (ec.flags & base_flags_mask)) {
      return PGR_ACH_ERR_RECONFIG;
    }
  } else {
    g.owner_tid.store(ec.owner_tid, std::memory_order_release);
    g.module_base = ec.module_base;
    g.module_size = ec.module_size;
    g.cfg_flags = ec.flags;
    g.meta_exports = ec.meta;
    g.runtime = ec.runtime;
  }
  g.cfg_flags |= (ec.flags & PGR_ACH_CFG_REQ_ALL);

  const bool want_acc = (ec.flags & PGR_ACH_CFG_REQ_ACCOUNTS) != 0;
  const bool want_adv = (ec.flags & PGR_ACH_CFG_REQ_ADVSYNC) != 0;
  const bool need_acc = want_acc && !g.accounts_attempted;
  const bool need_adv = want_adv && !g.advsync_attempted;
  int pending_status=PGR_ACH_OK;

  if (need_acc || need_adv) {
    PgrAccountHostMetadataOutput md_out{};
    // Revalidate only families not yet attempted. Installed Dobby sites no longer
    // contain the original prologue and must never be re-resolved/re-hooked.
    EffectiveConfig needed=ec;needed.flags&=~PGR_ACH_CFG_REQ_ALL;
    if(need_acc)needed.flags|=PGR_ACH_CFG_REQ_ACCOUNTS;
    if(need_adv)needed.flags|=PGR_ACH_CFG_REQ_ADVSYNC;
    resolve_metadata_internal(needed,&md_out);
    bool retry_acc=need_acc&&(md_out.accounts_error==PGR_ACH_NOT_READY||md_out.accounts_error==PGR_ACH_ERR_BUSY);
    bool retry_adv=need_adv&&(md_out.advsync_error==PGR_ACH_NOT_READY||md_out.advsync_error==PGR_ACH_ERR_BUSY);
    if(retry_acc){g.accounts_error=md_out.accounts_error;pending_status=md_out.accounts_error;}
    if(retry_adv){g.advsync_error=md_out.advsync_error;if(pending_status!=PGR_ACH_ERR_BUSY)pending_status=md_out.advsync_error;}
    if(g.stopping)return PGR_ACH_ERR_STOPPED;

    if (need_acc&&!retry_acc) {
      g.accounts_attempted = true;
      const auto *acc = accounts_api();
      if (acc && validate_accounts_metadata_pointers(md_out)) {
        pgrazor_accounts_config_v1_t acfg{};
        void *pointers[]={ec.runtime.boot.runtime_invoke,ec.runtime.boot.gchandle_new,ec.runtime.boot.gchandle_get_target,ec.runtime.boot.gchandle_free,ec.runtime.boot.object_get_class,ec.runtime.boot.object_unbox,ec.meta.class_get_type,ec.runtime.type_get_object,ec.runtime.string_new,ec.runtime.string_length,ec.runtime.boot.string_chars,ec.runtime.boot.field_get_value,md_out.boolean_class,md_out.string_class,md_out.future_class,md_out.map_scene_context_class,md_out.di_container_class,md_out.player_service_class,md_out.iplayer_service_class,md_out.auth_service_class,md_out.iauth_service_class,md_out.logout_wrapper_class,md_out.ilogout_wrapper_class,md_out.player_prefs_class,md_out.auth_active_provider_field,md_out.future_status_field,md_out.future_is_disposed_field,md_out.find_object_of_type_method,md_out.map_get_container_method,md_out.container_resolve_method,md_out.player_get_id_method,md_out.player_get_name_method,md_out.auth_get_is_connected_method,md_out.auth_get_cached_credentials_method,md_out.wrapper_logout_method,md_out.auth_logout_method,md_out.prefs_has_key_method,md_out.prefs_delete_key_method,md_out.prefs_save_method,md_out.auth_logout_target_fn};
        static_assert(sizeof(pointers)==320,"Accounts16 pointer ABI");
        std::memcpy(&acfg,pointers,sizeof(pointers));
        acfg.promise_succeeded_value = md_out.promise_succeeded_value;
        acfg.promise_failed_value = md_out.promise_failed_value;

        if (acc->api_init(&acfg) == 1) {
          g.caps.fetch_or(PGR_ACH_CAP_ACCOUNTS, std::memory_order_acq_rel);
          g.accounts_error.store(PGR_ACH_OK, std::memory_order_release);
          drain_accounts_events();
          emit_accounts_ready_once(true);
        } else {
          g.accounts_error.store(PGR_ACH_ERR_INSTALL, std::memory_order_release);
          emit_accounts_ready_once(false);
        }
      } else {
        g.accounts_error.store(md_out.accounts_error ? md_out.accounts_error : PGR_ACH_ERR_METADATA,
                               std::memory_order_release);
        emit_accounts_ready_once(false);
      }
    }

    if(g.stopping)return PGR_ACH_ERR_STOPPED;
    if (need_adv&&!retry_adv) {
      g.advsync_attempted = true;
      const auto *adv = advsync_api();
      if (adv && validate_advsync_metadata_pointers(md_out)) {
        pgrazor_advsync_config_v1 adv_cfg{};
        adv_cfg.version = 1;
        adv_cfg.size = sizeof(adv_cfg);
        adv_cfg.runtime_invoke = ec.runtime.boot.runtime_invoke;
        adv_cfg.gchandle_new = ec.runtime.boot.gchandle_new;
        adv_cfg.gchandle_get_target = ec.runtime.boot.gchandle_get_target;
        adv_cfg.gchandle_free = ec.runtime.boot.gchandle_free;
        adv_cfg.object_get_class = ec.runtime.boot.object_get_class;
        adv_cfg.method_on_confirm = md_out.adv_method_on_confirm;
        adv_cfg.method_on_decline = md_out.adv_method_on_decline;
        adv_cfg.code_update = md_out.adv_code_update;
        adv_cfg.code_on_confirm = md_out.adv_code_on_confirm;
        adv_cfg.code_on_decline = md_out.adv_code_on_decline;
        adv_cfg.code_init_initial = md_out.adv_code_init_initial;
        adv_cfg.code_init_instruction = md_out.adv_code_init_instruction;
        adv_cfg.code_init_result = md_out.adv_code_init_result;
        adv_cfg.controller_class = md_out.adv_controller_class;
        adv_cfg.off_on_confirm = md_out.adv_off_on_confirm;
        adv_cfg.off_on_decline = md_out.adv_off_on_decline;

        int irc = adv->install(&adv_cfg);
        if (irc == 0) {
          g.caps.fetch_or(PGR_ACH_CAP_ADVENTURE_SYNC, std::memory_order_acq_rel);
          g.advsync_error.store(PGR_ACH_OK, std::memory_order_release);
          // Bind unity_tid on Feature 2 immediately with a 0ms disarmed lease.
          adv->update(0, 0, ec.owner_tid);
          push_host_event(PGR_ACH_EVT_ADVSYNC_READY, 0u, "adventure-sync-native-ready", "");
        } else {
          g.advsync_error.store(PGR_ACH_ERR_INSTALL, std::memory_order_release);
          push_host_event(PGR_ACH_EVT_ADVSYNC_UNAVAILABLE, static_cast<uint32_t>(irc),
                          "adventure-sync-native-unavailable",
                          "\"reason\":\"native-install-failed\"", nullptr, nullptr,
                          "native-install-failed");
        }
      } else {
        g.advsync_error.store(md_out.advsync_error ? md_out.advsync_error : PGR_ACH_ERR_METADATA,
                              std::memory_order_release);
        push_host_event(PGR_ACH_EVT_ADVSYNC_UNAVAILABLE, 1u, "adventure-sync-native-unavailable",
                        "\"reason\":\"native-metadata-unavailable\"", nullptr, nullptr,
                        "native-metadata-unavailable");
      }
    }
  }

  if(g.stopping)return PGR_ACH_ERR_STOPPED;
  g.configured.store(g.caps.load()!=0||g.accounts_attempted||g.advsync_attempted, std::memory_order_release);
  if(pending_status!=PGR_ACH_OK)return pending_status;

  // If ONLY AdventureSync was requested and it failed, return its error code so caller can report failure.
  if (want_adv && !want_acc && !(g.caps.load(std::memory_order_acquire) & PGR_ACH_CAP_ADVENTURE_SYNC)) {
    return g.advsync_error.load(std::memory_order_acquire);
  }
  return PGR_ACH_OK;
}

// These two leaf policy setters do not invoke managed code or GC. Serialize only
// their publication against stop; never keep this mutex across tick/clear/drain.
void publish_accounts(bool allow,uint32_t lease,uint32_t kind,uint64_t token){
 std::lock_guard<std::mutex> lock(g.policy_mu);
 if(g.stopping||g.accounts_cleared){allow=false;lease=0;kind=PGR_ACC_CMD_NONE;token=0;}
 if(auto acc=accounts_api())acc->api_update(allow,lease,kind,token);
}
void publish_advsync(bool allow,uint32_t lease){
 std::lock_guard<std::mutex> lock(g.policy_mu);
 if(g.stopping){allow=false;lease=0;}
 if(auto adv=advsync_api())adv->update(allow,allow?lease:0,g.owner_tid.load());
}
void revoke_accounts_leaf() {
  if (g.caps.load(std::memory_order_acquire) & PGR_ACH_CAP_ACCOUNTS) {
    if (const auto *acc = accounts_api()) {
      acc->api_update(false,0,PGR_ACC_CMD_NONE,0);
    }
  }
}

void revoke_advsync_leaf() {
  if (g.caps.load(std::memory_order_acquire) & PGR_ACH_CAP_ADVENTURE_SYNC) {
    if (const auto *adv = advsync_api()) {
      adv->update(0, 0, g.owner_tid.load(std::memory_order_acquire));
    }
  }
}

int api_set_adventure_sync_allowed(int allowed) {
  if (!g.configured.load(std::memory_order_acquire)) return PGR_ACH_ERR_NOT_CONFIGURED;
  if (allowed != 0 && allowed != 1) {
    std::lock_guard<std::mutex> lk(g.policy_mu);
    g.adv_allowed = false;
    revoke_advsync_leaf();
    return PGR_ACH_ERR_BAD_CONFIG;
  }
  if (g.stopping.load(std::memory_order_acquire)) {
    revoke_advsync_leaf();
    return PGR_ACH_ERR_STOPPED;
  }

  const bool allow = (allowed == 1);
  {
    std::lock_guard<std::mutex> lk(g.policy_mu);
    g.adv_allowed = allow;
  }
  if (g.caps.load(std::memory_order_acquire) & PGR_ACH_CAP_ADVENTURE_SYNC) {
    publish_advsync(allow,PGR_ACH_MAX_ADVSYNC_LEASE_MS);
  }
  return PGR_ACH_OK;
}

int api_set_options(const PgrAccountHostOptions *options) {
  if (!g.configured.load(std::memory_order_acquire)) return PGR_ACH_ERR_NOT_CONFIGURED;
  if (!options || options->version != PGR_ACH_OPTIONS_VERSION ||
      options->size != sizeof(PgrAccountHostOptions) || options->reserved != 0 ||
      (options->flags & ~PGR_ACH_OPT_KNOWN_MASK) != 0 ||
      options->accounts_allowed > 1u || options->accounts_lease_ms > PGR_ACH_MAX_ACCOUNTS_LEASE_MS ||
      options->advsync_allowed > 1u || options->advsync_lease_ms > PGR_ACH_MAX_ADVSYNC_LEASE_MS) {
    {
      std::lock_guard<std::mutex> lk(g.policy_mu);
      g.acc_until_ms = 0;
      g.acc_cmd_kind = PGR_ACC_CMD_NONE;
      g.acc_cmd_token = 0;
      g.adv_allowed = false;
    }
    revoke_accounts_leaf();
    revoke_advsync_leaf();
    return PGR_ACH_ERR_BAD_CONFIG;
  }

  if (g.stopping.load(std::memory_order_acquire) || g.accounts_cleared.load(std::memory_order_acquire)) {
    revoke_accounts_leaf();
    revoke_advsync_leaf();
    return g.stopping.load(std::memory_order_acquire) ? PGR_ACH_ERR_STOPPED : PGR_ACH_OK;
  }

  const uint32_t eff_flags = (options->flags == 0) ? PGR_ACH_OPT_SET_ACCOUNTS : options->flags;

  if (eff_flags & PGR_ACH_OPT_REVOKE_ONLY) {
    {
      std::lock_guard<std::mutex> lk(g.policy_mu);
      g.acc_until_ms = 0;
      g.acc_cmd_kind = PGR_ACC_CMD_NONE;
      g.acc_cmd_token = 0;
    }
    revoke_accounts_leaf();
    return PGR_ACH_OK;
  }

  if (eff_flags & PGR_ACH_OPT_SET_ACCOUNTS) {
    uint64_t now_ms = clock_boottime_ms();
    if (!now_ms) {
      g.stopping.store(true, std::memory_order_release);
      revoke_accounts_leaf();
      return PGR_ACH_ERR_CLOCK;
    }

    if (options->accounts_allowed == 0) {
      {
        std::lock_guard<std::mutex> lk(g.policy_mu);
        g.acc_until_ms = 0;
        g.acc_cmd_kind = PGR_ACC_CMD_NONE;
        g.acc_cmd_token = 0;
      }
      revoke_accounts_leaf();
    } else {
      const char *raw_cmd = options->command ? options->command : "";
      size_t cmd_len = strnlen(raw_cmd, PGR_ACH_CMD_MAX_BYTES + 1);
      if (cmd_len > PGR_ACH_CMD_MAX_BYTES) {
        {
          std::lock_guard<std::mutex> lk(g.policy_mu);
          g.acc_until_ms = 0;
          g.acc_cmd_kind = PGR_ACC_CMD_NONE;
          g.acc_cmd_token = 0;
        }
        revoke_accounts_leaf();
      } else {
        uint32_t kind = classify_command(raw_cmd);
        uint64_t tok = 0;
        bool valid_cmd = true;
        if (kind != PGR_ACC_CMD_NONE) {
          tok = parse_token_exact64(options->command_token);
          if (tok == 0) {
            // Fail closed on missing/invalid/0/overflowed command token.
            valid_cmd = false;
          }
        }

        if (!valid_cmd) {
          {
            std::lock_guard<std::mutex> lk(g.policy_mu);
            g.acc_until_ms = 0;
            g.acc_cmd_kind = PGR_ACC_CMD_NONE;
            g.acc_cmd_token = 0;
          }
          revoke_accounts_leaf();
        } else {
          uint32_t lease = options->accounts_lease_ms ? options->accounts_lease_ms : PGR_ACH_MAX_ACCOUNTS_LEASE_MS;
          {
            std::lock_guard<std::mutex> lk(g.policy_mu);
            g.acc_until_ms = now_ms + lease;
            g.acc_cmd_kind = kind;
            g.acc_cmd_token = tok;
          }
          if (g.caps.load(std::memory_order_acquire) & PGR_ACH_CAP_ACCOUNTS) {
            if (const auto *acc = accounts_api()) {
              publish_accounts(true,lease,kind,tok);
            }
          }
        }
      }
    }
  }

  if (eff_flags & PGR_ACH_OPT_SET_ADVSYNC) {
    const bool allow = (options->advsync_allowed == 1u);
    const uint32_t lease = allow
        ? (options->advsync_lease_ms ? options->advsync_lease_ms : PGR_ACH_MAX_ADVSYNC_LEASE_MS)
        : 0u;
    {
      std::lock_guard<std::mutex> lk(g.policy_mu);
      g.adv_allowed = allow;
    }
    if (g.caps.load(std::memory_order_acquire) & PGR_ACH_CAP_ADVENTURE_SYNC) {
      publish_advsync(allow,lease);
    }
  }

  return PGR_ACH_OK;
}

void poll_transport52() {
  if (!(g.cfg_flags & PGR_ACH_CFG_USE_TRANSPORT)) return;
  const auto *tr = transport_api();
  if (!tr) return;

  PgrRuntimeTransportSample before{}, after{};
  if (tr->sample(&before) != PGR_TRANSPORT_OK || !before.snapshot_fresh || before.fault) {
    PgrAccountHostOptions off{};
    off.version = PGR_ACH_OPTIONS_VERSION;
    off.size = sizeof(off);
    off.flags = PGR_ACH_OPT_SET_ACCOUNTS | PGR_ACH_OPT_SET_ADVSYNC;
    api_set_options(&off);
    return;
  }

  int acc_allowed = 0;
  int adv_allowed = 0;
  if (tr->get_bool(0u, &acc_allowed) != PGR_TRANSPORT_OK) acc_allowed = 0;
  if (tr->get_bool(1u, &adv_allowed) != PGR_TRANSPORT_OK) adv_allowed = 0;

  char cmd_buf[PGR_ACH_CMD_MAX_BYTES + 1]{};
  char tok_buf[PGR_ACH_TOKEN_MAX_BYTES + 1]{};
  uint32_t cmd_len = 0;
  uint32_t tok_len = 0;
  if (acc_allowed == 1 && tr->get_command_snapshot) {
    if (tr->get_command_snapshot(cmd_buf, sizeof(cmd_buf), &cmd_len,
                                 tok_buf, sizeof(tok_buf), &tok_len) != PGR_TRANSPORT_OK) {
      acc_allowed = 0;
      cmd_buf[0] = '\0';
      tok_buf[0] = '\0';
    }
    // Native52 strings are length-prefixed and may contain NUL. This boundary
    // parses C strings: reject embedded NUL rather than accepting a valid prefix.
    if(cmd_len>PGR_ACH_CMD_MAX_BYTES||tok_len>PGR_ACH_TOKEN_MAX_BYTES||
       std::memchr(cmd_buf,0,cmd_len)||std::memchr(tok_buf,0,tok_len)){
      acc_allowed=0;cmd_buf[0]=tok_buf[0]='\0';
    }
  }

  if (tr->sample(&after) != PGR_TRANSPORT_OK || !after.snapshot_fresh || after.fault ||
      before.snapshot_seq != after.snapshot_seq) {
    acc_allowed = 0;
    adv_allowed = 0;
    cmd_buf[0] = '\0';
    tok_buf[0] = '\0';
  }

  PgrAccountHostOptions opts{};
  opts.version = PGR_ACH_OPTIONS_VERSION;
  opts.size = sizeof(opts);
  opts.flags = PGR_ACH_OPT_SET_ACCOUNTS | PGR_ACH_OPT_SET_ADVSYNC;
  opts.accounts_allowed = (acc_allowed == 1) ? 1u : 0u;
  opts.accounts_lease_ms = (acc_allowed == 1) ? PGR_ACH_MAX_ACCOUNTS_LEASE_MS : 0u;
  opts.advsync_allowed = (adv_allowed == 1) ? 1u : 0u;
  opts.advsync_lease_ms = (adv_allowed == 1) ? PGR_ACH_MAX_ADVSYNC_LEASE_MS : 0u;
  opts.command = cmd_buf;
  opts.command_token = tok_buf;
  api_set_options(&opts);
}

int api_stop(void) {
  g.stopping.store(true, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lk(g.policy_mu);
    g.acc_until_ms = 0;
    g.acc_cmd_kind = PGR_ACC_CMD_NONE;
    g.acc_cmd_token = 0;
    g.adv_allowed = false;
  }
  revoke_accounts_leaf();
  revoke_advsync_leaf();
  return PGR_ACH_OK;
}

int api_tick(uint64_t tid) {
  if (!g.configured.load(std::memory_order_acquire)) return PGR_ACH_ERR_NOT_CONFIGURED;
  if (!is_attached_owner(tid)) return PGR_ACH_ERR_OFF_OWNER;
  if (g.tick_busy.exchange(true, std::memory_order_acq_rel)) return PGR_ACH_ERR_BUSY;
  struct BusyGuard {
    ~BusyGuard() { g.tick_busy.store(false, std::memory_order_release); }
  } guard;

  // Query Feature 48 API 3 inspector when available (no duplicate invoke hooks).
  if (const auto *sched3 = scheduler_api3()) {
    if (sched3->executing_generation) {
      (void)sched3->executing_generation(tid);
    }
  }

  uint64_t now_ms = clock_boottime_ms();
  if (!now_ms) {
    api_stop();
    return PGR_ACH_ERR_CLOCK;
  }

  g.ticks.fetch_add(1, std::memory_order_relaxed);
  const uint32_t active_caps = g.caps.load(std::memory_order_acquire);

  if (g.stopping.load(std::memory_order_acquire)) {
    if (active_caps & PGR_ACH_CAP_ADVENTURE_SYNC) {
      if (const auto *adv = advsync_api()) {
        adv->retire(tid);
      }
    }
    if (active_caps & PGR_ACH_CAP_ACCOUNTS) {
      if (const auto *acc = accounts_api()) {
        if (!g.accounts_cleared.load(std::memory_order_acquire)) {
          acc->api_clear();
          pgrazor_account_snapshot_v1_t snap{};
          acc->api_sample(&snap);
          if (snap.active_depth == 0 && snap.active_gc_roots == 0) {
            g.accounts_cleared.store(true, std::memory_order_release);
          }
        }
        int drc = drain_accounts_events();
        if (drc != PGR_ACH_OK) return drc;
      }
    }
    return PGR_ACH_OK;
  }

  poll_transport52();

  if (active_caps & PGR_ACH_CAP_ACCOUNTS) {
    if (const auto *acc = accounts_api()) {
      uint64_t until = 0;
      uint32_t kind = PGR_ACC_CMD_NONE;
      uint64_t tok = 0;
      {
        std::lock_guard<std::mutex> lk(g.policy_mu);
        until = g.acc_until_ms;
        kind = g.acc_cmd_kind;
        tok = g.acc_cmd_token;
      }
      uint32_t rem = (until > now_ms)
                         ? static_cast<uint32_t>(std::min<uint64_t>(PGR_ACH_MAX_ACCOUNTS_LEASE_MS, until - now_ms))
                         : 0u;
      publish_accounts(rem>0,rem,rem?kind:PGR_ACC_CMD_NONE,rem?tok:0);
      acc->api_tick(now_ms,static_cast<int32_t>(tid));
      int drc = drain_accounts_events();
      if (drc != PGR_ACH_OK) return drc;
    }
  }

  if (active_caps & PGR_ACH_CAP_ADVENTURE_SYNC) {
    if (const auto *adv = advsync_api()) {
      // Proven Unity tick retires deferred GC handles without extending any existing lease.
      adv->retire(tid);
    }
  }

  return PGR_ACH_OK;
}

int api_sample(PgrAccountHostSample *out) {
  if (!out) return PGR_ACH_ERR_BAD_CONFIG;
  std::memset(out, 0, sizeof(*out));
  out->version = PGR_ACH_SAMPLE_VERSION;
  out->size = sizeof(PgrAccountHostSample);
  out->configured = g.configured.load(std::memory_order_acquire) ? 1u : 0u;
  out->capabilities = g.caps.load(std::memory_order_acquire);
  out->stopping = g.stopping.load(std::memory_order_acquire) ? 1u : 0u;
  out->accounts_cleared = g.accounts_cleared.load(std::memory_order_acquire) ? 1u : 0u;
  out->accounts_error = g.accounts_error.load(std::memory_order_acquire);
  out->advsync_error = g.advsync_error.load(std::memory_order_acquire);
  out->owner_tid = g.owner_tid.load(std::memory_order_acquire);
  out->ticks = g.ticks.load(std::memory_order_relaxed);

  {
    std::lock_guard<std::mutex> lk(g.policy_mu);
    out->last_cmd_kind = g.acc_cmd_kind;
    out->last_cmd_token = g.acc_cmd_token;
  }

  if (out->capabilities & PGR_ACH_CAP_ACCOUNTS) {
    if (const auto *acc = accounts_api()) {
      pgrazor_account_snapshot_v1_t snap{};
      acc->api_sample(&snap);
      out->active_hook_depth = snap.active_depth;
      out->active_roots = snap.active_gc_roots;
      out->owned = snap.active_depth + snap.active_gc_roots;
    }
  }
  return PGR_ACH_OK;
}

int api_next_event(PgrAccountHostEvent *out) {
  if (!out) return PGR_ACH_ERR_BAD_CONFIG;
  std::lock_guard<std::mutex> lk(g.event_mu);
  if (g.event_count == 0) {
    std::memset(out, 0, sizeof(*out));
    return PGR_ACH_ERR_EMPTY;
  }
  *out = g.events[g.event_head];
  g.event_head = (g.event_head + 1) % PGR_ACH_EVENT_QUEUE_MAX;
  --g.event_count;
  return PGR_ACH_OK;
}

void api_on_unity_pulse(void *, const PgrUnityPulse *pulse) {
  if (!pulse || pulse->reserved != 0 || pulse->tid <= 0) return;
  const auto *sched2 = scheduler_api2();
  if(!sched2||!sched2->valid||sched2->valid(pulse->generation)!=1)return;
  (void)api_tick(static_cast<uint64_t>(pulse->tid));
}

const PgrAccountHostApi kApi = {
    PGR_ACCOUNT_HOST_API_VERSION,
    sizeof(PgrAccountHostApi),
    api_configure,
    api_resolve_metadata,
    api_set_options,
    api_set_adventure_sync_allowed,
    api_tick,
    api_stop,
    api_sample,
    api_next_event,
    api_on_unity_pulse};

}  // namespace

extern "C" const void *pgrazor_account_host_api(uint32_t version) {
  return version == PGR_ACCOUNT_HOST_API_VERSION ? &kApi : nullptr;
}
