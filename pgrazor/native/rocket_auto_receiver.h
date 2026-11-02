#pragma once
namespace pgr_rocket_auto {
using MethodClass = void *(*)(const void *);
using ObjectClass = void *(*)(void *);
using Assignable = bool (*)(void *, void *);
using ValueType = bool (*)(void *);
inline bool reference_return_matches(void *declared,void *concrete,ValueType value,Assignable assignable){
 return declared&&concrete&&value&&assignable&&!value(declared)&&
  (assignable(declared,concrete)||assignable(concrete,declared));
}
// runtime_invoke does not validate the receiver: an unrelated instance can be
// interpreted using the declaring class layout and cause a native crash.
inline bool receiver_matches(const void *method, void *receiver, MethodClass declaring,
                             ObjectClass actual, Assignable assignable) {
 if (!method || !receiver || !declaring || !actual || !assignable) return false;
 void *owner=declaring(method), *type=actual(receiver);
 return owner && type && assignable(owner,type);
}
}
