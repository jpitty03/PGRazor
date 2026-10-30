#pragma once
#include <stdint.h>

// SendEncounterRequest resolves a presentation wrapper. The raw RPC response is
// stored on WildMapPokemon; it is not the promise's resolved value.
struct PgrSmartEncounterBinding {
 void *wild_class,*wrapper_class,*response_class;
 void *map_response_field,*status_field,*pokemon_field;
 int32_t success;
 void *(*object_class)(void*);
 void (*read_field)(void*,void*,void*);
};
inline int pgr_smart_validate_encounter(const PgrSmartEncounterBinding &b,void *target,void *wrapper){
 if(!target||!wrapper||b.object_class(target)!=b.wild_class||b.object_class(wrapper)!=b.wrapper_class)return -11;
 void *response=nullptr;b.read_field(target,b.map_response_field,&response);
 if(!response)return 0;
 if(b.object_class(response)!=b.response_class)return -11;
 int32_t status=0;void *pokemon=nullptr;
 b.read_field(response,b.status_field,&status);b.read_field(response,b.pokemon_field,&pokemon);
 return status==b.success&&pokemon?1:0;
}
