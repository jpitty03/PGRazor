#include "showcase_tags.h"
#include <cassert>
#include <cstdio>
using namespace pgr_showcase;
void test_showcase_tags() {
  TagOwnership ownership;
  std::string wire="T1|7|18446744073709551615";
  for(unsigned i=1;i<kTypes;++i)wire+="|0";
  assert(parse_tag_ownership(wire,ownership));
  assert(ownership.revision==7&&ownership.ids[0]==UINT64_MAX);
  std::string command;
  assert(tag_ownership_command(ownership,1,9,command));
  assert(command=="T1|7|1|0|9");
  assert(!tag_ownership_command(ownership,1,UINT64_MAX,command)&&command.empty());
  assert(!parse_tag_ownership(wire+"|0",ownership));
  assert(ownership.revision==0&&ownership.ids[0]==0);
  assert(!parse_tag_ownership("T0",ownership));
  assert(!parse_tag_ownership("T1|0|0",ownership));
  std::string tail;for(unsigned i=1;i<kTypes;++i)tail+="|0";
  for(const char* bad:{"18446744073709551616","-1","01","+1","1.0",""})
    assert(!parse_tag_ownership(std::string("T1|0|")+bad+tail,ownership));
  assert(!parse_tag_ownership("T1|9223372036854775808|0"+tail,ownership));
  assert(!parse_tag_ownership("T1|0|9|9"+tail.substr(2),ownership));
  assert(parse_tag_ownership("T2|3|10|0"+tail,ownership)&&ownership.pending_create==10&&ownership.revision==3);
  assert(!tag_creation_command(ownership,9,command));
  assert(!tag_ownership_command(ownership,8,700,command));
  assert(tag_ownership_command(ownership,9,700,command)&&command=="T1|3|9|0|700");
  assert(!parse_tag_ownership("T2|3|0|0"+tail,ownership));
  assert(!parse_tag_ownership("T2|3|19|0"+tail,ownership));
  assert(!parse_tag_ownership("T2|3|01|0"+tail,ownership));
  assert(parse_tag_ownership("T1|2|0"+tail,ownership));
  assert(tag_creation_command(ownership,9,command)&&command=="B1|2|9|0");
  Selection winners;winners.types[0]={2};winners.types[1]={1,2};
  std::array<uint64_t,kTypes> owned{};owned[0]=101;owned[1]=102;
  std::vector<TagMembership> cache={{1,101},{1,102},{2,999}};
  TagPlan plan;
  assert(plan_tags(winners,owned,cache,plan));
  assert((plan.add==std::vector<TagMembership>{{2,101},{2,102}}));
  assert((plan.remove==std::vector<TagMembership>{{1,101}}));
  assert(!tag_removals_ready(plan,cache));
  cache.push_back({2,101});assert(!tag_removals_ready(plan,cache));
  cache.push_back({2,102});assert(tag_removals_ready(plan,cache));
  // The dual-type losing one bucket keeps its other tag; unrelated tag999 survives.
  assert(plan.remove[0].tag!=102&&plan.remove[0].tag!=999);
  cache.erase(cache.begin());assert(plan_tags(winners,owned,cache,plan));
  assert(plan.add.empty()&&plan.remove.empty());
  // Newly enabled types require owned tag creation and confirmation, not adoption.
  owned[0]=0;assert(plan_tags(winners,owned,cache,plan));
  assert(plan.create_types==1&&!tag_removals_ready(plan,cache));
  assert(plan.remove.empty()); // no ownership of old tag101
  owned[0]=103;assert(plan_tags(winners,owned,cache,plan));
  assert((plan.add==std::vector<TagMembership>{{2,103}}));
  assert(!tag_removals_ready(plan,cache));
  cache.push_back({2,103});assert(tag_removals_ready(plan,cache));
  // Duplicate owned IDs / malformed selection cannot produce a partial delta.
  owned[0]=102;assert(!plan_tags(winners,owned,cache,plan));
  assert(plan.add.empty()&&plan.remove.empty()&&!plan.create_types);
  owned[0]=103;winners.types[0]={2,2};assert(!plan_tags(winners,owned,cache,plan));
  winners.types[0]={0};assert(!plan_tags(winners,owned,cache,plan));
  winners.types[0]={1,2,3,4};assert(!plan_tags(winners,owned,cache,plan));
  winners.types[0]={2};cache.push_back({2,103});assert(!plan_tags(winners,owned,cache,plan));
  cache.pop_back();cache.push_back({0,103});assert(!plan_tags(winners,owned,cache,plan));
  cache.assign(kMaxTagMemberships+1,{1,103});assert(!plan_tags(winners,owned,cache,plan));
  // Empty inventory removes only owned memberships and creates no empty tags.
  winners={};cache={{1,103},{1,999}};assert(plan_tags(winners,owned,cache,plan));
  assert(!plan.create_types&&plan.add.empty());
  assert((plan.remove==std::vector<TagMembership>{{1,103}}));
  std::puts("PASS Showcase tag delta: replacement, dual types, creation, cache confirmation, ownership, malformed/bounded input");
}
