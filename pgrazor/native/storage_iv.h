#pragma once
#include <stdint.h>
struct PgrSharedMetadataOutput;
// Pokemon storage list IVs: a small label under each cell's CP text. Transport string slot 14
// carries "S1|<mode>" (0 off, 1 attack/defense/stamina, 2 percent, 3 both).
void pgr_storage_iv_tick(uint64_t tid,const PgrSharedMetadataOutput *metadata);
void pgr_storage_iv_stop();
