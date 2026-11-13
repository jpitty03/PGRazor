#include <cassert>
#include <cstdio>
#include "always_hit.cpp"
// No hooks are installed: exercise the real eligibility/throw replacement.
int pgrazor_hook(void*,void*,void**){return -1;}
int pgrazor_unhook(void*){return -1;}
int pgrazor_instrument(void*,dobby_instrument_callback_t){return -1;}
namespace {
int test_class,item_field,bread_field;
struct Ball {int item;};
void *test_class_of(void*){return &test_class;}
void test_field(void *o,void *f,void *out){
 if(f==&item_field)*static_cast<int*>(out)=static_cast<Ball*>(o)->item;
 else{assert(f==&bread_field);*static_cast<uint8_t*>(out)=0;}
}
PgrAlwaysHitValue original_throw(void *o,uint8_t missed,void*){return {static_cast<Ball*>(o)->item,0.15f,1,1,missed,0x7b};}
}
int main(){
 g_config.ball_class=&test_class;g_config.object_get_class=reinterpret_cast<void*>(test_class_of);
 g_config.field_get_value=reinterpret_cast<void*>(test_field);g_config.field_item=&item_field;g_config.field_bread=&bread_field;
 g_orig_make_throw.store(original_throw);g_enabled=1;g_allowed=1;g_tid=gettid();g_lease_deadline=boottime_ms()+2500;
 Ball ball{7};auto throw_ball=[&](){return make_throw_replacement(&ball,1,nullptr);};
 assert(throw_ball().missed==1); // cannot accept an unresolved event item
 pgr_safari_inventory_item=7;
 auto result=throw_ball();assert(result.missed==0&&result.item==7&&result.radius==0.15f&&result.bullseye==1&&result.spinning==1&&result.pad==0x7b);
 for(int id:{1,2,3,5}){ball.item=id;assert(throw_ball().missed==0);}
 for(int id:{0,4,6,8,701,1000}){ball.item=id;assert(throw_ball().missed==1);}
 ball.item=7;g_allowed=0;assert(throw_ball().missed==1);g_allowed=1;g_lease_deadline=0;assert(throw_ball().missed==1);
 std::puts("PASS Safari Always Hit: resolved inventory ball, original payload, regular balls, excluded items, permission and lease");
}
