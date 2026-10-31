#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string>
#include <map>
#include <vector>
struct PgrSharedMetadataOutput;
namespace pgr_gym {
struct Settings {bool deploy=false,feed=false,fallback=false;int nanab=50,razz=50;};
bool parse(const char*,size_t,Settings&);
double distance(double,double,double,double);
struct Defender {uint64_t id=0;int species=0,cp=0;bool eligible=false,tagged=false,protected_fallback=false;};
uint64_t choose(const std::vector<Defender>&,const std::vector<int>&,bool fallback);
int berry(int nanab,int razz,const Settings&);
// Entries are not evicted while live. Saturation refuses work instead of losing
// deduplication guarantees. Attempts count even when the network outcome is unknown.
struct History {
 std::map<std::string,uint64_t> gyms;
 struct Feed {uint64_t until;unsigned count;};
 std::map<uint64_t,Feed> feeds;
 bool deploy_allowed(const std::string&,uint64_t);
 bool feed_allowed(uint64_t,uint64_t);
 bool deploy_attempt(const std::string&,uint64_t);
 bool feed_attempt(uint64_t,uint64_t);
 void refuse(uint64_t,uint64_t);
};
}
// Called only by the existing proven Unity owner; never starts a worker thread.
void pgr_gym_tick(uint64_t,const PgrSharedMetadataOutput*);
void pgr_gym_stop(uint64_t);
