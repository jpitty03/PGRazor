#include "details_completion.h"
#include "pgrazor_native_api.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <set>
#include <unistd.h>

namespace {
int cg,cv,cr,cd,cp,ca,cb,ci,cl,other;
int aliveMethod,clickMethod,countMethod,itemMethod;
int fState,fStatus,fCaught,fResponse,fPokemonId,fTarget,fData,fGuiCallback,fDataCallback,fPost,fIndex,fList;
struct Obj{void *klass,*a=nullptr,*b=nullptr;uint64_t id=0;int32_t value=0,index=0;uint8_t post=0;};
struct Scene{Obj gui,view,response,data,pokemon,action,list;};
Scene scenes[12];int sceneIndex=0;
Obj boxes[64];int boxIndex=0;
std::map<uintptr_t,void*> roots;std::set<uintptr_t> borrowed;
uintptr_t serial=0x1000;int allocations=0,failAllocation=0,clicks=0,aliveCalls=0,targetReads=0;
bool failedAllocation=false,moveInputs=false,moveBoxes=false,throwClick=false,gone=false;
bool badPostClass=false,badPostAliveClass=false,destroyDuringItem=false,badListDuringCount=false;
uint32_t moveMethods=0; // alive=1/count=2/item=4/click=8, each relocates once to a distinct scene
const PgrDetailsCompletionApi *api=nullptr;uint64_t owner=0,token=0;
PgrDetailsCompletionRequest req{};
void wire(Scene &s){s.gui.a=&s.data;s.gui.b=&s.action;s.view.a=&s.response;s.data.a=&s.action;s.data.b=&s.list;
 s.action.a=&s.view;s.list.a=&s.pokemon;}
void relocate(){
 assert(sceneIndex<11);Scene &old=scenes[sceneIndex],&next=scenes[++sceneIndex];next=old;wire(next);
 Obj *before[]={&old.gui,&old.view,&old.response,&old.data,&old.pokemon,&old.action,&old.list};
 Obj *after[]={&next.gui,&next.view,&next.response,&next.data,&next.pokemon,&next.action,&next.list};
 for(auto &r:roots)for(int i=0;i<7;i++)if(r.second==before[i]){r.second=after[i];break;}
 for(Obj *o:before){o->klass=&other;o->a=o->b=nullptr;o->id=0;o->value=99;}
}
uintptr_t gcNew(void *o,int){
 ++allocations;if(allocations==failAllocation){failedAllocation=true;return 0;}if(!o)return 0;
 const uintptr_t h=++serial;roots[h]=o;
 if(moveInputs&&o==&scenes[sceneIndex].gui){moveInputs=false;relocate();}
 if(moveBoxes&&o==&boxes[boxIndex]){
  Obj &old=boxes[boxIndex],&next=boxes[++boxIndex];next=old;
  for(auto &r:roots)if(r.second==&old)r.second=&next;
  old.klass=&other;old.value=99;
 }return h;
}
void *gcGet(uintptr_t h){auto i=roots.find(h);return i==roots.end()?nullptr:i->second;}
void gcFree(uintptr_t h){assert(!borrowed.count(h));assert(roots.erase(h)==1);}
void *klass(void *o){return o?static_cast<Obj*>(o)->klass:nullptr;}
void *unbox(void *o){assert(klass(o)==&cb||klass(o)==&ci);return &static_cast<Obj*>(o)->value;}
void field(void *object,void *f,void *out){
 auto *o=static_cast<Obj*>(object);assert(o&&o->klass!=&other);
 if(f==&fTarget){assert(o->klass==&ca);++targetReads;*static_cast<void**>(out)=o->a;}
 else if(f==&fResponse||f==&fData||f==&fDataCallback)*static_cast<void**>(out)=o->a;
 else if(f==&fGuiCallback||f==&fList)*static_cast<void**>(out)=o->b;
 else if(f==&fState||f==&fStatus)*static_cast<int32_t*>(out)=o->value;
 else if(f==&fCaught||f==&fPokemonId)memcpy(out,&o->id,8);
 else if(f==&fPost)*static_cast<uint8_t*>(out)=o->post;
 else if(f==&fIndex)*static_cast<int32_t*>(out)=o->index;
 else assert(false);
}
void *invoke(const void *m,void *self,void **args,void **exception){
 *exception=nullptr;uint32_t bit=0;
 if(m==&aliveMethod){bit=1;++aliveCalls;assert(!self&&args&&args[0]==&scenes[sceneIndex].gui);}
 else if(m==&countMethod){bit=2;assert(self==&scenes[sceneIndex].list&&klass(self)==&cl);}
 else if(m==&itemMethod){bit=4;assert(self==&scenes[sceneIndex].list&&klass(self)==&cl);
  assert(args&&*static_cast<int32_t*>(args[0])==0);}
 else{assert(m==&clickMethod);bit=8;++clicks;assert(self==&scenes[sceneIndex].gui);}
 if(moveMethods&bit){moveMethods&=~bit;relocate();}
 Scene &s=scenes[sceneIndex];
 if(m==&clickMethod){s.gui.value=3;if(badPostClass)s.gui.klass=&other;if(throwClick)*exception=&other;return nullptr;}
 if(m==&itemMethod){if(destroyDuringItem)gone=true;return &s.pokemon;}
 if(m==&countMethod&&badListDuringCount)s.list.klass=&other;
 if(m==&aliveMethod&&clicks&&badPostAliveClass)s.gui.klass=&other;
 Obj &box=boxes[++boxIndex];box={m==&aliveMethod?static_cast<void*>(&cb):static_cast<void*>(&ci)};
 box.value=m==&aliveMethod?(gone?0:1):1;return &box;
}
void *borrow(Obj *o){const uintptr_t h=++serial;roots[h]=o;borrowed.insert(h);return reinterpret_cast<void*>(h);}
void balanced(){assert(roots.size()==borrowed.size());for(auto h:borrowed)assert(roots.count(h));}
void reset(){
 balanced();roots.clear();borrowed.clear();sceneIndex=boxIndex=allocations=failAllocation=clicks=aliveCalls=targetReads=0;
 failedAllocation=moveInputs=moveBoxes=throwClick=gone=badPostClass=badPostAliveClass=destroyDuringItem=badListDuringCount=false;moveMethods=0;
 Scene &s=scenes[0];s={{&cg},{&cv},{&cr},{&cd},{&cp},{&ca},{&cl}};wire(s);
 s.gui.value=2;s.response.value=3;s.response.id=s.pokemon.id=UINT64_MAX;s.data.post=1;
 req={};req.version=1;req.size=sizeof(req);memset(req.expected_key,255,8);req.epoch=7;req.tid=owner;
 req.gui=borrow(&s.gui);req.view=borrow(&s.view);req.response=borrow(&s.response);req.data=borrow(&s.data);
 req.pokemon=borrow(&s.pokemon);req.dismissed=borrow(&s.action);
 assert(api->update(1,2500,7,owner)==0);token=0;assert(api->acquire(7,owner,&token)==0&&token);
}
PgrDetailsCompletionConfig config(){
 PgrDetailsCompletionConfig c{};c.version=1;c.size=sizeof(c);
 c.object_get_class=reinterpret_cast<void*>(klass);c.field_get_value=reinterpret_cast<void*>(field);
 c.runtime_invoke=reinterpret_cast<void*>(invoke);c.object_unbox=reinterpret_cast<void*>(unbox);
 c.gc_new=reinterpret_cast<void*>(gcNew);c.gc_get=reinterpret_cast<void*>(gcGet);c.gc_free=reinterpret_cast<void*>(gcFree);
 c.class_gui=&cg;c.class_view=&cv;c.class_response=&cr;c.class_data=&cd;c.class_pokemon=&cp;c.class_action=&ca;c.class_bool=&cb;c.class_int=&ci;c.class_list=&cl;
 c.m_alive=&aliveMethod;c.m_click=&clickMethod;c.m_count=&countMethod;c.m_item=&itemMethod;
 c.f_current_state=&fState;c.f_status=&fStatus;c.f_caught_id=&fCaught;c.f_view_response=&fResponse;c.f_pokemon_id=&fPokemonId;
 c.f_delegate_target=&fTarget;c.f_gui_data=&fData;c.f_gui_callback=&fGuiCallback;c.f_data_callback=&fDataCallback;
 c.f_post=&fPost;c.f_index=&fIndex;c.f_list=&fList;c.success_literal=3;c.state_open=2;return c;
}
PgrDetailsCompletionOutput complete(int expected=1){PgrDetailsCompletionOutput o{};assert(api->complete(&req,token,&o)==expected);balanced();return o;}
}
int main(int argc,char **argv){
 assert(argc==2);void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);assert(lib);
 auto get=reinterpret_cast<const void*(*)(uint32_t)>(dlsym(lib,"pgrazor_native_api"));assert(get);
 auto *core=static_cast<const pgrazor_native_api_v2*>(get(2));assert(core);
 api=static_cast<const PgrDetailsCompletionApi*>(core->query_feature(37,1));assert(api&&api->size==64);
 assert(sizeof(PgrDetailsCompletionConfig)==272&&sizeof(req)==80);owner=gettid();auto cfg=config();assert(api->install(&cfg)==0);
 reset();assert(api->validate(&req)==1&&targetReads>=2);balanced();
 allocations=0;auto out=complete();assert(clicks==1&&(out.flags&PGR_DC_FLAG_AFTER_KNOWN)&&out.after==3);
 const int completeRoots=allocations;
 reset();scenes[0].action.a=&scenes[0].gui;assert(api->validate(&req)==0&&clicks==0);balanced();
 // Alive, count, item and click each relocate ALL objects; native/host roots follow them while old
 // addresses are poisoned. Scalars and boxed values additionally move during their own root creation.
 reset();moveInputs=true;moveBoxes=true;moveMethods=15;
 out=complete();assert(sceneIndex==5&&clicks==1&&out.after==3&&!(out.flags&PGR_DC_FLAG_GONE));
 reset();scenes[0].gui.value=1;assert(api->validate(&req)==1);out=complete(0);assert(clicks==0);
 for(int n=1;n<=completeRoots;n++){reset();failAllocation=n;PgrDetailsCompletionOutput o{};
  api->complete(&req,token,&o);assert(failedAllocation&&clicks<=1);balanced();assert(!(o.flags&PGR_DC_FLAG_GONE));}
 for(bool duringAlive:{false,true}){reset();badPostClass=!duringAlive;badPostAliveClass=duringAlive;
  out=complete();assert(clicks==1&&(out.flags&PGR_DC_FLAG_INVOKED));
  assert(!(out.flags&PGR_DC_FLAG_AFTER_KNOWN)&&!(out.flags&PGR_DC_FLAG_GONE));}
 reset();throwClick=true;out=complete(PGR_DC_ERR_MANAGED);assert(clicks==1&&(out.flags&PGR_DC_FLAG_INVOKED));
 // A list getter invalidating its class must not lead to another virtual call or unsafe field read.
 reset();badListDuringCount=true;out=complete(0);assert(clicks==0);
 // Likewise a destroyed GUI after item() cannot pass the final pre-click liveness proof.
 reset();destroyDuringItem=true;out=complete(0);assert(clicks==0);
 balanced();roots.clear();borrowed.clear();
 puts("PASS details lifetime: ACTION callback ownership, borrowed handles, real relocation, opening validation, root failures, postcondition unknown, final liveness");
 dlclose(lib);
}
