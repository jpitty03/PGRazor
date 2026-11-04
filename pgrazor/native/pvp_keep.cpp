#include "pvp_keep.h"

#include "game_bindings.h"
#include "pvp_rank.h"
#include "runtime_bootstrap.h"
#include "runtime_metadata.h"
#include "runtime_transport.h"

#include <android/log.h>
#include <dlfcn.h>
#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

// Binds once, on the first check: five pinned fields, the Shadow literal and the game's Form enum, whose
// literal names are matched to the snapshot's form keys (StunfiskGalarian -> stunfiskgalarian, and the
// base entry also answers XNormal and an unset form). A form the snapshot does not know gets no PvP keep.
namespace {
std::mutex g_mutex;
int g_state = 0;  // 0 not bound, 1 bound, -1 failed for this process
uint64_t g_owner = 0;
void *g_class_pokemon = nullptr, *g_class_display = nullptr;
void *g_species = nullptr, *g_display = nullptr, *g_cpm = nullptr, *g_cpm_extra = nullptr, *g_form = nullptr, *g_alignment = nullptr;
int32_t g_shadow = 0;
void *(*g_object_class)(void *) = nullptr;
void (*g_get)(void *, void *, void *) = nullptr;
std::unordered_map<uint64_t, int> g_entries;  // (dex << 32 | form value) -> snapshot index

uint64_t slot(int dex, int32_t form) { return (uint64_t(uint32_t(dex)) << 32) | uint32_t(form); }

std::string normalize(const char *name) {
  std::string out;
  for (const char *p = name; p && *p; ++p)
    if (std::isalnum(static_cast<unsigned char>(*p))) out += char(std::tolower(static_cast<unsigned char>(*p)));
  return out;
}

int validate(const PgrMetaApi *md, void *owner, const char *name, uint64_t offset, uint32_t kind, uint32_t size,
             const char *type, void **out, void **type_class = nullptr) {
  PgrMetaFieldQuery q{};
  q.version = PGR_MD_QUERY_VERSION;
  q.size = sizeof(q);
  q.owner = owner;
  q.name = name;
  q.offset = offset;
  q.kind = kind;
  q.value_size = size;
  q.type_name = type;
  q.type_match = type ? PGR_MD_MATCH_EXACT : PGR_MD_MATCH_ANY;
  PgrMetaFieldResult r{};
  const int rc = md->validate_field(&q, &r);
  if (rc) return rc;
  *out = const_cast<void *>(r.field);
  if (type_class) *type_class = const_cast<void *>(r.type_class);
  return 0;
}

int resolve(const PgrMetaApi *md, const char *ns, const char *name, void **out) {
  PgrMetaClassQuery q{};
  q.version = PGR_MD_QUERY_VERSION;
  q.size = sizeof(q);
  q.name_space = ns;
  q.name = name;
  q.canonical_match = PGR_MD_MATCH_EXACT;
  PgrMetaClassResult r{};
  const int rc = md->resolve_class(&q, &r);
  if (!rc) *out = const_cast<void *>(r.klass);
  return rc;
}

int fail(const char *what) {
  g_state = -1;
  __android_log_print(ANDROID_LOG_WARN, "PGRazorPvp", "PvP keep unavailable (%s)", what);
  return -1;
}

int bind(uint64_t owner) {
  const auto *boot = static_cast<const PgrBootApi *>(pgrazor_runtime_bootstrap_api(PGR_BOOT_API_VERSION));
  const auto *md = static_cast<const PgrMetaApi *>(pgrazor_runtime_metadata_api(PGR_MD_API_VERSION));
  if (!boot || !md || boot->size != sizeof(PgrBootApi) || md->size != sizeof(PgrMetaApi)) return 0;
  PgrBootBinding snap{};
  snap.version = PGR_BOOT_BINDING_VERSION;
  snap.size = sizeof(snap);
  if (boot->snapshot(&snap) != PGR_BOOT_OK || !snap.handle) return 0;
  const auto &rt = snap.runtime;
  const auto &ex = snap.meta;
  void *alignment_enum = nullptr, *form_enum = nullptr;
  if (resolve(md, "Holoholo.Rpc", "PokemonProto", &g_class_pokemon) ||
      resolve(md, "Holoholo.Rpc", "PokemonDisplayProto", &g_class_display))
    return fail("classes");
  if (validate(md, g_class_pokemon, PGR_GAME_PVP_KEEP_CPP_POKEMONID__NAME, PGR_GAME_PVP_KEEP_CPP_POKEMONID__OFFSET,
               PGR_MD_FIELD_VALUE, 4, "System.Int32", &g_species) ||
      validate(md, g_class_pokemon, PGR_GAME_PVP_KEEP_CPP_POKEMONDISPLAY__NAME, PGR_GAME_PVP_KEEP_CPP_POKEMONDISPLAY__OFFSET,
               PGR_MD_FIELD_REF, 0, "Holoholo.Rpc.PokemonDisplayProto", &g_display) ||
      validate(md, g_class_pokemon, PGR_GAME_PVP_KEEP_CPP_CPMULTIPLIER__NAME, PGR_GAME_PVP_KEEP_CPP_CPMULTIPLIER__OFFSET,
               PGR_MD_FIELD_VALUE, 4, "System.Single", &g_cpm) ||
      validate(md, g_class_pokemon, PGR_GAME_PVP_KEEP_CPP_ADDITIONALCPMULTIPLIER__NAME,
               PGR_GAME_PVP_KEEP_CPP_ADDITIONALCPMULTIPLIER__OFFSET, PGR_MD_FIELD_VALUE, 4, "System.Single", &g_cpm_extra) ||
      validate(md, g_class_display, "Form", PGR_GAME_PVP_KEEP_CPP_FORM_OFFSET, PGR_MD_FIELD_ENUM_I32, 4, nullptr, &g_form, &form_enum) ||
      validate(md, g_class_display, "Alignment", PGR_GAME_PVP_KEEP_CPP_ALIGNMENT_OFFSET, PGR_MD_FIELD_ENUM_I32, 4, nullptr,
               &g_alignment, &alignment_enum) ||
      !form_enum || !alignment_enum)
    return fail("fields");
  auto fields = reinterpret_cast<void *(*)(void *, void **)>(::dlsym(snap.handle, "il2cpp_class_get_fields"));
  auto field_name = reinterpret_cast<const char *(*)(void *)>(::dlsym(snap.handle, "il2cpp_field_get_name"));
  auto flags = reinterpret_cast<int (*)(void *)>(ex.field_get_flags);
  auto static_value = reinterpret_cast<void (*)(void *, void *)>(rt.field_static_get_value);
  if (!fields || !field_name || !flags || !static_value) return fail("exports");
  // Enum literals are public static literal fields (0x8050).
  std::unordered_map<std::string, int32_t> forms;
  bool shadow = false;
  for (void *enum_class : {form_enum, alignment_enum}) {
    void *iter = nullptr;
    for (unsigned n = 0; n < 8192; ++n) {
      void *f = fields(enum_class, &iter);
      if (!f) break;
      if ((uint32_t(flags(f)) & 0x8050u) != 0x8050u) continue;
      int32_t value = 0;
      static_value(f, &value);
      const char *name = field_name(f);
      if (!name) continue;
      if (enum_class == alignment_enum) {
        if (!std::strcmp(name, "Shadow")) { g_shadow = value; shadow = true; }
      } else {
        forms.emplace(normalize(name), value);
      }
    }
  }
  if (!shadow || forms.size() < 100) return fail("enum literals");
  g_entries.clear();
  unsigned matched = 0;
  for (int i = 0; i < pgr_pvp::species_count(); ++i) {
    const int dex = pgr_pvp::species_dex(i);
    const std::string key = pgr_pvp::species_key(i);
    auto hit = forms.find(key);
    if (hit == forms.end()) hit = forms.find(key + "normal");
    if (hit != forms.end() && g_entries.emplace(slot(dex, hit->second), i).second) ++matched;
    if (pgr_pvp::species_base(i)) g_entries.emplace(slot(dex, 0), i);
  }
  g_object_class = reinterpret_cast<void *(*)(void *)>(rt.object_get_class);
  g_get = reinterpret_cast<void (*)(void *, void *, void *)>(rt.field_get_value);
  g_owner = owner;
  g_state = 1;
  __android_log_print(ANDROID_LOG_INFO, "PGRazorPvp", "PvP keep ready (%u of %d forms matched)", matched,
                      pgr_pvp::species_count());
  return 1;
}

bool settings(pgr_pvp::Settings *out) {
  *out = pgr_pvp::Settings{};
  const auto *tr = static_cast<const PgrRuntimeTransportApi *>(pgrazor_runtime_transport_api(1));
  char text[41]{};
  uint32_t length = 0;
  return tr && tr->get_string(16, text, sizeof(text), &length) == 0 && pgr_pvp::parse(text, length, out);
}

void write(char *text, uint32_t capacity, const char *value) {
  if (text && capacity) std::snprintf(text, capacity, "%s", value);
}

// The caller holds g_mutex. Binds on first use; false when this Pokemon cannot be looked up.
enum Lookup { LOOKUP_UNAVAILABLE, LOOKUP_UNKNOWN_FORM, LOOKUP_OK };
struct Facts { int entry = -1; bool shadow = false; double multiplier = 0; int species = 0; };
Lookup lookup(void *pokemon, uint64_t owner, Facts *out) {
  if (!pokemon || !owner || owner != uint64_t(gettid()) || (g_owner && g_owner != owner)) return LOOKUP_UNAVAILABLE;
  if (g_state == 0) bind(owner);
  if (g_state != 1 || g_object_class(pokemon) != g_class_pokemon) return LOOKUP_UNAVAILABLE;
  void *display = nullptr;
  g_get(pokemon, g_display, &display);
  if (!display || g_object_class(display) != g_class_display) return LOOKUP_UNAVAILABLE;
  int32_t species = 0, form = 0, alignment = 0;
  float cpm = 0, extra = 0;
  g_get(pokemon, g_species, &species);
  g_get(display, g_form, &form);
  g_get(display, g_alignment, &alignment);
  g_get(pokemon, g_cpm, &cpm);
  g_get(pokemon, g_cpm_extra, &extra);
  out->species = species;
  out->shadow = alignment == g_shadow;
  out->multiplier = double(cpm) + double(extra);
  auto entry = g_entries.find(slot(species, form));
  if (entry == g_entries.end()) return LOOKUP_UNKNOWN_FORM;  // form outside the snapshot
  out->entry = entry->second;
  return LOOKUP_OK;
}

// The IV format for this screen from "S2|<storage>|<encounter>|<details>".
bool pvp_format(int screen) {
  const auto *tr = static_cast<const PgrRuntimeTransportApi *>(pgrazor_runtime_transport_api(1));
  char text[16]{};
  uint32_t length = 0;
  return tr && tr->get_string(14, text, sizeof(text), &length) == 0 && length == 8 && !std::memcmp(text, "S2|", 3) &&
         screen >= 0 && screen <= 2 && text[3 + 2 * screen] == '4';
}

// Best ranks by species entry, shadow, IVs and level; cleared when full.
struct Cached { int rank[3], species[3]; };
std::unordered_map<uint64_t, Cached> g_best;
}  // namespace

extern "C" int pgr_pvp_keep(void *pokemon, int attack, int defense, int stamina, int species, uint64_t owner,
                            char *text, uint32_t capacity) {
  write(text, capacity, "");
  pgr_pvp::Settings config{};
  if (!settings(&config) || (!config.rank[0] && !config.rank[1] && !config.rank[2])) return 0;
  std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
  Facts facts{};
  const Lookup found = lock.owns_lock() ? lookup(pokemon, owner, &facts) : LOOKUP_UNAVAILABLE;
  if (found == LOOKUP_UNAVAILABLE || (found == LOOKUP_OK && facts.species != species)) {
    write(text, capacity, "pvp-check-unavailable");
    return 2;
  }
  if (found == LOOKUP_UNKNOWN_FORM) return 0;
  pgr_pvp::Keep why{};
  if (!pgr_pvp::keep(facts.entry, facts.shadow, attack, defense, stamina, facts.multiplier, config, &why)) return 0;
  static const char *const tags[] = {"GL", "UL", "ML"};
  if (text && capacity)
    std::snprintf(text, capacity, "PvP %s #%d %s", tags[why.league], why.rank, pgr_pvp::species_name(why.species));
  return 1;
}

extern "C" bool pgr_pvp_best(void *pokemon, int screen, int attack, int defense, int stamina, uint64_t owner,
                             PgrPvpBest *out) {
  if (!out) return false;
  *out = PgrPvpBest{};
  if (!pvp_format(screen)) return false;
  std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
  Facts facts{};
  if (!lock.owns_lock() || lookup(pokemon, owner, &facts) != LOOKUP_OK) return false;
  if (attack < 0 || attack > 15 || defense < 0 || defense > 15 || stamina < 0 || stamina > 15) return false;
  // The level only matters for the cap check; its multiplier in 1/10000 steps identifies it.
  const uint64_t level = uint64_t(facts.multiplier > 0 && facts.multiplier < 2 ? facts.multiplier * 10000 + 0.5 : 0);
  const uint64_t key = (uint64_t(facts.entry) << 32) | (uint64_t(facts.shadow) << 31) | (level << 12) |
                       uint64_t(attack << 8 | defense << 4 | stamina);
  auto hit = g_best.find(key);
  if (hit == g_best.end()) {
    // Every league and any rank, so the display can show where the IVs stand.
    pgr_pvp::Keep leagues[pgr_pvp::kLeagues];
    pgr_pvp::best(facts.entry, facts.shadow, attack, defense, stamina, facts.multiplier, 150, leagues);
    Cached c{};
    for (int i = 0; i < 3; ++i) {
      c.rank[i] = leagues[i].league >= 0 ? leagues[i].rank : 0;
      c.species[i] = leagues[i].species;
    }
    if (g_best.size() >= 8192) g_best.clear();
    hit = g_best.emplace(key, c).first;
  }
  bool any = false;
  for (int i = 0; i < 3; ++i) {
    out->rank[i] = hit->second.rank[i];
    if (!out->rank[i]) continue;
    any = true;
    if (hit->second.species[i] != facts.entry)
      std::snprintf(out->species[i], sizeof(out->species[i]), "%s", pgr_pvp::species_name(hit->second.species[i]));
  }
  return any;
}
