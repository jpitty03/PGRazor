#include "deep_radar_metadata.h"
#include "game_bindings.h"
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <string>

namespace {
using namespace pgr::radar;
const char *names[ClassCount]={"GetMapObjectsOutProto","ClientMapCellProto","NearbyPokemonProto","WildPokemonProto","MapPokemonProto","PokemonFortProto","PokemonProto","PokemonDisplayProto","NearbyPokemonSnapshot","PinnedNearbyPokemonSnapshot","String"};
struct Field {FieldSlot slot;ClassSlot owner;const char *name;uint64_t offset;uint32_t kind,size;const char *type;};
const Field fields[]={
 {DisplayShiny,DisplayClass,PGR_GAME_DEEP_RADAR_DISPLAYSHINY_NAME,PGR_GAME_DEEP_RADAR_DISPLAYSHINY_OFFSET,PGR_MD_FIELD_VALUE,1,"System.Boolean"},
 {ResponseStatus,ResponseClass,PGR_GAME_DEEP_RADAR_RESPONSESTATUS_NAME,PGR_GAME_DEEP_RADAR_RESPONSESTATUS_OFFSET,PGR_MD_FIELD_ENUM_I32,4,nullptr},
 {ResponseSnapshot,ResponseClass,PGR_GAME_DEEP_RADAR_RESPONSESNAPSHOT_NAME,PGR_GAME_DEEP_RADAR_RESPONSESNAPSHOT_OFFSET,PGR_MD_FIELD_REF,0,"Holoholo.Rpc.NearbyPokemonSnapshot"},
 {ResponsePinned,ResponseClass,PGR_GAME_DEEP_RADAR_RESPONSEPINNED_NAME,PGR_GAME_DEEP_RADAR_RESPONSEPINNED_OFFSET,PGR_MD_FIELD_REF,0,"Holoholo.Rpc.PinnedNearbyPokemonSnapshot"},
 {CellId,CellClass,PGR_GAME_DEEP_RADAR_CELLID_NAME,PGR_GAME_DEEP_RADAR_CELLID_OFFSET,PGR_MD_FIELD_VALUE,8,"System.UInt64"},{CellTime,CellClass,PGR_GAME_DEEP_RADAR_CELLTIME_NAME,PGR_GAME_DEEP_RADAR_CELLTIME_OFFSET,PGR_MD_FIELD_VALUE,8,"System.Int64"},{CellTruncated,CellClass,PGR_GAME_DEEP_RADAR_CELLTRUNCATED_NAME,PGR_GAME_DEEP_RADAR_CELLTRUNCATED_OFFSET,PGR_MD_FIELD_VALUE,1,"System.Boolean"},
 {NearbySpecies,NearbyClass,PGR_GAME_DEEP_RADAR_NEARBYSPECIES_NAME,PGR_GAME_DEEP_RADAR_NEARBYSPECIES_OFFSET,PGR_MD_FIELD_VALUE,4,"System.Int32"},{NearbyDistance,NearbyClass,PGR_GAME_DEEP_RADAR_NEARBYDISTANCE_NAME,PGR_GAME_DEEP_RADAR_NEARBYDISTANCE_OFFSET,PGR_MD_FIELD_VALUE,4,"System.Single"},{NearbyId,NearbyClass,PGR_GAME_DEEP_RADAR_NEARBYID_NAME,PGR_GAME_DEEP_RADAR_NEARBYID_OFFSET,PGR_MD_FIELD_VALUE,8,"System.UInt64"},{NearbyFort,NearbyClass,PGR_GAME_DEEP_RADAR_NEARBYFORT_NAME,PGR_GAME_DEEP_RADAR_NEARBYFORT_OFFSET,PGR_MD_FIELD_REF,0,"System.String"},{NearbyDisplay,NearbyClass,PGR_GAME_DEEP_RADAR_NEARBYDISPLAY_NAME,PGR_GAME_DEEP_RADAR_NEARBYDISPLAY_OFFSET,PGR_MD_FIELD_REF,0,"Holoholo.Rpc.PokemonDisplayProto"},
 {WildId,WildClass,PGR_GAME_DEEP_RADAR_WILDID_NAME,PGR_GAME_DEEP_RADAR_WILDID_OFFSET,PGR_MD_FIELD_VALUE,8,"System.UInt64"},{WildLat,WildClass,PGR_GAME_DEEP_RADAR_WILDLAT_NAME,PGR_GAME_DEEP_RADAR_WILDLAT_OFFSET,PGR_MD_FIELD_VALUE,8,"System.Double"},{WildLon,WildClass,PGR_GAME_DEEP_RADAR_WILDLON_NAME,PGR_GAME_DEEP_RADAR_WILDLON_OFFSET,PGR_MD_FIELD_VALUE,8,"System.Double"},{WildSpawn,WildClass,PGR_GAME_DEEP_RADAR_WILDSPAWN_NAME,PGR_GAME_DEEP_RADAR_WILDSPAWN_OFFSET,PGR_MD_FIELD_REF,0,"System.String"},{WildPokemon,WildClass,PGR_GAME_DEEP_RADAR_WILDPOKEMON_NAME,PGR_GAME_DEEP_RADAR_WILDPOKEMON_OFFSET,PGR_MD_FIELD_REF,0,"Holoholo.Rpc.PokemonProto"},{WildTth,WildClass,PGR_GAME_DEEP_RADAR_WILDTTH_NAME,PGR_GAME_DEEP_RADAR_WILDTTH_OFFSET,PGR_MD_FIELD_VALUE,4,"System.Int32"},
 {MapId,MapClass,PGR_GAME_DEEP_RADAR_MAPID_NAME,PGR_GAME_DEEP_RADAR_MAPID_OFFSET,PGR_MD_FIELD_VALUE,8,"System.UInt64"},{MapLat,MapClass,PGR_GAME_DEEP_RADAR_MAPLAT_NAME,PGR_GAME_DEEP_RADAR_MAPLAT_OFFSET,PGR_MD_FIELD_VALUE,8,"System.Double"},{MapLon,MapClass,PGR_GAME_DEEP_RADAR_MAPLON_NAME,PGR_GAME_DEEP_RADAR_MAPLON_OFFSET,PGR_MD_FIELD_VALUE,8,"System.Double"},{MapSpawn,MapClass,PGR_GAME_DEEP_RADAR_MAPSPAWN_NAME,PGR_GAME_DEEP_RADAR_MAPSPAWN_OFFSET,PGR_MD_FIELD_REF,0,"System.String"},{MapSpecies,MapClass,PGR_GAME_DEEP_RADAR_MAPSPECIES_NAME,PGR_GAME_DEEP_RADAR_MAPSPECIES_OFFSET,PGR_MD_FIELD_VALUE,4,"System.Int32"},{MapExpiry,MapClass,PGR_GAME_DEEP_RADAR_MAPEXPIRY_NAME,PGR_GAME_DEEP_RADAR_MAPEXPIRY_OFFSET,PGR_MD_FIELD_VALUE,8,"System.Int64"},{MapDisplay,MapClass,PGR_GAME_DEEP_RADAR_MAPDISPLAY_NAME,PGR_GAME_DEEP_RADAR_MAPDISPLAY_OFFSET,PGR_MD_FIELD_REF,0,"Holoholo.Rpc.PokemonDisplayProto"},
 {FortId,FortClass,PGR_GAME_DEEP_RADAR_FORTID_NAME,PGR_GAME_DEEP_RADAR_FORTID_OFFSET,PGR_MD_FIELD_REF,0,"System.String"},{FortLat,FortClass,PGR_GAME_DEEP_RADAR_FORTLAT_NAME,PGR_GAME_DEEP_RADAR_FORTLAT_OFFSET,PGR_MD_FIELD_VALUE,8,"System.Double"},{FortLon,FortClass,PGR_GAME_DEEP_RADAR_FORTLON_NAME,PGR_GAME_DEEP_RADAR_FORTLON_OFFSET,PGR_MD_FIELD_VALUE,8,"System.Double"},
 {PokemonSpecies,PokemonClass,PGR_GAME_DEEP_RADAR_POKEMONSPECIES_NAME,PGR_GAME_DEEP_RADAR_POKEMONSPECIES_OFFSET,PGR_MD_FIELD_VALUE,4,"System.Int32"},{PokemonDisplay,PokemonClass,PGR_GAME_DEEP_RADAR_POKEMONDISPLAY_NAME,PGR_GAME_DEEP_RADAR_POKEMONDISPLAY_OFFSET,PGR_MD_FIELD_REF,0,"Holoholo.Rpc.PokemonDisplayProto"},
 {DisplayForm,DisplayClass,PGR_GAME_DEEP_RADAR_DISPLAYFORM_NAME,PGR_GAME_DEEP_RADAR_DISPLAYFORM_OFFSET,PGR_MD_FIELD_ENUM_I32,4,nullptr},{DisplayCostume,DisplayClass,PGR_GAME_DEEP_RADAR_DISPLAYCOSTUME_NAME,PGR_GAME_DEEP_RADAR_DISPLAYCOSTUME_OFFSET,PGR_MD_FIELD_ENUM_I32,4,nullptr},{DisplayGender,DisplayClass,PGR_GAME_DEEP_RADAR_DISPLAYGENDER_NAME,PGR_GAME_DEEP_RADAR_DISPLAYGENDER_OFFSET,PGR_MD_FIELD_ENUM_I32,4,nullptr},
 {SnapshotStatus,SnapshotClass,PGR_GAME_DEEP_RADAR_SNAPSHOTSTATUS_NAME,PGR_GAME_DEEP_RADAR_SNAPSHOTSTATUS_OFFSET,PGR_MD_FIELD_ENUM_I32,4,nullptr},{PinnedStatus,PinnedClass,PGR_GAME_DEEP_RADAR_PINNEDSTATUS_NAME,PGR_GAME_DEEP_RADAR_PINNEDSTATUS_OFFSET,PGR_MD_FIELD_ENUM_I32,4,nullptr},{PinnedPokemon,PinnedClass,PGR_GAME_DEEP_RADAR_PINNEDPOKEMON_NAME,PGR_GAME_DEEP_RADAR_PINNEDPOKEMON_OFFSET,PGR_MD_FIELD_REF,0,"Holoholo.Rpc.NearbyPokemonProto"}
};
struct List {ListSlot slot;ClassSlot owner,element;const char *name;uint64_t offset;};
const List lists[]={{CellsList,ResponseClass,CellClass,PGR_GAME_DEEP_RADAR_CELLSLIST_NAME,PGR_GAME_DEEP_RADAR_CELLSLIST_OFFSET},{FortsList,CellClass,FortClass,PGR_GAME_DEEP_RADAR_FORTSLIST_NAME,PGR_GAME_DEEP_RADAR_FORTSLIST_OFFSET},{NearbyList,CellClass,NearbyClass,PGR_GAME_DEEP_RADAR_NEARBYLIST_NAME,PGR_GAME_DEEP_RADAR_NEARBYLIST_OFFSET},{WildList,CellClass,WildClass,PGR_GAME_DEEP_RADAR_WILDLIST_NAME,PGR_GAME_DEEP_RADAR_WILDLIST_OFFSET},{MapList,CellClass,MapClass,PGR_GAME_DEEP_RADAR_MAPLIST_NAME,PGR_GAME_DEEP_RADAR_MAPLIST_OFFSET},{SnapshotList,SnapshotClass,NearbyClass,PGR_GAME_DEEP_RADAR_SNAPSHOTLIST_NAME,PGR_GAME_DEEP_RADAR_SNAPSHOTLIST_OFFSET}};
bool field(const PgrMetaApi *api,const void *owner,const char *name,uint64_t off,uint32_t kind,uint32_t size,const char *type,PgrMetaFieldResult &out){
 PgrMetaFieldQuery q{1,sizeof(q),owner,name,off,0x50,0,kind,size,type,PGR_MD_MATCH_EXACT,0};return api->validate_field(&q,&out)==0;
}
const void *klass(const PgrMetaApi *api,const char *ns,const char *name){PgrMetaClassQuery q{1,sizeof(q),ns,name,nullptr,PGR_MD_MATCH_EXACT,0};PgrMetaClassResult r{};return api->resolve_class(&q,&r)==0?r.klass:nullptr;}
bool literal(const PgrBootBinding &b,const void *enumClass,const char *name,int32_t &value){
 auto lookup=reinterpret_cast<const void *(*)(const void*,const char*)>(b.meta.class_get_field_from_name);
 auto flags=reinterpret_cast<uint32_t(*)(const void*)>(b.meta.field_get_flags);
 const void *f=lookup(enumClass,name);if(!f||(flags(f)&0x8050)!=0x8050)return false;
 reinterpret_cast<void(*)(const void*,void*)>(b.runtime.field_static_get_value)(f,&value);return value>=0;
}
}
bool pgr_deep_radar_bind(const PgrBootBinding &b,pgr::radar::ReaderConfig &out,const void **code,char *reason,size_t cap){
 using namespace pgr::radar;ReaderConfig c{};*code=nullptr;
 auto fail=[&](const char *name){std::snprintf(reason,cap,"Radar metadata unavailable: %s",name);return false;};
 const auto *api=static_cast<const PgrMetaApi*>(pgrazor_runtime_metadata_api(1));if(!api||!b.handle)return fail("bootstrap");
 for(unsigned i=0;i<ClassCount;++i){c.classes[i]=klass(api,i==StringClass?"System":"Holoholo.Rpc",names[i]);if(!c.classes[i])return fail(names[i]);}
 for(const auto &f:fields){PgrMetaFieldResult r{};if(!field(api,c.classes[f.owner],f.name,f.offset,f.kind,f.size,f.type,r))return fail(f.name);c.fields[f.slot]=r.field;
  if(f.slot==ResponseStatus&&!literal(b,r.type_class,"Success",c.success))return fail("response Success");
  if(f.slot==SnapshotStatus&&!literal(b,r.type_class,"Complete",c.snapshot_complete))return fail("snapshot Complete");
  if(f.slot==PinnedStatus&&!literal(b,r.type_class,"Included",c.pinned_included))return fail("pinned Included");
 }
 for(const auto &l:lists){auto &v=c.lists[l.slot];const std::string inner=std::string("Holoholo.Rpc.")+names[l.element];PgrMetaFieldResult f{};
  const std::string a="Niantic.Protobuf.Collections.RepeatedField<"+inner+">",z="Niantic.Protobuf.Collections.RepeatedField`1<"+inner+">";
  if(!field(api,c.classes[l.owner],l.name,l.offset,PGR_MD_FIELD_REF,0,a.c_str(),f)&&!field(api,c.classes[l.owner],l.name,l.offset,PGR_MD_FIELD_REF,0,z.c_str(),f))return fail(l.name);
  v.field=f.field;v.klass=f.type_class;v.element_class=c.classes[l.element];if(!v.klass)return fail("repeated class");
  if(!field(api,v.klass,"array",PGR_GAME_DEEP_RADAR_ARRAY_OFFSET,PGR_MD_FIELD_REF,0,(inner+"[]").c_str(),f))return fail("repeated array");v.array=f.field;v.array_class=f.type_class;
  if(!field(api,v.klass,"count",PGR_GAME_DEEP_RADAR_COUNT_OFFSET,PGR_MD_FIELD_VALUE,4,"System.Int32",f))return fail("repeated count");v.count=f.field;
 }
 auto extra=[&](const char *name){void *p=dlsym(b.handle,name);PgrMetaCodeQuery q{1,sizeof(q),p,4,nullptr,0,0};return p&&api->validate_code(&q)==0?p:nullptr;};
 c.object_class=reinterpret_cast<decltype(c.object_class)>(b.runtime.object_get_class);c.field_get=reinterpret_cast<decltype(c.field_get)>(b.runtime.field_get_value);
 c.array_length=reinterpret_cast<decltype(c.array_length)>(extra("il2cpp_array_length"));c.string_length=reinterpret_cast<decltype(c.string_length)>(extra("il2cpp_string_length"));c.string_chars=reinterpret_cast<decltype(c.string_chars)>(b.runtime.string_chars);
 if(!c.object_class||!c.field_get||!c.array_length||!c.string_length||!c.string_chars)return fail("read exports");
 const void *handler=klass(api,"Niantic.Holoholo.Map","MapContentHandler");if(!handler)return fail("MapContentHandler");
 constexpr unsigned char prologue[]=PGR_GAME_DEEP_RADAR_RESPONSE_FINGERPRINT;
 PgrMetaMethodQuery q{};q.version=1;q.size=sizeof(q);q.owner=handler;q.name=PGR_GAME_DEEP_RADAR_RESPONSE_NAME;q.return_type="System.Void";q.return_match=PGR_MD_MATCH_EXACT;
 q.param_count=2;q.param_types[0]="Holoholo.Rpc.GetMapObjectsOutProto";q.param_types[1]="System.Boolean";q.param_match[0]=q.param_match[1]=PGR_MD_MATCH_EXACT;
 q.expected_code=static_cast<const char*>(b.module_base)+PGR_GAME_DEEP_RADAR_RESPONSE_RVA;q.prologue=prologue;q.prologue_len=sizeof(prologue);
 PgrMetaMethodResult result{};if(api->validate_method(&q,&result))return fail("response handler signature/fingerprint");
 out=c;*code=result.code;reason[0]=0;return true;
}
