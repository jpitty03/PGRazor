#include "rocket_actions.h"
#include "pgrazor_native_api.h"
#include <dlfcn.h>
#include <unistd.h>
#include <sys/wait.h>
#include <thread>
#include <cstdio>
const pgr_rocket_actions_api_v1 *api;
int scenario(int n){
 const auto tid=uint64_t(gettid());api->update(7,2500,tid);api->observe(PGR_ROCKET_ENTER,tid);
 auto token=api->begin(tid);if(!token)return 90;
 switch(n){
 case 0: if(!api->claim(token,1,tid)||api->claim(token,1,tid)||api->begin(tid))return 1;break;
 case 1: api->update(0,0,tid);if(api->claim(token,1,tid)||api->begin(tid))return 2;break;
 case 2: api->update(0,0,tid);api->update(7,2500,tid);if(api->claim(token,1,tid))return 3;break;
 case 3: api->observe(0,tid);api->observe(1,tid);if(api->claim(token,1,tid))return 4;break;
 case 4: api->observe(2,tid);token=api->begin(tid);if(!api->claim(token,4,tid))return 5;
  usleep(270000);token=api->begin(tid);if(api->claim(token,4,tid))return 6;
  api->observe(2,tid);token=api->begin(tid);if(!api->claim(token,4,tid))return 7;break;
 case 5: api->observe(2,tid);token=api->begin(tid);api->observe(4,tid);if(api->claim(token,4,tid))return 8;break;
 case 6: api->observe(2,tid);token=api->begin(tid);api->observe(3,tid);if(api->claim(token,4,tid))return 9;break;
 case 7: api->update(1,10,tid);token=api->begin(tid);usleep(30000);if(api->claim(token,1,tid))return 10;break;
 case 8: {bool bad=false;std::thread t([&](){bad=api->claim(token,1,tid)||api->claim(token,1,gettid())||api->observe(0,gettid());});t.join();
  if(bad||!api->claim(token,1,tid))return 11;break;}
 case 9: api->stop(tid);api->update(7,2500,tid);if(api->claim(token,1,tid)||api->begin(tid))return 12;break;
 case 10: api->update(1,2500,tid);token=api->begin(tid);if(api->claim(token,2,tid)||api->claim(token,1,tid))return 13;break;
 case 11: {auto newer=api->begin(tid);if(api->claim(token,1,tid)||!api->claim(newer,1,tid))return 14;break;}
 case 12: if(!api->claim(token,2,tid))return 15;usleep(300000);if(api->begin(tid))return 16;usleep(220000);if(!api->begin(tid))return 17;break;
 case 13: if(!api->claim(token,3,tid))return 18;usleep(520000);if(api->begin(tid))return 19;usleep(110000);if(!api->begin(tid))return 20;break;
 case 14: if(api->claim(token,4,tid))return 21;break;
 case 15: {std::thread t([&](){api->update(7,2500,tid);});t.join();if(api->claim(token,1,tid)||api->begin(tid))return 22;break;}
 case 16: {std::thread t([&](){api->stop(gettid());});t.join();if(api->claim(token,1,tid)||api->begin(tid))return 23;
  api->update(7,2500,tid);api->observe(1,tid);if(!api->begin(tid))return 24;break;}
 }
 return 0;
}
int main(int argc,char **argv){
 if(argc!=2)return 80;void *lib=dlopen(argv[1],RTLD_NOW);if(!lib)return 81;
 auto get=reinterpret_cast<const void *(*)(uint32_t)>(dlsym(lib,"pgrazor_native_api"));
 auto core=get?static_cast<const pgrazor_native_api_v2 *>(get(2)):nullptr;if(!core)return 82;
 api=static_cast<const pgr_rocket_actions_api_v1 *>(core->query_feature(20,1));
 if(!api||api->size!=48||core->query_feature(20,99))return 83;
 for(int n=0;n<17;++n){pid_t pid=fork();if(pid==0)_exit(scenario(n));int status=0;waitpid(pid,&status,0);
  if(!WIFEXITED(status)||WEXITSTATUS(status)){std::printf("FAIL Rocket actions %d code %d\n",n,WIFEXITED(status)?WEXITSTATUS(status):-1);return 1;}}
 std::puts("PASS 17 native Rocket action cases: scope, consumed tickets, revoke/rearm, shields once/manual/close, real expiry/cadence, wrong OS thread, foreign-thread stop and action-mask isolation");
}
