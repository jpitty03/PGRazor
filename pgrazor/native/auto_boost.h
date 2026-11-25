#pragma once
#include <stdint.h>
#include <cstdio>
struct PgrSharedMetadataOutput;
// Auto Lucky Egg / Auto Star Piece: while the map shows, uses a Lucky Egg or a Star Piece whenever
// that boost is not running and the bag has one, through the game's own
// IPlayerBuffService.SelectBuff (what using it from the item bag does). Settings ride transport
// string slot 26 ("B1|luckyEgg|starPiece"). Unity owner thread only.
void pgr_auto_boost_tick(uint64_t tid,const PgrSharedMetadataOutput *metadata);
void pgr_auto_boost_stop(uint64_t tid);

namespace pgr_auto_boost {
struct Config {bool valid=false,egg=false,star=false;};
// "B1|0/1|0/1"; anything else is invalid (both off).
inline Config parse(const char *s){
 Config c;int e=0,st=0,end=0;
 if(!s||std::sscanf(s,"B1|%d|%d%n",&e,&st,&end)!=2||s[end]||(e!=0&&e!=1)||(st!=0&&st!=1))return c;
 c.valid=true;c.egg=e==1;c.star=st==1;return c;
}
// Whether to use the item now: wanted, not active, in stock, and not inside the wait after the last
// use (or the longer back-off after uses that did not start the boost).
inline bool should_use(bool wanted,bool active,int count,uint64_t now,uint64_t not_before){
 return wanted&&!active&&count>0&&now>=not_before;
}
}
