#include "remote_gym_policy.h"
#include <cassert>
#include <cstring>
#include <cstdio>
int main(){using namespace pgr_remote_gym;Command c;
 auto parse_text=[&](const char *s){return parse(s,strlen(s),c);};
 assert(parse_text("1|9223372036854775807|1|0|1000|1|50|0"));
 assert(c.token==9223372036854775807ULL&&c.razz_reserve==1000&&c.pinap_enabled);
 for(const char *s:{"1|9223372036854775808|1|0|0|0|0|0","1|0|1|0|0|0|0|0","1|1|2|0|0|0|0|0","1|1|1|-1|0|0|0|0","1|1|1|1001|0|0|0|0","1|1|1|0|0|2|0|0","1|1|1|0|0|0|0|2","1|1|1|0|0|0|0","1|1|1|0|0|0|0|0|0","1|1|1|0|0|0|0|0\n"})assert(!parse_text(s));
 assert(parse_text("1|0|0|0|0|0|0|0"));
 assert(parse_text("1|123|1|10|20|0|30|0"));
 assert(select_berry(c,11,21,31)==703);assert(select_berry(c,10,21,31)==701);
 assert(select_berry(c,10,20,31)==0);c.pinap_enabled=true;assert(select_berry(c,10,20,31)==705);
 assert(select_berry(c,10,20,30)==0);assert(select_berry(c,-1,-1,-1)==0);
 assert(reserve_for(c,703)==10&&reserve_for(c,701)==20&&reserve_for(c,705)==30);
 assert(reserve_for(c,706)==std::numeric_limits<int>::max()&&reserve_for(c,708)==std::numeric_limits<int>::max());
 FeedHistory h;assert(!h.feed_allowed(0,1));
 for(unsigned i=0;i<10;i++)assert(h.feed_attempt(42,1+i));
 assert(!h.feed_attempt(42,11));assert(h.feed_allowed(42,10+FeedHistory::kWindowMs));
 h=FeedHistory{};for(unsigned i=1;i<=10;i++)assert(h.feed_attempt(i,100));assert(!h.feed_attempt(11,100));
 h.refuse(1,101);assert(!h.feed_allowed(1,102));assert(h.feed_allowed(1,101+FeedHistory::kWindowMs));
 std::puts("PASS remote_gym policy: command bounds, reserves, berry order, attempt limits");
}
