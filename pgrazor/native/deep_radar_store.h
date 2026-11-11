#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace pgr::radar {
enum Kind {Unlocated=0,FortLinked=1,Cell=2,Exact=3};
struct Fort {char id[257]{};double lat=0,lon=0;};
struct Observation {uint64_t encounter=0,cell=0;int32_t species=0,form=0,costume=0,gender=0;Kind kind=Unlocated;double lat=0,lon=0;char fort[257]{},spawn[257]{};uint64_t expiry_ms=0;int32_t uncaught=-1;float distance=0;bool distance_valid=false;};
struct Packet {uint64_t sequence=0,observed_ms=0;uint32_t fort_count=0,count=0,cell_count=0;bool truncated=false;std::array<Fort,2048> forts{};std::array<Observation,2048> observations{};};
struct Entry {Observation observation;uint64_t seen_ms=0,expires_ms=0;};
// No managed objects, network operations or locks. Owned by Unity tick. Capture
// buffers and Store are heap/static storage (never put a Packet on the stack).
class Store {
 struct CachedFort {Fort fort;uint64_t seen=0;};
 struct Tombstone {uint64_t id=0,until=0;};
 std::array<Entry,1024> rows_{};std::array<CachedFort,2048> forts_{};std::array<Tombstone,1024> expired_{};
 size_t count_=0,fort_count_=0,tombstone_next_=0;uint64_t sequence_=0,time_=0,evictions_=0;
 void join(Observation&,uint64_t now) const;
 void remember_expired(uint64_t,uint64_t);
 public:
 bool ingest(const Packet&);
 bool seed_forts(const Fort*,size_t count,uint64_t now);
 bool resolve(uint64_t encounter,double lat,double lon,uint64_t now);
 void expire(uint64_t now);
 void clear();
 size_t count()const{return count_;}
 const Entry& at(size_t i)const{return rows_[i];}
 uint64_t evictions()const{return evictions_;}
};
}
