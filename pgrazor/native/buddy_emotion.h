#pragma once
#include <stdint.h>
struct PgrSharedMetadataOutput;
void pgr_buddy_emotion_tick(uint64_t tid,const PgrSharedMetadataOutput *metadata);
void pgr_buddy_emotion_stop(uint64_t tid);
