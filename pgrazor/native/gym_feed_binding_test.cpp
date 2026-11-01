#include "gym_feed_binding.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdio>
namespace {
int bag,klass,method;unsigned argc=3,flags=0;bool found=true;
const char *parameters[]={"Holoholo.Rpc.Item","System.UInt64","System.String"};
const char *result="Niantic.Promises.IPromise<Holoholo.Rpc.GymFeedPokemonOutProto>";
void *get_class(void *o){assert(o==&bag);return &klass;}
void *get_method(void *k,const char *name,int n){assert(k==&klass&&!strcmp(name,"UseItemOnGymDefender")&&n==3);return found?&method:nullptr;}
unsigned count(void*){return argc;}
unsigned get_flags(void*,unsigned*){return flags;}
void *param(void*,unsigned i){return const_cast<char*>(parameters[i]);}
void *ret(void*){return const_cast<char*>(result);}
char *name(void *t){return strdup(static_cast<char*>(t));}
}
int main(){
 bool seen=false;pgr_gym_feed::observe_stock(seen,10,10);assert(!seen);
 pgr_gym_feed::observe_stock(seen,10,-1);assert(!seen);
 pgr_gym_feed::observe_stock(seen,10,9);assert(seen);
 pgr_gym_feed::observe_stock(seen,10,12);assert(seen);
 seen=false;pgr_gym_feed::observe_stock(seen,1,0);assert(seen);
 PgrBootBinding b{};b.runtime.object_get_class=reinterpret_cast<void*>(get_class);
 b.meta.class_get_method_from_name=reinterpret_cast<void*>(get_method);
 b.meta.method_get_param_count=reinterpret_cast<void*>(count);b.meta.method_get_flags=reinterpret_cast<void*>(get_flags);
 b.meta.method_get_param=reinterpret_cast<void*>(param);b.meta.method_get_return_type=reinterpret_cast<void*>(ret);
 b.meta.type_get_name=reinterpret_cast<void*>(name);b.meta.mem_free=reinterpret_cast<void*>(free);
 assert(pgr_gym_feed::resolve(b,&bag)==&method);
 result="Niantic.Promises.IPromise`1<Holoholo.Rpc.GymFeedPokemonOutProto>";assert(pgr_gym_feed::resolve(b,&bag)==&method);
 result="System.Void";assert(!pgr_gym_feed::resolve(b,&bag));result="Niantic.Promises.IPromise<Holoholo.Rpc.GymFeedPokemonOutProto>";
 parameters[1]="System.Int32";assert(!pgr_gym_feed::resolve(b,&bag));parameters[1]="System.UInt64";
 flags=0x10;assert(!pgr_gym_feed::resolve(b,&bag));flags=0;
 argc=2;assert(!pgr_gym_feed::resolve(b,&bag));argc=3;found=false;assert(!pgr_gym_feed::resolve(b,&bag));found=true;
 assert(!pgr_gym_feed::resolve(b,nullptr));b.meta.method_get_param=nullptr;assert(!pgr_gym_feed::resolve(b,&bag));
 puts("PASS gym item-bag binding: exact instance method, item/64-bit defender/string, typed promise, fail closed");
}
