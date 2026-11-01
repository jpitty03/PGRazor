#include <cassert>
#include <cstdio>
#include "profile_scroll.cpp"
extern "C" const void *pgrazor_runtime_bootstrap_api(uint32_t){return nullptr;}
extern "C" const void *pgrazor_runtime_metadata_api(uint32_t){return nullptr;}
int pgrazor_hook(void*,void*,void**){return -1;}
namespace {
int scroll_a,scroll_b,field_stub;
int forwarded=0,updated=0;
void field_read(void *self,void*,void *out){*static_cast<void**>(out)=self;}
void original_set(void*,float,int,const void*){++forwarded;}
void original_update(void *self,PlayerInfo info,const void *method){
 assert(info.day==123&&info.player==&field_stub&&method==&field_stub);++updated;
 int before=forwarded;
 set_position(self,1.f,1,nullptr);assert(forwarded==before);
 set_position(self,.4f,1,nullptr);assert(forwarded==before+1);
 set_position(self,1.f,0,nullptr);assert(forwarded==before+2);
 set_position(self==&scroll_a?&scroll_b:&scroll_a,1.f,1,nullptr);assert(forwarded==before+3);
 if(self==&scroll_a){refresh(&scroll_b,info,method);assert(refresh_scroll==self);}
}
}
int main(){
 original_position=original_set;original_refresh=original_update;read_field=field_read;scroll_field=&field_stub;
 // Initial opening and user scrolling outside the callback remain untouched.
 set_position(&scroll_a,1.f,1,nullptr);assert(forwarded==1);
 refresh(&scroll_a,PlayerInfo{&field_stub,nullptr,nullptr,123},&field_stub);
 assert(updated==2&&forwarded==7&&refresh_scroll==nullptr);
 set_position(&scroll_a,1.f,1,nullptr);assert(forwarded==8);
 pgr_profile_scroll_install();assert(!installed); // Retry while bootstrap absent.
 puts("profile_scroll_test: PASS (updates, scoped reset, other lists, nesting, ABI forwarding)");
}
