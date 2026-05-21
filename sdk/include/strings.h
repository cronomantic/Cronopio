/* <strings.h> — Cronopio SDK freestanding libc. See ctype.h for rationale.
 *
 * doomtype.h includes this for the BSD case-insensitive comparisons. They are
 * implemented in cvm_libc.c (shared with <string.h>); this is a thin re-decl.*/
#ifndef CVM_LIBC_STRINGS_H
#define CVM_LIBC_STRINGS_H

#include <stddef.h>

int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);

#endif /* CVM_LIBC_STRINGS_H */
