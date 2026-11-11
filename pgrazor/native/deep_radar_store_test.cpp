#include "deep_radar_store.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <memory>
#include <limits>
using namespace pgr::radar;
int main(){auto p=std::make_unique<Packet>();auto store=std::make_unique<Store>();p->sequence=1;p->observed_ms=1000;p->count=1;auto&o=p->observations[0];o.encounter=UINT64_MAX;o.species=885;std::strcpy(o.fort,"stop");assert(store->ingest(*p));assert(store->count()==1&&store->at(0).observation.kind==Unlocated);
 p->sequence=2;p->observed_ms=2000;p->fort_count=1;std::strcpy(p->forts[0].id,"stop");p->forts[0].lat=45;p->forts[0].lon=170;assert(store->ingest(*p));assert(store->at(0).observation.kind==FortLinked&&store->at(0).observation.lat==45);
 p->sequence=3;p->observed_ms=3000;p->fort_count=0;o.kind=Exact;o.lat=46;o.lon=171;o.expiry_ms=5000;assert(store->ingest(*p));assert(store->at(0).observation.kind==Exact);
 p->sequence=4;p->observed_ms=4000;o.kind=Unlocated;o.expiry_ms=0;assert(store->ingest(*p));assert(store->count()==1&&store->at(0).observation.lat==46&&store->at(0).expires_ms==5000);
 store->expire(5000);assert(store->count()==0);p->sequence=5;p->observed_ms=6000;assert(store->ingest(*p)&&store->count()==0); // known expired ID is not resurrected by Nearby
 o.encounter=42;p->sequence=6;p->observed_ms=7000;assert(store->ingest(*p)&&store->count()==1);assert(!store->ingest(*p));
 p->sequence=7;p->count=2;p->observations[1]=o;p->observations[1].encounter=43;p->observations[1].kind=Exact;p->observations[1].lat=std::numeric_limits<double>::quiet_NaN();assert(!store->ingest(*p)&&store->count()==1);p->observations[1].lat=0;assert(store->ingest(*p)&&store->count()==2);
 p->sequence=8;p->observed_ms=8000;p->count=0;assert(store->ingest(*p)&&store->count()==2); // delta omission is not deletion
 store->expire(98000);assert(store->count()==0);
 store->clear();p->sequence=1;p->observed_ms=100000;p->count=1025;for(unsigned i=0;i<p->count;++i){p->observations[i]=Observation{};p->observations[i].encounter=i+1;p->observations[i].species=1;}assert(store->ingest(*p)&&store->count()==1024&&store->evictions()==1);
 p->sequence=2;p->observed_ms=99999;assert(!store->ingest(*p));
 store->clear();p->sequence=1;p->observed_ms=1000;p->count=1;p->fort_count=0;p->observations[0]=Observation{};p->observations[0].encounter=77;p->observations[0].species=1;std::strcpy(p->observations[0].fort,"cached");assert(store->ingest(*p));
 Fort cached{};std::strcpy(cached.id,"cached");cached.lat=35;cached.lon=139;assert(store->seed_forts(&cached,1,1500));assert(store->at(0).observation.kind==FortLinked&&store->at(0).observation.lat==35);assert(store->at(0).seen_ms==1000&&store->at(0).expires_ms==91000);
 cached.lat=80;assert(store->seed_forts(&cached,1,1400));assert(store->at(0).observation.lat==35); // older scene snapshots cannot overwrite a newer cache entry
 cached.lat=100;assert(!store->seed_forts(&cached,1,2000)&&store->at(0).observation.lat==35);
 store->expire(91000);cached.lat=35;assert(store->seed_forts(&cached,1,92000)&&store->count()==0); // local cache never resurrects a spawn
 std::puts("PASS radar store: delayed fort join, uint64 dedup, exact precedence, known expiry, atomic malformed/stale rejection, deltas and capacity");
}
