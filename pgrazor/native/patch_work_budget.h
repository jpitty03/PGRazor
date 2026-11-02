#pragma once
#include <stdint.h>
namespace pgr_perf {
constexpr unsigned cell_batch=12,pvp_tables=192;
inline uint64_t egg_interval(bool active){return active?250:1000;}
inline uint64_t heal_interval(bool active){return active?1500:5000;}
inline bool audit_cell(unsigned index,unsigned count,unsigned cursor,uint64_t now,uint64_t warm_until,bool applied,bool urgent){
 return urgent||!applied||now<warm_until||(count&&((index+count-(cursor%count))%count)<cell_batch);
}
inline unsigned next_cursor(unsigned cursor,unsigned count){return count?(cursor+cell_batch)%count:0;}
}
