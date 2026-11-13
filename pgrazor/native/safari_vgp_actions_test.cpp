// Reuse the existing fake heap; execute production actions directly so this
// fixture can publish the optional resolved event-ball identity in the same image.
#define main unused_actions_fixture_main
#include "vgp_actions_test.cpp"
#undef main
#include "vgp_actions.cpp"
int main(){
 auto *api=static_cast<const PgrVgpActionsApi*>(pgrazor_vgp_actions_api(1));g_api=api;
 auto cfg=make_config();assert(api->install(&cfg)==0);
 uint64_t tid=this_tid();g_owner=tid;FakeWild wild{&cls_wild};uintptr_t h=0;
 assert(api->update(PGR_VA_MASK_CATCH,2000,1,tid)==0);
 auto r=make_target(&wild,1,tid);
 assert(pgr_vgp_capture_selected(&r,7,3,1,&h)==PGR_VA_ERR_BAD_VERSION&&g_capture_calls==0);
 pgr_safari_inventory_item=7;
 assert(pgr_vgp_capture_selected(&r,7,3,1,&h)==PGR_VA_ISSUED&&g_capture_calls==1&&h);
 int item=0;float radius=0;std::memcpy(&item,g_ball,4);std::memcpy(&radius,g_ball+4,4);
 assert(item==7&&radius==0.1f&&g_ball[8]==1&&g_ball[9]==1&&g_ball[10]==0&&g_ball[11]==0);
 fake_gc_free(h);h=0;
 for(int id:{0,4,5,6,8,701})assert(pgr_vgp_capture_selected(&r,id,3,1,&h)==PGR_VA_ERR_BAD_VERSION&&!h);
 assert(api->stop(tid)==0);assert(pgr_vgp_capture_selected(&r,7,3,1,&h)==PGR_VA_OK&&!h&&g_capture_calls==1);
 assert(g_handles.empty());std::puts("PASS VGP Safari action: resolved ball, Excellent curve Always Hit payload, special-ball exclusions, stop, roots");
}
