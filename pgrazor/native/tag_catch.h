#pragma once
#include <cstdint>
#include <string>
struct PgrSharedMetadataOutput;
// VGP+ tag rules: candy families of the selected Pokemon storage tags for "Catch only" and "Candy species",
// plus the shared "rare" criteria. Config from Java (TagCatchSettings.config):
//   "2;active;alsoRare;rareMask;ivMin;rareBerry;candyBerry;<ultra-rare species csv>;<catch tags>;<candy tags>;<fallback berries csv>"
// with tag names joined by '\t' (';' and '\t' are stripped from names in Java). The optional
// 11th field is the player's ordered berry list used when the configured berry is unavailable.
enum { PGR_TAG_RARE_CLASS = 1, PGR_TAG_RARE_SHINY = 2, PGR_TAG_RARE_SHUNDO = 4, PGR_TAG_RARE_HUNDO = 8,
       PGR_TAG_RARE_NUNDO = 16, PGR_TAG_RARE_IV = 32, PGR_TAG_RARE_COSTUME = 64, PGR_TAG_RARE_CARD = 128,
       PGR_TAG_RARE_SPECIES = 256 };
struct PgrTagRules {
  bool filter;        // "Catch only" tags are in use
  bool tagged_catch;  // species is in a "Catch only" tag family
  bool candy;         // species is in a "Candy species" tag family
  bool rare, also_rare;
  int rare_berry, candy_berry;
};
void pgr_tag_catch_tick(uint64_t tid, const PgrSharedMetadataOutput *metadata);
void pgr_tag_catch_config(const char *text);
// The encountered PokemonProto (with IVs) from the map Pokemon's EncounterOutProto, or null. Owner thread.
void *pgr_encounter_pokemon(void *wild);
// "\n"-joined names of the player's own tags, as last read (empty until the first read).
int pgr_tag_catch_tags(char *out, int cap);
// Before an encounter (owner thread). species <= 0 asks the state: 0 not configured (skip everything),
// 1 no tag filter, 2 tag filter active (or tags still loading: defer, do not skip).
// species > 0: 1 when tags or "also rare" want it encountered, 2 while tags load (defer), else 0.
int pgr_tag_catch_prefilter(int species);
// Bumped whenever the settings or the refreshed tag membership change, so VGP forgets skips made
// under the old rules. pgr_tag_catch_ready() says whether the refreshed membership is usable.
uint32_t pgr_tag_catch_revision();
bool pgr_tag_catch_ready();
// Manual encounters: iv_display reports each catch screen's map Pokemon (owner thread); the berry the tag
// rules want for it is then readable anywhere: up to three berry ids in fallback order (rare berry, Golden Razz,
// candy berry), packed 10 bits each from the low end; 0 for none.
extern "C" void pgr_tag_catch_note_encounter(void *map_pokemon);
int pgr_tag_catch_manual_berries();
// After an encounter (owner thread). -1 until the settings arrive or while the tags load (skip), else 0.
int pgr_tag_rules(void *pokemon, int species, bool shiny, int atk, int def, int sta, PgrTagRules *out);
