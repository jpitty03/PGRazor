#include "research_policy.h"

#include <cassert>
#include <cstdio>
#include <string>

using namespace pgr_research;
namespace {
bool parses(const std::string &s,Config &c){return parse(s.data(),s.size(),c);}
Quest quest(int progress,int target,const char *templ="CHALLENGE_CATCH_EASY",bool bonus=false){
 Quest q;q.context=2;q.status=1;q.progress=progress;q.target=target;q.bonus=bonus;q.id=std::string(templ)+":1";q.templ=templ;return q;
}
}
int main(){
 Config c;
 assert(parses("R2|0|0|0|",c)&&c.valid&&!c.remove&&!c.claim&&c.templates.empty());
 assert(parses("R2|1|1|1|B_TWO,A_ONE",c)&&c.remove&&c.claim&&c.catch_encounters&&c.templates.size()==2&&c.templates.count("A_ONE"));
 assert(parses("R2|0|0|1|",c)&&c.catch_encounters&&!c.claim);
 for(const char *bad:{"","R2|","R1|1|1|","R2|2|0|0|","R2|1|1|1|A,","R2|1|1|1|,A","R2|1|1|1|A,,B","R2|1|1|1|A,A","R2|1|1|1|lower","R2|1|1|1|A B","R2|1|1|1","R2|1|1|2|"})
  assert(!parses(bad,c)&&!c.valid);
 assert(!parses("R2|1|1|0|"+std::string(kMaxTemplateBytes+1,'A'),c));
 std::string many="R2|1|1|0|";for(size_t i=0;i<=kMaxTemplates;++i)many+=(i?",T":"T")+std::to_string(i);
 assert(!parses(many,c));

 const int challenge=2,active=1;
 assert(parses("R2|1|1|0|CHALLENGE_CATCH_EASY",c));
 assert(decide(quest(5,5),c,challenge,active)==CLAIM);                 // met goal claims, never removes
 assert(decide(quest(2,5),c,challenge,active)==REMOVE);                // selected unfinished task
 assert(decide(quest(2,5,"OTHER_TASK"),c,challenge,active)==NONE);     // unselected task kept
 assert(decide(quest(2,5,"CHALLENGE_CATCH_EASY",true),c,challenge,active)==NONE);  // gym bonus kept
 Quest special=quest(2,5);special.context=1;assert(decide(special,c,challenge,active)==NONE);
 Quest done=quest(5,5);done.status=2;assert(decide(done,c,challenge,active)==NONE);  // already claimed
 Quest no_goal=quest(0,0,"OTHER_TASK");assert(decide(no_goal,c,challenge,active)==NONE);
 assert(parses("R2|0|1|0|",c));
 assert(decide(quest(5,5),c,challenge,active)==CLAIM&&decide(quest(2,5),c,challenge,active)==NONE);
 assert(parses("R2|1|0|0|CHALLENGE_CATCH_EASY",c));
 assert(decide(quest(5,5),c,challenge,active)==NONE&&decide(quest(2,5),c,challenge,active)==REMOVE);
 assert(decide(quest(5,5,"CHALLENGE_CATCH_EASY",true),Config{},challenge,active)==NONE);  // invalid config
 assert(remove_retry_ms(1)==30000&&remove_retry_ms(2)==120000&&remove_retry_ms(3)==0&&remove_retry_ms(0)==0);
 std::puts("PASS research policy: wire config bounds, claim-before-remove, selected templates only, bonus/special/claimed tasks protected, bounded removal retries");
 return 0;
}
