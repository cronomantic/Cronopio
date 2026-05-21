/* <inttypes.h> — Cronopio SDK freestanding libc. See ctype.h for the rationale.
 *
 * The compiler ships <stdint.h> (fixed-width types) but not <inttypes.h>
 * (the PRI and SCN format-macro layer) that DOOM's doomtype.h includes. This
 * provides the format macros for a 32-bit little-endian target. Header-only.
 */
#ifndef CVM_LIBC_INTTYPES_H
#define CVM_LIBC_INTTYPES_H

#include <stdint.h>   /* compiler-provided */

/* 32-bit ILP32 target: int is 32-bit, long is 32-bit, long long is 64-bit. */
#define PRId8   "d"
#define PRId16  "d"
#define PRId32  "d"
#define PRId64  "lld"
#define PRIi8   "i"
#define PRIi16  "i"
#define PRIi32  "i"
#define PRIi64  "lli"
#define PRIu8   "u"
#define PRIu16  "u"
#define PRIu32  "u"
#define PRIu64  "llu"
#define PRIx8   "x"
#define PRIx16  "x"
#define PRIx32  "x"
#define PRIx64  "llx"
#define PRIX8   "X"
#define PRIX16  "X"
#define PRIX32  "X"
#define PRIX64  "llX"
#define PRIo8   "o"
#define PRIo16  "o"
#define PRIo32  "o"
#define PRIo64  "llo"

/* size-specific pointer/max forms used occasionally */
#define PRIdPTR "d"
#define PRIuPTR "u"
#define PRIxPTR "x"

#endif /* CVM_LIBC_INTTYPES_H */
