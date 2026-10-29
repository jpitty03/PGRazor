#include "transfer_policy.h"
namespace {
// 0 authorizes the existing transfer orchestration. Every other result keeps it.
int decide(const PgrTransferInput *v) {
    if(!v || v->version!=1 || v->size!=sizeof(*v))return 16;
    if(v->keep_all)return 1;
    if(v->keep_count>2000 || (v->keep_count&&!v->keep_ids))return 2;
    if(v->keep_count) {
        if(v->species<1 || v->species>2000)return 3;
        bool found=false;
        for(uint32_t i=0;i<v->keep_count;++i) {
            const int32_t id=v->keep_ids[i];
            if(id<1||id>2000)return 2;
            if(id==v->species)found=true;
        }
        if(found)return 4;
    }
    if(v->attack<0||v->attack>15||v->defense<0||v->defense>15||v->stamina<0||v->stamina>15)return 5;
    if((v->known&207)!=207)return 6; // rare/shiny/event/background/favorite/egg
    if((v->protect&48&~v->known)!=0)return 7;
    if(v->flags&192)return 8;
    if((v->attack+v->defense+v->stamina)*100>=90*45)return 9;
    for(uint32_t bit=0;bit<6;++bit)if(v->protect&v->flags&(1u<<bit))return 10+bit;
    return 0;
}
const PgrTransferApi api{1,sizeof(PgrTransferApi),decide};
static_assert(sizeof(PgrTransferInput)==56,"transfer input ABI");
static_assert(sizeof(PgrTransferApi)==16,"transfer API ABI");
}
const void *pgrazor_transfer_policy_api(uint32_t version){return version==1?&api:nullptr;}
