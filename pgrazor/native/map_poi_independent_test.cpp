// Reuse only fake metadata shapes/config construction; the original baseline main is not run.
#define main map_poi_worker_baseline_main
#include "map_poi_test.cpp"
#undef main
#include <algorithm>
#include <memory>
#include <vector>
#include <time.h>
#include <sys/wait.h>

namespace {
struct MovingHeap {
 FakeCandidate stop,gym;FakeFort stopFort,gymFort;FakeString stopId,gymId;
 FakeAliveBox alive;FakeTeamBox team;FakeRaid raid;FakeGymDisplay display;FakeBoss boss;
 FakeType stopType,gymType;FakeArray stopArray,gymArray;
};
MovingHeap heaps[256];int heapIndex=0,relocations=0,fortCalls=0,aliveCalls=0,managedCalls=0;
std::map<uintptr_t,void*> roots;uintptr_t nextRoot=100;size_t peakRoots=0;
const PgrMapPoiApi *testedApi=nullptr;PgrMapPoiOutput *activeOutput=nullptr;
PgrMapPoiRequest activeRequest{};
bool reenterOnStopCleanup=false,expireCandidate=false,expireAliveCleanup=false,expired=false;
int reentryStatus=0;bool reentered=false;
MovingHeap &h(){return heaps[heapIndex];}
std::vector<void*> objects(MovingHeap &v){return{&v.stop,&v.gym,&v.stopFort,&v.gymFort,&v.stopId,&v.gymId,
 &v.alive,&v.team,&v.raid,&v.display,&v.boss,&v.stopType,&v.gymType,&v.stopArray,&v.gymArray};}
int objectIndex(void *p){auto list=objects(h());for(size_t i=0;i<list.size();++i)if(list[i]==p)return static_cast<int>(i);assert(false&&"stale managed pointer");return -1;}
void moveHeap(){
 assert(heapIndex+1<256);MovingHeap &old=h(),&fresh=heaps[++heapIndex];fresh=old;
 auto previous=objects(old),next=objects(fresh);
 auto remap=[&](void *p){for(size_t i=0;i<previous.size();++i)if(p==previous[i])return next[i];return p;};
 for(auto &root:roots)root.second=remap(root.second);
 fresh.stop.fort=remap(fresh.stop.fort);fresh.gym.fort=remap(fresh.gym.fort);
 for(FakeFort *f:{&fresh.stopFort,&fresh.gymFort}){f->id=remap(f->id);f->raidInfo=remap(f->raidInfo);f->gymDisplay=remap(f->gymDisplay);}
 fresh.raid.boss=remap(fresh.raid.boss);
 for(void *&p:fresh.stopArray.data)p=remap(p);for(void *&p:fresh.gymArray.data)p=remap(p);
 for(void *p:previous)*static_cast<void**>(p)=&cls_other;++relocations;
}
void pauseExpired(){timespec t{0,100000000};nanosleep(&t,nullptr);expired=true;}
uintptr_t movingNew(void *p,int pinned){
 assert(!pinned);const int which=objectIndex(p);uintptr_t handle=++nextRoot;roots[handle]=p;
 peakRoots=std::max(peakRoots,roots.size());moveHeap();
 if(expireCandidate&&which==0){expireCandidate=false;pauseExpired();}
 return handle;
}
void *movingGet(uintptr_t handle){auto it=roots.find(handle);return it==roots.end()?nullptr:it->second;}
void movingFree(uintptr_t handle){
 auto it=roots.find(handle);assert(it!=roots.end());const int which=objectIndex(it->second);roots.erase(it);
 if(expireAliveCleanup&&which==6){expireAliveCleanup=false;pauseExpired();}
 if(reenterOnStopCleanup&&which==13){
  reenterOnStopCleanup=false;reentered=true;const int before=managedCalls;
  // Same output buffer, while the outer stop snapshot is provisional.
  reentryStatus=testedApi->scan(&activeRequest,activeOutput);
  assert(reentryStatus==PGR_MP_ERR_BUSY&&managedCalls==before);
 }
 moveHeap();
}
void *movingClass(void *p){objectIndex(p);return *static_cast<void**>(p);}
void movingField(void *p,void *field,void *out){objectIndex(p);fake_field_get_value(p,field,out);}
void *movingUnbox(void *p){objectIndex(p);return fake_object_unbox(p);}
void *movingType(void *klass){assert(klass==&cls_stop||klass==&cls_gym);return klass==&cls_stop?static_cast<void*>(&h().stopType):&h().gymType;}
uintptr_t movingLength(void *p){objectIndex(p);return static_cast<FakeArray*>(p)->length;}
void *movingVirtual(void *p,void *method){const int i=objectIndex(p);assert((i==0||i==1)&&method==&m_fort);return &m_fort_resolved;}
int32_t movingStrLength(void *p){objectIndex(p);return static_cast<FakeString*>(p)->length;}
uint16_t *movingChars(void *p){objectIndex(p);return static_cast<FakeString*>(p)->chars;}
void *movingInvoke(const void *method,void *receiver,void **args,void **exception){
 ++managedCalls;*exception=nullptr;int result=-1;
 if(method==&m_find){assert(!receiver&&args);objectIndex(args[0]);result=static_cast<FakeType*>(args[0])->represents==&cls_stop?13:14;}
 else if(method==&m_alive){assert(!receiver&&args);const int i=objectIndex(args[0]);assert(i==0||i==1);++aliveCalls;h().alive.value=static_cast<FakeCandidate*>(args[0])->alive;result=6;}
 else if(method==&m_fort_resolved){assert(!args);const int i=objectIndex(receiver);assert(i==0||i==1);++fortCalls;result=i==0?2:3;}
 else {assert(method==&m_team&&!args&&objectIndex(receiver)==1);h().team.value=h().gym.teamId;result=7;}
 moveHeap();return objects(h())[result];
}
void initializeHeap(){
 set_id(h().stopId,"stop-key");set_id(h().gymId,"gym-key");
 h().stop={&cls_stop,1,&h().stopFort,0};h().gym={&cls_gym,1,&h().gymFort,2};
 h().stopFort={&cls_fort,&h().stopId,1,2,nullptr,nullptr};h().gymFort={&cls_fort,&h().gymId,3,4,&h().raid,&h().display};
 h().alive={&cls_bool,1};h().team={&cls_teamBox,2};h().boss={&cls_boss,25};h().display={&cls_gymDisplay,3};
 h().raid={&cls_raid,0,1000,3000,1500,5,&h().boss};
 h().stopType={&cls_type,&cls_stop};h().gymType={&cls_type,&cls_gym};
 h().stopArray.klass=h().gymArray.klass=&cls_objarray;h().stopArray.length=h().gymArray.length=1;
 h().stopArray.data[0]=&h().stop;h().gymArray.data[0]=&h().gym;
}
int runIndependent(const char *scenario){
 initializeHeap();auto c=make_config(true,true,true,true);
 c.runtime_invoke=reinterpret_cast<void*>(&movingInvoke);c.object_get_class=reinterpret_cast<void*>(&movingClass);
 c.field_get_value=reinterpret_cast<void*>(&movingField);c.object_unbox=reinterpret_cast<void*>(&movingUnbox);
 c.gchandle_new=reinterpret_cast<void*>(&movingNew);c.gchandle_get_target=reinterpret_cast<void*>(&movingGet);c.gchandle_free=reinterpret_cast<void*>(&movingFree);
 c.type_get_object=reinterpret_cast<void*>(&movingType);c.array_length=reinterpret_cast<void*>(&movingLength);
 c.object_get_virtual_method=reinterpret_cast<void*>(&movingVirtual);c.string_length=reinterpret_cast<void*>(&movingStrLength);c.string_chars=reinterpret_cast<void*>(&movingChars);
 assert(testedApi->install(&c)==0);
 activeRequest.version=1;activeRequest.size=sizeof(activeRequest);activeRequest.tid=static_cast<uint64_t>(gettid());activeRequest.remaining_ms=2500;activeRequest.wall_ms=2000;
 auto output=std::unique_ptr<PgrMapPoiOutput>(new PgrMapPoiOutput{});activeOutput=output.get();
 const bool candidate=std::strcmp(scenario,"expire-candidate")==0,alive=std::strcmp(scenario,"expire-alive-cleanup")==0,invalid=std::strcmp(scenario,"invalid-bool")==0;
 if(candidate||alive){activeRequest.remaining_ms=50;expireCandidate=candidate;expireAliveCleanup=alive;}
 if(invalid)h().stop.alive=2;
 if(std::strcmp(scenario,"same-output-reentry")==0)reenterOnStopCleanup=true;
 const int status=testedApi->scan(&activeRequest,output.get());assert(output->status==status&&roots.empty()&&peakRoots<16);
 if(candidate||alive){assert(expired&&status==PGR_MP_SKIP&&output->count==0&&fortCalls==0);if(candidate)assert(aliveCalls==0);}
 else if(invalid){assert(status==PGR_MP_ERR_METADATA&&output->count==0&&fortCalls==0);}
 else {
  assert(status==PGR_MP_OK&&output->count==2&&relocations>15);
  const auto &stop=output->points[0],&gym=output->points[1];
  assert(stop.id_length==8&&stop.id[0]=='s'&&stop.lat==1&&stop.lon==2);
  assert(gym.id_length==7&&gym.id[0]=='g'&&gym.lat==3&&gym.lon==4);
  assert(gym.flags==(PGR_MP_FLAG_TEAM|PGR_MP_FLAG_SLOTS|PGR_MP_FLAG_LEVEL|PGR_MP_FLAG_BOSS));
  assert(gym.team_id==2&&gym.slots==3&&gym.level==5&&gym.boss==25&&gym.raid_end==3000&&gym.raid_start==1500);
  if(std::strcmp(scenario,"same-output-reentry")==0)assert(reentered&&reentryStatus==PGR_MP_ERR_BUSY);
 }
 std::printf("PASS independent map POI %s\n",scenario);return 0;
}
}
int main(int argc,char **argv){
 if(argc<2||argc>3)return 1;
 if(argc==2){
  const char *scenarios[]={"moving","same-output-reentry","expire-candidate","expire-alive-cleanup","invalid-bool"};
  for(const char *scenario:scenarios){
   const pid_t child=fork();if(child<0)return 3;
   if(child==0){execl(argv[0],argv[0],argv[1],scenario,static_cast<char*>(nullptr));_exit(127);}
   int status=0;if(waitpid(child,&status,0)!=child)return 3;
   if(!WIFEXITED(status)||WEXITSTATUS(status)!=0){std::printf("FAIL independent map POI %s (status %d)\n",scenario,status);return 4;}
  }
  std::puts("PASS independent map POI all five scenarios");return 0;
 }
 void *library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!library){std::puts(dlerror());return 2;}
 auto get=reinterpret_cast<const void*(*)(uint32_t)>(dlsym(library,"pgrazor_native_api"));
 auto core=get?static_cast<const pgrazor_native_api_v2*>(get(2)):nullptr;assert(core&&core->query_feature(42,99)==nullptr);
 testedApi=static_cast<const PgrMapPoiApi*>(core->query_feature(42,1));assert(testedApi&&testedApi->size==24);
 return runIndependent(argc==3?argv[2]:"moving");
}
