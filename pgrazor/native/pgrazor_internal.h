/* Internal (non-exported) declarations shared between the core and the JNI glue. */
#ifndef PGRAZOR_INTERNAL_H
#define PGRAZOR_INTERNAL_H

#include <stdint.h>

/* Deterministic spawn-policy self-check used by the JNI host surface.
 * Returns 0 on success or the id of the first failing case. No Dobby, no hooking. */
int pgrazor_policy_selfcheck(void);

#endif /* PGRAZOR_INTERNAL_H */
