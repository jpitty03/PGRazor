// Test-only bridge: the production hook engine has C++ linkage, while the included
// controller fixture inherits the C stub declaration. Keep both symbols distinct.
#include "../hook_engine.h"
extern "C" int pgrazor_test_real_hook(void *a, void *b, void **c) { return pgrazor_hook(a,b,c); }
