#include "rocket_actions.h"
#include <mutex>
#include <ctime>
#include <unistd.h>
#include <cstdint>
static_assert(sizeof(pgr_rocket_actions_api_v1)==48,"Rocket action table ABI");
namespace {
std::mutex lock;
uint64_t owner=0,generation=1,ticket=0,deadline=0,next=0;
uint32_t mask=0;
bool active=false,prompt=false,shield_used=false,exhausted=false;
uint64_t now_ms(){timespec ts{};if(clock_gettime(CLOCK_BOOTTIME,&ts)!=0)return 0;
 return uint64_t(ts.tv_sec)*1000+uint64_t(ts.tv_nsec)/1000000;}
bool thread(uint64_t tid){return tid && tid==uint64_t(gettid()) && (!owner || owner==tid);}
void revoke(){ticket=0;if(generation==UINT64_MAX){exhausted=true;mask=0;deadline=0;}else ++generation;}
int update(uint32_t value,uint64_t remaining,uint64_t tid){
 std::lock_guard<std::mutex> guard(lock);
 // Only verified Unity heartbeat may arm. Foreign callers can only revoke.
 if(!thread(tid)){mask=0;deadline=0;revoke();return 0;}
 if(!owner)owner=tid;
 const uint64_t now=now_ms();value&=7;
 if(value!=mask || !remaining || !now || now>=deadline)revoke();
 if(exhausted)return 0;
 mask=value;deadline=remaining&&now?now+(remaining>2500?2500:remaining):0;
 return 1;
}
int observe(uint32_t kind,uint64_t tid){
 std::lock_guard<std::mutex> guard(lock);
 if(!thread(tid)||!owner||kind>PGR_ROCKET_SHIELD_USED)return 0;
 revoke();
 if(kind==PGR_ROCKET_ENTER||kind==PGR_ROCKET_EXIT){active=kind==PGR_ROCKET_ENTER;prompt=false;shield_used=false;next=0;}
 else if(kind==PGR_ROCKET_PROMPT_OPEN){if(active){prompt=true;shield_used=false;}}
 else if(kind==PGR_ROCKET_PROMPT_CLOSE)prompt=false;
 else if(kind==PGR_ROCKET_SHIELD_USED)shield_used=true;
 return 1;
}
uint64_t begin(uint64_t tid){
 std::lock_guard<std::mutex> guard(lock);
 if(!thread(tid)||!owner)return 0;
 const uint64_t now=now_ms();revoke();
 if(exhausted||!active||!mask||!now||now>=deadline||now<next)return 0;
 return ticket=generation;
}
int claim(uint64_t token,uint32_t action,uint64_t tid){
 std::lock_guard<std::mutex> guard(lock);
 if(!thread(tid)||!owner||!token||token!=ticket)return 0;
 const uint64_t now=now_ms();
 // Consume even rejected claims. The caller may only invoke a game method on 1.
 ticket=0;
 if(exhausted||!active||!now||now>=deadline||now<next)return 0;
 uint32_t required=action==PGR_ROCKET_QUICK?1:
  (action==PGR_ROCKET_LEGACY_CHARGE||action==PGR_ROCKET_VNEXT_CHARGE)?2:
  action==PGR_ROCKET_SHIELD?4:0;
 if(!required||!(mask&required))return 0;
 if(action==PGR_ROCKET_SHIELD){if(!prompt||shield_used)return 0;shield_used=true;}
 next=now+(action==PGR_ROCKET_QUICK?150:action==PGR_ROCKET_LEGACY_CHARGE?500:action==PGR_ROCKET_VNEXT_CHARGE?600:250);
 return 1;
}
int stop(uint64_t){
 std::lock_guard<std::mutex> guard(lock);
 // Any thread may revoke; keep the Unity owner bound to prevent takeover.
 mask=0;deadline=0;active=false;prompt=false;shield_used=false;next=0;revoke();return 1;
}
const pgr_rocket_actions_api_v1 api={1,sizeof(api),update,observe,begin,claim,stop};
}
const void *pgrazor_rocket_actions_api(uint32_t version){return version==1?&api:nullptr;}
