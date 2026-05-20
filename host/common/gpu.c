/* Drawing primitives — convenience helpers that the syscall layer calls.
 *
 * All primitives write into the framebuffer region of the cart heap at
 * the offset resolved by the loader (passed in as fb_offset). Cart code
 * can also touch those bytes directly through the memory-mapped region
 * pointer it gets from cvm_sys_get_region("fb"). */

#include "console.h"

#include <stdint.h>

void cron_gpu_cls(uint8_t* heap, uint32_t fb_offset, int color) {
    uint8_t  c  = (uint8_t)(color & 0x1F);
    uint8_t* fb = heap + fb_offset;
    for (int i = 0; i < CRONOPIO_FB_BYTES; ++i) fb[i] = c;
}

void cron_gpu_pset(uint8_t* heap, uint32_t fb_offset, int x, int y, int color) {
    if ((unsigned)x >= CRONOPIO_SCREEN_W || (unsigned)y >= CRONOPIO_SCREEN_H) return;
    (heap + fb_offset)[y * CRONOPIO_SCREEN_W + x] = (uint8_t)(color & 0x1F);
}

void cron_gpu_rect(uint8_t* heap, uint32_t fb_offset, int x, int y, int w, int h, int color) {
    if (w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w; if (x1 > CRONOPIO_SCREEN_W) x1 = CRONOPIO_SCREEN_W;
    int y1 = y + h; if (y1 > CRONOPIO_SCREEN_H) y1 = CRONOPIO_SCREEN_H;
    uint8_t  c  = (uint8_t)(color & 0x1F);
    uint8_t* fb = heap + fb_offset;
    for (int yy = y0; yy < y1; ++yy) {
        uint8_t* row = fb + yy * CRONOPIO_SCREEN_W;
        for (int xx = x0; xx < x1; ++xx) row[xx] = c;
    }
}

void cron_gpu_line(uint8_t* heap, uint32_t fb_offset, int x0, int y0, int x1, int y1, int color) {
    int dx =  (x1 > x0 ? x1 - x0 : x0 - x1);
    int dy = -(y1 > y0 ? y1 - y0 : y0 - y1);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        cron_gpu_pset(heap, fb_offset, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void cron_gpu_blit(uint8_t* heap, uint32_t fb_offset,
                   const uint8_t* src, int sw, int sh, int dx, int dy) {
    if (sw <= 0 || sh <= 0) return;
    int x0 = dx < 0 ? -dx : 0;
    int y0 = dy < 0 ? -dy : 0;
    int x1 = sw; if (dx + x1 > CRONOPIO_SCREEN_W) x1 = CRONOPIO_SCREEN_W - dx;
    int y1 = sh; if (dy + y1 > CRONOPIO_SCREEN_H) y1 = CRONOPIO_SCREEN_H - dy;
    uint8_t* fb = heap + fb_offset;
    for (int yy = y0; yy < y1; ++yy) {
        const uint8_t* srow = src + yy * sw;
        uint8_t*       drow = fb + (dy + yy) * CRONOPIO_SCREEN_W + dx;
        for (int xx = x0; xx < x1; ++xx) drow[xx] = (uint8_t)(srow[xx] & 0x1F);
    }
}

/* Tiny built-in 8x8 font: only ASCII 0x20..0x5F (uppercase + digits + punct).
 * Glyph bits are placeholder — a real font ROM lives in v1. For now every
 * non-blank character renders as a 7x7 filled square so text shows up as a
 * legible row of blocks; whitespace renders blank. */
void cron_gpu_text(uint8_t* heap, uint32_t fb_offset,
                   const char* s, int len, int x, int y, int color) {
    for (int i = 0; i < len; ++i) {
        char ch = s[i];
        if (ch > ' ' && ch < 0x7F) {
            cron_gpu_rect(heap, fb_offset, x + i*8, y, 7, 7, color);
        }
    }
}
