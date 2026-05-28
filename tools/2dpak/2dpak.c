/*
 *  2dpak — convert an 8bpp (indexed-palette) BMP into a Cronopio cart C
 *  header. Input: a 32-colour-or-fewer BMP-8 (most paint tools export this
 *  via "Indexed mode" + "Save as BMP"; or via ImageMagick:
 *
 *      magick input.png BMP3:out.bmp
 *
 *  Output: a C header with the palette (uint32_t 0x00RRGGBB[32]), the
 *  pixel data (uint8_t[w*h]), and dimension macros. The cart includes
 *  it and registers via cron_image / cron_palette_set.
 *
 *  WHY BMP-8 (not PNG): PNG decoding requires DEFLATE + ~700 lines of
 *  careful C or a 5K-line vendored decoder. BMP-8 is a 50-line parser
 *  that any artist tool exports natively. The artist adds one
 *  ImageMagick step to their pipeline; the toolchain stays tiny.
 *
 *  Limits: the BMP must be uncompressed (BI_RGB), 8 bits per pixel, with
 *  a palette of <= 256 entries (we use the first 32; the rest are ignored).
 *  Top-down or bottom-up rows both supported. Real-world content from
 *  GIMP/Krita/Aseprite/ImageMagick comes out exactly this shape.
 *
 *  Usage:
 *      2dpak <input.bmp> <output.h> [--prefix=NAME] [--max-pal=32]
 *      2dpak --selftest <output.bmp>    # writes a known-good 32x16 test BMP
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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

typedef struct {
    int      w, h;
    uint32_t palette[256];    /* 0x00RRGGBB */
    int      pal_count;
    uint8_t *pix;             /* w*h, row-major, top-down */
} bmp8_t;

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
        "2dpak — Cronopio 2D asset packer (BMP-8 -> C header).\n\n"
        "  2dpak <input.bmp> <output.h> [--prefix=NAME] [--max-pal=N]\n"
        "  2dpak --selftest <output.bmp>\n\n"
        "Input requirements: BMP-8 (indexed), uncompressed (BI_RGB).\n"
        "Convert other formats with:  magick input.png BMP3:out.bmp\n"
        "Default --prefix derives from output basename; default --max-pal=32.\n");
}

int main(int argc, char **argv) {
    if (argc < 3) { usage(); return 2; }

    if (strcmp(argv[1], "--selftest") == 0) {
        return write_selftest(argv[2]);
    }

    const char *in = argv[1], *out = argv[2];
    const char *prefix = NULL;
    int max_pal = 32;
    for (int i = 3; i < argc; ++i) {
        if (strncmp(argv[i], "--prefix=", 9) == 0) prefix = argv[i] + 9;
        else if (strncmp(argv[i], "--max-pal=", 10) == 0) max_pal = atoi(argv[i] + 10);
        else { usage(); return 2; }
    }

    /* Default prefix from output basename without extension. */
    char prefix_buf[64];
    if (!prefix) {
        const char *p = out;
        for (const char *q = out; *q; ++q) if (*q == '/' || *q == '\\') p = q + 1;
        size_t n = 0;
        while (p[n] && p[n] != '.' && n + 1 < sizeof prefix_buf) {
            char c = p[n];
            /* C identifier-safe: alnum -> upper; everything else -> _ */
            if      (c >= 'a' && c <= 'z') prefix_buf[n] = (char)(c - 'a' + 'A');
            else if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) prefix_buf[n] = c;
            else                                                       prefix_buf[n] = '_';
            ++n;
        }
        prefix_buf[n] = '\0';
        if (n == 0) { fprintf(stderr, "empty derived prefix; use --prefix=NAME\n"); return 1; }
        prefix = prefix_buf;
    }
    if (max_pal < 1 || max_pal > 256) { fprintf(stderr, "--max-pal out of range 1..256\n"); return 1; }

    bmp8_t bmp = {0};
    if (read_bmp8(in, &bmp) != 0) return 1;
    fprintf(stderr, "%s: %dx%d, %d palette entries\n", in, bmp.w, bmp.h, bmp.pal_count);

    int rc = write_header(out, &bmp, prefix, max_pal);
    free(bmp.pix);
    if (rc == 0) fprintf(stderr, "wrote %s (prefix %s_, max_pal %d)\n", out, prefix, max_pal);
    return rc;
}
