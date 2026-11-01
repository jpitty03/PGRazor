#pragma once
#include "runtime_bootstrap.h"
#include <string>

// Use the game's item-bag operation, including its inventory cache update and
// response handling. Never predict/decrement inventory ourselves or replay it.
namespace pgr_gym_feed {
// Once observed, later gains from spins/rewards cannot erase this operation's
// cache acknowledgement. Success is still decided separately by the promise.
inline void observe_stock(bool &seen,int before,int current){
 if(before>0&&current>=0&&current<before)seen=true;
}
inline void *resolve(const PgrBootBinding &b,void *bag){
 if(!bag||!b.runtime.object_get_class||!b.meta.class_get_method_from_name||
    !b.meta.method_get_param_count||!b.meta.method_get_param||!b.meta.method_get_return_type||
    !b.meta.method_get_flags||!b.meta.type_get_name||!b.meta.mem_free)return nullptr;
 auto type=[&](void *t){
  if(!t)return std::string();
  char *s=reinterpret_cast<char*(*)(void*)>(b.meta.type_get_name)(t);
  std::string result=s?s:"";if(s)reinterpret_cast<void(*)(void*)>(b.meta.mem_free)(s);return result;
 };
 void *k=reinterpret_cast<void*(*)(void*)>(b.runtime.object_get_class)(bag);
 void *m=reinterpret_cast<void*(*)(void*,const char*,int)>(b.meta.class_get_method_from_name)(k,"UseItemOnGymDefender",3);
 if(!m||reinterpret_cast<unsigned(*)(void*)>(b.meta.method_get_param_count)(m)!=3)return nullptr;
 unsigned impl=0;if(reinterpret_cast<unsigned(*)(void*,unsigned*)>(b.meta.method_get_flags)(m,&impl)&0x10)return nullptr;
 const char *params[]={"Holoholo.Rpc.Item","System.UInt64","System.String"};
 for(unsigned i=0;i<3;i++)if(type(reinterpret_cast<void*(*)(void*,unsigned)>(b.meta.method_get_param)(m,i))!=params[i])return nullptr;
 auto result=type(reinterpret_cast<void*(*)(void*)>(b.meta.method_get_return_type)(m));
 if(result!="Niantic.Promises.IPromise<Holoholo.Rpc.GymFeedPokemonOutProto>"&&result!="Niantic.Promises.IPromise`1<Holoholo.Rpc.GymFeedPokemonOutProto>")return nullptr;
 return m;
}
}
