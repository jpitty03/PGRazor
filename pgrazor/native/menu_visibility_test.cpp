#include <cassert>
#include <cstdio>
#include "menu_visibility.cpp"
extern "C" const void *pgrazor_runtime_bootstrap_api(uint32_t){return nullptr;}
extern "C" const void *pgrazor_runtime_metadata_api(uint32_t){return nullptr;}
extern "C" const void *pgrazor_runtime_transport_api(uint32_t){return nullptr;}
int pgrazor_hook(void*,void*,void**){return -1;}
namespace {
bool state=false,fail=false,reject=false;
bool top=true,visible=true,opened_state=true,fail_root=false;
int opens=0,closes=0,roots=0,dismisses=0;
void *call(const void *method,void *self,void**,void **ex){assert(self==&state);if(fail)*ex=&state;return method?const_cast<void*>(method):&state;}
void *unbox_value(void *p){return p;}
void *create_root(void *p,int pinned){assert(!pinned);if(fail_root)return nullptr;++roots;return p;}
void *target_root(void *p){return p;}
void free_root(void *p){assert(p);--roots;}
void dismissed(void *self,const void*){assert(self==&state);assert(!controller_handle);++dismisses;}
void opened(void *self,const void *method){assert(self==&state&&method==&opens);assert(menu_open.load());++opens;if(!reject)state=true;}
void closed(void *self,const void *method){assert(self==&state&&method==&closes);++closes;if(!reject)state=false;}
}
int main(){
 original_open=opened;original_close=closed;invoke=call;unbox=unbox_value;
 gc_new=create_root;gc_target=target_root;gc_free=free_root;original_dismiss=dismissed;
 top_getter=&top;visible_getter=&visible;open_getter=&opened_state;
 assert(!map_controls_on_top()); // No map controller observed yet.
 open_menu(&state,&opens);assert(menu_open&&opens==1);
 reject=true;close_menu(&state,&closes);assert(menu_open&&closes==1);
 reject=false;close_menu(&state,&closes);assert(!menu_open&&closes==2);
 reject=true;open_menu(&state,&opens);assert(!menu_open&&opens==2);
 fail=true;close_menu(&state,&closes);assert(menu_open);
 fail=false;reject=false;original_initialize=closed;
 initialize_menu(&state,&closes);assert(!menu_open); // New map controller clears previous scene state.
 assert(map_controls_on_top()&&roots==1);
 retain_controller(&state);assert(roots==1); // Repeated observations do not accumulate roots.
 top=false;assert(!map_controls_on_top()); // Safety/news popup over a still-rendered map.
 top=true;assert(map_controls_on_top());
 visible=false;assert(!map_controls_on_top());visible=true;
 opened_state=false;assert(!map_controls_on_top());opened_state=true;
 fail=true;assert(!map_controls_on_top());fail=false;
 dismiss_menu(&state,nullptr);assert(dismisses==1&&roots==0&&!map_controls_on_top());
 fail_root=true;initialize_menu(&state,&closes);assert(!map_controls_on_top()&&roots==0);
 fail_root=false;initialize_menu(&state,&closes);assert(map_controls_on_top()&&roots==1);
 dismiss_menu(&state,nullptr);assert(roots==0);
 pgr_menu_visibility_tick(1000);assert(!attempted&&!ready);
 puts("menu_visibility_test: PASS (forwarding, rejected transitions, popups, hidden/dismissed map, failed reads/roots, root lifetime, bootstrap retry)");
}
