/* uvco-owned portability adapter. Original vendor bytes remain unchanged.
 * Full ASan/UBSan instrumentation is retained. See docs/design-kcp.md. */
#include <stddef.h>
#include "ikcp.h"
#undef IOFFSETOF
#define IOFFSETOF(TYPE, MEMBER) offsetof(TYPE, MEMBER)
#include "ikcp.c"
