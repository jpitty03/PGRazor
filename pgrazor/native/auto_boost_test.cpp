#include "auto_boost.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>
using namespace pgr_auto_boost;
int main(){
 const Config both=parse("B1|1|1"),egg=parse("B1|1|0"),star=parse("B1|0|1"),off=parse("B1|0|0");
 assert(both.valid&&both.egg&&both.star);assert(egg.valid&&egg.egg&&!egg.star);
 assert(star.valid&&!star.egg&&star.star);assert(off.valid&&!off.egg&&!off.star);
 const char *const bads[]={nullptr,"","B1|1","B1|2|0","B1|1|0|1","B2|1|1","B1|1|1x","b1|1|1"};
 for(const char *bad:bads){const Config c=parse(bad);assert(!c.valid&&!c.egg&&!c.star);}
 assert(should_use(true,false,3,100,100));
 assert(!should_use(false,false,3,100,0));   // switch off
 assert(!should_use(true,true,3,100,0));     // already running
 assert(!should_use(true,false,0,100,0));    // none in the bag
 assert(!should_use(true,false,3,99,100));   // still settling or backing off
 std::puts("PASS auto boost: config parsing and use decision (wanted, running, stock, wait)");
 return 0;
}
