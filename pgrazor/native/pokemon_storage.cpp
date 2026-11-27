#include "pokemon_storage.h"
#include "storage_json.h"
#include "runtime_bootstrap.h"
#include "runtime_metadata.h"
#include "shared_metadata.h"
#include <android/log.h>
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <string>

namespace {
using namespace pgr_shared_meta;
struct Failure { const char *where; };
void require(bool ok, const char *where) { if (!ok) throw Failure{where}; }
PgrBootBinding boot{};
const PgrSharedMetadataOutput *md = nullptr;
uint64_t owner = 0;
bool initialized = false, failed = false;

uint64_t clock_ms() { timespec t{}; clock_gettime(CLOCK_BOOTTIME, &t); return uint64_t(t.tv_sec) * 1000 + t.tv_nsec / 1000000; }
template <class F> F fn(const void *p) { require(p, "missing runtime export"); return reinterpret_cast<F>(const_cast<void *>(p)); }
void *exp(const char *name) { std::string n = "il2cpp_"; n += name; void *p = dlsym(boot.handle, n.c_str()); require(p, name); return p; }
struct Root {
  uintptr_t h = 0;
  Root() = default;
  explicit Root(void *v) { if (v) { h = fn<uintptr_t (*)(void *, int)>(boot.runtime.gchandle_new)(v, 0); require(h, "GC root"); } }
  Root(const Root &) = delete; Root &operator=(const Root &) = delete;
  ~Root() { if (h) fn<void (*)(uintptr_t)>(boot.runtime.gchandle_free)(h); }
  void *get() const { return h ? fn<void *(*)(uintptr_t)>(boot.runtime.gchandle_get_target)(h) : nullptr; }
};
void *klass(void *o) { require(o, "null object"); return fn<void *(*)(void *)>(boot.runtime.object_get_class)(o); }
void *cls(const char *ns, const char *name) {
  const auto *api = static_cast<const PgrMetaApi *>(pgrazor_runtime_metadata_api(1)); require(api, "metadata API");
  PgrMetaClassQuery q{}; q.version = 1; q.size = sizeof(q); q.name_space = ns; q.name = name; q.canonical_match = PGR_MD_MATCH_EXACT;
  PgrMetaClassResult out{}; require(api->resolve_class(&q, &out) == 0, name); return const_cast<void *>(out.klass);
}
void *method(void *k, const char *name, int count) { void *m = fn<void *(*)(void *, const char *, int)>(boot.meta.class_get_method_from_name)(k, name, count); require(m, name); return m; }
void *invoke(void *m, void *o, void **args = nullptr) {
  void *ex = nullptr; void *v = fn<void *(*)(void *, void *, void **, void **)>(boot.runtime.runtime_invoke)(m, o, args, &ex);
  require(!ex, "managed call failed"); return v;
}
void *call(void *o, const char *name, int n = 0, void **args = nullptr) { return invoke(method(klass(o), name, n), o, args); }
template <class T> T unbox(void *o) { require(o, "null result"); T v{}; std::memcpy(&v, fn<void *(*)(void *)>(boot.runtime.object_unbox)(o), sizeof(v)); return v; }
template <class T> T at(void *o, size_t off) { T v{}; std::memcpy(&v, static_cast<char *>(o) + off, sizeof(v)); return v; }
std::string utf8(void *s) {
  if (!s) return {};
  void *length = dlsym(boot.handle, "il2cpp_string_length"); require(length, "il2cpp_string_length");
  int n = fn<int (*)(void *)>(length)(s);
  const uint16_t *p = fn<const uint16_t *(*)(void *)>(boot.runtime.string_chars)(s);
  std::string out;
  // Modified UTF-8, as produced by JNI's GetStringUTFChars; surrogates stay separate units.
  for (int i = 0; i < n && i < 256; ++i) {
    uint32_t c = p[i];
    if (c > 0 && c < 0x80) out += char(c);
    else if (c < 0x800) { out += char(0xc0 | (c >> 6)); out += char(0x80 | (c & 63)); }
    else { out += char(0xe0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 63)); out += char(0x80 | (c & 63)); }
  }
  return out;
}
size_t off(void *k, const char *name) {
  void *f = fn<void *(*)(void *, const char *)>(boot.meta.class_get_field_from_name)(k, name); require(f, name);
  const size_t o = fn<size_t (*)(void *)>(exp("field_get_offset"))(f); require(o >= 0x10 && o < 0x1000, name); return o;
}
size_t off_or0(void *k, const char *name) {
  void *f = fn<void *(*)(void *, const char *)>(boot.meta.class_get_field_from_name)(k, name);
  return f ? fn<size_t (*)(void *)>(exp("field_get_offset"))(f) : 0;
}
void *h(unsigned id) { return const_cast<void *>(md->opt_handles[id]); }
void *type_object(void *k) { return fn<void *(*)(void *)>(exp("type_get_object"))(fn<void *(*)(void *)>(boot.meta.class_get_type)(k)); }
void *resolve(void *contract) {
  Root type(type_object(h(H_IL_MAP))); void *args[] = {type.get()}; Root map(invoke(h(H_IL_FIND_M), nullptr, args)); if (!map.get()) return nullptr;
  Root container(invoke(h(H_IL_CONTAINER_M), map.get())); if (!container.get()) return nullptr;
  Root target(type_object(contract)); void *resolve_args[] = {target.get()}; return invoke(h(H_IL_RESOLVE_M), container.get(), resolve_args);
}

struct Fields {
  void *k = nullptr, *dk = nullptr;
  size_t id, species, cp, hp, max_hp, atk, def, sta, cpm, extra, move1, move2, move3, display, egg, lucky, favorite, fort, station;
  size_t d_form, d_align, nickname, d_gender, d_shiny, d_costume, caught;
} f;
void load_fields() {
  if (f.k) return;
  Fields n; n.k = cls("Holoholo.Rpc", "PokemonProto"); n.dk = cls("Holoholo.Rpc", "PokemonDisplayProto");
  n.id = off(n.k, "id_"); n.species = off(n.k, "pokemonId_"); n.cp = off(n.k, "cp_"); n.hp = off(n.k, "stamina_"); n.max_hp = off(n.k, "maxStamina_");
  n.atk = off(n.k, "individualAttack_"); n.def = off(n.k, "individualDefense_"); n.sta = off(n.k, "individualStamina_");
  n.cpm = off(n.k, "cpMultiplier_"); n.extra = off(n.k, "additionalCpMultiplier_");
  n.move1 = off(n.k, "move1_"); n.move2 = off(n.k, "move2_"); n.move3 = off(n.k, "move3_");
  n.display = off(n.k, "pokemonDisplay_"); n.egg = off(n.k, "isEgg_");
  n.lucky = off_or0(n.k, "isLucky_"); n.favorite = off_or0(n.k, "favorite_");
  n.fort = off_or0(n.k, "deployedFortId_"); n.station = off_or0(n.k, "deployedStationId_");
  n.d_form = off(n.dk, "Form"); n.d_align = off(n.dk, "Alignment");
  n.nickname = off_or0(n.k, "nickname_"); n.caught = off_or0(n.k, "caughtTimeUtcMs_");
  n.d_gender = off_or0(n.dk, "gender_"); n.d_shiny = off_or0(n.dk, "shiny_"); n.d_costume = off_or0(n.dk, "costume_");
  f = n;
}
bool has_text(void *o, size_t o_off) { if (!o_off) return false; void *s = at<void *>(o, o_off); return s && at<int32_t>(s, 0x10) > 0; }

std::vector<PgrOwnedPokemon> cache;
uint64_t cache_at = 0;
int cache_count = -1;

void read_all(std::vector<PgrOwnedPokemon> &out, int &count_out, uint64_t max_age_ms) {
  load_fields();
  Root bag(resolve(cls("Niantic.Holoholo", "IPokemonBag"))); require(bag.get(), "Pokemon storage unavailable");
  Root all(call(bag.get(), "get_AllPokemon")); require(all.get(), "Pokemon storage list unavailable");
  void *k = klass(all.get());
  const int n = unbox<int32_t>(invoke(method(k, "get_Count", 0), all.get())); require(n >= 0 && n <= 30000, "Pokemon storage size");
  count_out = n;
  if (n == cache_count && clock_ms() - cache_at < max_age_ms) { out = cache; return; }
  Root array(fn<void *(*)(void *, uintptr_t)>(exp("array_new"))(f.k, uintptr_t(n))); require(array.get(), "storage copy");
  int zero = 0; void *args[] = {array.get(), &zero}; invoke(method(k, "CopyTo", 2), all.get(), args);
  std::vector<PgrOwnedPokemon> next; next.reserve(size_t(n));
  for (int i = 0; i < n; ++i) {
    void *p = at<void *>(array.get(), 0x20 + size_t(i) * 8);
    if (!p || klass(p) != f.k) continue;
    PgrOwnedPokemon o;
    o.id = at<uint64_t>(p, f.id); o.species = at<int32_t>(p, f.species); o.cp = at<int32_t>(p, f.cp);
    o.hp = at<int32_t>(p, f.hp); o.max_hp = at<int32_t>(p, f.max_hp);
    o.atk = at<int32_t>(p, f.atk); o.def = at<int32_t>(p, f.def); o.sta = at<int32_t>(p, f.sta);
    o.cpm = double(at<float>(p, f.cpm)) + double(at<float>(p, f.extra));
    o.move1 = at<int32_t>(p, f.move1); o.move2 = at<int32_t>(p, f.move2); o.move3 = at<int32_t>(p, f.move3);
    o.egg = at<bool>(p, f.egg); o.lucky = f.lucky && at<bool>(p, f.lucky); o.favorite = f.favorite && at<int32_t>(p, f.favorite) != 0;
    o.deployed = has_text(p, f.fort) || has_text(p, f.station);
    if (void *d = at<void *>(p, f.display)) {
      o.form = at<int32_t>(d, f.d_form); o.alignment = at<int32_t>(d, f.d_align);
      if (f.d_gender) o.gender = at<int32_t>(d, f.d_gender);
      if (f.d_shiny) o.shiny = at<bool>(d, f.d_shiny) ? 1 : 0;
      if (f.d_costume) o.costume = at<int32_t>(d, f.d_costume);
    }
    if (f.nickname) o.nickname = utf8(at<void *>(p, f.nickname));
    if (f.caught) o.caught_ms = at<uint64_t>(p, f.caught);
    next.push_back(o);
  }
  cache.swap(next); cache_at = clock_ms(); cache_count = n; out = cache;
}
}  // namespace

void pgr_storage_tick(uint64_t tid, const PgrSharedMetadataOutput *metadata) {
  if (initialized || failed || !tid || tid != uint64_t(gettid()) || !metadata) return;
  try {
    auto api = static_cast<const PgrBootApi *>(pgrazor_runtime_bootstrap_api(1));
    boot.version = 1; boot.size = sizeof(boot);
    require(api && api->snapshot(&boot) == 0, "runtime bootstrap");
    md = metadata; owner = tid; initialized = true;
  } catch (...) { failed = true; }
}

void pgr_storage_invalidate() {
  if (owner != uint64_t(gettid())) return;  // the cache is owned by the Unity thread
  cache.clear(); cache_at = 0; cache_count = -1;
}

bool pgr_storage_snapshot(std::vector<PgrOwnedPokemon> *out, uint64_t max_age_ms) {
  if (!out) return false;
  out->clear();
  if (!initialized || failed || owner != uint64_t(gettid())) return false;
  try { int count = 0; read_all(*out, count, max_age_ms); return true; }
  catch (const Failure &e) { __android_log_print(ANDROID_LOG_WARN, "PGRazorStorage", "%s", e.where); out->clear(); return false; }
  catch (...) { out->clear(); return false; }
}

std::string pgr_storage_json() {
  if (owner != uint64_t(gettid())) return "[]";
  std::vector<PgrOwnedPokemon> owned;
  if (!pgr_storage_snapshot(&owned, 0)) return "[]";  // always the live list
  std::string out = "[";
  for (size_t i = 0; i < owned.size(); ++i) {
    const PgrOwnedPokemon &o = owned[i];
    if (i) out += ',';
    char cpm[24]; std::snprintf(cpm, sizeof(cpm), "%.6f", o.cpm);
    out += "{\"id\":\"" + std::to_string(o.id) + "\"";
    out += ",\"species\":" + std::to_string(o.species);
    out += ",\"form\":" + std::to_string(o.form);
    out += ",\"cp\":" + std::to_string(o.cp);
    out += ",\"hp\":" + std::to_string(o.hp) + ",\"maxHp\":" + std::to_string(o.max_hp);
    out += ",\"iv\":[" + std::to_string(o.atk) + "," + std::to_string(o.def) + "," + std::to_string(o.sta) + "]";
    out += ",\"cpm\":" + std::string(cpm);
    out += ",\"moves\":[" + std::to_string(o.move1) + "," + std::to_string(o.move2) + "," + std::to_string(o.move3) + "]";
    out += ",\"alignment\":" + std::to_string(o.alignment);
    out += ",\"lucky\":" + std::string(o.lucky ? "true" : "false") + ",\"favorite\":" + std::string(o.favorite ? "true" : "false");
    out += ",\"egg\":" + std::string(o.egg ? "true" : "false") + ",\"deployed\":" + std::string(o.deployed ? "true" : "false");
    out += ",\"nickname\":"; pgr_storage_json_string(out, o.nickname);
    out += ",\"gender\":" + std::to_string(o.gender) + ",\"shiny\":" + std::to_string(o.shiny) + ",\"costume\":" + std::to_string(o.costume);
    out += ",\"caughtMs\":" + std::to_string(o.caught_ms);
    out += '}';
  }
  out += ']';
  return out;
}
