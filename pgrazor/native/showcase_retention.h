#pragma once
#include "showcase_tags.h"
#include <map>

namespace pgr_showcase {
// Native value state only. The host owns account/config/lease checks and managed
// snapshot acquisition. A rescan never turns an existing box into a cleanup job.
class Retention {
 public:
  Selection winners;
  std::map<uint64_t,Row> inventory;
  std::set<uint64_t> waiting_catches, release_candidates;
  std::set<uint64_t> background_catches, background_releases;
  bool initialized=false;

  bool caught(uint64_t id,bool honor_keep_all=true) {
    if(!id)return false;
    if(inventory.count(id))return true; // repeated catch notification
    if(waiting_catches.size()>=kMaxPokemon)return false;
    waiting_catches.insert(id);
    if(!honor_keep_all)background_catches.insert(id);
    return true;
  }
  bool refresh(const std::vector<Row>& rows,unsigned slots) {
    Selection next;
    if(!select(rows,slots,next))return false;
    std::map<uint64_t,Row> current;
    for(const auto& row:rows)current.emplace(row.id,row);
    // Infer replacement only across an otherwise unchanged inventory snapshot.
    // Evolution, trading, imports, manual transfers or scoring-rule changes do
    // not authorize a historical keeper to be deleted.
    bool only_catches=initialized;
    for(const auto& old:inventory) {
      auto it=current.find(old.first);
      if(it==current.end()||!same(old.second,it->second))only_catches=false;
    }
    bool added_catch=false;
    for(const auto& row:current)if(!inventory.count(row.first)) {
      if(!waiting_catches.count(row.first))only_catches=false;
      else added_catch=true;
    }
    if(only_catches&&added_catch)
      for(auto id:displaced(winners,next))release_candidates.insert(id);
    // Only observed successful catches are added here. Enabling and lowering
    // the slots setting do not enqueue the rest of storage.
    for(auto it=waiting_catches.begin();it!=waiting_catches.end();) {
      if(current.count(*it)) {
        if(!next.contains(*it)){
          release_candidates.insert(*it);
          if(background_catches.count(*it))background_releases.insert(*it);
        }
        background_catches.erase(*it);
        it=waiting_catches.erase(it);
      }else ++it;
    }
    for(auto it=release_candidates.begin();it!=release_candidates.end();)
      if(!current.count(*it)||next.contains(*it)){background_releases.erase(*it);it=release_candidates.erase(it);}else ++it;
    winners=std::move(next);inventory=std::move(current);initialized=true;return true;
  }
  // A missing/unranked Pokemon is always protected. A pending caught ID also
  // invalidates the old ranking until its inventory update is observed.
  bool keep(uint64_t id)const {
    return !initialized||!waiting_catches.empty()||!inventory.count(id)||winners.contains(id);
  }
  bool release_ready(uint64_t id,const TagPlan& plan,const std::vector<TagMembership>& cache)const {
    if(keep(id)||!release_candidates.count(id)||!tag_removals_ready(plan,cache))return false;
    for(const auto& obsolete:plan.remove)if(obsolete.pokemon==id)return false;
    return true;
  }
 private:
  static bool same(const Row& a,const Row& b) {
    return a.id==b.id&&a.created==b.created&&a.species==b.species&&a.types==b.types&&a.scores==b.scores;
  }
};
}
