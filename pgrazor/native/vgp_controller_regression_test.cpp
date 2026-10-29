// Reuse the production-query/fake-table scaffold without executing its baseline main. This separate
// executable focuses on coordinator fixes; it is never linked into libpgrazor or the app.
#define main vgp_controller_baseline_main_unused
#include "vgp_controller_test.cpp"
#undef main
#include <functional>

namespace {
int encounters=0,captures=0,spins=0,cooldowns=0,snapshots=0;
uint64_t cooldownLease=0;
bool failPendingBag=false,slowSnapshot=false;
std::function<void()> afterBalls;
void *expectedCapture=nullptr;
Obj movedCandidate(&wildClass),manualGui(&nameplateClass);

uint64_t regressionGcNew(void *o,int pinned){
  if(failPendingBag&&o==bagObj.p()){failPendingBag=false;return 0;}
  return gcNew(o,pinned);
}
int regressionDue(uint64_t){return g_due&&!g_blocked?PGR_VP_YES:PGR_VP_OK;}
int encounter(const PgrVgpActionsTarget *,uintptr_t *h){++encounters;return act_action(nullptr,h);}
int capture(const PgrVgpActionsTarget *r,uintptr_t *h){
  ++captures;if(expectedCapture)assert(r->target==expectedCapture);return act_action(nullptr,h);
}
int spin(const PgrVgpActionsSpin *,uintptr_t *h){++spins;return act_action(nullptr,h);}
int snapshot(void *svc,uint64_t owner,uint64_t lease,PgrVgpDiscoverySnapshot *out){
  ++snapshots;const int rc=dsc_snapshot(svc,owner,lease,out);
  if(slowSnapshot)::usleep(4000);return rc;
}
int balls(void *svc,uint64_t owner,uint64_t lease,int32_t *out){
  const int rc=mnt_balls(svc,owner,lease,out);
  if(afterBalls){auto cb=afterBalls;afterBalls=nullptr;cb();}return rc;
}
int cooldown(void *stop,void *response,uint64_t owner,uint64_t lease){
  ++cooldowns;cooldownLease=lease;return mnt_cooldown(stop,response,owner,lease);
}
void resetRegression(){
  assert(roots.empty());resetScenario();
  encounters=captures=spins=cooldowns=snapshots=0;cooldownLease=0;
  failPendingBag=slowSnapshot=false;afterBalls=nullptr;expectedCapture=nullptr;
  *reinterpret_cast<void**>(candObj.p())=&wildClass;
}
PgrVgpControllerTick catchTick(bool transfer=false){
  auto t=makeTick();t.mask=1;t.flags=PGR_VGC_TF_MANUAL_READY|(transfer?PGR_VGC_TF_TRANSFER_ENABLED:0);return t;
}
void drainEncounter(){
  g_pollRc=PGR_VR_DONE;auto t=catchTick();t.mask=0;t.lease_ms=0;run(t);assert(roots.empty());
}
void relocateCandidate(){
  memcpy(movedCandidate.buf,candObj.buf,sizeof(candObj.buf));
  for(auto &entry:roots)if(entry.second==candObj.p())entry.second=movedCandidate.p();
  *reinterpret_cast<void**>(candObj.p())=&otherClass;
}
}

int main(int argc,char **argv){
  assert(argc==2);void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);assert(lib);
  using GetApiFn=const void *(*)(uint32_t);
  auto get=reinterpret_cast<GetApiFn>(dlsym(lib,"pgrazor_native_api"));assert(get);
  const auto *core=static_cast<const pgrazor_native_api_v2*>(get(2));assert(core);
  api=static_cast<const PgrVgpControllerApi*>(core->query_feature(33,1));assert(api&&api->size==72);
  tid=static_cast<uint64_t>(gettid());
  POL.due=regressionDue;ACT.encounter=encounter;ACT.capture=capture;ACT.spin=spin;
  DSC.snapshot=snapshot;MNT.balls=balls;MNT.cooldown=cooldown;
  auto cfg=makeConfig();cfg.gc_new=reinterpret_cast<void*>(regressionGcNew);
  assert(api->install(&cfg)==PGR_VGC_OK);

  // Metadata/manual support readiness gates both a fresh scan and an encounter's later ball throw.
  resetRegression();
  {auto t=catchTick();t.flags=0;const auto o=run(t);
   assert(encounters==0&&captures==0&&o.status==PGR_VGC_ST_INIT_SUPPORT&&roots.empty());}
  run(catchTick());assert(encounters==1);g_pollRc=PGR_VR_DONE;
  {auto t=catchTick();t.flags=0;run(t);assert(captures==0);}
  drainEncounter();

  // An encounter is not a ball throw: only capture emits CATCH_ISSUED (and its UI cooldown event).
  resetRegression();
  const auto begin=run(catchTick());assert(encounters==1&&!(begin.flags&PGR_VGC_EV_CATCH_ISSUED));
  g_pollRc=PGR_VR_DONE;const auto thrown=run(catchTick());
  assert(captures==1&&(thrown.flags&PGR_VGC_EV_CATCH_ISSUED));
  {auto t=catchTick();t.mask=0;t.lease_ms=0;const auto done=run(t);
   assert(done.flags&PGR_VGC_EV_CATCH_RESULT);assert(!(done.flags&PGR_VGC_EV_CATCH_ISSUED));
   assert(done.success&&done.phase==PGR_VGC_PH_NONE&&roots.empty());}

  // Inventory roots can allocate successfully while the additional pending-bag root fails.
  resetRegression();failPendingBag=true;
  {const auto o=run(catchTick());assert(encounters==0&&captures==0&&o.status==PGR_VGC_ST_ERROR);
   assert(o.phase==PGR_VGC_PH_NONE&&roots.empty());assert(!failPendingBag);}

  // Sibling reads consume the original tick lease; they must never receive a fresh full lease later.
  resetRegression();slowSnapshot=true;
  {auto t=catchTick();t.lease_ms=1;const auto o=run(t);
   assert(snapshots==1&&encounters==0&&spins==0&&o.phase==PGR_VGC_PH_NONE&&roots.empty());}

  // A balls getter can relocate the rooted target without changing its identity.
  resetRegression();run(catchTick());g_pollRc=PGR_VR_DONE;
  afterBalls=[](){relocateCandidate();expectedCapture=movedCandidate.p();};
  {const auto o=run(catchTick());assert(captures==1&&o.phase==PGR_VGC_PH_CATCH);}
  drainEncounter();

  // A nested manual observer can claim that same target during inventory work, then GC relocates
  // BOTH roots. Admission must re-check the current manual identity before issuing the capture.
  resetRegression();run(catchTick());g_pollRc=PGR_VR_DONE;
  afterBalls=[](){assert(api->observe(manualGui.p(),candObj.p(),tid)==PGR_VGC_OK);relocateCandidate();};
  {const auto o=run(catchTick());assert(captures==0&&!(o.flags&PGR_VGC_EV_CATCH_ISSUED));}
  assert(api->observe(manualGui.p(),nullptr,tid)==PGR_VGC_OK);drainEncounter();

  // Successful issued spins still apply local cooldown through a separate completion window after Off.
  resetRegression();g_order=1;
  {auto t=makeTick();t.mask=2;t.flags=PGR_VGC_TF_MANUAL_READY;const auto o=run(t);
   assert(spins==1&&o.phase==PGR_VGC_PH_SPIN);}
  g_pollRc=PGR_VR_DONE;
  {auto t=makeTick();t.mask=0;t.lease_ms=0;t.allowed_ms=0;t.flags=0;const auto o=run(t);
   assert(cooldowns==1&&cooldownLease>0&&cooldownLease<=2500);
   assert((o.flags&PGR_VGC_EV_SPIN_RESULT)&&o.success&&o.phase==PGR_VGC_PH_NONE&&roots.empty());}

  // A handed-off record with no decision remains pending. A genuine foreign OS thread cannot spoof
  // the proven owner argument to submit terminal feedback and cancel that record.
  resetRegression();run(catchTick(true));g_pollRc=PGR_VR_DONE;run(catchTick(true));run(catchTick(true));
  {auto t=catchTick(true);api->revoke();t.mask=0;const auto o=run(t);
   assert(o.phase==PGR_VGC_PH_INVENTORY_WAIT&&!(o.flags&PGR_VGC_EV_TRANSFER_READY));}
  auto handoff=run(catchTick(true));assert(handoff.flags&PGR_VGC_EV_TRANSFER_READY);
  const uint64_t token=handoff.transfer_token;freeExports(handoff);assert(roots.empty());
  assert(api->transfer_feedback(token,0,tid)==PGR_VGC_OK);
  {const auto o=run(catchTick(true));assert(o.phase==PGR_VGC_PH_HANDED_OFF&&!(o.flags&PGR_VGC_EV_TRANSFER_CANCEL));}
  int foreign=0;std::thread worker([&](){foreign=api->transfer_feedback(token,PGR_VGC_FB_SUCCESS,tid);});worker.join();
  assert(foreign==PGR_VGC_ERR_OFF_OWNER);
  {const auto o=run(catchTick(true));assert(o.phase==PGR_VGC_PH_HANDED_OFF&&!(o.flags&PGR_VGC_EV_TRANSFER_CANCEL));}
  assert(api->transfer_feedback(token,PGR_VGC_FB_REQUESTED,tid)==PGR_VGC_OK);
  assert(run(catchTick(true)).status==PGR_VGC_ST_TRANSFERRING);
  assert(run(catchTick(true)).status==PGR_VGC_ST_TRANSFERRING);
  assert(api->transfer_feedback(token,PGR_VGC_FB_SUCCESS,tid)==PGR_VGC_OK);
  {const auto o=run(catchTick(true));assert(o.status==PGR_VGC_ST_TRANSFERRED&&o.phase==PGR_VGC_PH_NONE);
   assert(o.flags&PGR_VGC_EV_TRANSFER_CANCEL);assert(roots.empty());}
  assert(api->stop(tid)==PGR_VGC_OK);assert(roots.empty());
  puts("PASS VGP coordinator regressions: readiness, single capture event, lease decay, moving/manual targets, partial roots, offlease spin, pending/foreign feedback");
  dlclose(lib);
}
