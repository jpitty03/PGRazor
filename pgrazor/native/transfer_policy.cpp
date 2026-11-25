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
    const int keep_iv=pgr_transfer_keep_iv_percent(v->keep_iv);
    if(keep_iv<0)return 17;
    if((v->attack+v->defense+v->stamina)*100>=keep_iv*45)return 9;
    for(uint32_t bit=0;bit<6;++bit)if(v->protect&v->flags&(1u<<bit))return 10+bit;
    // Protect bit 7 (128): keep a costumed Pokemon (facts flag bit 8).
    if((v->protect&128)&&(v->flags&256))return 20;
    // Protect bit 8 (256): keep a shadow Pokemon (facts flag bit 9); unknown when bit 10 is missing.
    if((v->protect&256)&&!(v->flags&1024))return 6;
    if((v->protect&256)&&(v->flags&512))return 21;
    // Protect bits 9 (512) and 10 (1024): keep a Dynamax / Gigantamax Pokemon (facts flags 2048 / 4096);
    // unknown when the Max fact (8192) is missing.
    if((v->protect&1536)&&!(v->flags&8192))return 6;
    if((v->protect&512)&&(v->flags&2048))return 22;
    if((v->protect&1024)&&(v->flags&4096))return 23;
    // Protect bit 6 (64): keep a 0/0/0 Pokemon.
    if((v->protect&64)&&v->attack==0&&v->defense==0&&v->stamina==0)return 19;
    return 0;
}
const PgrTransferApi api{1,sizeof(PgrTransferApi),decide};
static_assert(sizeof(PgrTransferInput)==56,"transfer input ABI");
static_assert(sizeof(PgrTransferApi)==16,"transfer API ABI");
}
const void *pgrazor_transfer_policy_api(uint32_t version){return version==1?&api:nullptr;}
