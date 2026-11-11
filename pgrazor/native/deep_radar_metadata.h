#pragma once
#include "deep_radar_reader.h"
#include "runtime_bootstrap.h"

// Resolves only stable metadata pointers. No managed objects retained, no hooks.
// Returns false with a bounded reason; unsupported radar never disables VGP.
bool pgr_deep_radar_bind(const PgrBootBinding&,pgr::radar::ReaderConfig&,const void **response_code,char *reason,size_t reason_size);
