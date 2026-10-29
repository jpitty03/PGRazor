#pragma once
#include "dobby.h"
// All native modules share Dobby's process-global trampoline registry. Serialize
// mutations even when independent feature adapters initialize on different threads.
int pgrazor_hook(void *address, void *replacement, void **original);
int pgrazor_unhook(void *address);
int pgrazor_instrument(void *address, dobby_instrument_callback_t callback);
