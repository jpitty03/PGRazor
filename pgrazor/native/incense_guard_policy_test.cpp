#include "incense_guard_policy.h"
#include <cassert>
#include <cstring>
int main(){using namespace pgr_incense_guard;Settings s;
 const char *good="1|1234|2|0|0|1|5|1";assert(parse(good,strlen(good),s)&&s.active);
 const char *off="1|1234|2|0|0|1|5|0";assert(parse(off,strlen(off),s)&&!s.active);
 const char *bad="1|1234|2|0|0|1|5|2";assert(!parse(bad,strlen(bad),s));
 assert(bird(144,2801,2801,2800,2799));assert(!bird(144,958,2801,2800,2799));
 assert(!bird(145,2801,2801,2800,2799));assert(!bird(146,0,2801,2800,2799));
}
