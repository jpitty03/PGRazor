#include "smart_encounter_validation.h"
#include <cassert>
#include <cstdio>
struct Object {void *klass;void *ref;int status;};
int wild_class,wrapper_class,response_class,pokemon_class,map_field,status_field,pokemon_field;
void *get_class(void *obj){return static_cast<Object*>(obj)->klass;}
void read(void *obj,void *field,void *out){
 auto o=static_cast<Object*>(obj);
 if(field==&status_field)*static_cast<int*>(out)=o->status;
 else {assert(field==&map_field||field==&pokemon_field);*static_cast<void**>(out)=o->ref;}
}
int main(){
 Object pokemon{&pokemon_class},raw{&response_class,&pokemon,1},wild{&wild_class,&raw},wrapper{&wrapper_class};
 PgrSmartEncounterBinding b{&wild_class,&wrapper_class,&response_class,&map_field,&status_field,&pokemon_field,1,get_class,read};
 assert(pgr_smart_validate_encounter(b,&wild,&wrapper)==1);
 assert(pgr_smart_validate_encounter(b,&wild,&raw)==-11); // Raw RPC is NOT promise wrapper.
 raw.status=5;assert(pgr_smart_validate_encounter(b,&wild,&wrapper)==0);
 raw.status=1;raw.ref=nullptr;assert(pgr_smart_validate_encounter(b,&wild,&wrapper)==0);
 wild.ref=nullptr;assert(pgr_smart_validate_encounter(b,&wild,&wrapper)==0);
 wild.ref=&pokemon;assert(pgr_smart_validate_encounter(b,&wild,&wrapper)==-11);
 assert(pgr_smart_validate_encounter(b,nullptr,&wrapper)==-11);
 assert(pgr_smart_validate_encounter(b,&wild,nullptr)==-11);
 puts("PASS actual smart encounter validation: wrapper versus raw response, success, rejection, missing data and wrong types");
}
