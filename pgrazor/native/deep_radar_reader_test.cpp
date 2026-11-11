#include "deep_radar_reader.h"
#include "s2_geometry.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
using namespace pgr::radar;
namespace {
struct Obj {void *klass=nullptr;uint64_t words[128]{};};
struct Array {void *klass=nullptr,*monitor=nullptr,*bounds=nullptr;uintptr_t length=0;void *data[8]{};};
struct String {void *klass;std::u16string data;};
int classes[ClassCount+ListCount*2]{};uint64_t clock_value=1000;bool permitted=true;int reads=0,revoke_after=0;
struct Field {unsigned slot,size;};Field f[FieldCount],listFields[ListCount],arrayField{120,8},countField{121,4};
void get(void*o,const void*field,void*out){++reads;if(revoke_after&&reads>=revoke_after)permitted=false;auto d=static_cast<const Field*>(field);std::memcpy(out,&static_cast<Obj*>(o)->words[d->slot],d->size);}
template<class T>void put(Obj&o,unsigned field,T v){std::memcpy(&o.words[field],&v,sizeof(v));}
}
int main(){ReaderConfig c{};for(unsigned i=0;i<ClassCount;++i)c.classes[i]=&classes[i];for(unsigned i=0;i<FieldCount;++i){unsigned n=8;switch(i){case DisplayShiny:case ResponseStatus:case CellTruncated:case NearbySpecies:case NearbyDistance:case WildTth:case MapSpecies:case PokemonSpecies:case DisplayForm:case DisplayCostume:case DisplayGender:case SnapshotStatus:case PinnedStatus:n=(i==CellTruncated||i==DisplayShiny)?1:4;break;default:break;}f[i]={i,n};c.fields[i]=&f[i];}
 c.object_class=[](void*o){return *static_cast<void**>(o);};c.field_get=get;c.array_length=[](void*o){return static_cast<Array*>(o)->length;};c.string_length=[](void*o){return int32_t(static_cast<String*>(o)->data.size());};c.string_chars=[](void*o){return reinterpret_cast<const uint16_t*>(static_cast<String*>(o)->data.data());};c.clock_ms=[](){return clock_value;};c.allowed=[](){return permitted;};
 Obj response{&classes[ResponseClass]},cell{&classes[CellClass]},nearby{&classes[NearbyClass]},fort{&classes[FortClass]};Obj lists[ListCount];Array arrays[ListCount];
 const unsigned elements[ListCount]={CellClass,FortClass,NearbyClass,WildClass,MapClass,NearbyClass};
 for(unsigned i=0;i<ListCount;++i){listFields[i]={80+i,8};lists[i].klass=&classes[ClassCount+i];arrays[i].klass=&classes[ClassCount+ListCount+i];put(lists[i],120,&arrays[i]);c.lists[i]={&listFields[i],lists[i].klass,&arrayField,&countField,arrays[i].klass,&classes[elements[i]]};}
 put(response,ResponseStatus,int32_t(1));put(response,80+CellsList,&lists[CellsList]);arrays[CellsList].length=1;arrays[CellsList].data[0]=&cell;put(lists[CellsList],121,int32_t(1));
 for(unsigned i=FortsList;i<=MapList;++i)put(cell,80+i,&lists[i]);put(cell,CellId,uint64_t(0x1000000040000000ULL));
 arrays[NearbyList].length=1;arrays[NearbyList].data[0]=&nearby;put(lists[NearbyList],121,int32_t(1));put(nearby,NearbyId,UINT64_MAX);put(nearby,NearbySpecies,int32_t(885));put(nearby,NearbyDistance,200.0f);
 auto p=std::make_unique<Packet>();p->sequence=1;p->observed_ms=1000;ReadStats stats;assert(read_response(c,&response,*p,stats,2000)==ReadOk);assert(p->count==1&&p->observations[0].kind==Cell&&stats.distance_200==1&&stats.offstop==1);
 permitted=false;int before=reads;assert(read_response(c,&response,*p,stats,2000)==ReadExpired&&reads==before&&p->count==0);permitted=true;
 clock_value=2000;assert(read_response(c,&response,*p,stats,2000)==ReadExpired);clock_value=1000;
 put(lists[NearbyList],121,int32_t(9));assert(read_response(c,&response,*p,stats,2000)<0&&p->count==0);put(lists[NearbyList],121,int32_t(1));nearby.klass=&classes[WildClass];assert(read_response(c,&response,*p,stats,2000)<0&&p->count==0);nearby.klass=&classes[NearbyClass];
 String id{&classes[StringClass],u"stop"};put(nearby,NearbyFort,&id);assert(read_response(c,&response,*p,stats,2000)==ReadOk&&p->observations[0].kind==Unlocated);id.data=std::u16string(257,u'x');assert(read_response(c,&response,*p,stats,2000)<0&&p->count==0);
 put(response,ResponseStatus,int32_t(2));assert(read_response(c,&response,*p,stats,2000)==ReadIgnored&&p->count==0);
 put(response,ResponseStatus,int32_t(1));id.data=u"stop";
 arrays[FortsList].length=1;arrays[FortsList].data[0]=&fort;put(lists[FortsList],121,int32_t(1));
 const auto center=cell_center(0x1000000040000000ULL);
 put(fort,FortId,&id);put(fort,FortLat,center.lat);put(fort,FortLon,center.lon);
 Obj wild{&classes[WildClass]},pokemon{&classes[PokemonClass]},display{&classes[DisplayClass]},map{&classes[MapClass]};
 put(display,DisplayShiny,uint8_t(1));put(display,DisplayForm,int32_t(7));put(display,DisplayCostume,int32_t(3));put(display,DisplayGender,int32_t(2));
 put(pokemon,PokemonSpecies,int32_t(25));put(pokemon,PokemonDisplay,&display);
 put(wild,WildId,uint64_t(22));put(wild,WildPokemon,&pokemon);put(wild,WildLat,center.lat);put(wild,WildLon,center.lon);put(wild,WildSpawn,&id);put(wild,WildTth,int32_t(60000));
 arrays[WildList].length=1;arrays[WildList].data[0]=&wild;put(lists[WildList],121,int32_t(1));
 put(map,MapId,uint64_t(23));put(map,MapSpecies,int32_t(201));put(map,MapLat,center.lat);put(map,MapLon,center.lon);put(map,MapSpawn,&id);put(map,MapDisplay,&display);put(map,MapExpiry,int64_t(999999999999));
 arrays[MapList].length=1;arrays[MapList].data[0]=&map;put(lists[MapList],121,int32_t(1));
 assert(read_response(c,&response,*p,stats,2000)==ReadOk&&p->count==3&&p->fort_count==1);
 assert(stats.wild==1&&stats.catchable==1&&stats.short_tth==1&&stats.shiny_true==2);
 assert(p->observations[1].encounter==22&&p->observations[1].species==25&&p->observations[1].expiry_ms==61000);
 assert(p->observations[1].form==7&&p->observations[1].costume==3&&p->observations[1].gender==2);
 assert(p->observations[2].species==201&&p->observations[2].lat==center.lat&&p->observations[2].expiry_ms==0);
 put(wild,WildLat,0.0);put(wild,WildLon,120.0);put(fort,FortLat,0.0);put(fort,FortLon,120.0);
 assert(read_response(c,&response,*p,stats,2000)==ReadOk&&p->fort_count==0&&stats.missing_positions==2&&p->observations[1].kind==Unlocated);
 put(wild,WildLat,center.lat);put(wild,WildLon,center.lon);put(fort,FortLat,center.lat);put(fort,FortLon,center.lon);
 // Snapshot and pinned statuses are distinct schema literals. These rows must
 // never acquire the parent cell from an unrelated per-cell observation.
 Obj snapshot{&classes[SnapshotClass]},pinned{&classes[PinnedClass]};
 put(response,ResponseSnapshot,&snapshot);put(response,ResponsePinned,&pinned);
 put(snapshot,SnapshotStatus,int32_t(c.snapshot_complete));put(snapshot,80+SnapshotList,&lists[SnapshotList]);
 arrays[SnapshotList].length=1;arrays[SnapshotList].data[0]=&nearby;put(lists[SnapshotList],121,int32_t(1));
 put(pinned,PinnedStatus,int32_t(c.pinned_included));put(pinned,PinnedPokemon,&nearby);
 put(nearby,NearbyFort,static_cast<void*>(nullptr));
 assert(read_response(c,&response,*p,stats,2000)==ReadOk&&p->count==5&&stats.top_level==2);
 assert(p->observations[3].cell==0&&p->observations[3].kind==Unlocated&&p->observations[4].cell==0);
 put(snapshot,SnapshotStatus,int32_t(-1));put(pinned,PinnedStatus,int32_t(c.snapshot_complete));
 assert(read_response(c,&response,*p,stats,2000)==ReadOk&&p->count==3&&stats.top_level==0);
 // Unknown level retains exact/fort data but cannot be treated as an L15 area.
 put(cell,CellId,uint64_t(0x1000000000000000ULL));
 assert(read_response(c,&response,*p,stats,2000)==ReadOk&&stats.non_l15==1&&p->observations[0].kind==Unlocated);
 put(nearby,NearbyFort,&id);id.data=std::u16string(1,char16_t(0xd800));
 assert(read_response(c,&response,*p,stats,2000)<0&&p->count==0&&p->fort_count==0);
 id.data=u"stop";revoke_after=reads+12;
 assert(read_response(c,&response,*p,stats,2000)==ReadExpired&&p->count==0&&p->fort_count==0);
 revoke_after=0;permitted=true;
 arrays[MapList].klass=&classes[WildClass];
 assert(read_response(c,&response,*p,stats,2000)<0&&p->count==0&&p->fort_count==0);
 std::puts("PASS radar reader: LP64 ABI, exact/catchable/fort/display fields, distinct snapshot enums, uint64 IDs, diagnostic-only distance, revocation, bounds/class/string rejection and atomic failure");
}
