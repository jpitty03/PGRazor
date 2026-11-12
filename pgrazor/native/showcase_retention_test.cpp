#include "showcase_retention.h"
#include <cassert>
#include <cstdio>
using namespace pgr_showcase;
Row row(uint64_t id,double score,uint32_t types=1){Row r;r.id=id;r.created=id;r.species=1;r.types=types;for(unsigned i=0;i<18;++i)if(types&(1u<<i))r.scores[i]=score;return r;}
void test_showcase_retention(){
 Retention s;assert(s.keep(1));
 assert(s.refresh({row(1,900),row(2,800)},1));assert(s.keep(1)&&!s.keep(2));assert(s.release_candidates.empty());
 assert(s.caught(3));assert(s.keep(2)); // unknown catch invalidates stale ranking
 assert(s.refresh({row(1,900),row(2,800),row(3,1000)},1));
 assert(s.keep(3)&&!s.keep(1)&&s.release_candidates.count(1)&&!s.release_candidates.count(2));
 std::array<uint64_t,18> tags{};tags[0]=77;TagPlan plan;
 assert(plan_tags(s.winners,tags,{{1,77}},plan));assert(!s.release_ready(1,plan,{{1,77}}));
 assert(plan_tags(s.winners,tags,{{1,77},{3,77}},plan));assert(!s.release_ready(1,plan,{{1,77},{3,77}}));
 assert(plan_tags(s.winners,tags,{{3,77}},plan));assert(s.release_ready(1,plan,{{3,77}}));
 assert(s.caught(3));assert(s.waiting_catches.empty());
 assert(s.caught(4));assert(s.refresh({row(1,900),row(2,800),row(3,1000),row(4,700)},1));assert(s.release_candidates.count(4));
 Retention enabled;assert(enabled.refresh({row(1,900),row(2,800),row(3,700)},3));
 assert(enabled.refresh({row(1,900),row(2,800),row(3,700)},1));assert(enabled.release_candidates.empty());
 Retention dual;assert(dual.refresh({row(1,900,3),row(2,800)},1));assert(dual.caught(3));
 assert(dual.refresh({row(1,900,3),row(2,800),row(3,1000)},1));assert(dual.keep(1)&&dual.keep(3)&&dual.release_candidates.empty());
 Retention changed;assert(changed.refresh({row(1,900)},1));assert(changed.caught(2));
 assert(changed.refresh({row(1,100),row(2,1000)},1));assert(!changed.release_candidates.count(1));
 Retention imported;assert(imported.refresh({row(1,900)},1));assert(imported.caught(2));
 assert(imported.refresh({row(1,900),row(2,1000),row(3,800)},1));assert(!imported.release_candidates.count(1));
 assert(!s.caught(0));assert(s.keep(999));
 Retention origins;assert(origins.refresh({row(1,900)},1));
 assert(origins.caught(2,false)&&origins.caught(3,true));
 assert(origins.refresh({row(1,900),row(2,500),row(3,600)},1));
 assert(origins.background_catches.empty()&&origins.background_releases.count(2)&&!origins.background_releases.count(3));
 assert(origins.caught(4,false));assert(origins.refresh({row(1,900),row(2,500),row(3,600),row(4,1000)},1));
 assert(origins.release_candidates.count(1)&&!origins.background_releases.count(1)&&!origins.background_releases.count(4));
 assert(origins.refresh({row(1,900),row(3,600),row(4,1000)},1));assert(!origins.background_releases.count(2));
 std::puts("PASS Showcase retention: catch-only replacement, no initial cleanup, dual-type union, pending inventory and confirmed tags");
}
