/*
 *  cron_2dpak.h — cart-side parser for ROM blobs produced by tools/2dpak
 *  with --rom. Header-only, zero syscalls. Pair with --rom=FILE at
 *  cart-build time so cron_rom() returns the blob; this header pulls the
 *  palette + pixel pointers out of it without copying.
 *
 *  Blob layout (little-endian, i386-elf cart ABI):
 *
 *    offset  size     field
 *    0       4        magic '2DPK'
 *    4       2        version (must be 1)
 *    6       2        w
 *    8       2        h
 *    10      2        pal_count
 *    12      pal_n*4  palette (u32 0x00RRGGBB)
 *    ...     w*h      pixel data (u8)
 *
 *  Typical usage:
 *
 *      const uint8_t *rom = (const uint8_t *)cron_rom();
 *      cron_2dpak_image_t img;
 *      if (cron_2dpak_parse_image(rom, cron_rom_size(), &img) != 0) {
 *          cron_log("rom: bad 2dpak header\n", 22);
 *          return 1;
 *      }
 *      for (int i = 0; i < img.pal_count; ++i)
 *          cron_palette_set(i, img.palette[i]);
 *      cron_image(0, img.pixels, img.w, img.h);
 */

#ifndef _CVM_CRON_2DPAK_H
#define _CVM_CRON_2DPAK_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int              w, h;
    int              pal_count;
    const uint32_t  *palette;     /* aliased into the rom buffer (no copy) */
    const uint8_t   *pixels;      /* idem */
} cron_2dpak_image_t;

/* Parse a 2DPK blob in place. Returns 0 on success, non-zero on:
 *   1 — truncated (too short for header)
 *   2 — bad magic
 *   3 — unsupported version
 *   4 — declared body larger than the blob
 * On success, palette and pixels point inside `rom` — they live as long
 * as the blob does (which is always, since ROM is mapped read-only by
 * the host for the cart's lifetime). */
static inline int cron_2dpak_parse_image(const uint8_t *rom, size_t rom_size,
                                         cron_2dpak_image_t *out) {
    if (rom_size < 12) return 1;
    if (rom[0] != '2' || rom[1] != 'D' || rom[2] != 'P' || rom[3] != 'K') return 2;
    int version = rom[4] | (rom[5] << 8);
    if (version != 1) return 3;
    int w       = rom[6] | (rom[7] << 8);
    int h       = rom[8] | (rom[9] << 8);
    int pal_n   = rom[10] | (rom[11] << 8);
    size_t pal_bytes = (size_t)pal_n * 4u;
    size_t pix_bytes = (size_t)w * (size_t)h;
    if ((size_t)12 + pal_bytes + pix_bytes > rom_size) return 4;
    out->w         = w;
    out->h         = h;
    out->pal_count = pal_n;
    out->palette   = (const uint32_t *)(rom + 12);
    out->pixels    = rom + 12 + pal_bytes;
    return 0;
}

/* TMAP blob (separate magic from image blobs so the cart can sanity-check
 * which kind of asset it got):
 *
 *    offset  size     field
 *    0       4        magic 'TMAP'
 *    4       2        version (must be 1)
 *    6       2        w
 *    8       2        h
 *    10      w*h*2    cells (u16 LE — Cronopio layout: HFLIP/VFLIP in
 *                     top 2 bits, idx in low 14, 0xFFFF = empty)
 */
typedef struct {
    int              w, h;
    const uint16_t  *cells;
} cron_2dpak_tilemap_t;

static inline int cron_2dpak_parse_tilemap(const uint8_t *rom, size_t rom_size,
                                           cron_2dpak_tilemap_t *out) {
    if (rom_size < 10) return 1;
    if (rom[0] != 'T' || rom[1] != 'M' || rom[2] != 'A' || rom[3] != 'P') return 2;
    int version = rom[4] | (rom[5] << 8);
    if (version != 1) return 3;
    int w     = rom[6] | (rom[7] << 8);
    int h     = rom[8] | (rom[9] << 8);
    if ((size_t)10 + (size_t)w * (size_t)h * 2u > rom_size) return 4;
    out->w     = w;
    out->h     = h;
    out->cells = (const uint16_t *)(rom + 10);
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* _CVM_CRON_2DPAK_H */
