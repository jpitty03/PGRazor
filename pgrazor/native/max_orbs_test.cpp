// Exercise the production state reader with managed failures and moving roots.
#include "max_orbs.cpp"
#include <cassert>
#include <cstdio>
#include <map>

namespace {
int ids[20];
enum {STATE,FIELD,MODE,FIELDS,POSITION,LOCAL,PREDICTION,TYPE,ORBSTATE,COUNT,ENTRIES};
struct Obj {void *k=nullptr;int mode=0,count=0,type=2,orb_state=1;bool predicting=false;Position pos{};void *actor=nullptr,*dict=nullptr,*entries=nullptr;};
Obj state,actor,dict,orb,moved;
alignas(8) unsigned char entry_array[0x20+24*2]{};
std::map<uintptr_t,void*> roots;uintptr_t serial=0;int allocated=0,freed=0;
bool fail=false,move=false;
uintptr_t root_new(void *o,int){roots[++serial]=o;++allocated;return serial;}
void *root_get(uintptr_t h){return roots.at(h);}
void root_free(uintptr_t h){assert(roots.erase(h)==1);++freed;}
void *object_class(void *o){return static_cast<Obj*>(o)->k;}
void *object_unbox(void *o){return o;}
void *get_field(void*,const char *name){if(!std::strcmp(name,"_count"))return &ids[COUNT];if(!std::strcmp(name,"_entries"))return &ids[ENTRIES];assert(false);return nullptr;}
void field_value(void *o,void *f,void *out){
 Obj *v=static_cast<Obj*>(o);
 if(f==&ids[MODE])*static_cast<int*>(out)=v->mode;
 else if(f==&ids[FIELDS])*static_cast<void**>(out)=v->dict;
 else if(f==&ids[POSITION])*static_cast<Position*>(out)=v->pos;
 else if(f==&ids[COUNT])*static_cast<int*>(out)=v->count;
 else if(f==&ids[ENTRIES])*static_cast<void**>(out)=v->entries;
 else assert(false);
}
void *managed(void *m,void *o,void **args,void **ex){
 *ex=nullptr;if(fail){*ex=&ids[0];return nullptr;}
 auto v=static_cast<Obj*>(o);static int boxed=0;
 if(m==&ids[LOCAL])return v->actor;
 if(m==&ids[PREDICTION])return &v->predicting;
 if(m==&ids[TYPE]){
  boxed=v->type;
  if(move){move=false;for(auto &r:roots)if(r.second==&orb)r.second=&moved;}
  return &boxed;
 }
 if(m==&ids[ORBSTATE]){boxed=static_cast<Obj*>(args[0])->orb_state;return &boxed;}
 assert(false);return nullptr;
}
void reset(){
 assert(roots.empty());state=Obj{};actor=Obj{};dict=Obj{};orb=Obj{};moved=Obj{};
 state.k=&ids[STATE];state.actor=&actor;state.dict=&dict;dict.entries=entry_array;dict.count=1;
 orb.k=&ids[FIELD];actor.pos={2,0};orb.pos={1,0};moved=orb;moved.pos={3,0};
 std::memset(entry_array,0,sizeof(entry_array));uint64_t length=2;std::memcpy(entry_array+0x18,&length,8);
 int next=-1;std::memcpy(entry_array+0x24,&next,4);void *p=&orb;std::memcpy(entry_array+0x30,&p,8);
 fail=false;move=false;
}
void check(int expected){assert(pgr_max_orb_direction(&state,nullptr)==expected);assert(roots.empty()&&allocated==freed);}
}
const void *pgrazor_runtime_transport_api(uint32_t){return nullptr;}
const void *pgrazor_runtime_bootstrap_api(uint32_t){return nullptr;}
const void *pgrazor_runtime_metadata_api(uint32_t){return nullptr;}
int main(){
 boot.runtime.gchandle_new=(void*)root_new;boot.runtime.gchandle_get_target=(void*)root_get;boot.runtime.gchandle_free=(void*)root_free;
 boot.runtime.object_get_class=(void*)object_class;boot.runtime.object_unbox=(void*)object_unbox;
 boot.runtime.runtime_invoke=(void*)managed;boot.runtime.field_get_value=(void*)field_value;boot.meta.class_get_field_from_name=(void*)get_field;
 b.state_class=&ids[STATE];b.field_class=&ids[FIELD];b.mode=&ids[MODE];b.fields=&ids[FIELDS];b.position=&ids[POSITION];
 b.local=&ids[LOCAL];b.prediction=&ids[PREDICTION];b.orb_type=&ids[TYPE];b.orb_state=&ids[ORBSTATE];
 b.normal=0;b.collectible=2;b.idle=1;b.left=7;b.right=9;ready=true;
 reset();check(7);orb.pos={3,0};check(9);
 orb.pos={2,0};check(0);orb.pos={4,0};check(0);orb.pos={1,1};check(7);
 reset();for(int mode=1;mode<14;++mode){state.mode=mode;check(0);}
 reset();actor.predicting=true;check(0);
 reset();for(int phase:{0,2,3}){orb.orb_state=phase;check(0);}
 reset();orb.type=1;check(0);
 reset();dict.count=0;check(0);dict.count=3;check(0);
 reset();state.actor=nullptr;check(0);
 reset();fail=true;check(0);
 reset();move=true;check(9);
 reset();actor.pos={INT32_MAX,0};orb.pos={INT32_MIN,0};check(0);
 std::puts("PASS max_orbs: left/right, adjacency, phase/prediction/lifetime gates, bounds, managed failure, moving GC roots");
}
