// Standalone host/NDK fixture. Compile this TU alone with an Android log shim
// on a desktop host, or with -llog -ldl on Android. Never package it.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "batch_evolution.cpp"

namespace {
std::map<uintptr_t,void*> roots;
uintptr_t next_handle=1;
int allocations=0,frees=0,polls=0;
char promise_object,response_object,evolved_object,bag_object,player_object;
char original_object,replacement_object,boxed_candy,boxed_count;
char tag_service_object,tag_proto_object,tag_promise_object,change_object;
char change_list_object,add_list_object,remove_list_object,inventory_tags_object;
char boxed_selected_tag,boxed_other_tag;
uint64_t returned_id=99;int returned_species=2;bool has_evolved=true;
char response_class,evolved_class,enum_class,enum_type,bag_class,player_class,boxed_class;
char tag_service_class,tag_proto_class,tag_promise_class,change_class;
char change_list_class,tag_list_class,inventory_tags_class,boxed_tag_class;
char pokemon_method,candy_method,tag_set_method,ctor_method,add_method,count_method,item_method;
bool original_present=true,replacement_present=false;
int original_species=2,replacement_species=2,candy_balance=9;
int original_lookups=0,replacement_lookups=0;
bool selected_tag_present=true,other_tag_present=true;
bool tag_complete=false,tag_error=false,tag_accepted=false;
int tag_requests=0,change_adds=0,remove_adds=0,add_adds=0;
int tag_list_allocations=0;
void* changed_pokemon=nullptr;
uint64_t selected_tag_id=77,other_tag_id=88;
struct Field {const char* name;const char* type;};
Field int_type{"boxed","System.Int32"};
Field uint64_type{"boxed","System.UInt64"};
Field tag_ids_field{"pokemonTagIds_","Niantic.Protobuf.Collections.RepeatedField<System.UInt64>"};
Field pokemon_proto_field{"PokemonProto","Holoholo.Rpc.PokemonProto"};
Field tags_add_field{"TagsToAdd","System.Collections.Generic.List<Holoholo.Rpc.PokemonTagProto>"};
Field tags_remove_field{"TagsToRemove","System.Collections.Generic.List<Holoholo.Rpc.PokemonTagProto>"};
Field tag_error_field{"errorCalled","System.Boolean"};
Field tag_complete_field{"completeCalled","System.Boolean"};
Field tag_value_field{"completedValue","System.Boolean"};
Field tag_set_return{"return","Niantic.Promises.IPromise<System.Boolean>"};
Field tag_set_param{"param","System.Collections.Generic.List<Niantic.Holoholo.Tagging.PokemonTagChanges>"};
Field result_field{"Result","EvolutionResult"},success_field{"Success","EvolutionResult"};
Field evolved_field{"EvolvedPokemon","Holoholo.Rpc.PokemonProto"};
Field id_field{"id_","System.UInt64"},species_field{"pokemonId_","System.Int32"};
Field xp_field{"ExpAwarded","System.Int32"},award_field{"CandyAwarded","System.Int32"};
int result_code=1,poll_code=PGR_VR_DONE;
std::string config="1|42|2|Evolve";
bool live=true,ready=true,fresh=true,fault=false,sequence_change=false;
int sample_count=0,slot9_reads=0,slot16_reads=0,slot11_reads=0;
std::vector<std::string> events;

uintptr_t root_new(void* object,int pinned){assert(object&&!pinned);uintptr_t h=next_handle++;assert(roots.emplace(h,object).second);++allocations;return h;}
void* root_get(uintptr_t h){assert(roots.count(h));return roots.at(h);}
void root_free(uintptr_t h){assert(roots.erase(h)==1);++frees;}
void* object_class(void* object){
    if(object==&response_object)return &response_class;
    if(object==&evolved_object)return &evolved_class;
    if(object==&original_object||object==&replacement_object)return &evolved_class;
    if(object==&bag_object)return &bag_class;
    if(object==&player_object)return &player_class;
    if(object==&boxed_candy)return &boxed_class;
    if(object==&boxed_count)return &boxed_class;
    if(object==&tag_service_object)return &tag_service_class;
    if(object==&tag_proto_object)return &tag_proto_class;
    if(object==&tag_promise_object)return &tag_promise_class;
    if(object==&change_object)return &change_class;
    if(object==&change_list_object)return &change_list_class;
    if(object==&add_list_object||object==&remove_list_object)return &tag_list_class;
    if(object==&inventory_tags_object)return &inventory_tags_class;
    if(object==&boxed_selected_tag||object==&boxed_other_tag)return &boxed_tag_class;
    assert(false&&"unexpected object class");return nullptr;
}
void* lookup_field(void* klass,const char* name){
    if(klass==&response_class){
        for(Field* f:{&result_field,&evolved_field,&xp_field,&award_field})if(!strcmp(name,f->name))return f;
    }
    if(klass==&evolved_class){
        for(Field* f:{&id_field,&species_field})if(!strcmp(name,f->name))return f;
        if(!strcmp(name,"pokemonTagIds_"))return &tag_ids_field;
    }
    if(klass==&change_class){
        for(Field* f:{&pokemon_proto_field,&tags_add_field,&tags_remove_field})if(!strcmp(name,f->name))return f;
    }
    if(klass==&tag_promise_class){
        for(Field* f:{&tag_error_field,&tag_complete_field,&tag_value_field})if(!strcmp(name,f->name))return f;
    }
    if(klass==&enum_class&&!strcmp(name,"Success"))return &success_field;
    return nullptr;
}
void* field_type(void* f){return f==&result_field||f==&success_field?&enum_type:f;}
void* class_from_type(void* type){
    if(type==&enum_type)return &enum_class;
    if(type==&tag_ids_field)return &inventory_tags_class;
    if(type==&tag_set_param)return &change_list_class;
    if(type==&tags_add_field||type==&tags_remove_field)return &tag_list_class;
    assert(false&&"unexpected field type class");return nullptr;
}
bool class_is_value(void*){return false;}
int field_flags(void* f){return f==&success_field?0x8050:0;}
void static_field(void* f,void* out){assert(f==&success_field);*static_cast<int*>(out)=1;}
void read_field(void* object,void* f,void* out){
    if(object==&response_object){
        if(f==&result_field)*static_cast<int*>(out)=result_code;
        else if(f==&evolved_field)*static_cast<void**>(out)=has_evolved?&evolved_object:nullptr;
        else if(f==&xp_field)*static_cast<int*>(out)=500;
        else if(f==&award_field)*static_cast<int*>(out)=1;
        else assert(false&&"unexpected response field");
    }else if(object==&evolved_object){
        if(f==&id_field)*static_cast<uint64_t*>(out)=returned_id;
        else if(f==&species_field)*static_cast<int*>(out)=returned_species;
        else assert(false&&"unexpected evolved field");
    }else if(object==&original_object||object==&replacement_object){
        if(f==&species_field)*static_cast<int*>(out)=object==&original_object?original_species:replacement_species;
        else if(f==&tag_ids_field)*static_cast<void**>(out)=&inventory_tags_object;
        else assert(false&&"unexpected inventory field");
    }else if(object==&tag_promise_object){
        if(f==&tag_error_field)*static_cast<bool*>(out)=tag_error;
        else if(f==&tag_complete_field)*static_cast<bool*>(out)=tag_complete;
        else if(f==&tag_value_field)*static_cast<bool*>(out)=tag_accepted;
        else assert(false&&"unexpected tag promise field");
    }else assert(false&&"unexpected field owner");
}
void write_field(void* object,void* f,void* value){
    assert(object==&change_object);
    if(f==&pokemon_proto_field){changed_pokemon=value;return;}
    if(f==&tags_add_field){assert(value==&add_list_object&&add_adds==0);return;}
    if(f==&tags_remove_field){assert(value==&remove_list_object&&remove_adds==1);return;}
    assert(false&&"unexpected tag change field");
}
void reject_scalar_reference(void*,void* f,void*){
    assert(f!=&pokemon_proto_field&&f!=&tags_add_field&&f!=&tags_remove_field
           &&"reference field written through scalar field_set_value");
    assert(false&&"unexpected scalar field write");
}
void* lookup_method(void* klass,const char* name,int count){
    if(count==1){
        if(klass==&bag_class&&!strcmp(name,"GetPokemon"))return &pokemon_method;
        if(klass==&player_class&&!strcmp(name,"GetCandyCountForPokemonFamily"))return &candy_method;
        if(klass==&tag_service_class&&!strcmp(name,"SetPokemonTags"))return &tag_set_method;
        if((klass==&change_list_class||klass==&tag_list_class)&&!strcmp(name,"Add"))return &add_method;
        if(klass==&inventory_tags_class&&!strcmp(name,"get_Item"))return &item_method;
    }
    if(count==0){
        if((klass==&change_list_class||klass==&tag_list_class||klass==&change_class)&&!strcmp(name,".ctor"))return &ctor_method;
        if(klass==&inventory_tags_class&&!strcmp(name,"get_Count"))return &count_method;
    }
    return nullptr;
}
unsigned method_count(void* method){
    if(method==&ctor_method||method==&count_method)return 0;
    assert(method==&pokemon_method||method==&candy_method||method==&tag_set_method
           ||method==&add_method||method==&item_method);return 1;
}
void* method_return_type(void* method){assert(method==&tag_set_method);return &tag_set_return;}
void* method_param(void* method,unsigned index){assert(method==&tag_set_method&&index==0);return &tag_set_param;}
void* invoke_method(void* method,void* object,void** args,void** exception){
    assert(exception);*exception=nullptr;
    if(method==&pokemon_method){
        assert(object==&bag_object);
        uint64_t id=*static_cast<uint64_t*>(args[0]);
        if(id==99){++original_lookups;return original_present?&original_object:nullptr;}
        if(id==100){++replacement_lookups;return replacement_present?&replacement_object:nullptr;}
    }
    if(method==&candy_method){
        assert(object==&player_object&&*static_cast<int*>(args[0])==1);
        return &boxed_candy;
    }
    if(method==&ctor_method){
        assert(object==&change_object||object==&change_list_object||object==&add_list_object||object==&remove_list_object);
        return nullptr;
    }
    if(method==&count_method){
        assert(object==&inventory_tags_object);return &boxed_count;
    }
    if(method==&item_method){
        assert(object==&inventory_tags_object&&args);
        int index=*static_cast<int*>(args[0]);
        if(selected_tag_present){if(index==0)return &boxed_selected_tag;--index;}
        assert(other_tag_present&&index==0);return &boxed_other_tag;
    }
    if(method==&add_method){
        assert(args);
        if(object==&remove_list_object){assert(args[0]==&tag_proto_object);++remove_adds;}
        else if(object==&add_list_object){++add_adds;}
        else if(object==&change_list_object){assert(args[0]==&change_object);++change_adds;}
        else assert(false&&"unexpected tag list");
        return nullptr;
    }
    if(method==&tag_set_method){
        assert(object==&tag_service_object&&args&&args[0]==&change_list_object);
        assert(change_adds==1&&remove_adds==1&&add_adds==0);
        assert(changed_pokemon==&original_object||changed_pokemon==&replacement_object);
        ++tag_requests;return &tag_promise_object;
    }
    assert(false&&"unexpected managed call");return nullptr;
}
void* class_type(void* klass){
    if(klass==&boxed_class)return &int_type;
    if(klass==&boxed_tag_class)return &uint64_type;
    assert(false&&"unexpected boxed type");return nullptr;
}
void* unbox(void* object){
    if(object==&boxed_candy)return &candy_balance;
    if(object==&boxed_count){static int count=0;count=selected_tag_present+other_tag_present;return &count;}
    if(object==&boxed_selected_tag)return &selected_tag_id;
    if(object==&boxed_other_tag)return &other_tag_id;
    assert(false&&"unexpected unbox");return nullptr;
}
void* object_new(void* klass){
    if(klass==&change_class)return &change_object;
    if(klass==&change_list_class)return &change_list_object;
    if(klass==&tag_list_class){++tag_list_allocations;return tag_list_allocations==1?&add_list_object:&remove_list_object;}
    assert(false&&"unexpected allocation");return nullptr;
}
char* type_name(void* type){
    const char* name=type==&enum_type?"EvolutionResult":static_cast<Field*>(type)->type;
    size_t n=strlen(name)+1;char* copy=static_cast<char*>(malloc(n));assert(copy);memcpy(copy,name,n);return copy;
}
void type_free(void* p){free(p);}
int resolve_class(const PgrMetaClassQuery* q,PgrMetaClassResult* out){
    assert(q&&out);
    if(!strcmp(q->name_space,"Holoholo.Rpc")&&!strcmp(q->name,"EvolvePokemonOutProto"))out->klass=&response_class;
    else if(!strcmp(q->name_space,"Holoholo.Rpc")&&!strcmp(q->name,"PokemonProto"))out->klass=&evolved_class;
    else if(!strcmp(q->name_space,"Niantic.Holoholo.Tagging")&&!strcmp(q->name,"PokemonTagChanges"))out->klass=&change_class;
    else return -1;
    out->version=1;out->size=sizeof(*out);return 0;
}
int sample(PgrRuntimeTransportSample* out){
    *out={};out->snapshot_fresh=fresh;out->fault=fault;
    out->snapshot_seq=100+(sequence_change?sample_count:0);++sample_count;return 0;
}
int get_bool(uint32_t slot,int* out){
    if(slot==9){++slot9_reads;*out=live;return 0;}
    if(slot==16){++slot16_reads;*out=ready;return 0;}
    assert(false&&"unexpected bool slot");return -1;
}
int get_string(uint32_t slot,char* out,uint32_t capacity,uint32_t* len){
    assert(slot==11);++slot11_reads;
    if(config.size()>capacity)return -1;
    *len=static_cast<uint32_t>(config.size());memcpy(out,config.data(),*len);return 0;
}
int emit(uint32_t opcode,const int64_t* scalars,uint32_t scalar_count,
         const PgrRuntimeTransportString* strings,uint32_t count){
    assert(opcode==19&&!scalars&&!scalar_count&&count==2);
    assert(std::string(strings[0].ptr,strings[0].len)=="batch-evolution");
    events.emplace_back(strings[1].ptr,strings[1].len);return 0;
}
int poll(void* promise,uint32_t phase,uint64_t tid,PgrVgpResultsResult* out){
    assert(promise==&promise_object&&phase==1&&tid==owner);++polls;*out={};
    if(poll_code==PGR_VR_DONE){out->version=1;out->size=sizeof(*out);out->value_handle=root_new(&response_object,0);}
    return poll_code;
}
PgrRuntimeTransportApi transport{};
PgrVgpResultsApi results{1,sizeof(PgrVgpResultsApi),nullptr,poll,nullptr};
PgrMetaApi metadata{};

void fixture(){
    assert(roots.empty());next_handle=1;allocations=frees=polls=0;events.clear();
    returned_id=99;returned_species=2;has_evolved=true;
    selected_tag_present=other_tag_present=true;
    tag_complete=tag_error=tag_accepted=false;
    tag_requests=change_adds=remove_adds=add_adds=tag_list_allocations=0;changed_pokemon=nullptr;
    original_present=true;replacement_present=false;original_species=replacement_species=2;
    candy_balance=9;original_lookups=replacement_lookups=0;uncertain.clear();
    config="1|42|2|Evolve";live=ready=fresh=true;fault=sequence_change=false;
    sample_count=slot9_reads=slot16_reads=slot11_reads=0;result_code=1;poll_code=PGR_VR_DONE;
    transport={};transport.sample=sample;transport.get_bool=get_bool;
    transport.get_string=get_string;transport.emit=emit;
    metadata={};metadata.resolve_class=resolve_class;
    boot={};boot.runtime.gchandle_new=reinterpret_cast<void*>(root_new);
    boot.runtime.gchandle_get_target=reinterpret_cast<void*>(root_get);
    boot.runtime.gchandle_free=reinterpret_cast<void*>(root_free);
    boot.runtime.object_get_class=reinterpret_cast<void*>(object_class);
    boot.runtime.object_new=reinterpret_cast<void*>(object_new);
    boot.runtime.field_get_value=reinterpret_cast<void*>(read_field);
    boot.runtime.field_set_value=reinterpret_cast<void*>(reject_scalar_reference);
    reference_setter=reinterpret_cast<void*>(write_field);
    boot.runtime.runtime_invoke=reinterpret_cast<void*>(invoke_method);
    boot.runtime.object_unbox=reinterpret_cast<void*>(unbox);
    boot.meta.class_get_field_from_name=reinterpret_cast<void*>(lookup_field);
    boot.meta.class_get_method_from_name=reinterpret_cast<void*>(lookup_method);
    boot.meta.method_get_param_count=reinterpret_cast<void*>(method_count);
    boot.meta.method_get_return_type=reinterpret_cast<void*>(method_return_type);
    boot.meta.method_get_param=reinterpret_cast<void*>(method_param);
    boot.meta.class_get_type=reinterpret_cast<void*>(class_type);
    boot.meta.class_is_valuetype=reinterpret_cast<void*>(class_is_value);
    boot.meta.field_get_type=reinterpret_cast<void*>(field_type);
    boot.meta.class_from_type=reinterpret_cast<void*>(class_from_type);
    boot.meta.field_get_flags=reinterpret_cast<void*>(field_flags);
    boot.runtime.field_static_get_value=reinterpret_cast<void*>(static_field);
    boot.meta.type_get_name=reinterpret_cast<void*>(type_name);
    boot.meta.mem_free=reinterpret_cast<void*>(type_free);
    batch=Batch{};batch.token=42;batch.tag="Evolve";batch.phase="running";
    batch.sending.id=99;batch.sending.target=2;batch.sending.family=1;
    uncertain.insert(99); // start_request marks the original before sending.
    batch.sending.candy=12;batch.sending.item=0;batch.sending.item_count=0;
    batch.since=clock_ms();batch.promise=Root(&promise_object);
    owner=static_cast<uint64_t>(gettid());
}
void inventory_services(){
    batch.bag=Root(&bag_object);batch.player=Root(&player_object);
    batch.candy_before=20;batch.item_before=0;
}
void tag_fixture(bool remove_enabled){
    batch.promise.reset();inventory_services();batch.result_id=99;batch.completed=1;
    batch.remove_tag=remove_enabled;batch.tag_id=selected_tag_id;
    batch.tag_service=Root(&tag_service_object);batch.tag_proto=Root(&tag_proto_object);
    config=remove_enabled?"2|42|2|1|Evolve":"2|42|2|0|Evolve";
}
void finish_inventory_fixture(){
    batch=Batch{};
    assert(roots.empty()&&allocations==frees);
}
}

extern "C" const void* pgrazor_runtime_bootstrap_api(uint32_t){assert(false&&"no bootstrap in fixture");return nullptr;}
extern "C" const void* pgrazor_runtime_metadata_api(uint32_t v){assert(v==1);return &metadata;}
extern "C" const void* pgrazor_runtime_transport_api(uint32_t v){assert(v==1);return &transport;}
extern "C" const void* pgrazor_vgp_results_api(uint32_t v){assert(v==1);return &results;}

int main(){
    fixture();
    Permission p=permission();assert(p.valid&&p.allowed&&p.cmd.token==42&&p.cmd.mode==2&&p.cmd.tag=="Evolve");
    assert(slot11_reads==1&&slot9_reads==1&&slot16_reads==1);
    assert(authorized(p));p.cmd.token=43;assert(!authorized(p));p.cmd.token=42;
    p.cmd.tag="Other";assert(!authorized(p));p.cmd.tag="Evolve";
    p.cmd.mode=1;assert(!authorized(p));p.cmd.mode=2;
    batch.canceled=true;assert(!authorized(p));batch.canceled=false;
    config="1|42|0|Evolve";assert(permission().valid&&!permission().allowed);
    config="1|43|2|Evolve";assert(permission().allowed&&!authorized(permission()));
    config="bad";assert(!permission().valid);config="1|42|2|Evolve";
    fresh=false;assert(!permission().valid);fresh=true;
    fault=true;assert(!permission().valid);fault=false;
    sequence_change=true;assert(!permission().valid);sequence_change=false;
    live=false;assert(!permission().allowed);live=true;
    ready=false;assert(!permission().allowed);ready=true;
    batch.promise.reset();assert(roots.empty()&&allocations==frees);

    fixture();batch.candidates.resize(3);batch.completed=1;batch.skipped=2;
    batch.xp=500;batch.candy_cost=36;batch.item_count=1;
    publish("running","Two skipped");assert(events.size()==1);
    assert(events[0].find("\"token\":42")!=std::string::npos);
    assert(events[0].find("\"total\":3,\"completed\":1,\"skipped\":2,\"xp\":500")!=std::string::npos);
    assert(events[0].find("\"candyCost\":36,\"itemCount\":1")!=std::string::npos);
    batch.promise.reset();assert(roots.empty()&&allocations==frees);

    fixture();poll_code=PGR_VR_PENDING;
    poll_request();poll_request();assert(polls==2&&batch.promise&&!batch.await_inventory&&events.empty());
    batch.since=clock_ms()-15001;
    bool timed_out=false;try{poll_request();}catch(const Failure& e){timed_out=!strcmp(e.where,"evolution response timed out; inspect before retrying");}
    assert(timed_out&&polls==3);batch.promise.reset();assert(roots.empty()&&allocations==frees);

    fixture();poll_code=PGR_VR_REJECTED;
    bool rejected=false;try{poll_request();}catch(const Failure& e){rejected=!strcmp(e.where,"evolution request failed; inspect before retrying");}
    assert(rejected&&polls==1&&!batch.promise&&roots.empty()&&allocations==frees);

    fixture();result_code=2;poll_request();
    assert(batch.phase=="error"&&batch.skipped==1&&!batch.promise&&!batch.await_inventory);
    assert(events.size()==1&&roots.empty()&&allocations==frees);

    fixture();poll_request();
    assert(batch.completed==0&&batch.xp==0&&batch.pending_xp==500&&batch.candy_awarded==1&&batch.await_inventory&&batch.result_id==99);
    assert(!batch.promise&&polls==1&&events.size()==1&&roots.empty()&&allocations==frees);
    batch.await_inventory=false;
    fixture();returned_id=100;poll_request();
    assert(batch.await_inventory&&batch.result_id==100&&batch.completed==0&&batch.pending_xp==500);
    assert(uncertain.count(100)&&roots.empty());batch.await_inventory=false;
    fixture();returned_id=0;returned_species=0;poll_request();
    assert(batch.await_inventory&&batch.result_id==99&&batch.completed==0);batch.await_inventory=false;
    fixture();has_evolved=false;poll_request();
    assert(batch.await_inventory&&batch.result_id==99&&batch.completed==0);batch.await_inventory=false;
    fixture();returned_id=100;returned_species=3;poll_request();
    assert(batch.await_inventory&&batch.result_id==99&&batch.completed==0);batch.await_inventory=false;

    fixture();poll_request();inventory_services();
    poll_request();
    assert(!batch.await_inventory&&batch.completed==1&&batch.xp==500&&batch.phase=="running");
    assert(original_lookups==1&&replacement_lookups==0&&uncertain.empty()&&polls==1);
    finish_inventory_fixture();

    fixture();returned_id=100;poll_request();inventory_services();
    original_present=false;replacement_present=true;
    poll_request();
    assert(!batch.await_inventory&&batch.completed==1&&batch.xp==500);
    assert(original_lookups==1&&replacement_lookups==1&&uncertain.empty()&&polls==1);
    finish_inventory_fixture();

    fixture();returned_id=100;poll_request();inventory_services();
    replacement_present=true; // The old ID must disappear before replacement confirmation.
    poll_request();
    assert(batch.await_inventory&&batch.completed==0&&batch.xp==0&&uncertain.count(99)&&uncertain.count(100));
    original_present=false;poll_request();
    assert(!batch.await_inventory&&batch.completed==1&&uncertain.empty()&&polls==1);
    finish_inventory_fixture();

    fixture();poll_request();inventory_services();candy_balance=10;
    poll_request();
    assert(batch.await_inventory&&batch.completed==0&&batch.xp==0&&uncertain.count(99));
    candy_balance=9;poll_request();
    assert(!batch.await_inventory&&batch.completed==1&&batch.xp==500&&polls==1);
    finish_inventory_fixture();

    fixture();poll_request();inventory_services();batch.canceled=true;
    poll_request();
    assert(!batch.await_inventory&&batch.phase=="stopped"&&batch.completed==1&&batch.xp==500);
    assert(events.back().find("\"phase\":\"stopped\"")!=std::string::npos&&polls==1);
    finish_inventory_fixture();

    fixture();tag_fixture(false);remove_confirmed_tag();
    assert(tag_requests==0&&batch.phase=="running"&&selected_tag_present&&other_tag_present);
    finish_inventory_fixture();

    fixture();tag_fixture(true);batch.canceled=true;remove_confirmed_tag();
    assert(tag_requests==0&&batch.phase=="stopped"&&selected_tag_present&&other_tag_present);
    finish_inventory_fixture();

    fixture();tag_fixture(true);live=false;remove_confirmed_tag();
    assert(tag_requests==0&&batch.phase=="running"&&selected_tag_present&&other_tag_present);
    finish_inventory_fixture();

    fixture();tag_fixture(true);selected_tag_present=false;remove_confirmed_tag();
    assert(tag_requests==0&&batch.phase=="running"&&other_tag_present);
    finish_inventory_fixture();

    fixture();tag_fixture(true);
    {
        Root current(&original_object);
        void* set=method(klass(batch.tag_service.get()),"SetPokemonTags",1);
        Root changes=build_tag_changes(set,current);
        assert(changes&&changes.get()==&change_list_object);
        assert(change_adds==1&&remove_adds==1&&add_adds==0);
        assert(changed_pokemon==&original_object&&tag_requests==0);
    }
    finish_inventory_fixture();

    fixture();tag_fixture(true);remove_confirmed_tag();
    assert(tag_requests==1&&change_adds==1&&remove_adds==1&&add_adds==0);
    assert(changed_pokemon==&original_object&&batch.tag_pending&&batch.promise);
    poll_tag_removal();assert(batch.tag_pending&&!batch.await_tag&&tag_requests==1);
    tag_complete=true;tag_accepted=true;poll_tag_removal();
    assert(!batch.tag_pending&&batch.await_tag&&tag_requests==1&&selected_tag_present);
    selected_tag_present=false;poll_tag_removal();
    assert(!batch.await_tag&&batch.phase=="running"&&other_tag_present);
    assert(events.back().find("selected tag removed")!=std::string::npos);
    finish_inventory_fixture();

    fixture();tag_fixture(true);batch.result_id=100;
    original_present=false;replacement_present=true;
    remove_confirmed_tag();
    assert(tag_requests==1&&changed_pokemon==&replacement_object&&remove_adds==1&&add_adds==0);
    finish_inventory_fixture();

    fixture();tag_fixture(true);remove_confirmed_tag();tag_complete=true;tag_accepted=false;
    bool tag_declined=false;try{poll_tag_removal();}catch(const Failure& e){tag_declined=!strcmp(e.where,"Evolved; tag removal declined. Batch stopped.");}
    assert(tag_declined&&!batch.tag_pending&&!batch.await_tag&&selected_tag_present&&other_tag_present);
    finish_inventory_fixture();

    fixture();tag_fixture(true);remove_confirmed_tag();tag_error=true;
    bool tag_failed=false;try{poll_tag_removal();}catch(const Failure& e){tag_failed=!strcmp(e.where,"Evolved; tag removal failed. Batch stopped.");}
    assert(tag_failed&&batch.tag_pending&&selected_tag_present&&other_tag_present);
    finish_inventory_fixture();

    fixture();tag_fixture(true);remove_confirmed_tag();
    batch.since=clock_ms()-15001;
    bool tag_pending_timeout=false;
    try{poll_tag_removal();}catch(const Failure& e){
        tag_pending_timeout=!strcmp(e.where,"Evolved; tag removal timed out. Inspect tags before retrying.");
    }
    assert(tag_pending_timeout&&batch.tag_pending&&tag_requests==1&&selected_tag_present);
    finish_inventory_fixture();

    fixture();tag_fixture(true);remove_confirmed_tag();tag_complete=true;tag_accepted=true;
    poll_tag_removal();assert(batch.await_tag);
    poll_tag_removal();assert(batch.await_tag&&tag_requests==1&&other_tag_present);
    batch.since=clock_ms()-15001;
    bool tag_cache_timeout=false;
    try{poll_tag_removal();}catch(const Failure& e){
        tag_cache_timeout=!strcmp(e.where,"Evolved; tag update not confirmed. Inspect tags before retrying.");
    }
    assert(tag_cache_timeout&&batch.await_tag&&tag_requests==1&&other_tag_present);
    finish_inventory_fixture();

    fixture();tag_fixture(true);remove_confirmed_tag();
    assert(tag_requests==1&&batch.tag_pending);
    batch.canceled=true;tag_complete=true;tag_accepted=true;
    poll_tag_removal();assert(batch.await_tag&&tag_requests==1);
    selected_tag_present=false;poll_tag_removal();
    assert(!batch.await_tag&&batch.phase=="stopped"&&tag_requests==1&&other_tag_present);
    assert(events.back().find("\"phase\":\"stopped\"")!=std::string::npos);
    finish_inventory_fixture();

    fixture();batch.promise.reset();initialized=false;config="1|43|0|Evolve";
    pgr_evolution_tick(owner,nullptr);
    assert(!initialized&&batch.token==43&&batch.phase=="stopped"&&events.size()==1);
    pgr_evolution_tick(owner,nullptr);assert(events.size()==1&&polls==0);
    assert(roots.empty()&&allocations==frees);
    std::puts("PASS: batch evolution permissions, inventory confirmation, tag removal and stop drain");
}
