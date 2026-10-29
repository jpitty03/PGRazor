#include "routes_host.h"

#include "routes_actions.h"
#include "routes_discovery.h"
#include "routes_gate.h"
#include "routes_metadata.h"
#include "routes_observers.h"
#include "routes_schema.h"
#include "routes_services.h"
#include "routes_tracking.h"
#include "runtime_transport.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

using namespace pgr_routes_schema;

constexpr char kRoutesVersion[] = "routes-native-0.1.0";
constexpr uint64_t kMaxSafeInt = 9007199254740991ULL;

// Disasm-pinned instruction prefixes for leaves 45, 46, 50 (identical to routes.js).
constexpr unsigned char kPrefixRequestNearby[] = {
    0xfe, 0x67, 0xbc, 0xa9, 0xf8, 0x5f, 0x01, 0xa9, 0xf6, 0x57, 0x02, 0xa9,
    0xf4, 0x4f, 0x03, 0xa9, 0xf3, 0x4e, 0x02, 0xb0, 0xf4, 0x03, 0x00, 0xaa};
constexpr unsigned char kPrefixInRangeOfEnd[] = {
    0xeb, 0x2b, 0xbc, 0x6d, 0xe9, 0x23, 0x01, 0x6d, 0xfe, 0x57, 0x02, 0xa9,
    0xf4, 0x4f, 0x03, 0xa9, 0x54, 0x51, 0x02, 0x90, 0xf3, 0x03, 0x00, 0xaa};
constexpr unsigned char kPrefixRewardInit[] = {
    0xff, 0x43, 0x03, 0xd1, 0xea, 0x2b, 0x00, 0xfd, 0xe9, 0x23, 0x06, 0x6d,
    0xfd, 0x7b, 0x07, 0xa9, 0xfc, 0x6f, 0x08, 0xa9, 0xfa, 0x67, 0x09, 0xa9};
constexpr unsigned char kPrefixRatingInit[] = {
    0xff, 0x83, 0x01, 0xd1, 0xfe, 0x6f, 0x01, 0xa9, 0xfa, 0x67, 0x02, 0xa9,
    0xf8, 0x5f, 0x03, 0xa9, 0xf6, 0x57, 0x04, 0xa9, 0xf4, 0x4f, 0x05, 0xa9};
constexpr unsigned char kPrefixCompletionResp[] = {
    0xfe, 0x67, 0xbc, 0xa9, 0xf8, 0x5f, 0x01, 0xa9, 0xf6, 0x57, 0x02, 0xa9,
    0xf4, 0x4f, 0x03, 0xa9, 0xb5, 0x50, 0x02, 0xd0, 0xf4, 0x03, 0x01, 0xaa};
constexpr unsigned char kPrefixTrackingStatus[] = {
    0xff, 0x83, 0x01, 0xd1, 0xfe, 0x6f, 0x01, 0xa9, 0xfa, 0x67, 0x02, 0xa9,
    0xf8, 0x5f, 0x03, 0xa9, 0xf6, 0x57, 0x04, 0xa9, 0xf4, 0x4f, 0x05, 0xa9};
constexpr unsigned char kPrefixTrackingWaypoint[] = {
    0xfe, 0x0f, 0x1d, 0xf8, 0xf6, 0x57, 0x01, 0xa9, 0xf4, 0x4f, 0x02, 0xa9,
    0xd5, 0x4f, 0x02, 0xf0, 0xf4, 0x03, 0x01, 0xaa, 0xf3, 0x03, 0x00, 0xaa};
constexpr unsigned char kRoutesDebugPrologue[] = {
    0x08, 0x10, 0x05, 0x90, 0x08, 0x45, 0x47, 0xf9, 0x48, 0x01, 0x00, 0xb4,
    0x09, 0x10, 0x05, 0x90, 0x29, 0x49, 0x47, 0xf9, 0xe9, 0x00, 0x00, 0xb4};

constexpr uint64_t kRvaRequestNearby    = 175049024ULL;
constexpr uint64_t kRvaInRangeOfEnd     = 174741504ULL;
constexpr uint64_t kRvaRewardInit       = 175272412ULL;
constexpr uint64_t kRvaRatingInit       = 175286128ULL;
constexpr uint64_t kRvaCompletionResp   = 174816912ULL;
constexpr uint64_t kRvaTrackingStatus   = 174921352ULL;
constexpr uint64_t kRvaTrackingWaypoint = 174925564ULL;
constexpr char kLocationIcallName[]     = "UnityEngine.LocationService::IsServiceEnabledByUser()";

const char *const kActionReasons[] = {
    "none",
    "lease-expired",
    "awaiting-service",
    "feature-not-initialized",
    "not-permitted",
    "route-already-active",
    "route-not-discovered",
    "route-stale",
    "route-id-mismatch",
    "not-reversible",
    "alloc-failed",
    "no-current-route",
    "already-completing",
    "already-rewarded",
    "not-eligible",
    "already-paused",
    "stale-seq",
    "native-error"};
constexpr size_t kActionReasonCount = sizeof(kActionReasons) / sizeof(kActionReasons[0]);

// --- clock & tid helpers -----------------------------------------------------------
uint64_t current_tid() { return static_cast<uint64_t>(static_cast<pid_t>(gettid())); }

bool read_boottime_ms(uint64_t *out_ms) {
  struct timespec ts {};
  if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0 || ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L)
    return false;
  *out_ms = static_cast<uint64_t>(ts.tv_sec) * 1000ULL + static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
  return true;
}

// --- UTF-16 / UTF-8 / JSON formatting helpers --------------------------------------
void append_utf8_codepoint(std::string *out, uint32_t cp) {
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

std::string utf16_to_utf8(const uint16_t *chars, uint32_t len) {
  std::string out;
  if (!chars || !len) return out;
  out.reserve(len);
  for (uint32_t i = 0; i < len; ++i) {
    uint32_t w1 = chars[i];
    if (w1 >= 0xD800u && w1 <= 0xDBFFu && i + 1 < len) {
      uint32_t w2 = chars[i + 1];
      if (w2 >= 0xDC00u && w2 <= 0xDFFFu) {
        uint32_t cp = 0x10000u + (((w1 - 0xD800u) << 10) | (w2 - 0xDC00u));
        append_utf8_codepoint(&out, cp);
        ++i;
        continue;
      }
    }
    if (w1 >= 0xD800u && w1 <= 0xDFFFu) {
      append_utf8_codepoint(&out, 0xFFFDu);
    } else {
      append_utf8_codepoint(&out, w1);
    }
  }
  return out;
}

void append_json_escaped_utf8(std::string *out, const char *s, size_t len) {
  out->push_back('"');
  for (size_t i = 0; i < len; ++i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    switch (c) {
      case '"':  out->append("\\\""); break;
      case '\\': out->append("\\\\"); break;
      case '\b': out->append("\\b"); break;
      case '\f': out->append("\\f"); break;
      case '\n': out->append("\\n"); break;
      case '\r': out->append("\\r"); break;
      case '\t': out->append("\\t"); break;
      default:
        if (c < 0x20u) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
          out->append(buf);
        } else {
          out->push_back(static_cast<char>(c));
        }
        break;
    }
  }
  out->push_back('"');
}

void append_json_escaped_str(std::string *out, const std::string &s) {
  append_json_escaped_utf8(out, s.data(), s.size());
}

void append_json_double(std::string *out, double v) {
  if (!std::isfinite(v)) {
    out->append("0");
    return;
  }
  if (v == 0.0) {
    out->append("0");
    return;
  }
  double intpart = 0.0;
  if (std::modf(v, &intpart) == 0.0 && v >= -9007199254740991.0 && v <= 9007199254740991.0) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
    out->append(buf);
    return;
  }
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%.15g", v);
  out->append(buf);
}

// --- strict JSON command parser (100% parity with routes.js parseCommand) ----------
struct ParsedCommand {
  bool ok = false;
  PgrRoutesHostOptReason reason_code = PGR_RH_REASON_NO_COMMAND;
  uint64_t seq = 0;
  uint32_t action = 0; // PgrRoutesAction (1..4)
  bool has_id = false;
  uint32_t id_len = 0; // UTF-16 code units (1..128)
  uint16_t id_u16[PGR_RH_ID_MAX_CHARS]{};
  std::string id_utf8;
  bool has_reversed = false;
  bool reversed = false;
};

const char *opt_reason_str(PgrRoutesHostOptReason r) {
  switch (r) {
    case PGR_RH_REASON_NONE:         return "";
    case PGR_RH_REASON_NOT_ALLOWED:  return "not-allowed";
    case PGR_RH_REASON_NO_COMMAND:   return "no-command";
    case PGR_RH_REASON_BAD_JSON:     return "bad-json";
    case PGR_RH_REASON_BAD_SHAPE:    return "bad-shape";
    case PGR_RH_REASON_BAD_SEQ:      return "bad-seq";
    case PGR_RH_REASON_BAD_ACTION:   return "bad-action";
    case PGR_RH_REASON_BAD_ID:       return "bad-id";
    case PGR_RH_REASON_BAD_REVERSED: return "bad-reversed";
    case PGR_RH_REASON_ID_REQUIRED:  return "id-required";
    case PGR_RH_REASON_STALE_SEQ:    return "stale-seq";
    default:                         return "bad-json";
  }
}

const char *action_code_name(uint32_t act) {
  switch (act) {
    case PGR_RG_DISCOVER: return "discover";
    case PGR_RG_START:    return "start";
    case PGR_RG_COMPLETE: return "complete";
    case PGR_RG_PAUSE:    return "pause";
    default:              return "";
  }
}

struct JsonScanner {
  const char *src;
  size_t len;
  size_t pos = 0;

  void skip_ws() {
    while (pos < len) {
      char c = src[pos];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ++pos;
      else break;
    }
  }

  static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }

  bool parse_string(std::vector<uint16_t> *u16_out) {
    if (pos >= len || src[pos] != '"') return false;
    ++pos;
    if (u16_out) u16_out->clear();
    while (pos < len) {
      unsigned char c = static_cast<unsigned char>(src[pos++]);
      if (c == '"') return true;
      if (c < 0x20u) return false;
      if (c == '\\') {
        if (pos >= len) return false;
        char esc = src[pos++];
        uint16_t unit = 0;
        switch (esc) {
          case '"':  unit = '"'; break;
          case '\\': unit = '\\'; break;
          case '/':  unit = '/'; break;
          case 'b':  unit = '\b'; break;
          case 'f':  unit = '\f'; break;
          case 'n':  unit = '\n'; break;
          case 'r':  unit = '\r'; break;
          case 't':  unit = '\t'; break;
          case 'u': {
            if (pos + 4 > len) return false;
            int h0 = hex_val(src[pos]), h1 = hex_val(src[pos + 1]),
                h2 = hex_val(src[pos + 2]), h3 = hex_val(src[pos + 3]);
            if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) return false;
            unit = static_cast<uint16_t>((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
            pos += 4;
            break;
          }
          default:
            return false;
        }
        if (u16_out) u16_out->push_back(unit);
      } else if (c < 0x80u) {
        if (u16_out) u16_out->push_back(static_cast<uint16_t>(c));
      } else if ((c & 0xE0u) == 0xC0u) {
        if (pos >= len) return false;
        unsigned char c1 = static_cast<unsigned char>(src[pos++]);
        if ((c1 & 0xC0u) != 0x80u) return false;
        uint32_t cp = ((c & 0x1Fu) << 6) | (c1 & 0x3Fu);
        if (cp < 0x80u) return false;
        if (u16_out) u16_out->push_back(static_cast<uint16_t>(cp));
      } else if ((c & 0xF0u) == 0xE0u) {
        if (pos + 2 > len) return false;
        unsigned char c1 = static_cast<unsigned char>(src[pos++]);
        unsigned char c2 = static_cast<unsigned char>(src[pos++]);
        if ((c1 & 0xC0u) != 0x80u || (c2 & 0xC0u) != 0x80u) return false;
        uint32_t cp = ((c & 0x0Fu) << 12) | ((c1 & 0x3Fu) << 6) | (c2 & 0x3Fu);
        if (cp < 0x800u) return false;
        if (u16_out) u16_out->push_back(static_cast<uint16_t>(cp));
      } else if ((c & 0xF8u) == 0xF0u) {
        if (pos + 3 > len) return false;
        unsigned char c1 = static_cast<unsigned char>(src[pos++]);
        unsigned char c2 = static_cast<unsigned char>(src[pos++]);
        unsigned char c3 = static_cast<unsigned char>(src[pos++]);
        if ((c1 & 0xC0u) != 0x80u || (c2 & 0xC0u) != 0x80u || (c3 & 0xC0u) != 0x80u) return false;
        uint32_t cp = ((c & 0x07u) << 18) | ((c1 & 0x3Fu) << 12) | ((c2 & 0x3Fu) << 6) | (c3 & 0x3Fu);
        if (cp < 0x10000u || cp > 0x10FFFFu) return false;
        if (u16_out) {
          cp -= 0x10000u;
          u16_out->push_back(static_cast<uint16_t>(0xD800u + (cp >> 10)));
          u16_out->push_back(static_cast<uint16_t>(0xDC00u + (cp & 0x3FFu)));
        }
      } else {
        return false;
      }
    }
    return false;
  }

  bool parse_number(bool *is_pos_safe_int, uint64_t *int_val) {
    if (is_pos_safe_int) *is_pos_safe_int = false;
    if (int_val) *int_val = 0;
    size_t start = pos;
    bool neg = false;
    if (pos < len && src[pos] == '-') { neg = true; ++pos; }
    if (pos >= len) return false;
    if (src[pos] == '0') {
      ++pos;
      if (pos < len && src[pos] >= '0' && src[pos] <= '9') return false;
    } else if (src[pos] >= '1' && src[pos] <= '9') {
      while (pos < len && src[pos] >= '0' && src[pos] <= '9') ++pos;
    } else {
      return false;
    }
    bool has_frac_or_exp = false;
    if (pos < len && src[pos] == '.') {
      has_frac_or_exp = true;
      ++pos;
      if (pos >= len || src[pos] < '0' || src[pos] > '9') return false;
      while (pos < len && src[pos] >= '0' && src[pos] <= '9') ++pos;
    }
    if (pos < len && (src[pos] == 'e' || src[pos] == 'E')) {
      has_frac_or_exp = true;
      ++pos;
      if (pos < len && (src[pos] == '+' || src[pos] == '-')) ++pos;
      if (pos >= len || src[pos] < '0' || src[pos] > '9') return false;
      while (pos < len && src[pos] >= '0' && src[pos] <= '9') ++pos;
    }
    if (!neg && !has_frac_or_exp) {
      uint64_t v = 0;
      bool overflow = false;
      for (size_t i = start; i < pos; ++i) {
        uint64_t d = static_cast<uint64_t>(src[i] - '0');
        if (v > (kMaxSafeInt - d) / 10ULL) { overflow = true; break; }
        v = v * 10ULL + d;
      }
      if (!overflow && v > 0 && v <= kMaxSafeInt) {
        if (is_pos_safe_int) *is_pos_safe_int = true;
        if (int_val) *int_val = v;
      }
    }
    return true;
  }

  bool skip_value(int depth = 0) {
    if (depth > 64) return false;
    skip_ws();
    if (pos >= len) return false;
    char c = src[pos];
    if (c == '"') return parse_string(nullptr);
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(nullptr, nullptr);
    if (c == 't') {
      if (pos + 4 <= len && std::memcmp(src + pos, "true", 4) == 0) { pos += 4; return true; }
      return false;
    }
    if (c == 'f') {
      if (pos + 5 <= len && std::memcmp(src + pos, "false", 5) == 0) { pos += 5; return true; }
      return false;
    }
    if (c == 'n') {
      if (pos + 4 <= len && std::memcmp(src + pos, "null", 4) == 0) { pos += 4; return true; }
      return false;
    }
    if (c == '[') {
      ++pos;
      skip_ws();
      if (pos < len && src[pos] == ']') { ++pos; return true; }
      while (true) {
        if (!skip_value(depth + 1)) return false;
        skip_ws();
        if (pos >= len) return false;
        if (src[pos] == ']') { ++pos; return true; }
        if (src[pos] != ',') return false;
        ++pos;
      }
    }
    if (c == '{') {
      ++pos;
      skip_ws();
      if (pos < len && src[pos] == '}') { ++pos; return true; }
      while (true) {
        skip_ws();
        if (!parse_string(nullptr)) return false;
        skip_ws();
        if (pos >= len || src[pos] != ':') return false;
        ++pos;
        if (!skip_value(depth + 1)) return false;
        skip_ws();
        if (pos >= len) return false;
        if (src[pos] == '}') { ++pos; return true; }
        if (src[pos] != ',') return false;
        ++pos;
      }
    }
    return false;
  }
};

ParsedCommand parse_command_json(const char *json, uint32_t len) {
  ParsedCommand out{};
  if (!json || len == 0 || len > PGR_RH_COMMAND_MAX_BYTES) {
    out.reason_code = PGR_RH_REASON_NO_COMMAND;
    return out;
  }
  JsonScanner sc{json, len, 0};
  sc.skip_ws();
  if (sc.pos >= sc.len) {
    out.reason_code = PGR_RH_REASON_BAD_JSON;
    return out;
  }
  if (sc.src[sc.pos] != '{') {
    // Validate whether it is valid JSON of a non-object shape (null, number, string, bool; note in JS
    // typeof [] === 'object' so [] falls through to bad-seq, whereas null/primitives return bad-shape).
    bool is_array = (sc.src[sc.pos] == '[');
    if (!sc.skip_value()) {
      out.reason_code = PGR_RH_REASON_BAD_JSON;
      return out;
    }
    sc.skip_ws();
    if (sc.pos != sc.len) {
      out.reason_code = PGR_RH_REASON_BAD_JSON;
      return out;
    }
    out.reason_code = is_array ? PGR_RH_REASON_BAD_SEQ : PGR_RH_REASON_BAD_SHAPE;
    return out;
  }

  ++sc.pos; // consume '{'
  sc.skip_ws();

  bool seq_seen = false, seq_valid = false;
  uint64_t seq_val = 0;
  bool action_seen = false, action_valid = false;
  uint32_t action_val = 0;
  bool id_seen = false, id_valid = false;
  std::vector<uint16_t> id_u16;
  bool rev_seen = false, rev_valid = false;
  bool rev_val = false;

  if (sc.pos < sc.len && sc.src[sc.pos] == '}') {
    ++sc.pos;
  } else {
    while (true) {
      sc.skip_ws();
      std::vector<uint16_t> key_u16;
      if (!sc.parse_string(&key_u16)) {
        out.reason_code = PGR_RH_REASON_BAD_JSON;
        return out;
      }
      std::string key = utf16_to_utf8(key_u16.data(), static_cast<uint32_t>(key_u16.size()));
      sc.skip_ws();
      if (sc.pos >= sc.len || sc.src[sc.pos] != ':') {
        out.reason_code = PGR_RH_REASON_BAD_JSON;
        return out;
      }
      ++sc.pos;
      sc.skip_ws();
      if (sc.pos >= sc.len) {
        out.reason_code = PGR_RH_REASON_BAD_JSON;
        return out;
      }
      if (key == "seq") {
        seq_seen = true;
        char c = sc.src[sc.pos];
        if (c == '-' || (c >= '0' && c <= '9')) {
          bool ok_int = false;
          uint64_t v = 0;
          if (!sc.parse_number(&ok_int, &v)) { out.reason_code = PGR_RH_REASON_BAD_JSON; return out; }
          seq_valid = ok_int;
          seq_val = v;
        } else {
          if (!sc.skip_value()) { out.reason_code = PGR_RH_REASON_BAD_JSON; return out; }
          seq_valid = false;
        }
      } else if (key == "action") {
        action_seen = true;
        if (sc.src[sc.pos] == '"') {
          std::vector<uint16_t> act_u16;
          if (!sc.parse_string(&act_u16)) { out.reason_code = PGR_RH_REASON_BAD_JSON; return out; }
          std::string act = utf16_to_utf8(act_u16.data(), static_cast<uint32_t>(act_u16.size()));
          if (act == "discover") { action_valid = true; action_val = PGR_RG_DISCOVER; }
          else if (act == "start") { action_valid = true; action_val = PGR_RG_START; }
          else if (act == "complete") { action_valid = true; action_val = PGR_RG_COMPLETE; }
          else if (act == "pause") { action_valid = true; action_val = PGR_RG_PAUSE; }
          else { action_valid = false; action_val = 0; }
        } else {
          if (!sc.skip_value()) { out.reason_code = PGR_RH_REASON_BAD_JSON; return out; }
          action_valid = false;
        }
      } else if (key == "id") {
        id_seen = true;
        if (sc.src[sc.pos] == '"') {
          std::vector<uint16_t> val_u16;
          if (!sc.parse_string(&val_u16)) { out.reason_code = PGR_RH_REASON_BAD_JSON; return out; }
          if (!val_u16.empty() && val_u16.size() <= PGR_RH_ID_MAX_CHARS) {
            id_valid = true;
            id_u16 = val_u16;
          } else {
            id_valid = false;
          }
        } else {
          if (!sc.skip_value()) { out.reason_code = PGR_RH_REASON_BAD_JSON; return out; }
          id_valid = false;
        }
      } else if (key == "reversed") {
        rev_seen = true;
        if (sc.pos + 4 <= sc.len && std::memcmp(sc.src + sc.pos, "true", 4) == 0) {
          sc.pos += 4;
          rev_valid = true;
          rev_val = true;
        } else if (sc.pos + 5 <= sc.len && std::memcmp(sc.src + sc.pos, "false", 5) == 0) {
          sc.pos += 5;
          rev_valid = true;
          rev_val = false;
        } else {
          if (!sc.skip_value()) { out.reason_code = PGR_RH_REASON_BAD_JSON; return out; }
          rev_valid = false;
        }
      } else {
        if (!sc.skip_value()) { out.reason_code = PGR_RH_REASON_BAD_JSON; return out; }
      }
      sc.skip_ws();
      if (sc.pos >= sc.len) { out.reason_code = PGR_RH_REASON_BAD_JSON; return out; }
      if (sc.src[sc.pos] == '}') { ++sc.pos; break; }
      if (sc.src[sc.pos] != ',') { out.reason_code = PGR_RH_REASON_BAD_JSON; return out; }
      ++sc.pos;
    }
  }
  sc.skip_ws();
  if (sc.pos != sc.len) {
    out.reason_code = PGR_RH_REASON_BAD_JSON;
    return out;
  }

  if (!seq_seen || !seq_valid || seq_val == 0) {
    out.reason_code = PGR_RH_REASON_BAD_SEQ;
    return out;
  }
  if (!action_seen || !action_valid) {
    out.reason_code = PGR_RH_REASON_BAD_ACTION;
    return out;
  }
  if (id_seen && !id_valid) {
    out.reason_code = PGR_RH_REASON_BAD_ID;
    return out;
  }
  if (rev_seen && !rev_valid) {
    out.reason_code = PGR_RH_REASON_BAD_REVERSED;
    return out;
  }
  if ((action_val == PGR_RG_START || action_val == PGR_RG_COMPLETE) && !id_seen) {
    out.reason_code = PGR_RH_REASON_ID_REQUIRED;
    return out;
  }

  out.ok = true;
  out.reason_code = PGR_RH_REASON_NONE;
  out.seq = seq_val;
  out.action = action_val;
  out.has_id = id_seen;
  if (id_seen) {
    out.id_len = static_cast<uint32_t>(id_u16.size());
    std::memcpy(out.id_u16, id_u16.data(), out.id_len * sizeof(uint16_t));
    out.id_utf8 = utf16_to_utf8(out.id_u16, out.id_len);
  }
  out.has_reversed = rev_seen;
  out.reversed = rev_seen ? rev_val : false;
  return out;
}

// --- schema index lookup helpers ---------------------------------------------------
int find_class_idx(const char *key) {
  for (size_t i = 0; i < kClassCount; ++i)
    if (std::strcmp(kClasses[i].key, key) == 0) return static_cast<int>(i);
  return -1;
}
int find_field_idx(uint32_t owner, const char *name) {
  for (size_t i = 0; i < kFieldCount; ++i)
    if (kFields[i].owner == owner && std::strcmp(kFields[i].name, name) == 0) return static_cast<int>(i);
  return -1;
}
int find_method_idx(const char *key) {
  for (size_t i = 0; i < kMethodCount; ++i)
    if (std::strcmp(kMethods[i].key, key) == 0) return static_cast<int>(i);
  return -1;
}

// --- host state --------------------------------------------------------------------
struct QueuedEvent {
  PgrRoutesHostEvent header{};
  std::string json;
};

std::mutex g_config_mutex;
std::mutex g_options_mutex;
std::mutex g_event_mutex;

bool g_configured = false;
bool g_use_bootstrap_only = false;
PgrRoutesHostConfig g_cfg{};
PgrMetaExports g_cfg_meta_exports{};

std::atomic<bool> g_meta_ready{false};
std::atomic<bool> g_services_ready{false};
std::atomic<bool> g_tracking_ready{false};
std::atomic<bool> g_tick_busy{false};
std::atomic<uint64_t> g_owner_tid{0};
std::atomic<uint64_t> g_ticks_run{0};
std::atomic<int32_t> g_last_error{0};
std::atomic<uint32_t> g_cached_route_count{0};

// Gate 27 + pending command parameters (guarded by g_options_mutex).
uint64_t g_gate_handle = 0;
bool g_desired_allowed = false;
std::atomic<uint64_t> g_lease_until_ms{0};
std::atomic<bool> g_reset_cadence{false};
ParsedCommand g_pending_cmd{};
std::string g_last_transport_cmd;

// Owner-thread orchestration state (touched only under g_tick_busy on owner thread).
uint64_t g_next_meta_ms = 0;
uint64_t g_last_status_ms = 0;
uint64_t g_last_discover_ms = 0;
uint64_t g_routes_handle = 0;
uint64_t g_consumption_handle = 0;
std::string g_last_ready_key;
std::string g_last_current_key;
std::string g_cached_routes_json = "[]";
bool g_leaves_installed = false;

// Reusable heap buffers for discovery/observers/actions on the owner thread.
PgrRoutesDiscoveryOutput g_disc_out{};
PgrRoutesDiscoveryPoints g_points_out{};
PgrRoutesObserversOutput g_obs_out{};

// Event ring (guarded by g_event_mutex).
QueuedEvent g_events[PGR_RH_EVENT_QUEUE_MAX];
uint32_t g_event_head = 0;
uint32_t g_event_count = 0;

// --- leaf API getters --------------------------------------------------------------
const PgrRoutesGateApi *gate_api() {
  return static_cast<const PgrRoutesGateApi *>(pgrazor_routes_gate_api(1));
}
const PgrRoutesActionsApi *actions_api() {
  return static_cast<const PgrRoutesActionsApi *>(pgrazor_routes_actions_api(1));
}
const PgrRoutesDiscoveryApi *discovery_api() {
  return static_cast<const PgrRoutesDiscoveryApi *>(pgrazor_routes_discovery_api(1));
}
const PgrRoutesServicesApi *services_api() {
  return static_cast<const PgrRoutesServicesApi *>(pgrazor_routes_services_api(1));
}
const PgrRoutesObserversApi *observers_api() {
  return static_cast<const PgrRoutesObserversApi *>(pgrazor_routes_observers_api(1));
}
const PgrRoutesTrackingApi *tracking_api() {
  return static_cast<const PgrRoutesTrackingApi *>(pgrazor_routes_tracking_api(1));
}
const PgrRoutesMetadataApi *routes_meta_api() {
  return static_cast<const PgrRoutesMetadataApi *>(pgrazor_routes_metadata_api(1));
}
const PgrMetaApi *runtime_meta_api() {
  return static_cast<const PgrMetaApi *>(pgrazor_runtime_metadata_api(1));
}
const PgrBootApi *bootstrap_api() {
  return static_cast<const PgrBootApi *>(pgrazor_runtime_bootstrap_api(1));
}
const PgrRuntimeTransportApi *transport_api() {
  return static_cast<const PgrRuntimeTransportApi *>(pgrazor_runtime_transport_api(1));
}

uint64_t ensure_gate_locked() {
  if (g_gate_handle != 0) return g_gate_handle;
  const auto *g = gate_api();
  if (!g || g->version != 1 || g->size != sizeof(PgrRoutesGateApi) || !g->open) return 0;
  g_gate_handle = g->open(static_cast<double>(PGR_RH_MAX_LEASE_MS));
  return g_gate_handle;
}

// --- event emission (pull queue + optional Transport 52) ---------------------------
const char *event_kind_tag(uint32_t kind) {
  switch (kind) {
    case PGR_RH_EV_READY:                  return "ready";
    case PGR_RH_EV_METADATA_READY:         return "metadata-ready";
    case PGR_RH_EV_LOCATION_GATE_READY:    return "location-gate-r";
    case PGR_RH_EV_DIAGNOSTIC_UNAVAILABLE: return "diag-unavail";
    case PGR_RH_EV_SERVICE_CAPTURED:       return "svc-captured";
    case PGR_RH_EV_TRACKING_STATUS:        return "tracking-status";
    case PGR_RH_EV_WAYPOINT:               return "waypoint";
    case PGR_RH_EV_REWARD_CONFIRMED:       return "reward-confirm";
    case PGR_RH_EV_REWARD_REJECTED:        return "reward-reject";
    case PGR_RH_EV_RESULT_DISMISSED:       return "result-dismiss";
    case PGR_RH_EV_RESULT_DISMISS_ERROR:   return "dismiss-error";
    case PGR_RH_EV_CURRENT:                return "current";
    case PGR_RH_EV_SNAPSHOT:               return "snapshot";
    case PGR_RH_EV_READ_ERROR:             return "read-error";
    case PGR_RH_EV_ACTION:                 return "action";
    default:                               return "none";
  }
}

const char *event_full_name(uint32_t kind) {
  switch (kind) {
    case PGR_RH_EV_READY:                  return "routes-ready";
    case PGR_RH_EV_METADATA_READY:         return "routes-metadata-ready";
    case PGR_RH_EV_LOCATION_GATE_READY:    return "routes-location-gate-ready";
    case PGR_RH_EV_DIAGNOSTIC_UNAVAILABLE: return "routes-diagnostic-unavailable";
    case PGR_RH_EV_SERVICE_CAPTURED:       return "routes-service-captured";
    case PGR_RH_EV_TRACKING_STATUS:        return "routes-tracking-status";
    case PGR_RH_EV_WAYPOINT:               return "routes-waypoint";
    case PGR_RH_EV_REWARD_CONFIRMED:       return "routes-reward-confirmed";
    case PGR_RH_EV_REWARD_REJECTED:        return "routes-reward-rejected";
    case PGR_RH_EV_RESULT_DISMISSED:       return "routes-result-dismissed";
    case PGR_RH_EV_RESULT_DISMISS_ERROR:   return "routes-result-dismiss-error";
    case PGR_RH_EV_CURRENT:                return "routes-current";
    case PGR_RH_EV_SNAPSHOT:               return "routes-snapshot";
    case PGR_RH_EV_READ_ERROR:             return "routes-read-error";
    case PGR_RH_EV_ACTION:                 return "routes-action";
    default:                               return "routes-unknown";
  }
}

void emit_transport52(uint32_t kind, const std::string &fields_json, int64_t scalar0, const char *ready_reason) {
  const auto *tr = transport_api();
  if (!tr || tr->version != 1 || tr->size != sizeof(PgrRuntimeTransportApi) || !tr->emit) return;

  uint64_t ts = 0;
  read_boottime_ms(&ts);
  std::string wrapped = "{\"feature\":\"quick-close\",\"kind\":\"";
  wrapped.append(event_full_name(kind));
  wrapped.append("\",\"ts\":");
  char ts_buf[32];
  std::snprintf(ts_buf, sizeof(ts_buf), "%llu", static_cast<unsigned long long>(ts));
  wrapped.append(ts_buf);
  if (fields_json.size() > 2 && fields_json.front() == '{' && fields_json.back() == '}') {
    wrapped.push_back(',');
    wrapped.append(fields_json.data() + 1, fields_json.size() - 2);
  }
  wrapped.push_back('}');

  if (kind != PGR_RH_EV_SNAPSHOT) {
    PgrRuntimeTransportString strs[2] = {
        {"quick-close", 11u},
        {wrapped.data(), static_cast<uint32_t>(wrapped.size())}};
    tr->emit(19u, nullptr, 0u, strs, 2u);
  }
  if (kind == PGR_RH_EV_READY) {
    int64_t avail = scalar0 ? 1 : 0;
    const char *r = ready_reason ? ready_reason : "";
    PgrRuntimeTransportString s0{r, static_cast<uint32_t>(std::strlen(r))};
    tr->emit(7u, &avail, 1u, &s0, 1u);
  } else if (kind == PGR_RH_EV_SNAPSHOT) {
    PgrRuntimeTransportString s0{wrapped.data(), static_cast<uint32_t>(wrapped.size())};
    tr->emit(8u, nullptr, 0u, &s0, 1u);
  } else if (kind == PGR_RH_EV_ACTION) {
    PgrRuntimeTransportString s0{wrapped.data(), static_cast<uint32_t>(wrapped.size())};
    tr->emit(9u, nullptr, 0u, &s0, 1u);
  }
}

void push_event(uint32_t kind, const std::string &fields_json, uint64_t seq = 0, int64_t scalar0 = 0,
                double lat = 0.0, double lon = 0.0, const char *ready_reason = nullptr) {
  uint32_t flags = 0;
  {
    std::lock_guard<std::mutex> lk(g_config_mutex);
    flags = g_cfg.flags;
  }
  if (flags & PGR_RH_CFG_EMIT_TRANSPORT52) {
    emit_transport52(kind, fields_json, scalar0, ready_reason);
    if (flags & PGR_RH_CFG_SKIP_QUEUE) return;
  }

  std::lock_guard<std::mutex> lk(g_event_mutex);
  if (g_event_count >= PGR_RH_EVENT_QUEUE_MAX) {
    // Drop oldest if ring is full so fresh snapshots/actions are never blocked by an undrained ring.
    g_event_head = (g_event_head + 1) % PGR_RH_EVENT_QUEUE_MAX;
    --g_event_count;
  }
  uint32_t slot = (g_event_head + g_event_count) % PGR_RH_EVENT_QUEUE_MAX;
  QueuedEvent &ev = g_events[slot];
  std::memset(&ev.header, 0, sizeof(ev.header));
  ev.header.version = PGR_RH_EVENT_VERSION;
  ev.header.size = sizeof(PgrRoutesHostEvent);
  ev.header.kind = kind;
  ev.header.json_len = static_cast<uint32_t>(fields_json.size());
  ev.header.seq = seq;
  ev.header.scalar0 = scalar0;
  ev.header.lat = lat;
  ev.header.lon = lon;
  std::snprintf(ev.header.kind_tag, sizeof(ev.header.kind_tag), "%s", event_kind_tag(kind));
  ev.json = fields_json;
  ++g_event_count;
}

void emit_ready_dedup(bool available, const char *reason) {
  std::string key = available ? "true:" : "false:";
  if (reason && *reason) key.append(reason);
  if (key == g_last_ready_key) return;
  g_last_ready_key = key;

  std::string json = "{\"available\":";
  json.append(available ? "true" : "false");
  json.append(",\"reason\":");
  if (reason && *reason) {
    append_json_escaped_utf8(&json, reason, std::strlen(reason));
  } else {
    json.append("null");
  }
  json.push_back('}');
  push_event(PGR_RH_EV_READY, json, 0, available ? 1 : 0, 0.0, 0.0, reason);
}

void emit_action_event(uint64_t seq, uint32_t action_code, const char *status, const char *reason) {
  std::string json = "{\"seq\":";
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(seq));
  json.append(buf);
  json.append(",\"action\":");
  const char *act_name = action_code_name(action_code);
  append_json_escaped_utf8(&json, act_name, std::strlen(act_name));
  json.append(",\"status\":");
  append_json_escaped_utf8(&json, status, std::strlen(status));
  if (reason && *reason) {
    size_t rlen = std::min<size_t>(std::strlen(reason), 180u);
    json.append(",\"reason\":");
    append_json_escaped_utf8(&json, reason, rlen);
  }
  json.push_back('}');
  push_event(PGR_RH_EV_ACTION, json, seq, std::strcmp(status, "submitted") == 0 ? 1 : 0);
}

void emit_simple_reason_event(uint32_t kind, const char *reason) {
  std::string json = "{\"reason\":";
  const char *r = reason ? reason : "unknown";
  append_json_escaped_utf8(&json, r, std::strlen(r));
  json.push_back('}');
  push_event(kind, json);
}

// --- effective bindings & metadata / leaf installation -----------------------------
struct EffectiveBindings {
  const void *module_base = nullptr;
  uint64_t module_size = 0;
  PgrMetaExports meta{};
  pgr_meta_seg_fn seg_query = nullptr;
  void *seg_ctx = nullptr;
  void *runtime_invoke = nullptr;
  void *object_get_class = nullptr;
  void *field_get_value = nullptr;
  void *field_set_value = nullptr;
  void *field_set_value_object = nullptr;
  void *object_unbox = nullptr;
  void *object_new = nullptr;
  void *gchandle_new = nullptr;
  void *gchandle_get_target = nullptr;
  void *gchandle_free = nullptr;
  void *thread_current = nullptr;
  void *string_length = nullptr;
  void *string_chars = nullptr;
  void *type_get_type = nullptr;
  void *type_is_byref = nullptr;
  void *resolve_icall = nullptr;
  bool fixture_hooks = false;
  void *fixture_request_nearby = nullptr;
  void *fixture_in_range_of_end = nullptr;
  void *fixture_reward_init = nullptr;
  void *fixture_rating_init = nullptr;
  void *fixture_completion_resp = nullptr;
  void *fixture_tracking_status = nullptr;
  void *fixture_tracking_waypoint = nullptr;
  void *fixture_location_icall = nullptr;
  const void *unity_module_base = nullptr;
  uint64_t unity_module_size = 0;
};

bool resolve_effective_bindings(EffectiveBindings *out, std::string *err_reason) {
  PgrRoutesHostConfig local_cfg{};
  PgrMetaExports local_meta{};
  bool configured = false;
  bool boot_only = false;
  {
    std::lock_guard<std::mutex> lk(g_config_mutex);
    configured = g_configured;
    boot_only = g_use_bootstrap_only;
    local_cfg = g_cfg;
    local_meta = g_cfg_meta_exports;
  }
  if (!configured) {
    *err_reason = "not-configured";
    return false;
  }

  bool used_boot = false;
  if (boot_only || (local_cfg.flags & PGR_RH_CFG_PREFER_BOOTSTRAP) || !local_cfg.module_base) {
    const auto *boot = bootstrap_api();
    PgrBootBinding b{};
    if (boot && boot->version == 1 && boot->snapshot && boot->snapshot(&b) == PGR_BOOT_OK) {
      out->module_base = b.module_base;
      out->module_size = b.module_size;
      out->meta = b.meta;
      out->runtime_invoke = b.runtime.runtime_invoke;
      out->object_get_class = b.runtime.object_get_class;
      out->field_get_value = b.runtime.field_get_value;
      out->field_set_value = b.runtime.field_set_value;
      out->object_unbox = b.runtime.object_unbox;
      out->object_new = b.runtime.object_new;
      out->gchandle_new = b.runtime.gchandle_new;
      out->gchandle_get_target = b.runtime.gchandle_get_target;
      out->gchandle_free = b.runtime.gchandle_free;
      out->thread_current = b.runtime.thread_current;
      out->string_chars = b.runtime.string_chars;
      if (b.handle) {
        out->field_set_value_object = dlsym(b.handle, "il2cpp_field_set_value_object");
        out->string_length = dlsym(b.handle, "il2cpp_string_length");
        out->type_get_type = dlsym(b.handle, "il2cpp_type_get_type");
        out->type_is_byref = dlsym(b.handle, "il2cpp_type_is_byref");
        out->resolve_icall = dlsym(b.handle, "il2cpp_resolve_icall");
      }
      used_boot = true;
    } else if (boot_only || !local_cfg.module_base) {
      *err_reason = "bootstrap-not-ready";
      return false;
    }
  }

  if (!used_boot) {
    out->module_base = local_cfg.module_base;
    out->module_size = local_cfg.module_size;
    out->meta = local_meta;
    out->runtime_invoke = local_cfg.runtime_invoke;
    out->object_get_class = local_cfg.object_get_class;
    out->field_get_value = local_cfg.field_get_value;
    out->field_set_value = local_cfg.field_set_value;
    out->field_set_value_object = local_cfg.field_set_value_object;
    out->object_unbox = local_cfg.object_unbox;
    out->object_new = local_cfg.object_new;
    out->gchandle_new = local_cfg.gchandle_new;
    out->gchandle_get_target = local_cfg.gchandle_get_target;
    out->gchandle_free = local_cfg.gchandle_free;
    out->thread_current = local_cfg.thread_current;
    out->string_length = local_cfg.string_length;
    out->string_chars = local_cfg.string_chars;
    out->type_get_type = local_cfg.type_get_type;
    out->type_is_byref = local_cfg.type_is_byref;
    out->resolve_icall = local_cfg.resolve_icall;
  } else {
    // Allow explicit config to fill any non-bootstrap extra slots if dlsym handle was synthetic.
    if (!out->field_set_value_object) out->field_set_value_object = local_cfg.field_set_value_object;
    if (!out->string_length) out->string_length = local_cfg.string_length;
    if (!out->type_get_type) out->type_get_type = local_cfg.type_get_type;
    if (!out->type_is_byref) out->type_is_byref = local_cfg.type_is_byref;
    if (!out->resolve_icall) out->resolve_icall = local_cfg.resolve_icall;
  }

  out->seg_query = local_cfg.seg_query;
  out->seg_ctx = local_cfg.seg_ctx;
  out->fixture_hooks = (local_cfg.seg_query != nullptr) && ((local_cfg.flags & PGR_RH_CFG_FIXTURE_HOOKS) != 0);
  out->fixture_request_nearby = local_cfg.fixture_request_nearby;
  out->fixture_in_range_of_end = local_cfg.fixture_in_range_of_end;
  out->fixture_reward_init = local_cfg.fixture_reward_init;
  out->fixture_rating_init = local_cfg.fixture_rating_init;
  out->fixture_completion_resp = local_cfg.fixture_completion_resp;
  out->fixture_tracking_status = local_cfg.fixture_tracking_status;
  out->fixture_tracking_waypoint = local_cfg.fixture_tracking_waypoint;
  out->fixture_location_icall = local_cfg.fixture_location_icall;
  out->unity_module_base = local_cfg.unity_module_base;
  out->unity_module_size = local_cfg.unity_module_size;

  if (!out->module_base || !out->module_size || !out->runtime_invoke || !out->object_get_class ||
      !out->field_get_value || !out->field_set_value || !out->field_set_value_object ||
      !out->object_unbox || !out->object_new || !out->gchandle_new || !out->gchandle_get_target ||
      !out->gchandle_free || !out->thread_current || !out->string_length || !out->string_chars ||
      !out->type_get_type || !out->type_is_byref) {
    *err_reason = "missing-runtime-export";
    return false;
  }
  return true;
}

bool validate_hook_code(const PgrMetaApi *md, const EffectiveBindings &eb, const void *method_info,
                        uint64_t rva, const unsigned char *prefix, uint32_t prefix_len,
                        const void **out_addr, const char *label, std::string *err_reason) {
  const uintptr_t base = reinterpret_cast<uintptr_t>(eb.module_base);
  if (rva > eb.module_size || prefix_len > eb.module_size - rva || base > UINTPTR_MAX - rva) {
    *err_reason = std::string("code-range:") + label;
    return false;
  }
  const void *addr = reinterpret_cast<const void *>(base + rva);
  if (!method_info) {
    *err_reason = std::string("null-method:") + label;
    return false;
  }
  const void *method_code = nullptr;
  std::memcpy(&method_code, method_info, sizeof(method_code));
  if (method_code != addr) {
    *err_reason = std::string("address-mismatch:") + label;
    return false;
  }
  if (!g_leaves_installed) {
    PgrMetaCodeQuery q{};
    q.version = 1;
    q.size = sizeof(q);
    q.address = addr;
    q.length = prefix_len;
    q.prologue = prefix;
    q.prologue_len = prefix_len;
    if (md->validate_code(&q) != 0) {
      *err_reason = std::string("prefix-mismatch:") + label;
      return false;
    }
  }
  *out_addr = addr;
  return true;
}

bool validate_observer_hook_shape(const EffectiveBindings &eb, const void *method_info, const char *label,
                                  std::string *err_reason) {
  using ParamCountFn = uint32_t (*)(const void *);
  using FlagsFn      = uint32_t (*)(const void *, uint32_t *);
  using BoolFn       = uint8_t (*)(const void *);
  using RetTypeFn    = const void *(*)(const void *);
  using ParamTypeFn  = const void *(*)(const void *, uint32_t);
  using TypeKindFn   = int (*)(const void *);
  using ClassFromFn  = const void *(*)(const void *);

  uint32_t impl = 0;
  if (reinterpret_cast<ParamCountFn>(eb.meta.method_get_param_count)(method_info) != 1u ||
      (reinterpret_cast<FlagsFn>(eb.meta.method_get_flags)(method_info, &impl) & 0x10u) != 0u ||
      reinterpret_cast<BoolFn>(eb.meta.method_is_generic)(method_info) != 0 ||
      reinterpret_cast<BoolFn>(eb.meta.method_is_inflated)(method_info) != 0) {
    *err_reason = std::string("observer-method-shape:") + label;
    return false;
  }
  auto type_kind = reinterpret_cast<TypeKindFn>(eb.type_get_type);
  auto type_byref = reinterpret_cast<BoolFn>(eb.type_is_byref);
  const void *ret = reinterpret_cast<RetTypeFn>(eb.meta.method_get_return_type)(method_info);
  if (!ret || type_kind(ret) != 1) {
    *err_reason = std::string("observer-return-shape:") + label;
    return false;
  }
  const void *arg = reinterpret_cast<ParamTypeFn>(eb.meta.method_get_param)(method_info, 0);
  if (!arg || type_byref(arg) != 0) {
    *err_reason = std::string("observer-arg-shape:") + label;
    return false;
  }
  int kind = type_kind(arg);
  if (kind != 18 && kind != 21 && kind != 28 && kind != 29) {
    *err_reason = std::string("observer-arg-kind:") + label;
    return false;
  }
  const void *aklass = reinterpret_cast<ClassFromFn>(eb.meta.class_from_type)(arg);
  if (!aklass || reinterpret_cast<BoolFn>(eb.meta.class_is_valuetype)(aklass) != 0) {
    *err_reason = std::string("observer-arg-valuetype:") + label;
    return false;
  }
  return true;
}

void install_tracking_diagnostics(const PgrMetaApi *md, const EffectiveBindings &eb,
                                  const PgrRoutesMetadataOutput &meta_out) {
  const auto *trk = tracking_api();
  if (!trk || trk->version != 1 || trk->size != sizeof(PgrRoutesTrackingApi) || !trk->install || !trk->installed) {
    emit_simple_reason_event(PGR_RH_EV_DIAGNOSTIC_UNAVAILABLE, "Error: native-routes-tracking-table");
    return;
  }
  if (trk->installed() == 1) {
    g_tracking_ready.store(true, std::memory_order_release);
    push_event(PGR_RH_EV_LOCATION_GATE_READY, "{\"backend\":\"native\"}");
    return;
  }

  const int c_tracking = find_class_idx("tracking");
  const int c_waypoint = find_class_idx("waypoint");
  const int m_tstatus  = find_method_idx("trackingStatus");
  const int m_twaypt   = find_method_idx("trackingWaypoint");
  const int f_wplat    = find_field_idx(static_cast<uint32_t>(c_waypoint), "LatDegrees");
  const int f_wplng    = find_field_idx(static_cast<uint32_t>(c_waypoint), "LngDegrees");
  if (c_tracking < 0 || c_waypoint < 0 || m_tstatus < 0 || m_twaypt < 0 || f_wplat < 0 || f_wplng < 0) {
    emit_simple_reason_event(PGR_RH_EV_DIAGNOSTIC_UNAVAILABLE, "Error: routes-tracking-waypoint-meta");
    return;
  }

  const void *addr_tracking = nullptr;
  const void *addr_waypoint = nullptr;
  std::string reason;
  if (!validate_hook_code(md, eb, meta_out.methods[m_tstatus], kRvaTrackingStatus, kPrefixTrackingStatus,
                          sizeof(kPrefixTrackingStatus), &addr_tracking, "tracking", &reason) ||
      !validate_hook_code(md, eb, meta_out.methods[m_twaypt], kRvaTrackingWaypoint, kPrefixTrackingWaypoint,
                          sizeof(kPrefixTrackingWaypoint), &addr_waypoint, "waypoint", &reason)) {
    emit_simple_reason_event(PGR_RH_EV_DIAGNOSTIC_UNAVAILABLE, ("Error: " + reason).c_str());
    return;
  }

  // Validate CheckTrackingErrorStatus return (enum int32, size 4, align 4) and bjfz arg0 (RouteWaypointProto).
  using RetTypeFn   = const void *(*)(const void *);
  using ParamTypeFn = const void *(*)(const void *, uint32_t);
  using TypeKindFn  = int (*)(const void *);
  using BoolFn      = uint8_t (*)(const void *);
  using ClassFromFn = const void *(*)(const void *);
  using EnumBaseFn  = const void *(*)(const void *);
  using ValSizeFn   = int32_t (*)(const void *, uint32_t *);
  using OffsetFn    = size_t (*)(const void *);

  auto type_kind  = reinterpret_cast<TypeKindFn>(eb.type_get_type);
  auto type_byref = reinterpret_cast<BoolFn>(eb.type_is_byref);
  auto class_from = reinterpret_cast<ClassFromFn>(eb.meta.class_from_type);

  const void *t_ret = reinterpret_cast<RetTypeFn>(eb.meta.method_get_return_type)(meta_out.methods[m_tstatus]);
  if (!t_ret || type_byref(t_ret) != 0 || type_kind(t_ret) != 17) {
    emit_simple_reason_event(PGR_RH_EV_DIAGNOSTIC_UNAVAILABLE, "Error: routes-tracking-return:tracking");
    return;
  }
  const void *t_klass = class_from(t_ret);
  uint32_t align = 0;
  if (!t_klass || reinterpret_cast<BoolFn>(eb.meta.class_is_valuetype)(t_klass) == 0 ||
      reinterpret_cast<BoolFn>(eb.meta.class_is_enum)(t_klass) == 0) {
    emit_simple_reason_event(PGR_RH_EV_DIAGNOSTIC_UNAVAILABLE, "Error: routes-tracking-return:tracking");
    return;
  }
  const void *t_base = reinterpret_cast<EnumBaseFn>(eb.meta.class_enum_basetype)(t_klass);
  if (!t_base || type_byref(t_base) != 0 || type_kind(t_base) != 8 ||
      reinterpret_cast<ValSizeFn>(eb.meta.class_value_size)(t_klass, &align) != 4 || align != 4u) {
    emit_simple_reason_event(PGR_RH_EV_DIAGNOSTIC_UNAVAILABLE, "Error: routes-tracking-return:tracking");
    return;
  }

  const void *w_ret = reinterpret_cast<RetTypeFn>(eb.meta.method_get_return_type)(meta_out.methods[m_twaypt]);
  const void *w_arg = reinterpret_cast<ParamTypeFn>(eb.meta.method_get_param)(meta_out.methods[m_twaypt], 0);
  if (!w_ret || type_byref(w_ret) != 0 || type_kind(w_ret) != 1 ||
      !w_arg || type_byref(w_arg) != 0 || type_kind(w_arg) != 18 ||
      class_from(w_arg) != meta_out.classes[c_waypoint]) {
    emit_simple_reason_event(PGR_RH_EV_DIAGNOSTIC_UNAVAILABLE, "Error: routes-tracking-argument:waypoint");
    return;
  }

  if (!eb.resolve_icall) {
    emit_simple_reason_event(PGR_RH_EV_DIAGNOSTIC_UNAVAILABLE, "Error: routes-tracking-location-resolver");
    return;
  }
  if (!eb.seg_query) {
    PgrMetaCodeQuery rq{};
    rq.version = 1;
    rq.size = sizeof(rq);
    rq.address = eb.resolve_icall;
    rq.length = 4;
    if (md->validate_code(&rq) != 0) {
      emit_simple_reason_event(PGR_RH_EV_DIAGNOSTIC_UNAVAILABLE, "Error: routes-tracking-resolver-exec");
      return;
    }
  }
  using ResolveIcallFn = void *(*)(const char *);
  void *addr_loc = reinterpret_cast<ResolveIcallFn>(eb.resolve_icall)(kLocationIcallName);
  if (!addr_loc) {
    emit_simple_reason_event(PGR_RH_EV_DIAGNOSTIC_UNAVAILABLE, "Error: routes-tracking-location-unresolved");
    return;
  }

  // Verify location icall resides in libunity.so (either via resolve_module in production or unity_module_base in fixture).
  const void *unity_base = eb.unity_module_base;
  uint64_t unity_size = eb.unity_module_size;
  if (!unity_base && !eb.seg_query) {
    if (md->resolve_module("libunity.so", &unity_base, &unity_size) != 0) {
      emit_simple_reason_event(PGR_RH_EV_DIAGNOSTIC_UNAVAILABLE, "Error: routes-tracking-location-owner");
      return;
    }
  }
  if (unity_base && unity_size > 0) {
    const uintptr_t ubase = reinterpret_cast<uintptr_t>(unity_base);
    const uintptr_t laddr = reinterpret_cast<uintptr_t>(addr_loc);
    if (laddr < ubase || laddr - ubase > unity_size || 4u > unity_size - (laddr - ubase)) {
      emit_simple_reason_event(PGR_RH_EV_DIAGNOSTIC_UNAVAILABLE, "Error: routes-tracking-location-owner");
      return;
    }
  }

  const size_t lat_off = reinterpret_cast<OffsetFn>(eb.meta.field_get_offset)(meta_out.fields[f_wplat]);
  const size_t lng_off = reinterpret_cast<OffsetFn>(eb.meta.field_get_offset)(meta_out.fields[f_wplng]);
  if (lat_off != PGR_RT_WAYPOINT_LAT_OFFSET || lng_off != PGR_RT_WAYPOINT_LNG_OFFSET) {
    emit_simple_reason_event(PGR_RH_EV_DIAGNOSTIC_UNAVAILABLE, "Error: routes-tracking-waypoint-offset");
    return;
  }

  PgrRoutesTrackingConfig tcfg{};
  tcfg.version = PGR_RT_CONFIG_VERSION;
  tcfg.size = sizeof(tcfg);
  tcfg.object_get_class = eb.object_get_class;
  tcfg.class_waypoint = const_cast<void *>(meta_out.classes[c_waypoint]);
  tcfg.addr_tracking = eb.fixture_hooks && eb.fixture_tracking_status ? eb.fixture_tracking_status : const_cast<void *>(addr_tracking);
  tcfg.addr_waypoint = eb.fixture_hooks && eb.fixture_tracking_waypoint ? eb.fixture_tracking_waypoint : const_cast<void *>(addr_waypoint);
  tcfg.addr_location_icall = eb.fixture_hooks && eb.fixture_location_icall ? eb.fixture_location_icall : addr_loc;
  tcfg.lat_offset = static_cast<uint32_t>(lat_off);
  tcfg.lng_offset = static_cast<uint32_t>(lng_off);

  int rc = trk->install(&tcfg);
  if (rc != 0 || trk->installed() != 1) {
    emit_simple_reason_event(PGR_RH_EV_DIAGNOSTIC_UNAVAILABLE, "Error: native-routes-tracking-unavailable");
    return;
  }
  g_tracking_ready.store(true, std::memory_order_release);
  push_event(PGR_RH_EV_LOCATION_GATE_READY, "{\"backend\":\"native\"}");
}

bool init_metadata_and_leaves(uint64_t tid, std::string *err_reason) {
  EffectiveBindings eb{};
  if (!resolve_effective_bindings(&eb, err_reason)) return false;

  const auto *rmd = routes_meta_api();
  const auto *md = runtime_meta_api();
  const auto *act = actions_api();
  const auto *dsc = discovery_api();
  const auto *svc = services_api();
  const auto *obs = observers_api();
  if (!rmd || rmd->version != 1 || rmd->size != sizeof(PgrRoutesMetadataApi) || !rmd->resolve ||
      !md || md->version != 1 || md->size != sizeof(PgrMetaApi) ||
      !act || act->version != 1 || act->size != sizeof(PgrRoutesActionsApi) || !act->install ||
      !dsc || dsc->version != 1 || dsc->size != sizeof(PgrRoutesDiscoveryApi) || !dsc->install ||
      !svc || svc->version != 1 || svc->size != sizeof(PgrRoutesServicesApi) || !svc->install ||
      !obs || obs->version != 1 || obs->size != sizeof(PgrRoutesObserversApi) || !obs->install) {
    *err_reason = "native-leaf-table-missing";
    return false;
  }

  PgrRoutesMetadataConfig mcfg{};
  mcfg.version = 1;
  mcfg.size = sizeof(mcfg);
  mcfg.binding.version = 1;
  mcfg.binding.size = sizeof(mcfg.binding);
  mcfg.binding.exports = &eb.meta;
  mcfg.binding.module_base = eb.module_base;
  mcfg.binding.module_size = eb.module_size;
  mcfg.binding.seg_query = eb.seg_query;
  mcfg.binding.seg_ctx = eb.seg_ctx;
  if (!eb.seg_query) {
    mcfg.binding.debug_prologue = kRoutesDebugPrologue;
    mcfg.binding.debug_prologue_len = sizeof(kRoutesDebugPrologue);
  }
  mcfg.thread_current = eb.thread_current;
  mcfg.owner_tid = tid;

  PgrRoutesMetadataOutput meta_out{};
  int rc = rmd->resolve(&mcfg, &meta_out);
  if (rc != 0) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "resolve:%d", rc);
    *err_reason = buf;
    g_last_error.store(rc, std::memory_order_release);
    return false;
  }

  // Derive collection / primitive classes from validated metadata 53 output.
  using FieldTypeFn  = const void *(*)(const void *);
  using MethodRetFn  = const void *(*)(const void *);
  using ClassFromFn  = void *(*)(const void *);
  using TypeKindFn   = int (*)(const void *);
  using GetMethodFn  = const void *(*)(const void *, const char *, int);
  using MethodClsFn  = const void *(*)(const void *);

  auto field_type = reinterpret_cast<FieldTypeFn>(eb.meta.field_get_type);
  auto method_ret = reinterpret_cast<MethodRetFn>(eb.meta.method_get_return_type);
  auto class_from = reinterpret_cast<ClassFromFn>(eb.meta.class_from_type);
  auto type_kind  = reinterpret_cast<TypeKindFn>(eb.type_get_type);

  const int c_consumption = find_class_idx("consumption");
  const int c_routes      = find_class_idx("routes");
  const int c_nearby      = find_class_idx("nearby");
  const int c_startData   = find_class_idx("startData");
  const int c_shared      = find_class_idx("shared");
  const int c_anchor      = find_class_idx("anchor");
  const int c_waypoint    = find_class_idx("waypoint");
  const int c_play        = find_class_idx("play");
  const int c_progress    = find_class_idx("progress");
  const int c_rewardGui   = find_class_idx("rewardGui");
  const int c_ratingGui   = find_class_idx("ratingGui");
  const int c_playerStats = find_class_idx("playerStats");
  const int c_bool        = find_class_idx("bool");

  const int f_shared_id   = find_field_idx(static_cast<uint32_t>(c_shared), "Id");
  const int f_nearby_list = find_field_idx(static_cast<uint32_t>(c_nearby), "<Routes>k__BackingField");
  const int f_shared_wps  = find_field_idx(static_cast<uint32_t>(c_shared), "Waypoints");

  void *class_string = class_from(field_type(meta_out.fields[f_shared_id]));
  void *class_int32  = class_from(method_ret(meta_out.list_count));
  void *class_list   = class_from(field_type(meta_out.fields[f_nearby_list]));
  void *class_wps    = class_from(field_type(meta_out.fields[f_shared_wps]));
  if (!class_string || !class_int32 || !class_list || !class_wps) {
    *err_reason = "derived-class-null";
    return false;
  }

  // 1. Build and install Feature 43 (routes_actions).
  PgrRoutesActionsConfig acfg{};
  acfg.version = PGR_RA_CONFIG_VERSION;
  acfg.size = sizeof(acfg);
  acfg.runtime_invoke = eb.runtime_invoke;
  acfg.object_get_class = eb.object_get_class;
  acfg.field_get_value = eb.field_get_value;
  acfg.object_unbox = eb.object_unbox;
  acfg.gchandle_new = eb.gchandle_new;
  acfg.gchandle_get_target = eb.gchandle_get_target;
  acfg.gchandle_free = eb.gchandle_free;
  acfg.thread_current = eb.thread_current;
  acfg.object_new = eb.object_new;
  acfg.field_set_value_object = eb.field_set_value_object;
  acfg.field_set_value = eb.field_set_value;
  acfg.class_consumption = const_cast<void *>(meta_out.classes[c_consumption]);
  acfg.class_routes = const_cast<void *>(meta_out.classes[c_routes]);
  acfg.class_play = const_cast<void *>(meta_out.classes[c_play]);
  acfg.class_shared = const_cast<void *>(meta_out.classes[c_shared]);
  acfg.class_startData = const_cast<void *>(meta_out.classes[c_startData]);
  acfg.class_bool = const_cast<void *>(meta_out.classes[c_bool]);
  acfg.class_string = class_string;
  acfg.m_currentRoute = const_cast<void *>(meta_out.methods[find_method_idx("currentRoute")]);
  acfg.m_hasStarted = const_cast<void *>(meta_out.methods[find_method_idx("hasStarted")]);
  acfg.m_inRangeOfEnd = const_cast<void *>(meta_out.methods[find_method_idx("inRangeOfEnd")]);
  acfg.m_inRangeToResume = const_cast<void *>(meta_out.methods[find_method_idx("inRangeToResume")]);
  acfg.m_isCompleting = const_cast<void *>(meta_out.methods[find_method_idx("isCompleting")]);
  acfg.m_flagInitialized = const_cast<void *>(meta_out.methods[find_method_idx("flagInitialized")]);
  acfg.m_enabledForPlayer = const_cast<void *>(meta_out.methods[find_method_idx("enabledForPlayer")]);
  acfg.m_isConsuming = const_cast<void *>(meta_out.methods[find_method_idx("isConsuming")]);
  acfg.m_isPaused = const_cast<void *>(meta_out.methods[find_method_idx("isPaused")]);
  acfg.m_start = const_cast<void *>(meta_out.methods[find_method_idx("start")]);
  acfg.m_confirmComplete = const_cast<void *>(meta_out.methods[find_method_idx("confirmComplete")]);
  acfg.m_pause = const_cast<void *>(meta_out.methods[find_method_idx("pause")]);
  acfg.m_requestNearby = const_cast<void *>(meta_out.methods[find_method_idx("requestNearby")]);
  acfg.m_ctor = const_cast<void *>(meta_out.constructor);
  acfg.f_playRoute = const_cast<void *>(meta_out.fields[find_field_idx(static_cast<uint32_t>(c_play), "Route")]);
  acfg.f_playPaused = const_cast<void *>(meta_out.fields[find_field_idx(static_cast<uint32_t>(c_play), "Paused")]);
  acfg.f_playCompletedWalk = const_cast<void *>(meta_out.fields[find_field_idx(static_cast<uint32_t>(c_play), "CompletedWalk")]);
  acfg.f_playAcquiredReward = const_cast<void *>(meta_out.fields[find_field_idx(static_cast<uint32_t>(c_play), "AcquiredReward")]);
  acfg.f_playTravelInReverse = const_cast<void *>(meta_out.fields[find_field_idx(static_cast<uint32_t>(c_play), "TravelInReverse")]);
  acfg.f_sharedId = const_cast<void *>(meta_out.fields[f_shared_id]);
  acfg.f_sharedReversible = const_cast<void *>(meta_out.fields[find_field_idx(static_cast<uint32_t>(c_shared), "Reversible")]);
  acfg.f_dataPublishedRoute = const_cast<void *>(meta_out.fields[find_field_idx(static_cast<uint32_t>(c_startData), "PublishedRouteProto")]);
  acfg.f_dataReversed = const_cast<void *>(meta_out.fields[find_field_idx(static_cast<uint32_t>(c_startData), "Reversed")]);
  acfg.string_length = eb.string_length;
  acfg.string_chars = eb.string_chars;

  rc = act->install(&acfg);
  if (rc != 0) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "actions-install:%d", rc);
    *err_reason = buf;
    g_last_error.store(rc, std::memory_order_release);
    return false;
  }

  // 2. Build and install Feature 44 (routes_discovery).
  PgrRoutesDiscoveryConfig dcfg{};
  dcfg.version = PGR_RD_CONFIG_VERSION;
  dcfg.size = sizeof(dcfg);
  dcfg.runtime_invoke = eb.runtime_invoke;
  dcfg.object_get_class = eb.object_get_class;
  dcfg.field_get_value = eb.field_get_value;
  dcfg.object_unbox = eb.object_unbox;
  dcfg.gchandle_new = eb.gchandle_new;
  dcfg.gchandle_get_target = eb.gchandle_get_target;
  dcfg.gchandle_free = eb.gchandle_free;
  dcfg.thread_current = eb.thread_current;
  dcfg.string_length = eb.string_length;
  dcfg.string_chars = eb.string_chars;
  dcfg.class_routes = const_cast<void *>(meta_out.classes[c_routes]);
  dcfg.class_nearby = const_cast<void *>(meta_out.classes[c_nearby]);
  dcfg.class_shared = const_cast<void *>(meta_out.classes[c_shared]);
  dcfg.class_anchor = const_cast<void *>(meta_out.classes[c_anchor]);
  dcfg.class_waypoint = const_cast<void *>(meta_out.classes[c_waypoint]);
  dcfg.class_playerStats = const_cast<void *>(meta_out.classes[c_playerStats]);
  dcfg.class_string = class_string;
  dcfg.class_int32 = class_int32;
  dcfg.m_listCount = const_cast<void *>(meta_out.list_count);
  dcfg.m_listItem = const_cast<void *>(meta_out.list_item);
  dcfg.m_wpCount = const_cast<void *>(meta_out.waypoint_count);
  dcfg.m_wpItem = const_cast<void *>(meta_out.waypoint_item);
  dcfg.f_routes_didn = const_cast<void *>(meta_out.fields[find_field_idx(static_cast<uint32_t>(c_routes), "didn")]);
  dcfg.f_nearby_routes = const_cast<void *>(meta_out.fields[f_nearby_list]);
  dcfg.f_shared_id = const_cast<void *>(meta_out.fields[f_shared_id]);
  dcfg.f_shared_name = const_cast<void *>(meta_out.fields[find_field_idx(static_cast<uint32_t>(c_shared), "Name")]);
  dcfg.f_shared_reversible = const_cast<void *>(meta_out.fields[find_field_idx(static_cast<uint32_t>(c_shared), "Reversible")]);
  dcfg.f_shared_distance = const_cast<void *>(meta_out.fields[find_field_idx(static_cast<uint32_t>(c_shared), "RouteDistanceMeters")]);
  dcfg.f_shared_stats = const_cast<void *>(meta_out.fields[find_field_idx(static_cast<uint32_t>(c_shared), "PlayerStats")]);
  dcfg.f_shared_waypoints = const_cast<void *>(meta_out.fields[f_shared_wps]);
  dcfg.f_shared_startPoi = const_cast<void *>(meta_out.fields[find_field_idx(static_cast<uint32_t>(c_shared), "StartPoi")]);
  dcfg.f_shared_endPoi = const_cast<void *>(meta_out.fields[find_field_idx(static_cast<uint32_t>(c_shared), "EndPoi")]);
  dcfg.f_anchor_anchor = const_cast<void *>(meta_out.fields[find_field_idx(static_cast<uint32_t>(c_anchor), "Anchor")]);
  dcfg.f_wp_lat = const_cast<void *>(meta_out.fields[find_field_idx(static_cast<uint32_t>(c_waypoint), "LatDegrees")]);
  dcfg.f_wp_lon = const_cast<void *>(meta_out.fields[find_field_idx(static_cast<uint32_t>(c_waypoint), "LngDegrees")]);
  dcfg.f_stats_cooldown = const_cast<void *>(meta_out.fields[find_field_idx(static_cast<uint32_t>(c_playerStats), "CooldownFinishMs")]);
  dcfg.class_list = class_list;
  dcfg.class_waypoints = class_wps;

  rc = dsc->install(&dcfg);
  if (rc != 0) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "discovery-install:%d", rc);
    *err_reason = buf;
    g_last_error.store(rc, std::memory_order_release);
    return false;
  }

  // 3. Validate and install Feature 45 (routes_services).
  const void *m_req_nearby = meta_out.methods[find_method_idx("requestNearby")];
  const void *m_in_range   = meta_out.methods[find_method_idx("inRangeOfEnd")];
  const void *addr_req = nullptr;
  const void *addr_ir  = nullptr;
  if (!validate_hook_code(md, eb, m_req_nearby, kRvaRequestNearby, kPrefixRequestNearby,
                          sizeof(kPrefixRequestNearby), &addr_req, "request-nearby", err_reason) ||
      !validate_hook_code(md, eb, m_in_range, kRvaInRangeOfEnd, kPrefixInRangeOfEnd,
                          sizeof(kPrefixInRangeOfEnd), &addr_ir, "in-range-of-end", err_reason)) {
    g_last_error.store(-33, std::memory_order_release);
    return false;
  }
  if (type_kind(method_ret(m_req_nearby)) != 21 || type_kind(method_ret(m_in_range)) != 2) {
    *err_reason = "services-return-type";
    g_last_error.store(-33, std::memory_order_release);
    return false;
  }

  PgrRoutesServicesConfig scfg{};
  scfg.version = PGR_RS_CONFIG_VERSION;
  scfg.size = sizeof(scfg);
  scfg.object_get_class = eb.object_get_class;
  scfg.field_get_value = eb.field_get_value;
  scfg.gchandle_new = eb.gchandle_new;
  scfg.gchandle_get_target = eb.gchandle_get_target;
  scfg.gchandle_free = eb.gchandle_free;
  scfg.thread_current = eb.thread_current;
  scfg.class_routes = const_cast<void *>(meta_out.classes[c_routes]);
  scfg.class_consumption = const_cast<void *>(meta_out.classes[c_consumption]);
  scfg.f_routes_dica = const_cast<void *>(meta_out.fields[find_field_idx(static_cast<uint32_t>(c_routes), "dica")]);
  scfg.addr_request_nearby = eb.fixture_hooks && eb.fixture_request_nearby ? eb.fixture_request_nearby : const_cast<void *>(addr_req);
  scfg.addr_in_range_of_end = eb.fixture_hooks && eb.fixture_in_range_of_end ? eb.fixture_in_range_of_end : const_cast<void *>(addr_ir);

  rc = svc->install(&scfg);
  if (rc != 0) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "services-install:%d", rc);
    *err_reason = buf;
    g_last_error.store(rc, std::memory_order_release);
    return false;
  }

  // 4. Validate and install Feature 46 (routes_observers).
  auto get_method = reinterpret_cast<GetMethodFn>(eb.meta.class_get_method_from_name);
  auto method_cls = reinterpret_cast<MethodClsFn>(eb.meta.method_get_class);
  const void *m_reward_init = get_method(meta_out.classes[c_rewardGui], "Initialize", 1);
  const void *m_rating_init = get_method(meta_out.classes[c_ratingGui], "Initialize", 1);
  const void *m_comp_resp   = meta_out.methods[find_method_idx("completionResponse")];
  if (!m_reward_init || method_cls(m_reward_init) != meta_out.classes[c_rewardGui] ||
      !m_rating_init || method_cls(m_rating_init) != meta_out.classes[c_ratingGui]) {
    *err_reason = "observers-init-method";
    g_last_error.store(-33, std::memory_order_release);
    return false;
  }
  const void *addr_reward = nullptr;
  const void *addr_rating = nullptr;
  const void *addr_bjan   = nullptr;
  if (!validate_hook_code(md, eb, m_reward_init, kRvaRewardInit, kPrefixRewardInit,
                          sizeof(kPrefixRewardInit), &addr_reward, "reward-init", err_reason) ||
      !validate_hook_code(md, eb, m_rating_init, kRvaRatingInit, kPrefixRatingInit,
                          sizeof(kPrefixRatingInit), &addr_rating, "rating-init", err_reason) ||
      !validate_hook_code(md, eb, m_comp_resp, kRvaCompletionResp, kPrefixCompletionResp,
                          sizeof(kPrefixCompletionResp), &addr_bjan, "completion-response", err_reason) ||
      !validate_observer_hook_shape(eb, m_reward_init, "reward-init", err_reason) ||
      !validate_observer_hook_shape(eb, m_rating_init, "rating-init", err_reason) ||
      !validate_observer_hook_shape(eb, m_comp_resp, "completion-response", err_reason)) {
    g_last_error.store(-33, std::memory_order_release);
    return false;
  }

  PgrRoutesObserversConfig ocfg{};
  ocfg.version = PGR_RO_CONFIG_VERSION;
  ocfg.size = sizeof(ocfg);
  ocfg.runtime_invoke = eb.runtime_invoke;
  ocfg.object_get_class = eb.object_get_class;
  ocfg.field_get_value = eb.field_get_value;
  ocfg.gchandle_new = eb.gchandle_new;
  ocfg.gchandle_get_target = eb.gchandle_get_target;
  ocfg.gchandle_free = eb.gchandle_free;
  ocfg.thread_current = eb.thread_current;
  ocfg.string_length = eb.string_length;
  ocfg.string_chars = eb.string_chars;
  ocfg.class_consumption = const_cast<void *>(meta_out.classes[c_consumption]);
  ocfg.class_progress = const_cast<void *>(meta_out.classes[c_progress]);
  ocfg.class_play = const_cast<void *>(meta_out.classes[c_play]);
  ocfg.class_shared = const_cast<void *>(meta_out.classes[c_shared]);
  ocfg.class_reward_gui = const_cast<void *>(meta_out.classes[c_rewardGui]);
  ocfg.class_rating_gui = const_cast<void *>(meta_out.classes[c_ratingGui]);
  ocfg.m_current_route = const_cast<void *>(meta_out.methods[find_method_idx("currentRoute")]);
  ocfg.m_dismiss_gui = const_cast<void *>(meta_out.methods[find_method_idx("dismissGui")]);
  ocfg.f_play_route = const_cast<void *>(meta_out.fields[find_field_idx(static_cast<uint32_t>(c_play), "Route")]);
  ocfg.f_shared_id = const_cast<void *>(meta_out.fields[f_shared_id]);
  ocfg.addr_reward_init = eb.fixture_hooks && eb.fixture_reward_init ? eb.fixture_reward_init : const_cast<void *>(addr_reward);
  ocfg.addr_rating_init = eb.fixture_hooks && eb.fixture_rating_init ? eb.fixture_rating_init : const_cast<void *>(addr_rating);
  ocfg.addr_completion_resp = eb.fixture_hooks && eb.fixture_completion_resp ? eb.fixture_completion_resp : const_cast<void *>(addr_bjan);

  rc = obs->install(&ocfg);
  if (rc != 0) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "observers-install:%d", rc);
    *err_reason = buf;
    g_last_error.store(rc, std::memory_order_release);
    return false;
  }

  g_leaves_installed = true;

  // Apply current lease state to newly installed leaves.
  uint64_t now_ms = 0;
  read_boottime_ms(&now_ms);
  if (g_lease_until_ms.load(std::memory_order_acquire) <= now_ms) {
    act->revoke();
    dsc->revoke();
    obs->revoke();
  }

  // 5. Install Feature 50 (routes_tracking). Contained just like routes.js try { installTrackingDiagnostics() }.
  install_tracking_diagnostics(md, eb, meta_out);
  return true;
}

// --- snapshot & current state helpers ----------------------------------------------
uint64_t current_residual_ms() {
  uint64_t now = 0;
  if (!read_boottime_ms(&now)) return 0;
  uint64_t until = g_lease_until_ms.load(std::memory_order_acquire);
  if (until <= now) return 0;
  uint64_t rem = until - now;
  return rem > PGR_RH_MAX_LEASE_MS ? PGR_RH_MAX_LEASE_MS : rem;
}

const char *map_action_reason(int32_t status, int32_t reason) {
  if (status < 0) {
    if (reason == PGR_RA_REASON_ALLOC_FAILED) return "alloc-failed";
    return "native-error";
  }
  if (reason >= 0 && static_cast<size_t>(reason) < kActionReasonCount) {
    return kActionReasons[reason];
  }
  return "native-error";
}

bool read_current_json(uint64_t tid, std::string *out_current_json, std::string *err_reason) {
  const auto *act = actions_api();
  uint64_t res = current_residual_ms();
  if (!res) { *err_reason = "Error: lease-expired"; return false; }
  uint64_t gen = act->generation();

  PgrRoutesActionsRequest req{};
  req.version = PGR_RA_REQUEST_VERSION;
  req.size = sizeof(req);
  req.tid = tid;
  req.residual_ms = res;
  req.generation = gen;
  req.service_handle = g_consumption_handle;
  req.action = PGR_RA_ACT_CURRENT;

  PgrRoutesActionsOutput out{};
  int status = act->execute(&req, &out);
  if (status != PGR_RA_SUCCESS || out.status != PGR_RA_SUCCESS) {
    *err_reason = std::string("Error: ") + map_action_reason(status, out.reason);
    return false;
  }
  if (act->generation() != gen || current_residual_ms() == 0) {
    *err_reason = "Error: lease-expired";
    return false;
  }

  if (!(out.flags & PGR_RA_FLAG_PRESENT)) {
    *out_current_json = "null";
    return true;
  }
  if (out.id_length < 1 || out.id_length > PGR_RA_ID_MAX) {
    *err_reason = "Error: native-routes-actions-current-id";
    return false;
  }
  std::string id_utf8 = utf16_to_utf8(out.id, out.id_length);
  std::string j = "{\"id\":";
  append_json_escaped_str(&j, id_utf8);
  j.append(",\"started\":");
  j.append((out.flags & PGR_RA_FLAG_STARTED) ? "true" : "false");
  j.append(",\"paused\":");
  j.append((out.flags & PGR_RA_FLAG_PAUSED) ? "true" : "false");
  j.append(",\"completedWalk\":");
  j.append((out.flags & PGR_RA_FLAG_COMPLETED_WALK) ? "true" : "false");
  j.append(",\"acquiredReward\":");
  j.append((out.flags & PGR_RA_FLAG_ACQUIRED_REWARD) ? "true" : "false");
  j.append(",\"inRangeOfEnd\":");
  j.append((out.flags & PGR_RA_FLAG_IN_RANGE_END) ? "true" : "false");
  j.append(",\"inRangeToResume\":");
  j.append((out.flags & PGR_RA_FLAG_IN_RANGE_RESUME) ? "true" : "false");
  j.append(",\"isCompleting\":");
  j.append((out.flags & PGR_RA_FLAG_IS_COMPLETING) ? "true" : "false");
  j.append(",\"reversed\":");
  j.append((out.flags & PGR_RA_FLAG_REVERSED) ? "true" : "false");
  j.push_back('}');
  *out_current_json = j;
  return true;
}

bool valid_coord(double lat, double lon) {
  return std::isfinite(lat) && std::isfinite(lon) && std::fabs(lat) <= 90.0 && std::fabs(lon) <= 180.0;
}

bool read_discovery_json(uint64_t tid, std::string *out_routes_json, uint32_t *out_count) {
  const auto *dsc = discovery_api();
  uint64_t res = current_residual_ms();
  if (!res || !g_routes_handle) return false;
  uint64_t gen = dsc->generation();

  PgrRoutesDiscoveryRequest req{};
  req.version = PGR_RD_REQUEST_VERSION;
  req.size = sizeof(req);
  req.tid = tid;
  req.residual_ms = res;
  req.generation = gen;
  req.service_handle = g_routes_handle;
  req.index = 0;

  int status = dsc->discover(&req, &g_disc_out);
  if (status != PGR_RD_OK || g_disc_out.status != PGR_RD_OK || g_disc_out.count > PGR_RD_MAX_ROUTES) {
    return false;
  }

  std::string arr = "[";
  size_t budget = 0;
  uint32_t admitted = 0;

  for (uint32_t i = 0; i < g_disc_out.count; ++i) {
    if (admitted >= PGR_RH_ROUTES_MAX) break;
    uint64_t left = current_residual_ms();
    if (!left || dsc->generation() != gen) return false;

    const PgrRoutesDiscoveryRoute &r = g_disc_out.routes[i];
    if ((r.flags & ~7u) != 0 || r.id_length < 1 || r.id_length > PGR_RD_ID_MAX ||
        r.name_length > PGR_RD_NAME_MAX || r.point_count > PGR_RD_MAX_POINTS) {
      return false;
    }

    PgrRoutesDiscoveryRequest preq{};
    preq.version = PGR_RD_REQUEST_VERSION;
    preq.size = sizeof(preq);
    preq.tid = tid;
    preq.residual_ms = left;
    preq.generation = gen;
    preq.service_handle = 0;
    preq.index = i;

    int pstatus = dsc->points(&preq, i, &g_points_out);
    if (pstatus != PGR_RD_OK || g_points_out.status != PGR_RD_OK || g_points_out.count != r.point_count) {
      return false;
    }

    // boundSnapshot filter: points must be 2..5000 and all valid finite coords.
    if (r.point_count < 2 || r.point_count > PGR_RH_POINTS_MAX) continue;
    bool all_pts_valid = true;
    for (uint32_t p = 0; p < r.point_count; ++p) {
      if (!valid_coord(g_points_out.points[p].lat, g_points_out.points[p].lon)) {
        all_pts_valid = false;
        break;
      }
    }
    if (!all_pts_valid) continue;

    std::string id_str = utf16_to_utf8(r.id, r.id_length);
    uint32_t name_units = std::min<uint32_t>(r.name_length, PGR_RH_NAME_MAX_CHARS);
    std::string name_str = utf16_to_utf8(r.name, name_units);

    std::string item = "{\"id\":";
    append_json_escaped_str(&item, id_str);
    item.append(",\"name\":");
    append_json_escaped_str(&item, name_str);
    item.append(",\"reversible\":");
    item.append((r.flags & PGR_RD_FLAG_REVERSIBLE) ? "true" : "false");
    item.append(",\"distanceMeters\":");
    append_json_double(&item, std::isfinite(r.distance_meters) ? r.distance_meters : 0.0);
    item.append(",\"cooldownFinishMs\":");
    append_json_double(&item, std::isfinite(r.cooldown_finish_ms) ? r.cooldown_finish_ms : 0.0);
    item.append(",\"points\":[");
    for (uint32_t p = 0; p < r.point_count; ++p) {
      if (p > 0) item.push_back(',');
      item.push_back('[');
      append_json_double(&item, g_points_out.points[p].lat);
      item.push_back(',');
      append_json_double(&item, g_points_out.points[p].lon);
      item.push_back(']');
    }
    item.push_back(']');
    if ((r.flags & PGR_RD_FLAG_HAS_START) && valid_coord(r.start_lat, r.start_lon)) {
      item.append(",\"start\":[");
      append_json_double(&item, r.start_lat);
      item.push_back(',');
      append_json_double(&item, r.start_lon);
      item.push_back(']');
    }
    if ((r.flags & PGR_RD_FLAG_HAS_END) && valid_coord(r.end_lat, r.end_lon)) {
      item.append(",\"end\":[");
      append_json_double(&item, r.end_lat);
      item.push_back(',');
      append_json_double(&item, r.end_lon);
      item.push_back(']');
    }
    item.push_back('}');

    if (budget + item.size() > PGR_RH_SNAPSHOT_BUDGET) break;
    budget += item.size();
    if (admitted > 0) arr.push_back(',');
    arr.append(item);
    ++admitted;
  }

  if (dsc->generation() != gen || current_residual_ms() == 0) return false;
  arr.push_back(']');
  *out_routes_json = arr;
  *out_count = admitted;
  return true;
}

void execute_command(uint64_t tid, const ParsedCommand &cmd) {
  const auto *act = actions_api();
  const auto *dsc = discovery_api();
  const auto *obs = observers_api();

  uint64_t res = current_residual_ms();
  if (!res) {
    emit_action_event(cmd.seq, cmd.action, "error", "lease-expired");
    return;
  }

  if (cmd.action == PGR_RG_DISCOVER) {
    if (!g_routes_handle) {
      emit_action_event(cmd.seq, cmd.action, "error", "awaiting-service");
      return;
    }
    uint64_t gen = act->generation();
    PgrRoutesActionsRequest req{};
    req.version = PGR_RA_REQUEST_VERSION;
    req.size = sizeof(req);
    req.tid = tid;
    req.residual_ms = res;
    req.generation = gen;
    req.service_handle = g_routes_handle;
    req.seq = cmd.seq;
    req.action = PGR_RA_ACT_DISCOVER;
    PgrRoutesActionsOutput out{};
    int status = act->execute(&req, &out);
    if (status == PGR_RA_SUCCESS && out.status == PGR_RA_SUCCESS) {
      emit_action_event(cmd.seq, cmd.action, "submitted", nullptr);
    } else {
      emit_action_event(cmd.seq, cmd.action, "error", map_action_reason(status, out.reason));
    }
    return;
  }

  if (cmd.action == PGR_RG_START) {
    if (!g_consumption_handle) {
      emit_action_event(cmd.seq, cmd.action, "error", "awaiting-service");
      return;
    }
    if (!g_routes_handle) {
      emit_action_event(cmd.seq, cmd.action, "error", "route-not-discovered");
      return;
    }
    PgrRoutesDiscoveryLookup lk{};
    lk.version = PGR_RD_LOOKUP_VERSION;
    lk.size = sizeof(lk);
    lk.tid = tid;
    lk.residual_ms = res;
    lk.generation = dsc->generation();
    lk.service_handle = g_routes_handle;
    lk.id_length = cmd.id_len;
    std::memcpy(lk.id, cmd.id_u16, cmd.id_len * sizeof(uint16_t));
    uint64_t route_handle = 0;
    int lrc = dsc->lookup(&lk, &route_handle);
    if (lrc != PGR_RD_OK || route_handle == 0 || current_residual_ms() == 0) {
      emit_action_event(cmd.seq, cmd.action, "error", "route-not-discovered");
      return;
    }

    uint64_t left = current_residual_ms();
    if (!left) {
      emit_action_event(cmd.seq, cmd.action, "error", "lease-expired");
      return;
    }
    PgrRoutesActionsRequest req{};
    req.version = PGR_RA_REQUEST_VERSION;
    req.size = sizeof(req);
    req.tid = tid;
    req.residual_ms = left;
    req.generation = act->generation();
    req.service_handle = g_consumption_handle;
    req.route_handle = route_handle;
    req.seq = cmd.seq;
    req.action = PGR_RA_ACT_START;
    req.reversed = cmd.reversed ? 1u : 0u;
    req.id_length = cmd.id_len;
    std::memcpy(req.id, cmd.id_u16, cmd.id_len * sizeof(uint16_t));
    PgrRoutesActionsOutput out{};
    int status = act->execute(&req, &out);
    if (status == PGR_RA_SUCCESS && out.status == PGR_RA_SUCCESS) {
      emit_action_event(cmd.seq, cmd.action, "submitted", nullptr);
    } else {
      emit_action_event(cmd.seq, cmd.action, "error", map_action_reason(status, out.reason));
    }
    return;
  }

  if (cmd.action == PGR_RG_COMPLETE) {
    if (!g_consumption_handle) {
      emit_action_event(cmd.seq, cmd.action, "error", "awaiting-service");
      return;
    }
    // Latch completing in Feature 46 BEFORE the invoke so synchronous bjan correlates it.
    PgrRoutesObserversCompleting comp{};
    comp.version = PGR_RO_COMPLETING_VERSION;
    comp.size = sizeof(comp);
    comp.tid = tid;
    comp.residual_ms = res;
    comp.generation = obs->generation();
    comp.consumption_handle = g_consumption_handle;
    comp.seq = cmd.seq;
    comp.id_length = cmd.id_len;
    std::memcpy(comp.id, cmd.id_u16, cmd.id_len * sizeof(uint16_t));
    obs->set_completing(&comp);

    uint64_t left = current_residual_ms();
    PgrRoutesActionsRequest req{};
    req.version = PGR_RA_REQUEST_VERSION;
    req.size = sizeof(req);
    req.tid = tid;
    req.residual_ms = left;
    req.generation = act->generation();
    req.service_handle = g_consumption_handle;
    req.seq = cmd.seq;
    req.action = PGR_RA_ACT_COMPLETE;
    req.id_length = cmd.id_len;
    std::memcpy(req.id, cmd.id_u16, cmd.id_len * sizeof(uint16_t));
    PgrRoutesActionsOutput out{};
    int status = act->execute(&req, &out);
    if (status == PGR_RA_SUCCESS && out.status == PGR_RA_SUCCESS) {
      emit_action_event(cmd.seq, cmd.action, "submitted", nullptr);
    } else {
      PgrRoutesObserversRequest clr{};
      clr.version = PGR_RO_REQUEST_VERSION;
      clr.size = sizeof(clr);
      clr.tid = tid;
      clr.residual_ms = current_residual_ms();
      clr.generation = obs->generation();
      obs->clear_completing(&clr);
      emit_action_event(cmd.seq, cmd.action, "error", map_action_reason(status, out.reason));
    }
    return;
  }

  if (cmd.action == PGR_RG_PAUSE) {
    if (!g_consumption_handle) {
      emit_action_event(cmd.seq, cmd.action, "error", "awaiting-service");
      return;
    }
    PgrRoutesActionsRequest req{};
    req.version = PGR_RA_REQUEST_VERSION;
    req.size = sizeof(req);
    req.tid = tid;
    req.residual_ms = res;
    req.generation = act->generation();
    req.service_handle = g_consumption_handle;
    req.seq = cmd.seq;
    req.action = PGR_RA_ACT_PAUSE;
    PgrRoutesActionsOutput out{};
    int status = act->execute(&req, &out);
    if (status == PGR_RA_SUCCESS && out.status == PGR_RA_SUCCESS) {
      emit_action_event(cmd.seq, cmd.action, "submitted", nullptr);
    } else {
      emit_action_event(cmd.seq, cmd.action, "error", map_action_reason(status, out.reason));
    }
    return;
  }
}

// --- API implementations -----------------------------------------------------------
int api_configure(const PgrRoutesHostConfig *config) {
  if (config != nullptr) {
    if (config->version != PGR_RH_CONFIG_VERSION) return PGR_RH_ERR_BAD_VERSION;
    if (config->size != sizeof(PgrRoutesHostConfig) || config->reserved0 != 0) return PGR_RH_ERR_BAD_SIZE;
    if ((config->flags & PGR_RH_CFG_FIXTURE_HOOKS) && config->seg_query == nullptr) return PGR_RH_ERR_MISSING_PTR;
  }

  std::lock_guard<std::mutex> lk(g_config_mutex);
  if (g_configured) {
    if (config == nullptr && g_use_bootstrap_only) return PGR_RH_OK;
    if (config != nullptr && !g_use_bootstrap_only) {
      PgrRoutesHostConfig cmp_a = g_cfg;
      PgrRoutesHostConfig cmp_b = *config;
      cmp_a.meta_exports = nullptr;
      cmp_b.meta_exports = nullptr;
      bool meta_same = (config->meta_exports == nullptr && g_cfg.meta_exports == nullptr) ||
                       (config->meta_exports != nullptr && g_cfg.meta_exports != nullptr &&
                        std::memcmp(config->meta_exports, &g_cfg_meta_exports, sizeof(PgrMetaExports)) == 0);
      if (std::memcmp(&cmp_a, &cmp_b, sizeof(PgrRoutesHostConfig)) == 0 && meta_same) return PGR_RH_OK;
    }
    return PGR_RH_ERR_RECONFIG;
  }

  if (config == nullptr) {
    std::memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.version = PGR_RH_CONFIG_VERSION;
    g_cfg.size = sizeof(PgrRoutesHostConfig);
    g_cfg.flags = PGR_RH_CFG_PREFER_BOOTSTRAP;
    std::memset(&g_cfg_meta_exports, 0, sizeof(g_cfg_meta_exports));
    g_use_bootstrap_only = true;
  } else {
    g_cfg = *config;
    if (config->meta_exports) {
      g_cfg_meta_exports = *config->meta_exports;
      g_cfg.meta_exports = &g_cfg_meta_exports;
    } else {
      std::memset(&g_cfg_meta_exports, 0, sizeof(g_cfg_meta_exports));
    }
    g_use_bootstrap_only = false;
  }
  g_configured = true;

  std::lock_guard<std::mutex> glk(g_options_mutex);
  ensure_gate_locked();
  return PGR_RH_OK;
}

int api_set_options(int allowed, const char *command_json, uint32_t command_len, PgrRoutesHostOptionsOut *out) {
  PgrRoutesHostOptionsOut local_out{};
  local_out.version = PGR_RH_OPTIONS_OUT_VERSION;
  local_out.size = sizeof(PgrRoutesHostOptionsOut);

  if (command_len > 0 && command_json == nullptr) return PGR_RH_ERR_NULL_ARG;

  uint64_t now_ms = 0;
  if (!read_boottime_ms(&now_ms)) return PGR_RH_ERR_CLOCK;

  const bool allow_bool = (allowed == 1);
  ParsedCommand parsed = parse_command_json(command_json, command_len);
  int gate_parsed = parsed.ok ? 1 : (parsed.reason_code == PGR_RH_REASON_NO_COMMAND ? 0 : -1);

  int gate_rc = PGR_RG_NOT_ALLOWED;
  {
    std::lock_guard<std::mutex> lk(g_options_mutex);
    uint64_t gh = ensure_gate_locked();
    if (!gh) return PGR_RH_ERR_BAD_TABLE;
    if (allow_bool && !g_desired_allowed) {
      g_reset_cadence.store(true, std::memory_order_release);
    }
    g_desired_allowed = allow_bool;
    g_lease_until_ms.store(allow_bool ? (now_ms + PGR_RH_MAX_LEASE_MS) : 0ULL, std::memory_order_release);

    const auto *g = gate_api();
    gate_rc = g->set(gh, allow_bool ? 1 : 0, gate_parsed, parsed.seq, parsed.action, static_cast<double>(now_ms));
    if (gate_rc == PGR_RG_ACCEPTED) {
      g_pending_cmd = parsed;
    }
  }

  if (!allow_bool) {
    if (g_meta_ready.load(std::memory_order_acquire)) {
      if (const auto *act = actions_api()) act->revoke();
      if (const auto *dsc = discovery_api()) dsc->revoke();
      if (const auto *obs = observers_api()) obs->revoke();
    }
    local_out.status_code = PGR_RH_OPT_OFF;
    local_out.reason_code = PGR_RH_REASON_NOT_ALLOWED;
    std::snprintf(local_out.reason, sizeof(local_out.reason), "%s", opt_reason_str(PGR_RH_REASON_NOT_ALLOWED));
    if (out) *out = local_out;
    return PGR_RH_OK;
  }

  if (gate_rc == PGR_RG_ACCEPTED) {
    local_out.status_code = PGR_RH_OPT_ACCEPTED;
    local_out.reason_code = PGR_RH_REASON_NONE;
    local_out.action_code = parsed.action;
    local_out.seq = parsed.seq;
    local_out.reason[0] = '\0';
  } else {
    local_out.status_code = PGR_RH_OPT_REJECTED;
    PgrRoutesHostOptReason rcode = parsed.ok
        ? (gate_rc == PGR_RG_STALE_SEQ ? PGR_RH_REASON_STALE_SEQ : PGR_RH_REASON_NOT_ALLOWED)
        : parsed.reason_code;
    local_out.reason_code = rcode;
    std::snprintf(local_out.reason, sizeof(local_out.reason), "%s", opt_reason_str(rcode));
  }
  if (out) *out = local_out;
  return PGR_RH_OK;
}

void poll_transport52_options() {
  uint32_t flags = 0;
  {
    std::lock_guard<std::mutex> lk(g_config_mutex);
    if (!g_configured) return;
    flags = g_cfg.flags;
  }
  if (!(flags & PGR_RH_CFG_POLL_TRANSPORT52)) return;
  const auto *tr = transport_api();
  if (!tr || tr->version != 1 || tr->size != sizeof(PgrRuntimeTransportApi) || !tr->get_bool || !tr->get_string)
    return;

  // In Feature 52: bool[2] is gameRoutesAllowed (guarded bool: returns 0 when stale); string[1] is gameRoutesCommand.
  int allowed = 0;
  if (tr->get_bool(2u, &allowed) != PGR_TRANSPORT_OK) allowed = 0;
  char cmd_buf[PGR_RH_COMMAND_MAX_BYTES + 1]{};
  uint32_t cmd_len = 0;
  if (allowed == 1) {
    int src = tr->get_string(1u, cmd_buf, PGR_RH_COMMAND_MAX_BYTES, &cmd_len);
    if (src != PGR_TRANSPORT_OK) cmd_len = 0;
  }
  api_set_options(allowed == 1 ? 1 : 0, cmd_buf, cmd_len, nullptr);
}

int api_tick(uint64_t tid) {
  if (tid == 0 || tid != current_tid()) return PGR_RH_ERR_OFF_OWNER;
  uint64_t expected_owner = 0;
  g_owner_tid.compare_exchange_strong(expected_owner, tid, std::memory_order_acq_rel);
  if (g_owner_tid.load(std::memory_order_acquire) != tid) return PGR_RH_ERR_OFF_OWNER;

  {
    std::lock_guard<std::mutex> lk(g_config_mutex);
    if (!g_configured) return PGR_RH_ERR_NOT_CONFIGURED;
  }

  if (g_tick_busy.exchange(true, std::memory_order_acq_rel)) return PGR_RH_ERR_BUSY;
  struct BusyGuard {
    ~BusyGuard() { g_tick_busy.store(false, std::memory_order_release); }
  } guard;

  poll_transport52_options();

  uint64_t now_ms = 0;
  if (!read_boottime_ms(&now_ms)) return PGR_RH_ERR_CLOCK;

  if (g_reset_cadence.exchange(false, std::memory_order_acq_rel)) {
    g_last_status_ms = 0;
    g_last_discover_ms = 0;
  }

  g_ticks_run.fetch_add(1, std::memory_order_relaxed);

  // 1. Metadata 53 + leaf installation (throttled to 2000 ms on cold-start failure).
  if (!g_meta_ready.load(std::memory_order_acquire)) {
    if (g_next_meta_ms != 0 && now_ms < g_next_meta_ms) return PGR_RH_OK;
    g_next_meta_ms = now_ms + PGR_RH_META_RETRY_MS;
    std::string err;
    if (!init_metadata_and_leaves(tid, &err)) {
      std::string full_reason = "metadata:Error: " + err;
      emit_ready_dedup(false, full_reason.c_str());
      return PGR_RH_OK;
    }
    g_meta_ready.store(true, std::memory_order_release);
    std::string meta_json = "{\"version\":\"";
    meta_json.append(kRoutesVersion);
    meta_json.append("\",\"actionsBackend\":\"native\",\"discoveryBackend\":\"native\",\"captureBackend\":\"native\",\"observersBackend\":\"native\",\"leaseMs\":2500,\"statusMs\":1000}");
    push_event(PGR_RH_EV_METADATA_READY, meta_json);
  }

  const auto *svc = services_api();
  const auto *dsc = discovery_api();
  const auto *obs = observers_api();
  const auto *g   = gate_api();

  // 2. Service capture heartbeat & dica derivation (Feature 45).
  {
    PgrRoutesServicesRequest sreq{};
    sreq.version = PGR_RS_REQUEST_VERSION;
    sreq.size = sizeof(sreq);
    sreq.tid = tid;
    sreq.residual_ms = PGR_RH_MAX_LEASE_MS;
    sreq.generation = svc->generation();
    PgrRoutesServicesOutput sout{};
    int src = svc->poll(&sreq, &sout);
    if (src == PGR_RS_OK && sout.status == PGR_RS_OK && sout.generation == sreq.generation) {
      g_routes_handle = (sout.flags & PGR_RS_FLAG_ROUTES_PRESENT) ? sout.routes_handle : 0;
      g_consumption_handle = (sout.flags & PGR_RS_FLAG_CONSUMPTION_PRESENT) ? sout.consumption_handle : 0;
      if ((sout.flags & PGR_RS_FLAG_ROUTES_REPLACED) && g_routes_handle != 0) {
        push_event(PGR_RH_EV_SERVICE_CAPTURED, "{\"service\":\"routes\"}");
      }
      if ((sout.flags & PGR_RS_FLAG_CONSUMPTION_REPLACED) && g_consumption_handle != 0) {
        if (sout.flags & PGR_RS_FLAG_CONSUMPTION_FROM_DICA) {
          push_event(PGR_RH_EV_SERVICE_CAPTURED, "{\"service\":\"consumption\",\"source\":\"routes-dependency\"}");
        } else {
          push_event(PGR_RH_EV_SERVICE_CAPTURED, "{\"service\":\"consumption\"}");
        }
      }
    } else {
      g_routes_handle = 0;
      g_consumption_handle = 0;
    }
  }

  const bool have_services = (g_routes_handle != 0 && g_consumption_handle != 0);
  g_services_ready.store(have_services, std::memory_order_release);
  emit_ready_dedup(have_services, have_services ? nullptr : "awaiting-service");

  // 3. Lease check, discovery drain when off-lease, and observers poll (Feature 46).
  uint64_t gh = 0;
  {
    std::lock_guard<std::mutex> lk(g_options_mutex);
    gh = g_gate_handle;
  }
  uint64_t poll_now = 0;
  read_boottime_ms(&poll_now);
  const bool lease_on = gh != 0 && g->active(gh, static_cast<double>(poll_now)) == 1 && current_residual_ms() > 0;
  if (!lease_on) {
    PgrRoutesDiscoveryRequest dreq{};
    dreq.version = PGR_RD_REQUEST_VERSION;
    dreq.size = sizeof(dreq);
    dreq.tid = tid;
    dreq.residual_ms = 0;
    dreq.generation = dsc->generation();
    dsc->drain(&dreq);
  }

  std::string completion_json = "null";
  {
    PgrRoutesObserversRequest oreq{};
    oreq.version = PGR_RO_REQUEST_VERSION;
    oreq.size = sizeof(oreq);
    oreq.tid = tid;
    oreq.residual_ms = current_residual_ms();
    oreq.generation = obs->generation();
    int orc = obs->poll(&oreq, &g_obs_out);
    if (orc == PGR_RO_OK && g_obs_out.status == PGR_RO_OK && g_obs_out.generation == oreq.generation) {
      const uint32_t ev_cnt = std::min<uint32_t>(g_obs_out.event_count, PGR_RO_MAX_EVENTS);
      for (uint32_t i = 0; i < ev_cnt; ++i) {
        const PgrRoutesObserversEvent &ev = g_obs_out.events[i];
        if (ev.type == PGR_RO_EV_TRACKING_STATUS) {
          char buf[64];
          std::snprintf(buf, sizeof(buf), "{\"code\":%lld}", static_cast<long long>(ev.i0));
          push_event(PGR_RH_EV_TRACKING_STATUS, buf, 0, ev.i0);
        } else if (ev.type == PGR_RO_EV_WAYPOINT) {
          std::string j = "{\"lat\":";
          append_json_double(&j, ev.f0);
          j.append(",\"lon\":");
          append_json_double(&j, ev.f1);
          j.push_back('}');
          push_event(PGR_RH_EV_WAYPOINT, j, 0, 0, ev.f0, ev.f1);
        } else if (ev.type == PGR_RO_EV_REWARD_CONFIRMED) {
          std::string id_str = utf16_to_utf8(ev.id, std::min<uint32_t>(ev.id_length, PGR_RO_ID_MAX));
          std::string j = "{\"id\":";
          append_json_escaped_str(&j, id_str);
          char sbuf[32];
          std::snprintf(sbuf, sizeof(sbuf), ",\"seq\":%lld}", static_cast<long long>(ev.i0));
          j.append(sbuf);
          push_event(PGR_RH_EV_REWARD_CONFIRMED, j, static_cast<uint64_t>(ev.i0), ev.i0);
        } else if (ev.type == PGR_RO_EV_REWARD_REJECTED) {
          std::string id_str = utf16_to_utf8(ev.id, std::min<uint32_t>(ev.id_length, PGR_RO_ID_MAX));
          std::string j = "{\"id\":";
          append_json_escaped_str(&j, id_str);
          char sbuf[32];
          std::snprintf(sbuf, sizeof(sbuf), ",\"status\":%lld}", static_cast<long long>(ev.i0));
          j.append(sbuf);
          push_event(PGR_RH_EV_REWARD_REJECTED, j, 0, ev.i0);
        } else if (ev.type == PGR_RO_EV_RESULT_DISMISSED) {
          const char *scr = ev.aux == 1 ? "Niantic.Holoholo.Routes.RouteRatingGuiController"
                                        : "Niantic.Holoholo.Routes.RouteCompletionGuiController";
          std::string j = "{\"screen\":\"";
          j.append(scr);
          j.append("\"}");
          push_event(PGR_RH_EV_RESULT_DISMISSED, j, 0, static_cast<int64_t>(ev.aux));
        } else if (ev.type == PGR_RO_EV_RESULT_DISMISS_ERR) {
          const char *scr = ev.aux == 1 ? "Niantic.Holoholo.Routes.RouteRatingGuiController"
                                        : "Niantic.Holoholo.Routes.RouteCompletionGuiController";
          std::string j = "{\"reason\":\"native-dismiss-error\",\"screen\":\"";
          j.append(scr);
          j.append("\"}");
          push_event(PGR_RH_EV_RESULT_DISMISS_ERROR, j, 0, static_cast<int64_t>(ev.aux));
        }
      }
      if ((g_obs_out.flags & PGR_RO_FLAG_COMPLETION_READY) && g_obs_out.completion_id_length >= 1 &&
          g_obs_out.completion_id_length <= PGR_RO_ID_MAX) {
        std::string cid = utf16_to_utf8(g_obs_out.completion_id, g_obs_out.completion_id_length);
        completion_json = "{\"id\":";
        append_json_escaped_str(&completion_json, cid);
        char sbuf[32];
        std::snprintf(sbuf, sizeof(sbuf), ",\"seq\":%llu}", static_cast<unsigned long long>(g_obs_out.completion_seq));
        completion_json.append(sbuf);
      }
    }
  }

  // 4. Authoritative current state + discovery snapshot under active lease.
  if (lease_on && have_services && (g_last_status_ms == 0 || poll_now - g_last_status_ms >= PGR_RH_STATUS_CADENCE_MS)) {
    g_last_status_ms = poll_now;
    std::string cur_json;
    std::string read_err;
    if (!read_current_json(tid, &cur_json, &read_err)) {
      emit_simple_reason_event(PGR_RH_EV_READ_ERROR, read_err.c_str());
    } else {
      std::string cur_wrapped = "{\"current\":" + cur_json + "}";
      if (cur_wrapped != g_last_current_key) {
        g_last_current_key = cur_wrapped;
        push_event(PGR_RH_EV_CURRENT, cur_wrapped, 0, cur_json != "null" ? 1 : 0);
      }
      bool disc_ok = true;
      if (g_routes_handle != 0 && (g_last_discover_ms == 0 || poll_now - g_last_discover_ms >= PGR_RH_DISCOVER_CADENCE_MS)) {
        g_last_discover_ms = poll_now;
        std::string routes_json;
        uint32_t route_cnt = 0;
        if (!read_discovery_json(tid, &routes_json, &route_cnt)) {
          disc_ok = false;
          emit_simple_reason_event(PGR_RH_EV_READ_ERROR, "Error: native-routes-discovery-read");
        } else {
          g_cached_routes_json = routes_json;
          g_cached_route_count.store(route_cnt, std::memory_order_release);
        }
      }
      if (disc_ok) {
        std::string snap_json = "{\"routes\":";
        snap_json.append(g_cached_routes_json);
        snap_json.append(",\"current\":");
        snap_json.append(cur_json);
        snap_json.append(",\"completion\":");
        snap_json.append(completion_json);
        snap_json.push_back('}');
        push_event(PGR_RH_EV_SNAPSHOT, snap_json);
      }
    }
  }

  // 5. One-shot command consumption from Feature 27 gate.
  if (gh != 0) {
    uint64_t take_now = 0;
    read_boottime_ms(&take_now);
    PgrRoutesGateResult taken{};
    int trc = 0;
    ParsedCommand cmd_copy{};
    {
      std::lock_guard<std::mutex> lk(g_options_mutex);
      trc = g->take(gh, static_cast<double>(take_now), &taken);
      if (trc == 1) cmd_copy = g_pending_cmd;
    }
    if (trc == 1) {
      const bool take_lease_on = g->active(gh, static_cast<double>(take_now)) == 1 && current_residual_ms() > 0;
      if (taken.expired || !take_lease_on || cmd_copy.seq != taken.seq || cmd_copy.action != taken.action) {
        emit_action_event(taken.seq, taken.action, "error", "lease-expired");
      } else {
        execute_command(tid, cmd_copy);
      }
    }
  }

  return PGR_RH_OK;
}

int api_next_event(PgrRoutesHostEvent *out, char *json_buf, uint32_t json_cap) {
  if (!out) return PGR_RH_ERR_NULL_ARG;
  std::lock_guard<std::mutex> lk(g_event_mutex);
  if (g_event_count == 0) {
    std::memset(out, 0, sizeof(*out));
    out->version = PGR_RH_EVENT_VERSION;
    out->size = sizeof(*out);
    return 0;
  }
  const QueuedEvent &ev = g_events[g_event_head];
  *out = ev.header;
  if (json_buf != nullptr) {
    if (json_cap < ev.header.json_len) {
      return PGR_RH_ERR_BUFFER_TOO_SMALL;
    }
    if (ev.header.json_len > 0) {
      std::memcpy(json_buf, ev.json.data(), ev.header.json_len);
    }
    if (json_cap > ev.header.json_len) {
      json_buf[ev.header.json_len] = '\0';
    }
  }
  g_events[g_event_head].json.clear();
  g_event_head = (g_event_head + 1) % PGR_RH_EVENT_QUEUE_MAX;
  --g_event_count;
  return PGR_RH_EVENT_POPPED;
}

int api_stop(uint64_t tid) {
  g_lease_until_ms.store(0, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lk(g_options_mutex);
    g_desired_allowed = false;
    if (g_gate_handle != 0) {
      if (const auto *g = gate_api()) g->clear(g_gate_handle);
    }
  }
  if (g_meta_ready.load(std::memory_order_acquire)) {
    if (const auto *act = actions_api()) act->revoke();
    if (const auto *dsc = discovery_api()) dsc->revoke();
    if (const auto *svc = services_api()) svc->revoke();
    if (const auto *obs = observers_api()) obs->revoke();

    const uint64_t owner = g_owner_tid.load(std::memory_order_acquire);
    if (tid != 0 && tid == current_tid() && (owner == 0 || owner == tid)) {
      if (const auto *dsc = discovery_api()) {
        PgrRoutesDiscoveryRequest dreq{PGR_RD_REQUEST_VERSION, sizeof(dreq), tid, 0, dsc->generation(), 0, 0, 0};
        dsc->drain(&dreq);
      }
      if (const auto *svc = services_api()) {
        PgrRoutesServicesRequest sreq{PGR_RS_REQUEST_VERSION, sizeof(sreq), tid, 0, svc->generation()};
        svc->drain(&sreq);
      }
      if (const auto *obs = observers_api()) {
        PgrRoutesObserversRequest oreq{PGR_RO_REQUEST_VERSION, sizeof(oreq), tid, 0, obs->generation()};
        obs->drain(&oreq);
      }
      g_routes_handle = 0;
      g_consumption_handle = 0;
      g_services_ready.store(false, std::memory_order_release);
      g_cached_routes_json = "[]";
      g_cached_route_count.store(0, std::memory_order_release);
    }
  }
  return PGR_RH_OK;
}

int api_sample(PgrRoutesHostSample *out) {
  if (!out) return PGR_RH_ERR_NULL_ARG;
  std::memset(out, 0, sizeof(*out));
  out->version = PGR_RH_SAMPLE_VERSION;
  out->size = sizeof(PgrRoutesHostSample);
  {
    std::lock_guard<std::mutex> lk(g_config_mutex);
    out->configured = g_configured ? 1u : 0u;
  }
  out->meta_ready = g_meta_ready.load(std::memory_order_acquire) ? 1u : 0u;
  out->services_ready = g_services_ready.load(std::memory_order_acquire) ? 1u : 0u;
  out->tracking_ready = g_tracking_ready.load(std::memory_order_acquire) ? 1u : 0u;
  out->lease_active = current_residual_ms() > 0 ? 1u : 0u;
  {
    std::lock_guard<std::mutex> lk(g_event_mutex);
    out->queued_events = g_event_count;
  }
  out->owner_tid = g_owner_tid.load(std::memory_order_acquire);
  {
    std::lock_guard<std::mutex> lk(g_options_mutex);
    const auto *g = gate_api();
    out->last_seq = (g && g_gate_handle) ? g->last_seq(g_gate_handle) : 0ULL;
  }
  out->ticks_run = g_ticks_run.load(std::memory_order_relaxed);
  out->last_error = g_last_error.load(std::memory_order_acquire);
  out->cached_route_count = g_cached_route_count.load(std::memory_order_acquire);
  return PGR_RH_OK;
}

void api_on_unity_pulse(void *, const PgrUnityPulse *pulse) {
  if (!pulse || pulse->tid <= 0) return;
  api_tick(static_cast<uint64_t>(pulse->tid));
}

const PgrRoutesHostApi kApiTable = {
    PGR_RH_API_VERSION,
    static_cast<uint32_t>(sizeof(PgrRoutesHostApi)),
    &api_configure,
    &api_set_options,
    &api_tick,
    &api_next_event,
    &api_stop,
    &api_sample,
    &api_on_unity_pulse,
};

} // namespace

extern "C" const void *pgrazor_routes_host_api(uint32_t version) {
  return version == PGR_RH_API_VERSION ? &kApiTable : nullptr;
}
