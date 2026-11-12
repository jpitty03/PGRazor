#pragma once
#include "showcase_policy.h"
#include <set>
#include <limits>
#include <string>
#include <string_view>

namespace pgr_showcase {
struct TagOwnership {
  uint64_t revision=0;
  std::array<uint64_t,kTypes> ids{};
  unsigned pending_create=0; // 0, or canonical type index + 1; durable pre-RPC journal
};
// Transport slot24 mirrors ShowcaseTagStore's canonical decimal encoding.
// T0, duplicate ownership, signed/overflowing IDs and partial frames fail closed.
inline bool parse_tag_ownership(std::string_view wire,TagOwnership& out) {
  out={};
  const bool journal=wire.substr(0,3)=="T2|";
  if(wire.size()>512 || (!journal&&wire.substr(0,3)!="T1|"))return false;
  wire.remove_prefix(3);TagOwnership parsed;
  std::set<uint64_t> seen;
  const unsigned last=kTypes+(journal?1:0);
  for(unsigned field=0;field<=last;++field) {
    size_t end=wire.find('|');
    if((field<last)==(end==std::string_view::npos))return false;
    auto token=wire.substr(0,end);
    if(token.empty()||token.size()>20||(token.size()>1&&token.front()=='0'))return false;
    uint64_t value=0;
    for(char c:token) {
      if(c<'0'||c>'9')return false;
      unsigned digit=unsigned(c-'0');
      if(value>(std::numeric_limits<uint64_t>::max()-digit)/10)return false;
      value=value*10+digit;
    }
    if(!field) {
      if(value>uint64_t(std::numeric_limits<int64_t>::max()))return false;
      parsed.revision=value;
    } else if(journal&&field==1) {
      if(!value||value>kTypes)return false;
      parsed.pending_create=unsigned(value);
    } else {
      if(value&&!seen.insert(value).second)return false;
      parsed.ids[field-(journal?2:1)]=value;
    }
    if(end!=std::string_view::npos)wire.remove_prefix(end+1);
  }
  out=parsed;return true;
}
inline bool tag_ownership_command(const TagOwnership& before,unsigned type,uint64_t new_id,std::string& out) {
  out.clear();
  if(type>=kTypes||!new_id||before.revision>=uint64_t(std::numeric_limits<int64_t>::max())||
    (before.pending_create&&before.pending_create!=type+1))return false;
  std::set<uint64_t> seen;
  for(unsigned i=0;i<kTypes;++i) {
    if(before.ids[i]&&!seen.insert(before.ids[i]).second)return false;
    if(i!=type&&before.ids[i]==new_id)return false;
  }
  out="T1|"+std::to_string(before.revision)+"|"+std::to_string(type)+"|"+
      std::to_string(before.ids[type])+"|"+std::to_string(new_id);
  return true;
}
inline bool tag_creation_command(const TagOwnership& before,unsigned type,std::string& out) {
  out.clear();if(type>=kTypes||before.pending_create||before.revision>=uint64_t(std::numeric_limits<int64_t>::max()))return false;
  std::set<uint64_t> seen;for(auto id:before.ids)if(id&&!seen.insert(id).second)return false;
  out="B1|"+std::to_string(before.revision)+"|"+std::to_string(type)+"|"+std::to_string(before.ids[type]);return true;
}
// Ownership comes from account-scoped, persisted tag IDs. A matching display
// name alone never grants permission to remove a user's tag membership.
struct TagMembership {
  uint64_t pokemon=0, tag=0;
  bool operator<(const TagMembership& b)const {
    return pokemon!=b.pokemon?pokemon<b.pokemon:tag<b.tag;
  }
  bool operator==(const TagMembership& b)const{return pokemon==b.pokemon&&tag==b.tag;}
};
struct TagPlan {
  uint32_t create_types=0;
  std::vector<TagMembership> add, remove, required;
};
constexpr size_t kMaxTagMemberships=kMaxPokemon*2;

// Pure delta planning: no RPC or transfer authorization. A missing owned tag is
// created only if that type has a winner. The host must persist the returned tag
// ID and replan after cache confirmation. Never adopt an existing tag by name.
inline bool plan_tags(const Selection& winners,const std::array<uint64_t,kTypes>& owned,
                      const std::vector<TagMembership>& observed,TagPlan& out) {
  out={};
  if(observed.size()>kMaxTagMemberships)return false;
  std::set<uint64_t> ids;
  for(auto id:owned)if(id && !ids.insert(id).second)return false;
  std::set<TagMembership> current,desired;
  for(const auto& m:observed) {
    if(!m.pokemon || !m.tag)return false;
    // Other tags may appear in the cache snapshot; preserve them verbatim.
    if(ids.count(m.tag) && !current.insert(m).second)return false;
  }
  TagPlan result;
  for(unsigned type=0;type<kTypes;++type) {
    const auto& group=winners.types[type];
    if(group.size()>3)return false;
    std::set<uint64_t> unique;
    for(auto pokemon:group) {
      if(!pokemon || !unique.insert(pokemon).second)return false;
      if(owned[type])desired.insert({pokemon,owned[type]});
      else result.create_types|=uint32_t(1)<<type;
    }
  }
  for(const auto& m:desired) {
    result.required.push_back(m);
    if(!current.count(m))result.add.push_back(m);
  }
  for(const auto& m:current)if(!desired.count(m))result.remove.push_back(m);
  out=std::move(result);return true;
}

// All winners must be present in the latest confirmed cache before any obsolete
// membership is removed. RPC success alone is insufficient. The host must also
// recheck account/config/ranking generation and lease before each mutation.
inline bool tag_removals_ready(const TagPlan& plan,const std::vector<TagMembership>& confirmed) {
  if(plan.create_types || confirmed.size()>kMaxTagMemberships)return false;
  std::set<TagMembership> seen;
  for(const auto& m:confirmed) {
    if(!m.pokemon || !m.tag || !seen.insert(m).second)return false;
  }
  for(const auto& m:plan.required)if(!seen.count(m))return false;
  return true;
}
}
