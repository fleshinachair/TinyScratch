#ifndef TS_ALLOC_H
#define TS_ALLOC_H

/* All heap use goes through these so tests can swap in a leak/overrun checker.
 * Allocation only ever happens in create/load paths, never in render. */
#include <stdlib.h>

#ifndef TS_CALLOC
#define TS_CALLOC(n) calloc(1, (n))
#endif
#ifndef TS_FREE
#define TS_FREE(p) free(p)
#endif

#endif
