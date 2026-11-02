#pragma once
#include <stdint.h>
void pgr_manual_transfer_tick(uint64_t now,uint64_t owner_tid,bool cleanup);
void pgr_manual_transfer_stop();
