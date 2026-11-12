#pragma once
#include <stdint.h>
struct PgrTransferExecRequest;
struct PgrTransferExecEvent;
struct PgrSharedMetadataOutput;
// 0: ordinary transfer; 1: queued; 2: kept/refused. Never scan existing storage.
int pgr_evolve_transfer_offer(const PgrTransferExecRequest*,bool honor_keep_all);
void pgr_evolve_transfer_tick(uint64_t,const PgrSharedMetadataOutput*);
void pgr_evolve_transfer_stop(uint64_t);
bool pgr_evolve_transfer_event(const PgrTransferExecEvent*);
bool pgr_evolve_transfer_busy();
