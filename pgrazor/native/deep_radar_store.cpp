#include "deep_radar_store.h"
#include "s2_geometry.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
namespace pgr::radar { namespace {
constexpr uint64_t ttl=90000,fort_ttl=600000;
bool coords(double a,double b){return std::isfinite(a)&&std::isfinite(b)&&std::fabs(a)<=90&&std::fabs(b)<=180;}
bool text(const char (&s)[257]){return std::memchr(s,0,257)!=nullptr;}
bool valid(const Observation&o){return o.encounter&&o.species>0&&o.species<=100000&&o.form>=0&&o.costume>=0&&o.gender>=0&&o.kind>=Unlocated&&o.kind<=Exact&&o.uncaught>=-1&&o.uncaught<=1&&text(o.fort)&&text(o.spawn)&&((o.kind!=Exact&&o.kind!=FortLinked)||coords(o.lat,o.lon))&&(o.kind!=Cell||valid_l15(o.cell))&&(!o.distance_valid||(std::isfinite(o.distance)&&o.distance>=0));}
}
void Store::clear(){count_=fort_count_=tombstone_next_=0;sequence_=time_=evictions_=0;for(auto&t:expired_)t={};}
void Store::remember_expired(uint64_t id,uint64_t now){expired_[tombstone_next_++%expired_.size()]={id,now+ttl};}
void Store::expire(uint64_t now){for(size_t i=0;i<count_;){if(rows_[i].expires_ms<=now){if(rows_[i].observation.expiry_ms)remember_expired(rows_[i].observation.encounter,now);rows_[i]=rows_[--count_];}else ++i;}
 for(size_t i=0;i<fort_count_;){if(now>=forts_[i].seen&&now-forts_[i].seen>=fort_ttl)forts_[i]=forts_[--fort_count_];else ++i;}}
void Store::join(Observation&o,uint64_t now)const{if(o.kind==Exact||!o.fort[0])return;o.kind=Unlocated;
 for(size_t i=0;i<fort_count_;++i)if((now<forts_[i].seen||now-forts_[i].seen<fort_ttl)&&!std::strcmp(o.fort,forts_[i].fort.id)){o.lat=forts_[i].fort.lat;o.lon=forts_[i].fort.lon;o.kind=FortLinked;return;}}
bool Store::seed_forts(const Fort*points,size_t count,uint64_t now){
 if(!points||count>2048||!now)return false;
 for(size_t i=0;i<count;++i)if(!text(points[i].id)||!points[i].id[0]||!coords(points[i].lat,points[i].lon))return false;
 for(size_t i=0;i<count;++i){size_t at=0;for(;at<fort_count_;++at)if(!std::strcmp(forts_[at].fort.id,points[i].id))break;
  const bool existing=at<fort_count_;if(!existing){if(fort_count_<forts_.size())++fort_count_;else{at=0;for(size_t j=1;j<fort_count_;++j)if(forts_[j].seen<forts_[at].seen)at=j;++evictions_;}}
  if(!existing||now>=forts_[at].seen)forts_[at]={points[i],now};
 }
 for(size_t i=0;i<count_;++i)join(rows_[i].observation,now);
 return true;
}
bool Store::resolve(uint64_t encounter,double lat,double lon,uint64_t now){
 if(!encounter||!coords(lat,lon))return false;
 for(size_t i=0;i<count_;++i){auto&e=rows_[i];auto&o=e.observation;if(o.encounter!=encounter||e.expires_ms<=now)continue;
  if(o.cell&&valid_l15(o.cell)&&!cell_contains(o.cell,{lat,lon}))return false;
  o.lat=lat;o.lon=lon;o.kind=Exact;return true;
 }return false;
}
bool Store::ingest(const Packet&p){
 if(!p.sequence||p.sequence<=sequence_||!p.observed_ms||p.observed_ms<time_||p.observed_ms>std::numeric_limits<uint64_t>::max()-fort_ttl||p.count>p.observations.size()||p.fort_count>p.forts.size()||p.cell_count>128)return false;
 // Validate the complete batch before changing cache/TTL state.
 for(size_t i=0;i<p.fort_count;++i)if(!text(p.forts[i].id)||!p.forts[i].id[0]||!coords(p.forts[i].lat,p.forts[i].lon))return false;
 for(size_t i=0;i<p.count;++i)if(!valid(p.observations[i]))return false;
 sequence_=p.sequence;time_=p.observed_ms;expire(time_);
 for(size_t i=0;i<p.fort_count;++i){size_t at=0;for(;at<fort_count_;++at)if(!std::strcmp(forts_[at].fort.id,p.forts[i].id))break;
  if(at==fort_count_){if(fort_count_<forts_.size())++fort_count_;else{at=0;for(size_t j=1;j<fort_count_;++j)if(forts_[j].seen<forts_[at].seen)at=j;++evictions_;}}
  forts_[at]={p.forts[i],time_};}
 for(size_t i=0;i<count_;++i)join(rows_[i].observation,time_);
 for(size_t i=0;i<p.count;++i){Observation o=p.observations[i];bool dead=false;for(const auto&t:expired_)if(t.id==o.encounter&&time_<t.until){dead=true;break;}if(dead)continue;
  if(o.expiry_ms&&o.expiry_ms<=time_){remember_expired(o.encounter,time_);continue;}join(o,time_);
  size_t at=0;for(;at<count_;++at)if(rows_[at].observation.encounter==o.encounter)break;
  if(at==count_){if(count_<rows_.size())++count_;else{at=0;for(size_t j=1;j<count_;++j)if(rows_[j].seen_ms<rows_[at].seen_ms||(rows_[j].seen_ms==rows_[at].seen_ms&&rows_[j].observation.encounter<rows_[at].observation.encounter))at=j;++evictions_;}rows_[at]={o,time_,std::min(time_+ttl,o.expiry_ms?o.expiry_ms:time_+ttl)};}
  else{Entry &e=rows_[at];const uint64_t known=e.observation.expiry_ms;
   // Preserve more precise location, but update last seen and never extend a
   // known despawn because a lower-precision Nearby record has no expiry.
   if(o.kind>=e.observation.kind){e.observation=o;if(!o.cell)e.observation.cell=e.observation.kind==Exact?o.cell:0;}
   if(known&&(!e.observation.expiry_ms||known<e.observation.expiry_ms))e.observation.expiry_ms=known;
   e.seen_ms=time_;e.expires_ms=std::min(time_+ttl,e.observation.expiry_ms?e.observation.expiry_ms:time_+ttl);
  }
 }return true;
}
}
