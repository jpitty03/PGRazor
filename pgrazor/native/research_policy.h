#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
// Pure field research policy: the Java wire config and the per-quest decision. No game calls.
namespace pgr_research {
constexpr size_t kMaxTemplates=300,kMaxTemplateBytes=96;
struct Config {bool valid=false,remove=false,claim=false,catch_encounters=false;std::set<std::string> templates;};
// Template ids are the game's quest template keys, e.g. CHALLENGE_CATCH_EASY_POKEMON.
inline bool template_char(char c){return (c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_';}
// "R2|remove|claim|catch|T1,T2,..." with 0/1 flags and an optional, duplicate-free template list.
inline bool flag(char c){return c=='0'||c=='1';}
inline bool parse(const char *s,size_t n,Config &out){
 out=Config{};if(!s||n<9||n>kMaxTemplates*(kMaxTemplateBytes+1)+10||std::memcmp(s,"R2|",3))return false;
 if(!flag(s[3])||s[4]!='|'||!flag(s[5])||s[6]!='|'||!flag(s[7])||s[8]!='|')return false;
 Config c;c.remove=s[3]=='1';c.claim=s[5]=='1';c.catch_encounters=s[7]=='1';
 size_t at=9;
 while(at<n){
  size_t end=at;while(end<n&&s[end]!=',')++end;
  if(end==at||end-at>kMaxTemplateBytes)return false;
  for(size_t i=at;i<end;++i)if(!template_char(s[i]))return false;
  if(!c.templates.insert(std::string(s+at,end-at)).second||c.templates.size()>kMaxTemplates)return false;
  if(end==n)break;
  at=end+1;if(at==n)return false;  // trailing comma
 }
 c.valid=true;out=c;return true;
}
struct Quest {int context=0,status=0,progress=0,target=0;bool bonus=false;std::string id,templ;};
enum Action {NONE,CLAIM,REMOVE};
// Only active field research (context == challenge) is ever touched. A met goal is claimed when
// claiming is on and is never removed; an unmet goal is removed only when its exact template is
// selected. Gym bonus tasks are never removed.
inline Action decide(const Quest &q,const Config &c,int challenge,int active){
 if(!c.valid||q.context!=challenge||q.status!=active||q.id.empty()||q.templ.empty())return NONE;
 if(q.target>0&&q.progress>=q.target)return c.claim?CLAIM:NONE;
 if(c.remove&&!q.bonus&&c.templates.count(q.templ))return REMOVE;
 return NONE;
}
// A failed removal (refused, no answer, or interrupted) is retried while the task is still listed,
// later each time: 30 s after the first failure, 2 min after the second, then never. Repeating a
// removal is harmless; claims are never repeated.
inline uint64_t remove_retry_ms(int failed_attempts){return failed_attempts==1?30000:failed_attempts==2?120000:0;}
}  // namespace pgr_research
