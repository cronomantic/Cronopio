/* <memory.h> — Cronopio SDK freestanding libc.
 *
 * A legacy SVR4/SysV header that declared the mem* routines (memcpy/memset/
 * memcmp/...) before they were consolidated into <string.h> by ANSI C. Some
 * older codebases (e.g. UQM's state.c) still #include it. We simply forward to
 * <string.h>, which holds the actual prototypes. */
#ifndef CVM_LIBC_MEMORY_H
#define CVM_LIBC_MEMORY_H

#include <string.h>

#endif /* CVM_LIBC_MEMORY_H */
