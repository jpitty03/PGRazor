#include "showcase_score.h"
#include <cmath>
#include <cstring>

namespace pgr_showcase {
namespace {
thread_local bool busy = false;
struct Busy { Busy(){busy=true;} ~Busy(){busy=false;} };
struct Root {
  const ScoreBindings& b; uintptr_t handle;
  ~Root(){if(handle)b.release(handle);}
};
bool valid(const ScoreBindings& b) {
  return b.root && b.target && b.release && b.object_class && b.invoke &&
    b.unbox && b.field_get && b.boottime_ms && b.owner_valid &&
    b.service_class && b.pokemon_class && b.contest_class && b.single_class &&
    b.score_method && b.pokemon_id_field;
}
bool live(const ScoreBindings& b,const ScoreRequest& r) {
  if(!r.owner || !r.generation || !b.owner_valid(r.owner,r.generation)) return false;
  const auto now=b.boottime_ms();
  return now && r.deadline_ms>now && r.deadline_ms-now<=2500;
}
bool exact(const ScoreBindings& b,uintptr_t h,void* cls) {
  void* v=b.target(h);return v && b.object_class(v)==cls;
}
bool identity(const ScoreBindings& b,const ScoreRequest& r) {
  if(!exact(b,r.service,b.service_class) || !exact(b,r.pokemon,b.pokemon_class) ||
     !exact(b,r.contest,b.contest_class))return false;
  uint64_t id=0;b.field_get(b.target(r.pokemon),b.pokemon_id_field,&id);
  return id==r.pokemon_id;
}
}
ScoreStatus read_score(const ScoreBindings& b,const ScoreRequest& r,double& score) {
  score=0;
  if(!valid(b))return ScoreStatus::bad_binding;
  if(busy || !live(b,r))return ScoreStatus::refused;
  Busy guard;
  if(!r.service || !r.pokemon || !r.contest || !r.pokemon_id || !identity(b,r))
    return ScoreStatus::bad_identity;
  if(!live(b,r))return ScoreStatus::refused;
  // target()/class/field reads are nonallocating IL2CPP exports. Re-resolve every
  // argument after any allocation or invoke; never retain raw inputs afterward.
  void* args[]={b.target(r.pokemon),b.target(r.contest)};
  void* exception=nullptr;
  void* result=b.invoke(b.score_method,b.target(r.service),args,&exception);
  if(exception)return ScoreStatus::managed_error;
  if(!result)return ScoreStatus::bad_score;
  if(!live(b,r))return ScoreStatus::refused;
  float value=0;
  {
    Root box{b,b.root(result,0)};
    if(!box.handle)return ScoreStatus::bad_score;
    if(!live(b,r))return ScoreStatus::refused;
    if(!exact(b,box.handle,b.single_class))return ScoreStatus::bad_score;
    void* data=b.unbox(b.target(box.handle));
    if(!data)return ScoreStatus::bad_score;
    std::memcpy(&value,data,sizeof(value));
    if(!std::isfinite(value) || value<=0)return ScoreStatus::bad_score;
  }
  // Root release can be a safepoint too. Publish only after cleanup, another
  // identity check, and the final lease check. Failure always leaves score zero.
  if(!live(b,r))return ScoreStatus::refused;
  if(!identity(b,r))return ScoreStatus::bad_identity;
  if(!live(b,r))return ScoreStatus::refused;
  score=value;return ScoreStatus::valid;
}
}
