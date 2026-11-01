#pragma once
#include <stdint.h>
struct PgrSharedMetadataOutput;
void pgr_gift_batch_tick(uint64_t tid,const PgrSharedMetadataOutput *metadata);
void pgr_gift_batch_stop();
