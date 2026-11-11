#include "deep_radar_reader.h"
#include "s2_geometry.h"
#include <cmath>
#include <algorithm>
#include <cstring>
#include <limits>

namespace pgr::radar { namespace {
struct Reader {
 const ReaderConfig &c; Packet &p;ReadStats &stats; uint64_t deadline;
 int error=ReadMetadata;
 bool gate(){if(!c.allowed()||!c.clock_ms()||c.clock_ms()>=deadline){error=ReadExpired;return false;}return true;}
 bool exact(void *o,ClassSlot k){return o&&c.object_class(o)==c.classes[k];}
 template<class T>T get(void *o,FieldSlot f){T value{};c.field_get(o,c.fields[f],&value);return value;}
 void location(void *o,FieldSlot a,FieldSlot b,double &lat,double &lon){
  c.field_get(o,c.fields[a],&lat);
  c.field_get(o,c.fields[b],&lon);
 }
 bool text(void *s,char (&out)[257]) {
  out[0]=0;if(!s)return true;if(!exact(s,StringClass))return false;
  const int n=c.string_length(s);if(n<0||n>256)return false;
  const uint16_t *v=c.string_chars(s);if(n&&!v)return false;unsigned at=0;
  for(int i=0;i<n;++i){uint32_t cp=v[i];
   if(cp==0)return false;
   if(cp>=0xd800&&cp<=0xdbff){if(++i>=n||v[i]<0xdc00||v[i]>0xdfff)return false;cp=0x10000+((cp-0xd800)<<10)+(v[i]-0xdc00);}
   else if(cp>=0xdc00&&cp<=0xdfff)return false;
   const unsigned bytes=cp<0x80?1:cp<0x800?2:cp<0x10000?3:4;if(at+bytes>256)return false;
   if(bytes==1)out[at++]=char(cp);
   else {out[at++]=char((bytes==2?0xc0:bytes==3?0xe0:0xf0)|(cp>>(6*(bytes-1))));for(int j=int(bytes)-2;j>=0;--j)out[at++]=char(0x80|((cp>>(6*j))&63));}
  }out[at]=0;return true;
 }
 bool display(void *d,Observation &o){if(!d)return true;if(!exact(d,DisplayClass))return false;
  o.form=get<int32_t>(d,DisplayForm);o.costume=get<int32_t>(d,DisplayCostume);o.gender=get<int32_t>(d,DisplayGender);
  const uint8_t shiny=get<uint8_t>(d,DisplayShiny);if(shiny>1)return false;++stats.display_count;if(shiny)++stats.shiny_true;
  return o.form>=0&&o.costume>=0&&o.gender>=0;
 }
 bool coords(double lat,double lon){return std::isfinite(lat)&&std::isfinite(lon)&&std::fabs(lat)<=90&&std::fabs(lon)<=180;}
 template<class F>bool each(void *owner,ListSlot slot,uint32_t cap,F body) {
  if(!gate())return false;const auto &b=c.lists[slot];void *list=nullptr;c.field_get(owner,b.field,&list);
  if(!list||c.object_class(list)!=b.klass)return false;
  int32_t count=0;void *array=nullptr;c.field_get(list,b.count,&count);c.field_get(list,b.array,&array);
  if(count<0||uint32_t(count)>cap){error=ReadBounds;return false;}
  if(!array||c.object_class(array)!=b.array_class||c.array_length(array)<uintptr_t(count))return false;
  for(int32_t i=0;i<count;++i){if(!gate())return false;void *item=nullptr;
   std::memcpy(&item,static_cast<const char*>(array)+32+size_t(i)*sizeof(void*),sizeof(item));
   if(!item||c.object_class(item)!=b.element_class||!body(item))return false;
  }return true;
 }
 Observation *row(){if(p.count>=p.observations.size()){error=ReadBounds;return nullptr;}auto *o=&p.observations[p.count];*o=Observation{};return o;}
 bool nearby(void *n,uint64_t cell) {
  auto *o=row();if(!o)return false;o->encounter=get<uint64_t>(n,NearbyId);o->species=get<int32_t>(n,NearbySpecies);o->cell=cell;
  if(!text(get<void*>(n,NearbyFort),o->fort)||!display(get<void*>(n,NearbyDisplay),*o))return false;
  o->distance=get<float>(n,NearbyDistance);o->distance_valid=std::isfinite(o->distance)&&o->distance>=0;
  if(!o->fort[0]&&o->distance_valid){if(!stats.offstop_distance_count)stats.offstop_distance_min=stats.offstop_distance_max=o->distance;else{stats.offstop_distance_min=std::min(stats.offstop_distance_min,o->distance);stats.offstop_distance_max=std::max(stats.offstop_distance_max,o->distance);}++stats.offstop_distance_count;if(o->distance==200.0f)++stats.offstop_distance_200;}
  if(o->distance_valid){if(!stats.distance_count)stats.distance_min=stats.distance_max=o->distance;else{if(o->distance<stats.distance_min)stats.distance_min=o->distance;if(o->distance>stats.distance_max)stats.distance_max=o->distance;}++stats.distance_count;if(o->distance==200.0f)++stats.distance_200;}
  // DistanceMeters is never used to locate or discard a spawn.
  o->kind=o->fort[0]?Unlocated:(cell&&valid_l15(cell)?Cell:Unlocated);
  ++stats.nearby;if(!o->fort[0])++stats.offstop;++p.count;return true;
 }
 bool cell(void *v) {
  const uint64_t id=get<uint64_t>(v,CellId);++stats.cells;if(!valid_l15(id))++stats.non_l15;
  const uint8_t truncated=get<uint8_t>(v,CellTruncated);if(truncated>1)return false;p.truncated|=truncated!=0;
  if(!each(v,FortsList,2048,[&](void *f){if(p.fort_count>=p.forts.size()){error=ReadBounds;return false;}
    auto &fort=p.forts[p.fort_count];fort=Fort{};if(!text(get<void*>(f,FortId),fort.id)||!fort.id[0])return false;
    location(f,FortLat,FortLon,fort.lat,fort.lon);if(!coords(fort.lat,fort.lon))return false;
    if(valid_l15(id)&&!cell_contains(id,{fort.lat,fort.lon})){++stats.missing_positions;return true;}
    ++p.fort_count;return true;}))return false;
  if(!each(v,NearbyList,2048,[&](void *n){return nearby(n,id);} ))return false;
  if(!each(v,WildList,2048,[&](void *w){auto *o=row();if(!o)return false;o->kind=Exact;o->cell=id;
    o->encounter=get<uint64_t>(w,WildId);location(w,WildLat,WildLon,o->lat,o->lon);
    void *pokemon=get<void*>(w,WildPokemon);if(!exact(pokemon,PokemonClass))return false;
    o->species=get<int32_t>(pokemon,PokemonSpecies);if(!text(get<void*>(w,WildSpawn),o->spawn)||!display(get<void*>(pokemon,PokemonDisplay),*o)||!coords(o->lat,o->lon))return false;
    // Only positive, short countdowns are taken as an expiry observation. No
    // hourly prediction: packet timestamp semantics have not been proven live.
    const int32_t tth=get<int32_t>(w,WildTth);if(tth>0&&tth<=90000){o->expiry_ms=p.observed_ms+uint64_t(tth);++stats.short_tth;}
    if(valid_l15(id)&&!cell_contains(id,{o->lat,o->lon})){o->kind=Unlocated;o->lat=o->lon=0;++stats.missing_positions;}
    ++p.count;++stats.wild;return true;}))return false;
  if(!each(v,MapList,2048,[&](void *w){auto *o=row();if(!o)return false;o->kind=Exact;o->cell=id;
    o->encounter=get<uint64_t>(w,MapId);o->species=get<int32_t>(w,MapSpecies);location(w,MapLat,MapLon,o->lat,o->lon);
    if(!text(get<void*>(w,MapSpawn),o->spawn)||!display(get<void*>(w,MapDisplay),*o)||!coords(o->lat,o->lon))return false;
    // ExpirationTimeMs is a server wall timestamp. Do not subtract BOOTTIME or
    // assume local wall-clock alignment; use freshness TTL until calibrated.
    if(valid_l15(id)&&!cell_contains(id,{o->lat,o->lon})){o->kind=Unlocated;o->lat=o->lon=0;++stats.missing_positions;}
    ++p.count;++stats.catchable;return true;}))return false;
  return true;
 }
 int run(void *v){
  if(!gate())return error;if(!exact(v,ResponseClass))return ReadMetadata;
  if(get<int32_t>(v,ResponseStatus)!=c.success)return ReadIgnored;
  if(!each(v,CellsList,128,[&](void *cellObject){return cell(cellObject);} ))return error;
  p.cell_count=stats.cells;
  void *snapshot=get<void*>(v,ResponseSnapshot);
  if(snapshot){if(!exact(snapshot,SnapshotClass))return ReadMetadata;
   if(get<int32_t>(snapshot,SnapshotStatus)==c.snapshot_complete){const auto before=p.count;if(!each(snapshot,SnapshotList,2048,[&](void *n){return nearby(n,0);} ))return error;stats.top_level+=p.count-before;}}
  void *pinned=get<void*>(v,ResponsePinned);
  if(pinned){if(!exact(pinned,PinnedClass))return ReadMetadata;if(get<int32_t>(pinned,PinnedStatus)==c.pinned_included){void *n=get<void*>(pinned,PinnedPokemon);if(n){if(!exact(n,NearbyClass)||!nearby(n,0))return error;++stats.top_level;}}}
  return gate()?ReadOk:error;
 }
};
}
int read_response(const ReaderConfig &c,void *response,Packet &p,ReadStats &s,uint64_t deadline){
 p.count=p.fort_count=p.cell_count=0;p.truncated=false;s={};
 if(!c.clock_ms||!c.allowed||!c.object_class||!c.field_get||!c.array_length||!c.string_length||!c.string_chars)return ReadMetadata;
 for(auto k:c.classes)if(!k)return ReadMetadata;for(auto f:c.fields)if(!f)return ReadMetadata;
 for(const auto &l:c.lists)if(!l.field||!l.klass||!l.array||!l.count||!l.array_class||!l.element_class)return ReadMetadata;
 Reader r{c,p,s,deadline};const int rc=r.run(response);if(rc!=ReadOk){p.count=p.fort_count=p.cell_count=0;s={};}return rc;
}
}
