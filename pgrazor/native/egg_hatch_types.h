#pragma once
namespace pgr_egg {
using Assignable = bool (*)(void *, void *);
inline bool hatch_type(void *base,void *actual,Assignable assignable){
 return base&&actual&&(base==actual||(assignable&&assignable(base,actual)));
}
}
