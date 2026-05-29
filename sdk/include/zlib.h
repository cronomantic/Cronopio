/* <zlib.h> — Cronopio SDK: zlib API shim backed by vendored miniz (see
 * sdk/lib/miniz.c, miniz.h; public domain / Unlicense). miniz exposes the
 * zlib-compatible streaming names (z_stream, inflate, inflateInit2,
 * inflateEnd, inflateReset, Z_OK, …) unless MINIZ_NO_ZLIB_COMPATIBLE_NAMES
 * is set — we leave them on. Only the inflate side is built; the cart
 * defines MINIZ_NO_DEFLATE_APIS / MINIZ_NO_ARCHIVE_APIS / MINIZ_NO_STDIO /
 * MINIZ_NO_TIME (see build flags). Surfaced by ports that read ZIP/zlib
 * streams — e.g. UQM's libs/uio zip filesystem reading its .uqm packs. */
#ifndef CVM_LIBC_ZLIB_SHIM
#define CVM_LIBC_ZLIB_SHIM

#include <miniz.h>

#endif /* CVM_LIBC_ZLIB_SHIM */
