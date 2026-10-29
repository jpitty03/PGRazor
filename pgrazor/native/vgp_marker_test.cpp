#include "vgp_marker.h"
#include "pgrazor_native_api.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <dlfcn.h>
#include <thread>
#include <unordered_map>
#include <unistd.h>

namespace {
int kb,ki,kv,kc,kt,kw,ks,ma,mc,mt,mp,mproj,mw,mh;
struct Object{void *klass;bool live=true;float v[3]{};int32_t i=0;uint8_t b=0;};
Object target{&kw},camera{&kc},transform{&kt},position{&kv},screen{&kv},width{&ki},height{&ki},boolean{&kb};
std::unordered_map<uintptr_t,Object*> roots;
uintptr_t serial=0;int rootCalls=0,failRoot=0,invokes=0;
void *throws=nullptr;bool nullCamera=false,badWidth=false,delayProjection=false;
uintptr_t root(void *o,int){++rootCalls;if(rootCalls==failRoot)return 0;roots[++serial]=static_cast<Object*>(o);return serial;}
void *get(uintptr_t h){auto i=roots.find(h);return i==roots.end()?nullptr:i->second;}
void release(uintptr_t h){assert(roots.erase(h)==1);}
void *klass(void *o){return static_cast<Object*>(o)->klass;}
bool assign(void *a,void *b){return a==b;}
void *unbox(void *o){auto *v=static_cast<Object*>(o);return v->klass==&kb?static_cast<void*>(&v->b):v->klass==&ki?static_cast<void*>(&v->i):static_cast<void*>(v->v);}
void *invoke(void *m,void *,void **args,void **exc){
 ++invokes;*exc=nullptr;if(m==throws){*exc=&ma;return nullptr;}
 if(m==&ma){boolean.b=static_cast<Object*>(args[0])->live;return &boolean;}
 if(m==&mc)return nullCamera?nullptr:&camera;if(m==&mt)return &transform;
 if(m==&mp)return &position;if(m==&mproj){assert(static_cast<float*>(args[0])[0]==position.v[0]);if(delayProjection)usleep(4000);return &screen;}
 if(m==&mw)return badWidth?&screen:&width;if(m==&mh)return &height;
 assert(false);return nullptr;
}
void reset(){assert(roots.empty());target.live=camera.live=transform.live=true;
 position.v[0]=1;position.v[1]=2;position.v[2]=3;
 screen.v[0]=250;screen.v[1]=100;screen.v[2]=1;width.i=1000;height.i=400;
 rootCalls=failRoot=invokes=0;throws=nullptr;nullCamera=badWidth=delayProjection=false;}
}
int main(int argc,char **argv){
 assert(argc==2);void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);assert(lib);
 auto load=reinterpret_cast<const void *(*)(uint32_t)>(dlsym(lib,"pgrazor_native_api"));assert(load);
 auto *core=static_cast<const pgrazor_native_api_v2*>(load(2));assert(core);
 assert(!core->query_feature(28,2));auto *api=static_cast<const PgrVgpMarkerApi*>(core->query_feature(28,1));
 assert(api&&api->version==1&&api->size==24);
 PgrVgpMarkerConfig cfg{1,sizeof(cfg),reinterpret_cast<void*>(root),reinterpret_cast<void*>(get),reinterpret_cast<void*>(release),
  reinterpret_cast<void*>(klass),reinterpret_cast<void*>(invoke),reinterpret_cast<void*>(unbox),reinterpret_cast<void*>(assign),
  &ma,&mc,&mt,&mp,&mproj,&mw,&mh,&kb,&ki,&kv,&kc,&kt,&kw,&ks};
 assert(api->install(nullptr)<0);assert(api->install(&cfg)==0);assert(api->install(&cfg)==0);
 const uint64_t tid=gettid();PgrVgpMarkerResult out{};
 auto run=[&]{int rc=api->project(&target,2500,tid,&out);assert(roots.empty());return rc;};
 reset();assert(run()==1&&out.visible&&std::abs(out.x-.25)<1e-9&&std::abs(out.y-.75)<1e-9);
 reset();assert(api->project(&target,0,tid,&out)==0&&!out.visible&&invokes==0);
 reset();assert(api->project(&target,2500,0,&out)==0&&!out.visible);
 reset();delayProjection=true;assert(api->project(&target,1,tid,&out)==0&&!out.visible&&roots.empty());
 reset();target.live=false;assert(run()==0&&!out.visible);
 reset();camera.live=false;assert(run()==0&&!out.visible);
 reset();transform.live=false;assert(run()==0&&!out.visible);
 reset();nullCamera=true;assert(run()==0&&!out.visible);
 for(int dimension: {0,-1}){reset();width.i=dimension;assert(run()==0&&!out.visible);reset();height.i=dimension;assert(run()==0&&!out.visible);}
 for(float z:{0.f,-1.f,NAN}){reset();screen.v[2]=z;assert(run()==0&&!out.visible);}
 reset();screen.v[0]=NAN;assert(run()==0&&!out.visible);
 reset();position.v[1]=INFINITY;assert(run()==0&&!out.visible);
 reset();screen.v[0]=1001;assert(run()==1&&!out.visible&&out.x>1);
 reset();screen.v[1]=-1;assert(run()==1&&!out.visible&&out.y>1);
 reset();screen.v[0]=1000;screen.v[1]=400;assert(run()==1&&out.visible&&out.x==1&&out.y==0);
 reset();badWidth=true;assert(run()<0&&!out.visible);
 for(void *m:{static_cast<void*>(&ma),static_cast<void*>(&mc),static_cast<void*>(&mt),static_cast<void*>(&mp),static_cast<void*>(&mproj),static_cast<void*>(&mw),static_cast<void*>(&mh)}){
  reset();throws=m;assert(run()<=0&&!out.visible);
 }
 // Fail each temporary root in turn, including the liveness result roots.
 for(int n=1;n<=12;n++){reset();failRoot=n;const int rc=run();if(n<=rootCalls)assert(rc<=0&&!out.visible);}
 reset();std::thread other([&]{assert(api->project(&target,2500,tid,&out)==0&&!out.visible);});other.join();assert(invokes==0&&roots.empty());
 puts("PASS VGP marker production API: projection, thread/lease, liveness, dimensions, classes, throws, zero roots");
 dlclose(lib);
}
