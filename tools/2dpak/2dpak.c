/*
 *  2dpak — convert an indexed-palette image into a Cronopio cart C header.
 *  Accepts indexed BMP-8 and indexed PNG-8 directly; both preserve the
 *  artist's palette ordering (so cart code that says "col 1 = skin, col 2
 *  = hair" stays stable across edits).
 *
 *  Output: a C header with the palette (uint32_t 0x00RRGGBB[N]), the
 *  pixel data (uint8_t[w*h]), and dimension macros. The cart includes
 *  it and registers via cron_image / cron_palette_set.
 *
 *  Format detection is by magic bytes — PNG (89 50 4E 47), BMP (42 4D).
 *  PNG path:
 *    - Manually parses the PLTE chunk to recover the artist's palette
 *      in their authored order (stb_image always decodes to RGB and
 *      doesn't expose PLTE).
 *    - Uses stb_image (vendored under vendor/stb_image.h, public domain)
 *      for the IDAT-decoded pixel data. After decode, each RGB pixel is
 *      mapped back to its index via the PLTE.
 *    - Requires an indexed PNG (PNG-8 with a PLTE chunk). Truecolour PNGs
 *      get rejected with a clear message — convert via "Image > Mode >
 *      Indexed" in GIMP / Aseprite / Krita first.
 *  BMP path:
 *    - Native ~50-line parser; uncompressed BI_RGB, 8 bpp, palette in the
 *      BITMAPINFOHEADER. Top-down or bottom-up rows both supported.
 *
 *  Usage:
 *      2dpak <input.{png,bmp}> <output.h> [--prefix=NAME] [--max-pal=32]
 *      2dpak --selftest <output.bmp>    # writes a known-good 32x16 test BMP
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_JPEG
#define STBI_NO_TGA
#define STBI_NO_GIF
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_BMP        /* we have our own BMP path */
#include "external/stb_image.h"

typedef struct {
    int      w, h;
    uint32_t palette[256];    /* 0x00RRGGBB */
    int      pal_count;
    uint8_t *pix;             /* w*h, row-major, top-down */
} bmp8_t;

/* ---------- Median-cut palette quantization ----------------------------- */

/* Classic Heckbert median-cut: split RGB space along its longest axis at
 * the median until N boxes remain; each box's RGB centroid is one palette
 * entry. Good enough for "I made art in RGB and want to use it" cases;
 * artist-curated indexed art still goes through the PLTE-preserving path.
 *
 * Operates directly on the pixel array (no histogram), which is fine for
 * 320x240 = 76800 px and log2(32) = 5 split levels: ~380k compare ops,
 * sub-millisecond on native. */

typedef struct { uint8_t r, g, b; } rgb_t;
typedef struct { int begin, end; uint8_t r0,g0,b0, r1,g1,b1; } qbox_t;

static int cmp_r(const void *a, const void *b) { return ((rgb_t*)a)->r - ((rgb_t*)b)->r; }
static int cmp_g(const void *a, const void *b) { return ((rgb_t*)a)->g - ((rgb_t*)b)->g; }
static int cmp_b(const void *a, const void *b) { return ((rgb_t*)a)->b - ((rgb_t*)b)->b; }

static void box_bounds(rgb_t *p, qbox_t *b) {
    b->r0 = b->g0 = b->b0 = 255;
    b->r1 = b->g1 = b->b1 = 0;
    for (int i = b->begin; i < b->end; ++i) {
        if (p[i].r < b->r0) b->r0 = p[i].r;
        if (p[i].r > b->r1) b->r1 = p[i].r;
        if (p[i].g < b->g0) b->g0 = p[i].g;
        if (p[i].g > b->g1) b->g1 = p[i].g;
        if (p[i].b < b->b0) b->b0 = p[i].b;
        if (p[i].b > b->b1) b->b1 = p[i].b;
    }
}

/* Quantize `pixels` (n RGB triples, modified in-place — sorted by box)
 * into up to `max_colors` palette entries. Writes `out_pal[]` and returns
 * the actual palette count. The input is shuffled during boxing but never
 * lost — caller uses it only for the index remap afterwards (via the
 * nearest-colour map). */
static int median_cut(rgb_t *pixels, int n, int max_colors,
                      uint32_t *out_pal) {
    if (n <= 0 || max_colors <= 0) return 0;
    qbox_t *boxes = (qbox_t *)calloc((size_t)max_colors, sizeof(qbox_t));
    if (!boxes) return 0;
    boxes[0].begin = 0; boxes[0].end = n;
    box_bounds(pixels, &boxes[0]);
    int nb = 1;
    while (nb < max_colors) {
        /* Find box with the largest single-axis range. */
        int best = -1, best_range = 0;
        for (int i = 0; i < nb; ++i) {
            int dr = boxes[i].r1 - boxes[i].r0;
            int dg = boxes[i].g1 - boxes[i].g0;
            int db = boxes[i].b1 - boxes[i].b0;
            int m = dr > dg ? (dr > db ? dr : db) : (dg > db ? dg : db);
            if (m > best_range && (boxes[i].end - boxes[i].begin) > 1) {
                best_range = m; best = i;
            }
        }
        if (best < 0) break;
        /* Sort along longest axis, split at median. */
        int dr = boxes[best].r1 - boxes[best].r0;
        int dg = boxes[best].g1 - boxes[best].g0;
        int db = boxes[best].b1 - boxes[best].b0;
        int (*cmp)(const void*, const void*) =
            dr >= dg && dr >= db ? cmp_r : (dg >= db ? cmp_g : cmp_b);
        qsort(pixels + boxes[best].begin,
              (size_t)(boxes[best].end - boxes[best].begin),
              sizeof(rgb_t), cmp);
        int mid = (boxes[best].begin + boxes[best].end) / 2;
        boxes[nb].begin = mid;
        boxes[nb].end   = boxes[best].end;
        boxes[best].end = mid;
        box_bounds(pixels, &boxes[best]);
        box_bounds(pixels, &boxes[nb]);
        ++nb;
    }
    /* Per-box centroid as the palette entry. */
    for (int i = 0; i < nb; ++i) {
        long sr = 0, sg = 0, sb = 0;
        int k = boxes[i].end - boxes[i].begin;
        for (int j = boxes[i].begin; j < boxes[i].end; ++j) {
            sr += pixels[j].r; sg += pixels[j].g; sb += pixels[j].b;
        }
        int r = k ? (int)(sr / k) : 0;
        int g = k ? (int)(sg / k) : 0;
        int b = k ? (int)(sb / k) : 0;
        out_pal[i] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
    free(boxes);
    return nb;
}

/* For each RGB pixel, find the nearest palette index by squared distance. */
static uint8_t nearest_pal(uint32_t c, const uint32_t *pal, int pal_n) {
    int r = (int)((c >> 16) & 0xFF);
    int g = (int)((c >> 8) & 0xFF);
    int b = (int)(c & 0xFF);
    int best = 0, best_d = 0x7FFFFFFF;
    for (int p = 0; p < pal_n; ++p) {
        int pr = (int)((pal[p] >> 16) & 0xFF);
        int pg = (int)((pal[p] >> 8) & 0xFF);
        int pb = (int)(pal[p] & 0xFF);
        int dr = pr - r, dg = pg - g, db = pb - b;
        int d  = dr*dr + dg*dg + db*db;
        if (d < best_d) { best_d = d; best = p; }
    }
    return (uint8_t)best;
}

/* ---------- PNG-8 parsing (PLTE manually, IDAT via stb_image) ----------- */

/* Returns 1 if file starts with the PNG signature. */
static int is_png(const uint8_t *buf, size_t n) {
    static const uint8_t sig[8] = {0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A};
    return (n >= 8 && memcmp(buf, sig, 8) == 0);
}

/* Walk PNG chunks looking for PLTE. Format per chunk:
 *   length(4 BE) | type(4) | data(length) | crc(4)
 * PLTE data is N×3 bytes (RGB). Returns 0 on success and fills palette;
 * returns 1 if no PLTE (i.e. truecolour PNG — caller should error). */
static int read_png_plte(const uint8_t *buf, size_t n,
                         uint32_t out_pal[256], int *out_n) {
    size_t pos = 8;   /* skip signature; IHDR is next but we don't need its fields */
    while (pos + 12 <= n) {
        uint32_t len = ((uint32_t)buf[pos] << 24) | ((uint32_t)buf[pos+1] << 16)
                     | ((uint32_t)buf[pos+2] << 8) |  (uint32_t)buf[pos+3];
        const uint8_t *type = buf + pos + 4;
        const uint8_t *data = buf + pos + 8;
        if (pos + 8 + len + 4 > n) return 1;   /* truncated */
        if (memcmp(type, "PLTE", 4) == 0) {
            int pn = (int)(len / 3);
            if (pn > 256) pn = 256;
            for (int i = 0; i < pn; ++i) {
                out_pal[i] = ((uint32_t)data[i*3+0] << 16)
                           | ((uint32_t)data[i*3+1] << 8)
                           |  (uint32_t)data[i*3+2];
            }
            *out_n = pn;
            return 0;
        }
        if (memcmp(type, "IEND", 4) == 0) return 1;   /* end without PLTE */
        pos += 8 + len + 4;
    }
    return 1;
}

static int read_png(const char *path, bmp8_t *out, int quant_max) {
    /* Slurp the file for our own PLTE walk; stb_image will re-read via
     * stbi_load and decode the IDAT to RGB independently. */
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "open '%s' failed\n", path); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    if (sz < 0 || sz > (1 << 28)) { fclose(f); fprintf(stderr, "%s: implausible size\n", path); return 1; }
    uint8_t *raw = (uint8_t *)malloc((size_t)sz);
    if (!raw) { fclose(f); fprintf(stderr, "%s: oom\n", path); return 1; }
    if (fread(raw, 1, (size_t)sz, f) != (size_t)sz) {
        free(raw); fclose(f); fprintf(stderr, "%s: short read\n", path); return 1;
    }
    fclose(f);

    int pal_n = 0;
    int has_plte = (read_png_plte(raw, (size_t)sz, out->palette, &pal_n) == 0);

    int w, h, ch;
    uint8_t *rgb = stbi_load_from_memory(raw, (int)sz, &w, &h, &ch, 3);
    free(raw);
    if (!rgb) { fprintf(stderr, "%s: stb_image failed: %s\n", path, stbi_failure_reason()); return 1; }
    out->w = w; out->h = h;
    out->pix = (uint8_t *)malloc((size_t)w * (size_t)h);
    if (!out->pix) { stbi_image_free(rgb); fprintf(stderr, "%s: oom %d bytes\n", path, w*h); return 1; }

    if (has_plte) {
        /* PLTE-preserved path: linear scan to remap each RGB to its index
         * in the artist's authored palette. Stable indices across edits. */
        out->pal_count = pal_n;
        int warned = 0;
        for (int i = 0; i < w * h; ++i) {
            uint32_t c = ((uint32_t)rgb[i*3+0] << 16)
                       | ((uint32_t)rgb[i*3+1] << 8)
                       |  (uint32_t)rgb[i*3+2];
            int idx = 0;
            for (int p = 0; p < pal_n; ++p) {
                if (out->palette[p] == c) { idx = p; break; }
                if (p == pal_n - 1 && !warned) {
                    fprintf(stderr, "warning: pixel %d has colour 0x%06X not in PLTE; using index 0\n", i, c);
                    warned = 1;
                }
            }
            out->pix[i] = (uint8_t)idx;
        }
    } else {
        /* No PLTE — quantize via median-cut. The artist gets WHATEVER
         * palette assignment median-cut produces, so this path is best
         * for "I have an RGB image and don't care about specific index
         * numbers". For predictable indices: re-author as PLTE-indexed. */
        fprintf(stderr, "%s: truecolour PNG — quantizing to %d colours (median-cut).\n"
                        "  For stable indices, re-author as indexed PNG (PLTE) instead.\n",
                path, quant_max);
        /* Copy RGB into median-cut working set (modified in-place). */
        rgb_t *work = (rgb_t *)malloc((size_t)w * (size_t)h * sizeof(rgb_t));
        if (!work) { stbi_image_free(rgb); fprintf(stderr, "%s: oom\n", path); return 1; }
        for (int i = 0; i < w * h; ++i) {
            work[i].r = rgb[i*3+0]; work[i].g = rgb[i*3+1]; work[i].b = rgb[i*3+2];
        }
        out->pal_count = median_cut(work, w * h, quant_max, out->palette);
        free(work);
        /* Remap via nearest-colour from the ORIGINAL RGB (not the shuffled
         * work set — the in-place sort lost the spatial position). */
        for (int i = 0; i < w * h; ++i) {
            uint32_t c = ((uint32_t)rgb[i*3+0] << 16)
                       | ((uint32_t)rgb[i*3+1] << 8)
                       |  (uint32_t)rgb[i*3+2];
            out->pix[i] = nearest_pal(c, out->palette, out->pal_count);
        }
    }
    stbi_image_free(rgb);
    return 0;
}

/* ---------- BMP-8 parsing ----------------------------------------------- */

#pragma pack(push, 1)
typedef struct {
    uint16_t bfType;          /* 'BM' (0x4D42) */
    uint32_t bfSize;          /* total file size */
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;       /* offset to pixel data */
} bmp_file_hdr_t;

typedef struct {
    uint32_t biSize;          /* 40 for BITMAPINFOHEADER */
    int32_t  biWidth;
    int32_t  biHeight;        /* +ve = bottom-up, -ve = top-down */
    uint16_t biPlanes;
    uint16_t biBitCount;      /* 8 here */
    uint32_t biCompression;   /* 0 = BI_RGB */
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;       /* palette entries (0 = 2^bpp) */
    uint32_t biClrImportant;
} bmp_info_hdr_t;
#pragma pack(pop)

static int read_bmp8(const char *path, bmp8_t *out) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "open '%s' failed\n", path); return 1; }

    bmp_file_hdr_t fh;
    bmp_info_hdr_t ih;
    if (fread(&fh, sizeof fh, 1, f) != 1) { fclose(f); fprintf(stderr, "%s: short file header\n", path); return 1; }
    if (fh.bfType != 0x4D42) { fclose(f); fprintf(stderr, "%s: not a BMP (magic 0x%04X)\n", path, fh.bfType); return 1; }
    if (fread(&ih, sizeof ih, 1, f) != 1) { fclose(f); fprintf(stderr, "%s: short info header\n", path); return 1; }
    if (ih.biSize < 40) { fclose(f); fprintf(stderr, "%s: unsupported info header (size=%u, need >=40)\n", path, ih.biSize); return 1; }
    if (ih.biBitCount != 8) { fclose(f); fprintf(stderr, "%s: need 8 bits per pixel, got %u (export indexed)\n", path, ih.biBitCount); return 1; }
    if (ih.biCompression != 0) { fclose(f); fprintf(stderr, "%s: need uncompressed BI_RGB (got compression=%u — try BMP3)\n", path, ih.biCompression); return 1; }
    if (ih.biWidth <= 0) { fclose(f); fprintf(stderr, "%s: bad width\n", path); return 1; }

    int w = ih.biWidth;
    int h = ih.biHeight < 0 ? -ih.biHeight : ih.biHeight;
    int top_down = ih.biHeight < 0;
    if (w > 4096 || h > 4096) { fclose(f); fprintf(stderr, "%s: implausible dims %dx%d\n", path, w, h); return 1; }

    /* Skip ANY remaining header bytes (e.g. V4/V5 extensions) to land on
     * the palette. */
    if (ih.biSize > 40) fseek(f, ih.biSize - 40, SEEK_CUR);

    int pal_n = (int)(ih.biClrUsed ? ih.biClrUsed : 256);
    if (pal_n > 256) pal_n = 256;
    for (int i = 0; i < pal_n; ++i) {
        uint8_t bgra[4];
        if (fread(bgra, 4, 1, f) != 1) { fclose(f); fprintf(stderr, "%s: short palette\n", path); return 1; }
        /* BMP stores BGRA (alpha=0 in BMP3) — flip to 0x00RRGGBB. */
        out->palette[i] = ((uint32_t)bgra[2] << 16) | ((uint32_t)bgra[1] << 8) | bgra[0];
    }
    /* Some headers report fewer palette entries than fill the gap to
     * bfOffBits — seek straight to the pixels regardless. */
    if (fseek(f, (long)fh.bfOffBits, SEEK_SET) != 0) { fclose(f); fprintf(stderr, "%s: pixel seek failed\n", path); return 1; }

    /* BMP rows are padded to 4-byte boundaries. */
    int row_bytes = ((w + 3) / 4) * 4;
    uint8_t *raw = (uint8_t *)malloc((size_t)row_bytes * (size_t)h);
    if (!raw) { fclose(f); fprintf(stderr, "%s: oom %d bytes\n", path, row_bytes * h); return 1; }
    if (fread(raw, (size_t)row_bytes, (size_t)h, f) != (size_t)h) {
        free(raw); fclose(f); fprintf(stderr, "%s: short pixel data\n", path); return 1;
    }
    fclose(f);

    out->w = w; out->h = h; out->pal_count = pal_n;
    out->pix = (uint8_t *)malloc((size_t)w * (size_t)h);
    if (!out->pix) { free(raw); fprintf(stderr, "%s: oom %d bytes\n", path, w * h); return 1; }

    /* Repack into top-down, contiguous w*h. */
    for (int y = 0; y < h; ++y) {
        int src_y = top_down ? y : (h - 1 - y);
        memcpy(out->pix + (size_t)y * (size_t)w,
               raw + (size_t)src_y * (size_t)row_bytes,
               (size_t)w);
    }
    free(raw);
    return 0;
}

/* ---------- Tiled .tmx tilemap parser ----------------------------------- */

/* A 16-bit tilemap (matches Cronopio cells: u16 per cell, 0xFFFF = empty,
 * bits 15/14 = HFLIP/VFLIP, bits 13..0 = tile index). Same struct stride
 * as bmp8_t for write-side reuse — we pretend the tilemap is a "u8" buffer
 * of size 2*w*h so write_header / write_rom can dump it byte-for-byte.
 * For ROM the layout is identical (magic '2DPK', but pixels are actually
 * u16 little-endian); the cart-side parser distinguishes by file extension
 * or naming convention. For now we keep it simple and reuse. */

typedef struct {
    int        w, h;          /* in cells */
    uint16_t  *cells;         /* w*h, row-major */
} tilemap_t;

/* Minimal XML scaffolding — look for substring patterns rather than a real
 * parser. Tiled CSV maps are small and well-formed; ~80 lines of careful
 * string-search beats vendoring a 3 KLoC XML parser. Limits: only "csv"
 * encoding, single <layer>, no nested external tilesets resolved (just
 * pull the data block). For richer needs a future revision can swap in a
 * proper XML walker. */

/* Tiny memmem-style helper (Windows MinGW libc lacks memmem). */
static const void *memmem_in(const void *hay, size_t hl, const void *nee, size_t nl) {
    if (nl == 0) return hay;
    if (hl < nl) return NULL;
    const char *h = (const char *)hay;
    for (size_t i = 0; i + nl <= hl; ++i)
        if (memcmp(h + i, nee, nl) == 0) return h + i;
    return NULL;
}

/* Returns pointer to start of the inner text of the next <data ...> tag,
 * and sets *out_end to the position of "</data>". NULL if not found. */
static const char *find_csv_data(const char *src, const char **out_end) {
    const char *p = src;
    while ((p = strstr(p, "<data")) != NULL) {
        const char *gt = strchr(p, '>');
        if (!gt) return NULL;
        /* Only accept encoding="csv" — anything else (base64, base64+zlib)
         * needs DEFLATE which we don't have without extending this tool. */
        if (!memmem_in(p, (size_t)(gt - p), "encoding=\"csv\"", 14) &&
            !memmem_in(p, (size_t)(gt - p), "encoding='csv'", 14)) {
            p = gt + 1; continue;
        }
        const char *end = strstr(gt + 1, "</data>");
        if (!end) return NULL;
        *out_end = end;
        return gt + 1;
    }
    return NULL;
}

/* Extract width="N" / height="N" attribute values from the <map> tag. */
static int find_int_attr(const char *src, const char *name, int *out) {
    char pat[32];
    int n = snprintf(pat, sizeof pat, "%s=\"", name);
    if (n <= 0) return 1;
    const char *p = strstr(src, pat);
    if (!p) return 1;
    p += n;
    char *endp = NULL;
    long v = strtol(p, &endp, 10);
    if (endp == p) return 1;
    *out = (int)v;
    return 0;
}

/* Read the whole file into a malloc'd null-terminated buffer. */
static char *slurp_text(const char *path, size_t *out_n) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "open '%s' failed\n", path); return NULL; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    if (sz < 0 || sz > (1 << 26)) { fclose(f); fprintf(stderr, "%s: too big\n", path); return NULL; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); fprintf(stderr, "%s: short read\n", path); return NULL;
    }
    buf[sz] = '\0';
    fclose(f);
    if (out_n) *out_n = (size_t)sz;
    return buf;
}

/* Parse a Tiled .tmx, CSV-encoded. The first <layer>'s <data encoding="csv">
 * is consumed; map width/height come from the <map> tag. Tiled GIDs are
 * 1-based (0 = empty) and carry flip bits in the top 3 bits:
 *   0x80000000 — flipped horizontally
 *   0x40000000 — flipped vertically
 *   0x20000000 — flipped anti-diagonally (rotated 90 — not representable
 *                in our HFLIP/VFLIP-only scheme; we warn and ignore).
 * Our cell layout:
 *   0xFFFF     — empty
 *   bit 15     — HFLIP
 *   bit 14     — VFLIP
 *   bits 13..0 — tile index, 0-based (so Tiled GID N becomes index N-1) */
static int read_tmx(const char *path, tilemap_t *out) {
    size_t sz = 0;
    char *src = slurp_text(path, &sz);
    if (!src) return 1;

    /* The TMX root <map> tag holds the dims. */
    const char *map_tag = strstr(src, "<map");
    if (!map_tag) { free(src); fprintf(stderr, "%s: no <map> tag\n", path); return 1; }
    int W = 0, H = 0;
    if (find_int_attr(map_tag, "width", &W)  != 0 ||
        find_int_attr(map_tag, "height", &H) != 0) {
        free(src);
        fprintf(stderr, "%s: missing width/height on <map>\n", path);
        return 1;
    }
    if (W <= 0 || H <= 0 || W > 4096 || H > 4096) {
        free(src);
        fprintf(stderr, "%s: implausible dims %dx%d\n", path, W, H);
        return 1;
    }

    const char *data_end = NULL;
    const char *csv = find_csv_data(src, &data_end);
    if (!csv) {
        free(src);
        fprintf(stderr, "%s: no <data encoding=\"csv\"> — re-export Tiled "
                        "with 'Tile Layer Format: CSV'.\n", path);
        return 1;
    }

    out->w = W; out->h = H;
    out->cells = (uint16_t *)malloc((size_t)W * (size_t)H * 2u);
    if (!out->cells) { free(src); fprintf(stderr, "%s: oom\n", path); return 1; }

    int diag_warned = 0;
    int range_warned = 0;
    int filled = 0;
    const char *p = csv;
    while (p < data_end && filled < W * H) {
        /* Skip whitespace and commas */
        while (p < data_end && (*p == ' ' || *p == '\t' || *p == '\n' ||
                                *p == '\r' || *p == ',')) ++p;
        if (p >= data_end) break;
        char *endp = NULL;
        unsigned long gid = strtoul(p, &endp, 10);
        if (endp == p) break;          /* not a number, end of useful data */
        p = endp;
        int hflip = (gid & 0x80000000ul) != 0;
        int vflip = (gid & 0x40000000ul) != 0;
        if ((gid & 0x20000000ul) && !diag_warned) {
            fprintf(stderr, "%s: warning — anti-diagonal flip not "
                            "representable; rendered as no-flip\n", path);
            diag_warned = 1;
        }
        unsigned long idx = gid & 0x1FFFFFFFul;     /* strip top 3 bits */
        uint16_t cell;
        if (idx == 0) {
            cell = 0xFFFF;       /* empty */
        } else {
            unsigned tile_idx = (unsigned)idx - 1u;  /* GID is 1-based */
            if (tile_idx > 0x3FFF) {
                if (!range_warned) {
                    fprintf(stderr, "%s: warning — tile index %u exceeds 14-bit"
                                    " cap, wrapping mod 16384\n", path, tile_idx);
                    range_warned = 1;
                }
                tile_idx &= 0x3FFF;
            }
            cell = (uint16_t)(tile_idx |
                              (hflip ? 0x8000u : 0u) |
                              (vflip ? 0x4000u : 0u));
        }
        out->cells[filled++] = cell;
    }
    free(src);

    if (filled != W * H) {
        fprintf(stderr, "%s: only %d/%d cells parsed — file may be truncated\n",
                path, filled, W * H);
        free(out->cells);
        out->cells = NULL;
        return 1;
    }
    return 0;
}

/* Write a tilemap to a C header. Format mirrors write_header for images
 * but emits uint16_t cells. */
static int write_tilemap_header(const char *path, const tilemap_t *tm, const char *prefix) {
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "open '%s' for write failed\n", path); return 1; }
    fprintf(f, "/* Generated by 2dpak --tilemap. DO NOT EDIT. */\n"
               "#ifndef _%s_TILEMAP_H\n#define _%s_TILEMAP_H\n\n"
               "#include <stdint.h>\n\n"
               "#define %s_W %d\n#define %s_H %d\n\n",
               prefix, prefix, prefix, tm->w, prefix, tm->h);
    fprintf(f, "static const uint16_t %s_cells[%s_W * %s_H] = {\n    ", prefix, prefix, prefix);
    for (int i = 0; i < tm->w * tm->h; ++i) {
        fprintf(f, "0x%04Xu,%s", tm->cells[i], ((i & 7) == 7) ? "\n    " : " ");
    }
    fprintf(f, "\n};\n\n#endif\n");
    fclose(f);
    return 0;
}

/* Write a tilemap to ROM blob. Format:
 *   magic 'TMAP' | version u16 | w u16 | h u16 | cells (w*h u16 LE) */
static int write_tilemap_rom(const char *path, const tilemap_t *tm) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "open '%s' for write failed\n", path); return 1; }
    uint8_t hdr[10];
    hdr[0]='T'; hdr[1]='M'; hdr[2]='A'; hdr[3]='P';
    hdr[4]=1; hdr[5]=0;
    hdr[6] = (uint8_t)(tm->w & 0xFF); hdr[7] = (uint8_t)((tm->w >> 8) & 0xFF);
    hdr[8] = (uint8_t)(tm->h & 0xFF); hdr[9] = (uint8_t)((tm->h >> 8) & 0xFF);
    fwrite(hdr, 1, 10, f);
    for (int i = 0; i < tm->w * tm->h; ++i) {
        uint8_t le[2] = { (uint8_t)(tm->cells[i] & 0xFF), (uint8_t)(tm->cells[i] >> 8) };
        fwrite(le, 1, 2, f);
    }
    fclose(f);
    return 0;
}

/* ---------- ROM-blob writer --------------------------------------------- */

/* Binary asset blob layout (little-endian throughout — i386-elf cart ABI):
 *
 *   offset  size     field
 *   0       4        magic '2DPK'
 *   4       2        version (currently 1)
 *   6       2        w
 *   8       2        h
 *   10      2        pal_count
 *   12      pal_n*4  palette (u32 0x00RRGGBB)
 *   ...     w*h      pixel data (u8)
 *
 * The cart parses via sdk/include/cron_2dpak.h (see cron_2dpak_parse_image).
 * Use this when the asset would bloat the .c — typically once you cross
 * a couple hundred KB of pixels. The header-output mode (default) is
 * fine for everything smaller. */
static int write_rom(const char *path, const bmp8_t *bmp, int max_pal) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "open '%s' for write failed\n", path); return 1; }
    int pal_n = max_pal > bmp->pal_count ? max_pal : bmp->pal_count;
    if (pal_n > 256) pal_n = 256;
    /* Header */
    uint8_t hdr[12];
    hdr[0] = '2'; hdr[1] = 'D'; hdr[2] = 'P'; hdr[3] = 'K';
    hdr[4] = 1;   hdr[5] = 0;   /* version */
    hdr[6] = (uint8_t)(bmp->w & 0xFF); hdr[7] = (uint8_t)((bmp->w >> 8) & 0xFF);
    hdr[8] = (uint8_t)(bmp->h & 0xFF); hdr[9] = (uint8_t)((bmp->h >> 8) & 0xFF);
    hdr[10] = (uint8_t)(pal_n & 0xFF); hdr[11] = (uint8_t)((pal_n >> 8) & 0xFF);
    fwrite(hdr, 1, 12, f);
    /* Palette */
    for (int i = 0; i < pal_n; ++i) {
        uint32_t c = i < bmp->pal_count ? bmp->palette[i] : 0u;
        uint8_t le[4] = { (uint8_t)(c & 0xFF), (uint8_t)((c >> 8) & 0xFF),
                          (uint8_t)((c >> 16) & 0xFF), (uint8_t)((c >> 24) & 0xFF) };
        fwrite(le, 1, 4, f);
    }
    /* Pixels — verbatim. */
    fwrite(bmp->pix, 1, (size_t)bmp->w * (size_t)bmp->h, f);
    fclose(f);
    return 0;
}

/* ---------- C header writer --------------------------------------------- */

static int write_header(const char *path, const bmp8_t *bmp,
                        const char *prefix, int max_pal) {
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "open '%s' for write failed\n", path); return 1; }

    /* Sanity: ensure no pixel index exceeds the chosen palette span — a
     * file with col 200 but max_pal=32 would render garbage cart-side. */
    int max_used = 0;
    for (int i = 0; i < bmp->w * bmp->h; ++i)
        if (bmp->pix[i] > max_used) max_used = bmp->pix[i];
    if (max_used >= max_pal) {
        fprintf(stderr,
                "warning: pixel index %d used but --max-pal=%d; cart will "
                "see colours wrap mod %d\n",
                max_used, max_pal, max_pal);
    }

    fprintf(f, "/* Generated by 2dpak. DO NOT EDIT. */\n"
               "#ifndef _%s_DATA_H\n#define _%s_DATA_H\n\n"
               "#include <stdint.h>\n\n"
               "#define %s_W %d\n#define %s_H %d\n"
               "#define %s_PAL_COUNT %d\n\n",
               prefix, prefix, prefix, bmp->w, prefix, bmp->h, prefix, max_pal);

    /* Palette */
    fprintf(f, "static const uint32_t %s_pal[%s_PAL_COUNT] = {\n    ", prefix, prefix);
    for (int i = 0; i < max_pal; ++i) {
        fprintf(f, "0x%06Xu,%s",
                i < bmp->pal_count ? bmp->palette[i] : 0u,
                ((i & 7) == 7) ? "\n    " : " ");
    }
    fprintf(f, "\n};\n\n");

    /* Pixels */
    fprintf(f, "static const uint8_t %s_pix[%s_W * %s_H] = {\n    ", prefix, prefix, prefix);
    int n = bmp->w * bmp->h;
    for (int i = 0; i < n; ++i) {
        fprintf(f, "%3u,%s", bmp->pix[i], ((i & 15) == 15) ? "\n    " : "");
    }
    fprintf(f, "\n};\n\n#endif\n");
    fclose(f);
    return 0;
}

/* ---------- self-test BMP writer ---------------------------------------- */

/* Write a known-good 32x16 BMP-8 (4 tiles 8x16, 4 distinct palette cols)
 * so the round-trip can be tested without an external paint tool. */
static int write_selftest(const char *path) {
    static const uint32_t pal[] = {
        0x000000,  /* 0: black (the colkey by convention) */
        0xFF0000,  /* 1: red */
        0x00FF00,  /* 2: green */
        0x0000FF,  /* 3: blue */
    };
    int W = 32, H = 16;
    int pal_n = 4;
    int row_bytes = ((W + 3) / 4) * 4;
    bmp_file_hdr_t fh = {0};
    bmp_info_hdr_t ih = {0};
    fh.bfType    = 0x4D42;
    fh.bfOffBits = (uint32_t)(sizeof fh + sizeof ih + 4u * (uint32_t)pal_n);
    fh.bfSize    = fh.bfOffBits + (uint32_t)(row_bytes * H);
    ih.biSize    = sizeof ih;
    ih.biWidth   = W;
    ih.biHeight  = H;            /* bottom-up: tests the flip path */
    ih.biPlanes  = 1;
    ih.biBitCount    = 8;
    ih.biCompression = 0;
    ih.biClrUsed     = (uint32_t)pal_n;
    ih.biClrImportant= (uint32_t)pal_n;

    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "open '%s' for write failed\n", path); return 1; }
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&ih, sizeof ih, 1, f);
    for (int i = 0; i < pal_n; ++i) {
        uint8_t bgra[4];
        bgra[0] = pal[i] & 0xFF;
        bgra[1] = (pal[i] >> 8) & 0xFF;
        bgra[2] = (pal[i] >> 16) & 0xFF;
        bgra[3] = 0;
        fwrite(bgra, 4, 1, f);
    }
    /* Pixel rows. Each row of 32 px = four 8-wide bands of colours 0..3. */
    uint8_t row[64];               /* row_bytes <= 32 for W=32; oversize ok */
    memset(row, 0, sizeof row);
    for (int x = 0; x < W; ++x) row[x] = (uint8_t)((x >> 3) & 3);
    for (int y = 0; y < H; ++y) fwrite(row, (size_t)row_bytes, 1, f);
    fclose(f);
    return 0;
}

/* ---------- CLI --------------------------------------------------------- */

static void usage(void) {
    fprintf(stderr,
        "2dpak — Cronopio 2D asset packer.\n\n"
        "  IMAGE:    2dpak <input.{png,bmp}> <output.h> [--prefix=NAME] [--max-pal=N] [--rom]\n"
        "  TILEMAP:  2dpak --tilemap <input.tmx> <output.h> [--prefix=NAME] [--rom]\n"
        "  SELFTEST: 2dpak --selftest <output.bmp>\n\n"
        "Image inputs: indexed PNG-8 (PLTE preserved) or BMP-8 (BI_RGB).\n"
        "  Truecolour PNG gets median-cut quantized to --max-pal colours.\n"
        "Tilemap input: Tiled (.tmx) with 'Tile Layer Format: CSV'.\n"
        "  Output cells use the Cronopio u16 layout (HFLIP=bit15, VFLIP=bit14,\n"
        "  tile index = bits 13..0, 0xFFFF = empty).\n"
        "With --rom the output is a binary blob (cron_rom-loadable via the\n"
        "sdk/include/cron_2dpak.h helper) instead of a C header.\n"
        "Default --prefix derives from output basename; default --max-pal=32.\n");
}

int main(int argc, char **argv) {
    if (argc < 3) { usage(); return 2; }

    if (strcmp(argv[1], "--selftest") == 0) {
        return write_selftest(argv[2]);
    }

    /* --tilemap takes precedence — shifts the in/out args by one. */
    int tmx_mode = 0;
    int argbase = 1;
    if (strcmp(argv[1], "--tilemap") == 0) {
        if (argc < 4) { usage(); return 2; }
        tmx_mode = 1;
        argbase = 2;
    }
    const char *in = argv[argbase], *out = argv[argbase + 1];
    const char *prefix = NULL;
    int max_pal = 32;
    int rom_mode = 0;
    for (int i = argbase + 2; i < argc; ++i) {
        if (strncmp(argv[i], "--prefix=", 9) == 0)      prefix = argv[i] + 9;
        else if (strncmp(argv[i], "--max-pal=", 10) == 0) max_pal = atoi(argv[i] + 10);
        else if (strcmp(argv[i], "--rom") == 0)         rom_mode = 1;
        else { usage(); return 2; }
    }

    /* Prefix only matters for the C-header path; ROM mode skips it. */
    char prefix_buf[64];
    if (!prefix && !rom_mode) {
        const char *p = out;
        for (const char *q = out; *q; ++q) if (*q == '/' || *q == '\\') p = q + 1;
        size_t out_n = 0;
        /* C identifiers can't start with a digit — prepend an underscore
         * if needed so files like "8tiles.png" don't produce "8TILES"
         * prefixes that fail to compile. */
        if (p[0] >= '0' && p[0] <= '9') prefix_buf[out_n++] = '_';
        for (size_t i = 0; p[i] && p[i] != '.' && out_n + 1 < sizeof prefix_buf; ++i) {
            char c = p[i];
            /* C identifier-safe: alnum -> upper; everything else -> _ */
            if      (c >= 'a' && c <= 'z') prefix_buf[out_n] = (char)(c - 'a' + 'A');
            else if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) prefix_buf[out_n] = c;
            else                                                       prefix_buf[out_n] = '_';
            ++out_n;
        }
        prefix_buf[out_n] = '\0';
        if (out_n == 0 || (out_n == 1 && prefix_buf[0] == '_')) {
            fprintf(stderr, "empty derived prefix; use --prefix=NAME\n");
            return 1;
        }
        prefix = prefix_buf;
    }
    if (max_pal < 1 || max_pal > 256) { fprintf(stderr, "--max-pal out of range 1..256\n"); return 1; }

    if (tmx_mode) {
        tilemap_t tm = {0};
        if (read_tmx(in, &tm) != 0) return 1;
        fprintf(stderr, "%s: %dx%d cells\n", in, tm.w, tm.h);
        int rc = rom_mode ? write_tilemap_rom(out, &tm)
                          : write_tilemap_header(out, &tm, prefix);
        free(tm.cells);
        if (rc == 0) {
            if (rom_mode) fprintf(stderr, "wrote %s (tilemap ROM blob)\n", out);
            else          fprintf(stderr, "wrote %s (tilemap, prefix %s_)\n", out, prefix);
        }
        return rc;
    }

    /* Sniff magic to pick the parser. PNG = 89 50 4E 47, BMP = 42 4D. */
    bmp8_t bmp = {0};
    {
        FILE *fh = fopen(in, "rb");
        if (!fh) { fprintf(stderr, "open '%s' failed\n", in); return 1; }
        uint8_t magic[8] = {0};
        size_t got = fread(magic, 1, 8, fh);
        fclose(fh);
        if (got >= 8 && is_png(magic, got))           { if (read_png(in, &bmp, max_pal) != 0) return 1; }
        else if (got >= 2 && magic[0]=='B' && magic[1]=='M') { if (read_bmp8(in, &bmp) != 0) return 1; }
        else { fprintf(stderr, "%s: unrecognised format (magic %02X %02X) — need indexed PNG or BMP-8\n",
                       in, magic[0], magic[1]); return 1; }
    }
    fprintf(stderr, "%s: %dx%d, %d palette entries\n", in, bmp.w, bmp.h, bmp.pal_count);

    int rc = rom_mode ? write_rom(out, &bmp, max_pal)
                      : write_header(out, &bmp, prefix, max_pal);
    free(bmp.pix);
    if (rc == 0) {
        if (rom_mode) fprintf(stderr, "wrote %s (ROM blob, max_pal %d)\n", out, max_pal);
        else          fprintf(stderr, "wrote %s (prefix %s_, max_pal %d)\n", out, prefix, max_pal);
    }
    return rc;
}
