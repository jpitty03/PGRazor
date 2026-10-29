// Same production host translation unit with boundary spies. Tests configuration
// and transport glue; does NOT claim to execute actual Dobby leaves10/11.
#include "spin_host.cpp"
#include <cassert>
#include <vector>
#include <string>
#include <thread>
#include <sys/wait.h>
namespace spin_fixture {
using namespace pgr_shared_meta;
PgrSpinCameraConfig camera_config{};PgrQuickSpinConfig quick_config{};
PgrSpinCameraSample camera_sample{};PgrQuickSpinSample quick_sample{};
int camera_installs=0,quick_installs=0,camera_allowed=-1,quick_allowed=-1,clear_count=0;
uint64_t camera_lease=0,quick_lease=0,quick_sequence=0,quick_opener=0;
bool attached=true,fresh=true,allowed=true,reenter=false;int install_quick_error=0;
std::vector<unsigned> opcodes;std::vector<std::string> payloads;
void *thread(){static int token;return attached?&token:nullptr;}
int camera_install(const PgrSpinCameraConfig *p){camera_config=*p;++camera_installs;return 0;}
int quick_install(const PgrQuickSpinConfig *p){quick_config=*p;++quick_installs;return install_quick_error;}
int camera_update(int a,uint64_t l,uint64_t tid,uint64_t){assert(tid==uint64_t(gettid()));camera_allowed=a;camera_lease=l;return 0;}
int quick_update(int a,uint64_t l,uint64_t,uint64_t){quick_allowed=a;quick_lease=l;return 0;}
int camera_tick(uint64_t){return 0;}
int quick_tick(uint64_t tid,uint64_t seq,uint64_t opener){quick_sequence=seq;quick_opener=opener;if(reenter){reenter=false;auto a=static_cast<const PgrSpinHostApi*>(pgrazor_spin_host_api(1));assert(a->tick(tid,seq,opener)==PGR_SPH_BUSY);}return 0;}
int camera_read(PgrSpinCameraSample *p){*p=camera_sample;return 0;}
int quick_read(PgrQuickSpinSample *p){*p=quick_sample;return 0;}
int clear(){++clear_count;return 0;}
int emit(uint32_t op,const int64_t *n,uint32_t nc,const PgrRuntimeTransportString *s,uint32_t sc){
 opcodes.push_back(op);if(op==44){assert(nc==1&&sc==1&&n[0]>=0&&n[0]<=1);}if(op==17){assert(!nc&&sc==1);assert(std::string(s[0].ptr,s[0].len)=="Pokestop spin");}if(op==19){assert(!nc&&sc==2);assert(std::string(s[0].ptr,s[0].len)=="quick-close");payloads.emplace_back(s[1].ptr,s[1].len);}return 0;
}
int get_bool(uint32_t index,int *out){assert(index==24);*out=allowed;return 0;}
int transport_sample(PgrRuntimeTransportSample *p){*p={};p->snapshot_fresh=fresh;p->snapshot_seq=1;return 0;}
int valid(uint64_t g){return g==9;}
int shared_sample(PgrSharedHostSample *p){*p={};p->hook_frames=7;return 0;}
const PgrSpinCameraApi camera_table{1,sizeof(PgrSpinCameraApi),camera_install,camera_update,camera_tick,camera_read};
const PgrQuickSpinApi quick_table{1,sizeof(PgrQuickSpinApi),quick_install,quick_update,quick_tick,quick_read,clear};
PgrRuntimeTransportApi transport_table{};PgrUnitySchedulerApi scheduler_table{};PgrSharedHostApi shared_table{};
void *pointer(unsigned i){return reinterpret_cast<void*>(uintptr_t(0x10000+i*16));}
void scenario(unsigned mode){
 transport_table.emit=emit;transport_table.get_bool=get_bool;transport_table.sample=transport_sample;scheduler_table.valid=valid;shared_table.sample=shared_sample;
 PgrSharedMetadataOutput md{};md.version=2;md.size=sizeof(md);
 for(unsigned i=0;i<PGR_OPT_HANDLE_COUNT;++i)md.opt_handles[i]=pointer(i);
 for(unsigned i=0;i<kOptClassCount;++i)md.opt_classes[i]=pointer(1000+i);
 md.classes[6]=pointer(2000);md.fields[1]=pointer(2001);md.methods[5]=pointer(2002);md.opt_ints[I_SPIN_SUCCESS]=23;
 if(mode!=1)md.optional[OPT_SPIN_CAMERA]=PGR_SHARED_OPT_AVAILABLE;
 if(mode!=2)md.optional[OPT_SPIN]=PGR_SHARED_OPT_AVAILABLE;
 if(mode==3)install_quick_error=-7;
 PgrSharedMetaExtraExports ex{};ex.gchandle_new=pointer(2100);ex.gchandle_get_target=pointer(2101);ex.gchandle_free=pointer(2102);
 PgrSpinHostConfig c{1,sizeof(c),mode==4?1u:0u,1,&md,&ex,pointer(2110),pointer(2111),pointer(2112),pointer(2113),thread,uint64_t(gettid())};
 const auto a=static_cast<const PgrSpinHostApi*>(pgrazor_spin_host_api(1));assert(a&&a->size==64&&!pgrazor_spin_host_api(2));
 auto bad=c;bad.exclusive_owner=0;assert(a->configure(&bad)==PGR_SPH_BAD_CONFIG);
 assert(a->configure(&c)==0&&a->configure(&c)==0);bad=c;bad.object_unbox=pointer(2114);assert(a->configure(&bad)==PGR_SPH_REBIND);
 PgrSpinHostSample s{};assert(a->sample(&s)==0);assert(s.capabilities==((mode==1?0:1)|(mode==2||mode==3?0:2)));
 assert(camera_installs==(mode!=1)&&quick_installs==(mode!=2));
 if(mode!=1){assert(camera_config.in_enter_addr==md.opt_handles[H_SC_APP_ENTER_CODE]);assert(camera_config.move_next_addr==md.opt_handles[H_SC_MOVE_CODE]);assert(camera_config.owner_field==md.opt_handles[H_SC_THIS_F]);assert(camera_config.spline_class==md.opt_classes[OC_SPLINE]);}
 if(mode!=2){assert(quick_config.success_value==23&&quick_config.state_open_value==2);assert(quick_config.spin_address==md.opt_handles[H_SPIN_SPIN_CODE]);assert(quick_config.spin_method==md.opt_handles[H_SPIN_SPIN_M]);assert(quick_config.alive_method==md.methods[5]);assert(quick_config.current_state_field==md.fields[1]);assert(quick_config.spin_result_class==md.opt_classes[OC_SPINRESULT]);}
 PgrSpinHostOptions p{1,sizeof(p),1,2500,1};assert(a->update(&p)==0);
 assert(a->tick(c.owner_tid,1,0)==0);assert(camera_allowed<=0&&quick_allowed<=0);
 assert(a->tick(c.owner_tid,2,0)==0);reenter=true;assert(a->tick(c.owner_tid,3,4)==0);
 if(mode!=2&&mode!=3){assert(quick_allowed==1&&quick_lease<=2500&&quick_sequence==3&&quick_opener==4);}
 if(mode!=1)assert(camera_allowed==1&&camera_lease<=2500);
 attached=false;assert(a->tick(c.owner_tid,4,0)==PGR_SPH_OFF_OWNER);attached=true;
 std::thread wrong([&]{assert(a->tick(c.owner_tid,4,0)==PGR_SPH_OFF_OWNER);});wrong.join();
 if(mode==4){quick_sample.cooldown_serial=2;quick_sample.request_serial=1;quick_sample.last_tid=c.owner_tid;quick_sample.last_gui_state=2;
  assert(a->tick(c.owner_tid,4,0)==0);unsigned cooldowns=0;for(auto op:opcodes)cooldowns+=op==17;assert(cooldowns==2);assert(!payloads.empty());
  PgrUnityPulse pulse{9,5,int32_t(c.owner_tid),0};a->on_unity_pulse(nullptr,&pulse);assert(quick_opener==7);
  // Force the next permitted settings poll without sleeping.
  last_poll=0;fresh=false;assert(a->tick(c.owner_tid,6,0)==0);assert(camera_allowed==0&&quick_allowed==0);
 }
 p.enabled=0;p.epoch=2;assert(a->update(&p)==0);assert(a->tick(c.owner_tid,7,0)==0);assert(camera_allowed<=0&&quick_allowed<=0);
 assert(a->stop()==0&&a->stop()==0);assert(a->update(&p)==PGR_SPH_STOPPED);assert(a->tick(c.owner_tid,8,0)==PGR_SPH_STOPPED);
 assert(a->sample(&s)==0&&s.stopping);if(mode==3)assert(s.quick_error==-7&&s.capabilities==1);
}
}
extern "C" const void *pgrazor_spin_camera_api(uint32_t v){return v==1?&spin_fixture::camera_table:nullptr;}
extern "C" const void *pgrazor_quick_spin_api(uint32_t v){return v==1?&spin_fixture::quick_table:nullptr;}
const void *pgrazor_runtime_transport_api(uint32_t v){return v==1?&spin_fixture::transport_table:nullptr;}
extern "C" const void *pgrazor_unity_scheduler_api(uint32_t v){return v==2?&spin_fixture::scheduler_table:nullptr;}
extern "C" const void *pgrazor_shared_host_api(uint32_t v){return v==1?&spin_fixture::shared_table:nullptr;}
int main(){for(unsigned mode=0;mode<5;++mode){pid_t child=fork();assert(child>=0);if(!child){spin_fixture::scenario(mode);_exit(0);}int status=0;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);}puts("PASS spin_host production-source boundary fixture: independent caps, config mapping, lease/proof/epoch, transport actions, stop and reentry");}
