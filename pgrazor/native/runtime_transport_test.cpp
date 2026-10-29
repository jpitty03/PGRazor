#include "runtime_transport.h"
#include "pgrazor_native_api.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <unistd.h>
#include <vector>

// Native-process unit harness for feature 52 (compiled runtime transport). This module installs NO hook and
// makes NO managed/il2cpp call -- it is pure bounded typed bookkeeping -- so the meaningful actual-native test
// is this process-level harness resolved through the exported pgrazor_native_api v2 registry
// (query_feature(52,1)), NOT a Dobby attach (there is no target). The coordinator builds and runs it against
// libpgrazor.so after registering query_feature case 52 and linking runtime_transport.cpp. It exercises frame
// malformation, snapshot staleness/freshness, guarded getters, coord null sentinel, exact 64-bit scalars,
// pulse edges, event ordering/drain-cap/overflow/byte-budget, consume-before-dispatch (no replay), and stop.

namespace {

using GetApiFn = const void *(*)(uint32_t);
const PgrRuntimeTransportApi *g_api = nullptr;

// Little-endian writers mirroring the wire the host produces.
void put_u32(std::vector<uint8_t> &b, uint32_t off, uint32_t v) {
  b[off] = v; b[off + 1] = v >> 8; b[off + 2] = v >> 16; b[off + 3] = v >> 24;
}
void put_u64(std::vector<uint8_t> &b, uint32_t off, uint64_t v) {
  put_u32(b, off, static_cast<uint32_t>(v)); put_u32(b, off + 4, static_cast<uint32_t>(v >> 32));
}
uint32_t get_u32(const uint8_t *p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t get_u64(const uint8_t *p) { return get_u32(p) | (static_cast<uint64_t>(get_u32(p + 4)) << 32); }

// A mutable snapshot builder with legacy-ordered field groups.
struct Snap {
  uint64_t seq = 1, wall = 123456;
  uint32_t flags = 0;
  uint8_t bools[PGR_TRANSPORT_BOOLS] = {};
  int32_t ints[PGR_TRANSPORT_INTS] = {};
  double doubles[PGR_TRANSPORT_DOUBLES] = {0.0, 0.0};
  uint64_t pulses[PGR_TRANSPORT_PULSES] = {};
  std::string strings[PGR_TRANSPORT_STRINGS];

  std::vector<uint8_t> frame() const {
    uint32_t size = PGR_TRANSPORT_SNAPSHOT_FIXED;
    for (auto &s : strings) size += 4 + static_cast<uint32_t>(s.size());
    std::vector<uint8_t> b(size, 0);
    put_u32(b, PGR_TRANSPORT_OFF_MAGIC, PGR_TRANSPORT_SNAPSHOT_MAGIC);
    put_u32(b, PGR_TRANSPORT_OFF_VERSION, PGR_TRANSPORT_FRAME_VERSION);
    put_u32(b, PGR_TRANSPORT_OFF_FRAME_SIZE, size);
    put_u32(b, PGR_TRANSPORT_OFF_FLAGS, flags);
    put_u64(b, PGR_TRANSPORT_OFF_SEQ, seq);
    put_u64(b, PGR_TRANSPORT_OFF_WALL_MS, wall);
    for (uint32_t i = 0; i < PGR_TRANSPORT_BOOLS; ++i) b[PGR_TRANSPORT_OFF_BOOLS + i] = bools[i];
    for (uint32_t i = 0; i < PGR_TRANSPORT_INTS; ++i) put_u32(b, PGR_TRANSPORT_OFF_INTS + i * 4, static_cast<uint32_t>(ints[i]));
    for (uint32_t i = 0; i < PGR_TRANSPORT_DOUBLES; ++i) {
      uint64_t bits; std::memcpy(&bits, &doubles[i], 8); put_u64(b, PGR_TRANSPORT_OFF_DOUBLES + i * 8, bits);
    }
    for (uint32_t i = 0; i < PGR_TRANSPORT_PULSES; ++i) put_u64(b, PGR_TRANSPORT_OFF_PULSES + i * 8, pulses[i]);
    uint32_t at = PGR_TRANSPORT_OFF_STRINGS;
    for (auto &s : strings) {
      put_u32(b, at, static_cast<uint32_t>(s.size())); at += 4;
      std::memcpy(b.data() + at, s.data(), s.size()); at += static_cast<uint32_t>(s.size());
    }
    return b;
  }
};

// Push a snapshot (drain discarded). Returns the exchange status.
int push(const std::vector<uint8_t> &frame) {
  std::vector<uint8_t> out(4096);
  uint32_t out_len = 0;
  return g_api->exchange(frame.data(), static_cast<uint32_t>(frame.size()), out.data(),
                         static_cast<uint32_t>(out.size()), &out_len);
}

// One drained event, decoded.
struct Ev {
  uint32_t opcode = 0;
  std::vector<int64_t> scalars;
  std::vector<std::string> strings;
};

// Drain (no snapshot update) into a buffer of `cap` bytes; decode the frame.
std::vector<Ev> drain(uint32_t cap, uint32_t *count_out = nullptr, uint64_t *gen_out = nullptr) {
  std::vector<uint8_t> out(cap);
  uint32_t out_len = 0;
  int rc = g_api->exchange(nullptr, 0, out.data(), cap, &out_len);
  assert(rc == PGR_TRANSPORT_OK);
  assert(out_len >= PGR_TRANSPORT_EVENT_HEADER && out_len <= cap);
  assert(get_u32(out.data() + PGR_TRANSPORT_OFF_MAGIC) == PGR_TRANSPORT_EVENTS_MAGIC);
  assert(get_u32(out.data() + PGR_TRANSPORT_OFF_VERSION) == PGR_TRANSPORT_FRAME_VERSION);
  assert(get_u32(out.data() + PGR_TRANSPORT_OFF_FRAME_SIZE) == out_len);
  uint32_t count = get_u32(out.data() + 12);
  if (gen_out) *gen_out = get_u64(out.data() + 24);
  if (count_out) *count_out = count;
  std::vector<Ev> events;
  uint32_t at = PGR_TRANSPORT_EVENT_HEADER;
  for (uint32_t i = 0; i < count; ++i) {
    Ev e;
    e.opcode = get_u32(out.data() + at); at += 4;
    uint32_t ns = get_u32(out.data() + at); at += 4;
    uint32_t nstr = get_u32(out.data() + at); at += 4;
    for (uint32_t s = 0; s < ns; ++s) { e.scalars.push_back(static_cast<int64_t>(get_u64(out.data() + at))); at += 8; }
    for (uint32_t s = 0; s < nstr; ++s) {
      uint32_t slen = get_u32(out.data() + at); at += 4;
      e.strings.emplace_back(reinterpret_cast<const char *>(out.data() + at), slen); at += slen;
    }
    events.push_back(std::move(e));
  }
  assert(at == out_len);
  return events;
}

PgrRuntimeTransportSample sample() {
  PgrRuntimeTransportSample s{};
  assert(g_api->sample(&s) == PGR_TRANSPORT_OK && s.version == 1 && s.size == sizeof(s));
  return s;
}

int emit_scalar(uint32_t opcode, std::vector<int64_t> scalars) {
  return g_api->emit(opcode, scalars.empty() ? nullptr : scalars.data(),
                     static_cast<uint32_t>(scalars.size()), nullptr, 0);
}

PgrRuntimeTransportConfig make_config() {
  PgrRuntimeTransportConfig c{};
  c.version = 1;
  c.size = sizeof(c);
  c.lease_ms = 100;         // short enough to exercise expiry without wedging the harness
  c.max_events = 200;       // lets the drain-cap (128) and count-overflow cases both run
  c.max_string_bytes = 65536;
  c.queue_byte_budget = PGR_TRANSPORT_MAX_FRAME;  // 1 MiB
  return c;
}

}  // namespace

int main(int argc, char **argv) {
  const char *so_path = argc > 1 ? argv[1] : "./libpgrazor.so";
  void *lib = ::dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
  if (!lib) { std::fprintf(stderr, "dlopen(%s) failed: %s\n", so_path, ::dlerror()); return 1; }
  auto getter = reinterpret_cast<GetApiFn>(::dlsym(lib, "pgrazor_native_api"));
  assert(getter);
  const auto *core = static_cast<const pgrazor_native_api_v2 *>(getter(2));
  assert(core && core->size == 48);
  assert(core->query_feature(52, 99) == nullptr);
  const auto *api = static_cast<const PgrRuntimeTransportApi *>(core->query_feature(52, 1));
  assert(api != nullptr && api->version == 1 && api->size == sizeof(PgrRuntimeTransportApi));
  g_api = api;

  std::printf("ABI sizes: config=%zu sample=%zu table=%zu\n",
              sizeof(PgrRuntimeTransportConfig), sizeof(PgrRuntimeTransportSample), sizeof(PgrRuntimeTransportApi));
  assert(sizeof(PgrRuntimeTransportConfig) == 32 && sizeof(PgrRuntimeTransportSample) == 88 &&
         sizeof(PgrRuntimeTransportApi) == 96);

  PgrRuntimeTransportConfig cfg = make_config();

  // 1. Validation + not-installed + idempotence + reconfig.
  assert(api->install(nullptr) == PGR_TRANSPORT_ERR_NULL_ARG);
  { auto b = cfg; b.version = 2; assert(api->install(&b) == PGR_TRANSPORT_ERR_BAD_VERSION); }
  { auto b = cfg; b.size = sizeof(cfg) - 1; assert(api->install(&b) == PGR_TRANSPORT_ERR_BAD_SIZE); }
  { auto b = cfg; b.lease_ms = PGR_TRANSPORT_MAX_LEASE_MS + 1; assert(api->install(&b) == PGR_TRANSPORT_ERR_BAD_BOUND); }
  { auto b = cfg; b.max_events = PGR_TRANSPORT_MAX_EVENTS + 1; assert(api->install(&b) == PGR_TRANSPORT_ERR_BAD_BOUND); }
  { int v = 0; assert(api->get_bool(0, &v) == PGR_TRANSPORT_ERR_NOT_INSTALLED); }
  assert(api->install(&cfg) == PGR_TRANSPORT_OK);
  assert(api->install(&cfg) == PGR_TRANSPORT_OK);  // idempotent
  { auto b = cfg; b.lease_ms = 50; assert(api->install(&b) == PGR_TRANSPORT_ERR_RECONFIG); }

  // 2. No snapshot yet: non-guarded getter STALE; guarded bool returns 0.
  { int v = 7; assert(api->get_int(0, &v) == PGR_TRANSPORT_STALE); }
  { int v = 7; assert(api->get_bool(0 /*accountManagementAllowed, guarded*/, &v) == PGR_TRANSPORT_OK && v == 0); }
  { int v = 7; assert(api->get_bool(4 /*autoIncubate, NOT guarded*/, &v) == PGR_TRANSPORT_STALE); }

  // 3. Push a full snapshot; typed round-trip for every field group, coord null sentinel, pulse edge.
  {
    Snap s;
    s.bools[3] = 1;          // active
    s.bools[4] = 1;          // autoIncubate (not guarded)
    s.ints[0] = -12345;      // battleOptions
    s.ints[7] = 2147483647;  // throwGrade at int32 max
    s.doubles[0] = 37.5;     // latitude
    s.doubles[1] = std::nan("");  // longitude -> null coord sentinel
    s.pulses[0] = 3;         // takeCameraSampleRequest count
    s.strings[0] = "signin";                 // accountCommand
    s.strings[2] = u8"berryÃ©";          // effectiveBerryPriority (multibyte UTF-8)
    s.strings[5] = "cmd";                    // accountCommandSnapshot[0]
    s.strings[6] = "18446744073709551615";   // accountCommandSnapshot[1] (beyond JS precision)
    assert(push(s.frame()) == PGR_TRANSPORT_OK);

    int bv = 0; assert(api->get_bool(3, &bv) == PGR_TRANSPORT_OK && bv == 1);
    assert(api->get_bool(4, &bv) == PGR_TRANSPORT_OK && bv == 1);
    int32_t iv = 0; assert(api->get_int(0, &iv) == PGR_TRANSPORT_OK && iv == -12345);
    assert(api->get_int(7, &iv) == PGR_TRANSPORT_OK && iv == 2147483647);
    double dv = 0; int has = 9;
    assert(api->get_double(0, &dv, &has) == PGR_TRANSPORT_OK && has == 1 && dv == 37.5);
    assert(api->get_double(1, &dv, &has) == PGR_TRANSPORT_OK && has == 0);  // NaN -> null coord
    char buf[64]; uint32_t n = 0;
    assert(api->get_string(0, buf, sizeof(buf), &n) == PGR_TRANSPORT_OK && std::string(buf, n) == "signin");
    assert(api->get_string(2, buf, sizeof(buf), &n) == PGR_TRANSPORT_OK && std::string(buf, n) == u8"berryÃ©");
    char cmd[32], tok[32]; uint32_t cl = 0, tl = 0;
    assert(api->get_command_snapshot(cmd, sizeof(cmd), &cl, tok, sizeof(tok), &tl) == PGR_TRANSPORT_OK);
    assert(std::string(cmd, cl) == "cmd" && std::string(tok, tl) == "18446744073709551615");
    // Capacity too small fails closed but reports the required length.
    assert(api->get_string(6, buf, 3, &n) == PGR_TRANSPORT_ERR_RANGE && n == 20);
    int fired = 0;
    assert(api->take_pulse(0, &fired) == PGR_TRANSPORT_OK && fired == 1);  // first edge
    assert(api->take_pulse(0, &fired) == PGR_TRANSPORT_OK && fired == 0);  // no new count -> no edge
    assert(api->take_pulse(1, &fired) == PGR_TRANSPORT_OK && fired == 0);  // count 0
  }

  // 4. Malformed snapshot frames are rejected; the previously stored snapshot stays intact.
  {
    auto bad_magic = Snap{}.frame(); put_u32(bad_magic, PGR_TRANSPORT_OFF_MAGIC, 0xDEADBEEF);
    assert(push(bad_magic) == PGR_TRANSPORT_ERR_BAD_FRAME);
    auto bad_ver = Snap{}.frame(); put_u32(bad_ver, PGR_TRANSPORT_OFF_VERSION, 2);
    assert(push(bad_ver) == PGR_TRANSPORT_ERR_BAD_VERSION);
    auto bad_size = Snap{}.frame(); put_u32(bad_size, PGR_TRANSPORT_OFF_FRAME_SIZE, 999999);
    assert(push(bad_size) == PGR_TRANSPORT_ERR_BAD_SIZE);
    auto bad_bool = Snap{}.frame(); bad_bool[PGR_TRANSPORT_OFF_BOOLS + 1] = 5;
    assert(push(bad_bool) == PGR_TRANSPORT_ERR_BAD_FRAME);
    { Snap s; s.strings[0] = std::string("\xc3\x28", 2); assert(push(s.frame()) == PGR_TRANSPORT_ERR_BAD_UTF8); }
    auto trailing = Snap{}.frame(); trailing.push_back(0); put_u32(trailing, PGR_TRANSPORT_OFF_FRAME_SIZE,
                                                                   static_cast<uint32_t>(trailing.size()));
    assert(push(trailing) == PGR_TRANSPORT_ERR_BAD_FRAME);  // string heap does not reach frame end
    // Stored snapshot from case 3 survived every rejection.
    int bv = 0; assert(api->get_bool(3, &bv) == PGR_TRANSPORT_OK && bv == 1);
  }

  // 5. Snapshot staleness: after the lease elapses every getter is STALE (guarded booleans stay 0).
  {
    ::usleep((cfg.lease_ms + 40) * 1000);
    int32_t iv = 0; assert(api->get_int(0, &iv) == PGR_TRANSPORT_STALE);
    int bv = 7; assert(api->get_bool(3 /*active, guarded*/, &bv) == PGR_TRANSPORT_OK && bv == 0);
    int fired = 9; assert(api->take_pulse(0, &fired) == PGR_TRANSPORT_OK && fired == 0);  // stale -> no edge
    // Refresh with a STRICTLY NEWER frame (seq advances past the stored 1; pulses do not regress below 3).
    Snap r; r.seq = 2; r.pulses[0] = 3; assert(push(r.frame()) == PGR_TRANSPORT_OK);
  }

  // 5b. Replay/stale snapshot rejection: a non-advancing seq or a regressed pulse count is refused WITHOUT
  // overwriting the stored snapshot or refreshing the lease (fail closed; fault latched).
  {
    uint64_t stored_seq = sample().snapshot_seq, rejected0 = sample().snapshots_rejected;
    { Snap d; d.seq = 2; d.pulses[0] = 3; assert(push(d.frame()) == PGR_TRANSPORT_ERR_REPLAY); }   // duplicate seq
    { Snap o; o.seq = 1; o.pulses[0] = 9; assert(push(o.frame()) == PGR_TRANSPORT_ERR_REPLAY); }   // older seq
    { Snap p; p.seq = 3; p.pulses[0] = 2; assert(push(p.frame()) == PGR_TRANSPORT_ERR_REPLAY); }   // pulse regressed
    assert(sample().snapshot_seq == stored_seq);               // stored snapshot untouched
    assert(sample().snapshots_rejected == rejected0 + 3 && sample().fault == 1);
    // A genuinely newer frame (seq advances, pulses equal) is still accepted.
    { Snap n; n.seq = 4; n.pulses[0] = 3; assert(push(n.frame()) == PGR_TRANSPORT_OK); }
  }

  // 5c. Strict snapshot framing: unknown flag bits and nonzero layout padding are rejected; stored intact.
  {
    auto bad_flag = Snap{}.frame(); put_u32(bad_flag, PGR_TRANSPORT_OFF_FLAGS, 0x4);
    assert(push(bad_flag) == PGR_TRANSPORT_ERR_BAD_FRAME);  // unknown flag bit
    auto bad_pad = Snap{}.frame(); bad_pad[PGR_TRANSPORT_OFF_BOOLS + PGR_TRANSPORT_BOOLS] = 1;  // the single pad byte @71
    assert(push(bad_pad) == PGR_TRANSPORT_ERR_BAD_FRAME);
    assert(sample().snapshot_seq == 4);  // survived
  }

  // 6. Emit schema validation (arity + UTF-8).
  { int64_t one = 1; assert(g_api->emit(PGR_TRANSPORT_ACTIONS, &one, 1, nullptr, 0) == PGR_TRANSPORT_ERR_BAD_OPCODE); }
  assert(emit_scalar(1 /*setAccountsReady, 1 scalar*/, {}) == PGR_TRANSPORT_ERR_BAD_SCHEMA);  // wrong arity
  { PgrRuntimeTransportString str{"ok", 2};
    assert(g_api->emit(1, nullptr, 0, &str, 1) == PGR_TRANSPORT_ERR_BAD_SCHEMA); }  // string where scalar expected
  { PgrRuntimeTransportString bad{"\xff", 1};
    assert(g_api->emit(8 /*setGameRoutesSnapshot(String)*/, nullptr, 0, &bad, 1) == PGR_TRANSPORT_ERR_BAD_UTF8); }

  // 6b. Emit TYPE + DOMAIN validation (not arity alone): a bad native producer cannot push a poisoned scalar.
  {
    auto bits = [](double d) { uint64_t b; std::memcpy(&b, &d, 8); return static_cast<int64_t>(b); };
    assert(emit_scalar(1 /*setAccountsReady(boolean)*/, {2}) == PGR_TRANSPORT_ERR_DOMAIN);  // boolean not 0/1
    int64_t h_badint[4] = {bits(20.0), 1LL << 40, 1, 0};
    assert(g_api->emit(57, h_badint, 4, nullptr, 0) == PGR_TRANSPORT_ERR_DOMAIN);           // int out of signed-32
    int64_t h_neg[4] = {bits(20.0), 7, 1, -1};
    assert(g_api->emit(57, h_neg, 4, nullptr, 0) == PGR_TRANSPORT_ERR_DOMAIN);              // timestamp < 0 (domain)
    int64_t h_inf[4] = {bits(std::nan("")), 7, 1, 0};
    assert(g_api->emit(57, h_inf, 4, nullptr, 0) == PGR_TRANSPORT_ERR_DOMAIN);              // nonfinite double
    int64_t tap_bad[5] = {bits(91.0), bits(0.0), 1, 0, bits(0.0)};
    assert(g_api->emit(10, tap_bad, 5, nullptr, 0) == PGR_TRANSPORT_ERR_DOMAIN);            // |lat| > 90
    int64_t tap_mode[5] = {bits(0.0), bits(0.0), 4, 0, bits(0.0)};
    assert(g_api->emit(10, tap_mode, 5, nullptr, 0) == PGR_TRANSPORT_ERR_DOMAIN);           // mode out of 0..3
    // Exact 64-bit long: Long.MAX_VALUE is a valid timestamp and round-trips bit-for-bit (no JS-safe clamp).
    (void)drain(4096);
    int64_t h_max[4] = {bits(20.0), 7, 1, INT64_MAX};
    assert(g_api->emit(57, h_max, 4, nullptr, 0) == PGR_TRANSPORT_OK);
    auto evs = drain(4096);
    assert(evs.size() == 1 && evs[0].scalars[3] == INT64_MAX);
  }

  // 6c. An event too large to ever fit a bounded frame cannot wedge the queue, and a head event that does not
  // fit the caller's output buffer fails EXPLICITLY instead of looping on an empty frame forever.
  {
    (void)drain(1 << 20);
    assert(emit_scalar(2 /*accountSessionEnded: 12 wire bytes*/, {}) == PGR_TRANSPORT_OK);
    std::vector<uint8_t> tiny(PGR_TRANSPORT_EVENT_HEADER + 11);  // one byte short of the 12-byte head event
    uint32_t tiny_len = 123;
    int rc = g_api->exchange(nullptr, 0, tiny.data(), static_cast<uint32_t>(tiny.size()), &tiny_len);
    assert(rc == PGR_TRANSPORT_ERR_RANGE && tiny_len == 0);
    assert(sample().queue_depth == 1);  // not dropped -- still queued
    (void)drain(1 << 20);
    assert(sample().queue_depth == 0);
  }

  // 7. Event ordering + exact 64-bit scalars + string args round-trip through the drain.
  {
    (void)drain(4096);  // clear any residue
    // applyMapTap: double,double,int,int,double -> 5 scalars as raw bits / integer values.
    auto bits = [](double d) { uint64_t b; std::memcpy(&b, &d, 8); return static_cast<int64_t>(b); };
    int64_t tap[5] = {bits(37.5), bits(-122.25), 2, 7, bits(0.0)};
    assert(g_api->emit(10, tap, 5, nullptr, 0) == PGR_TRANSPORT_OK);
    // accountIdentity: two string args.
    PgrRuntimeTransportString pair[2] = {{"idv", 3}, {"name", 4}};
    assert(g_api->emit(0, nullptr, 0, pair, 2) == PGR_TRANSPORT_OK);
    // updateCameraHeadingAt: double,int,boolean,long with a large exact long.
    int64_t heading[4] = {bits(20.0), 7, 1, 9007199254740991LL};
    assert(g_api->emit(57, heading, 4, nullptr, 0) == PGR_TRANSPORT_OK);

    auto evs = drain(4096);
    assert(evs.size() == 3);
    assert(evs[0].opcode == 10 && evs[0].scalars.size() == 5);
    double lat; uint64_t lb = static_cast<uint64_t>(evs[0].scalars[0]); std::memcpy(&lat, &lb, 8);
    assert(lat == 37.5 && evs[0].scalars[2] == 2 && evs[0].scalars[3] == 7);
    assert(evs[1].opcode == 0 && evs[1].strings.size() == 2 && evs[1].strings[0] == "idv" && evs[1].strings[1] == "name");
    assert(evs[2].opcode == 57 && evs[2].scalars[3] == 9007199254740991LL);
  }

  // 8. Drain cap of 128: 130 events -> 128 then 2; consume-before-dispatch means no replay.
  {
    (void)drain(1 << 20);
    for (int i = 0; i < 130; ++i) assert(emit_scalar(2 /*accountSessionEnded*/, {}) == PGR_TRANSPORT_OK);
    uint32_t c1 = 0, c2 = 0;
    auto first = drain(1 << 20, &c1);
    assert(c1 == PGR_TRANSPORT_DRAIN_CAP && first.size() == PGR_TRANSPORT_DRAIN_CAP);
    auto second = drain(1 << 20, &c2);
    assert(c2 == 2 && second.size() == 2);
    assert(drain(1 << 20, &c1).empty() && c1 == 0);  // fully consumed, nothing replays
  }

  // 9. A small output buffer drains only what fits; the remainder stays queued (not dropped).
  {
    (void)drain(1 << 20);
    for (int i = 0; i < 5; ++i) assert(emit_scalar(2, {}) == PGR_TRANSPORT_OK);
    uint32_t c = 0;
    auto part = drain(PGR_TRANSPORT_EVENT_HEADER + 2 * 12, &c);  // room for 2 no-arg events
    assert(c == 2 && part.size() == 2);
    assert(sample().queue_depth == 3);
    (void)drain(1 << 20);
    assert(sample().queue_depth == 0);
  }

  // 10. Count overflow fails closed with an explicit status + latched fault (no silent drop).
  {
    for (uint32_t i = 0; i < cfg.max_events; ++i) assert(emit_scalar(2, {}) == PGR_TRANSPORT_OK);
    assert(emit_scalar(2, {}) == PGR_TRANSPORT_ERR_OVERFLOW);
    assert(sample().events_dropped >= 1 && sample().fault == 1);
    (void)drain(1 << 20);  // clear
  }

  // 11. Byte-budget overflow (independent of count).
  {
    std::string big(65536, 'x');
    PgrRuntimeTransportString s{big.data(), static_cast<uint32_t>(big.size())};
    uint32_t fit = cfg.queue_byte_budget / 65536;  // 16 for a 1 MiB budget
    for (uint32_t i = 0; i < fit; ++i) assert(g_api->emit(41 /*setMapPoiSnapshot*/, nullptr, 0, &s, 1) == PGR_TRANSPORT_OK);
    assert(g_api->emit(41, nullptr, 0, &s, 1) == PGR_TRANSPORT_ERR_OVERFLOW);
    (void)drain(1 << 20);
  }

  // 12. stop(): queue cleared, snapshot getters STALE, generation bumped, emit becomes a clean no-op.
  {
    assert(emit_scalar(2, {}) == PGR_TRANSPORT_OK);
    uint64_t gen_before = sample().generation;
    assert(api->stop() == PGR_TRANSPORT_OK);
    assert(sample().generation == gen_before + 1 && sample().queue_depth == 0);
    int32_t iv = 0; assert(api->get_int(0, &iv) == PGR_TRANSPORT_STALE);
    assert(emit_scalar(2, {}) == PGR_TRANSPORT_STOPPED);  // clean shutdown, not a fault
    uint64_t gen_out = 0;
    assert(drain(4096, nullptr, &gen_out).empty() && gen_out == gen_before + 1);
  }

  std::printf("runtime_transport_test passed: ABI/validation/idempotence/reconfig, typed snapshot round-trip "
              "(bool/int/double-null/string/command-pair/pulse-edge), guarded stale default, malformed-frame "
              "rejection with intact snapshot, lease staleness, emit schema/UTF8 rejection, exact 64-bit "
              "scalar+string ordering, drain cap 128 + partial-buffer backpressure, count/byte overflow "
              "fail-closed, consume-before-dispatch no-replay, stop generation+no-op\n");
  return 0;
}
