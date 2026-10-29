#pragma once
#include <stdint.h>

extern "C" const void *pgrazor_interaction_host_api(uint32_t);

// Private owner-side view of feature69 API2. No dependency on API1's diagnostic
// snapshot size. Missing API2 is an inert observer, never permission to re-hook.
namespace pgr_interaction_owner {
struct Api {
    uint32_t version,size;
    int (*wants_target)(uint32_t);
    uint64_t (*observe_enter)(uint32_t,void*,void*);
    void (*observe_leave)(uint64_t,void*);
    void (*observe_abort)(uint64_t);
};
static_assert(sizeof(Api)==40,"interaction observer API2");
struct Phase {
    const Api *api=nullptr;uint64_t token=0;uint32_t target;
    explicit Phase(uint32_t id):target(id){
        const auto *p=static_cast<const Api*>(pgrazor_interaction_host_api(2));
        if(p&&p->version==2&&p->size==sizeof(Api)&&p->wants_target&&p->observe_enter&&p->observe_leave&&p->observe_abort&&p->wants_target(id))api=p;
    }
    bool wanted()const{return api!=nullptr;}
    void enter(void *self,void *arg=nullptr){if(api)token=api->observe_enter(target,self,arg);}
    void leave(void *result=nullptr){if(token){auto t=token;token=0;api->observe_leave(t,result);}}
    void abort(){if(token){auto t=token;token=0;api->observe_abort(t);}}
    ~Phase(){abort();}
    Phase(const Phase&)=delete;Phase&operator=(const Phase&)=delete;
};
// Optional observer pins only. Inactive probes never call a GC export. Owner
// callbacks must keep this alive through their original and Phase's leave/abort.
struct Pins {
    using New=uintptr_t(*)(void*,int);using Get=void*(*)(uintptr_t);using Free=void(*)(uintptr_t);
    New make;Get get;Free free;uintptr_t handles[2]{};void *raw[2]{};bool valid=true;
    Pins(bool active,void *new_fn,void *get_fn,void *free_fn,void *self,void *arg=nullptr):
        make(reinterpret_cast<New>(new_fn)),get(reinterpret_cast<Get>(get_fn)),free(reinterpret_cast<Free>(free_fn)){
        raw[0]=self;raw[1]=arg;
        if(active){if(!make||!get||!free){valid=false;return;}
            for(unsigned i=0;i<2;++i)if(raw[i]){handles[i]=make(raw[i],1);if(!handles[i])valid=false;}}
    }
    void *at(unsigned i)const{return handles[i]?get(handles[i]):raw[i];}
    ~Pins(){for(unsigned i=2;i;--i)if(handles[i-1])free(handles[i-1]);}
    Pins(const Pins&)=delete;Pins&operator=(const Pins&)=delete;
};
struct AbortBeforeRoots {Phase &phase;~AbortBeforeRoots(){phase.abort();}};
}
