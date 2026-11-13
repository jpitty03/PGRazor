#include <cassert>
#include <cstdio>
#include "throws.cpp"
int pgrazor_hook(void*,void*,void**){return -1;}
int pgrazor_unhook(void*){return -1;}
int pgrazor_instrument(void*,dobby_instrument_callback_t){return -1;}
int main(){
 PgrThrowValue input{7,0.9f,0,0,0,0x7b};
 auto unchanged=transform(input,3,1,ROUTE_SUBMIT,1000);assert(unchanged.radius==0.9f&&unchanged.spinning==0);
 pgr_safari_inventory_item=7;
 auto excellent=transform(input,3,1,ROUTE_SUBMIT,1001);
 assert(excellent.item==7&&excellent.radius>=0.1f&&excellent.radius<=0.25f&&excellent.spinning==1&&excellent.bullseye==1&&excellent.pad==0x7b&&excellent.missed==0);
 auto paired=transform(excellent,3,1,ROUTE_HIT,1002);assert(paired.radius==excellent.radius);
 for(int id:{4,6,8,701}){input.item=id;auto out=transform(input,3,1,ROUTE_SUBMIT,1003);assert(out.radius==input.radius&&out.spinning==0);}
 input.item=7;input.missed=1;assert(transform(input,3,1,ROUTE_SUBMIT,1004).missed==1);
 std::puts("PASS Safari throw grading: resolved ball, Excellent curve, paired hit, excluded items, missed throw");
}
