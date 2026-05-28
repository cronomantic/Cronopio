/*
 *  cron_collide.h — Cart-side 2D collision helpers for Cronopio.
 *
 *  PURE CART CODE. No syscalls; nothing in here touches the host. Cronopio's
 *  Model D leaves sprite-list management to the cart (no host-side sprite
 *  manager), so collision queries are pure logic over cart-owned data and
 *  don't need to round-trip through the VM boundary.
 *
 *  Stage 1 ships axis-aligned bounding-box (AABB) checks — what 95 % of
 *  arcade games need. Pixel-perfect collision over sprite source rasters
 *  is sketched as a TODO at the bottom; add it when a port actually needs
 *  it (cart provides the sprite buffer + colkey; helper walks the overlap
 *  rect and reports any non-colkey pair).
 *
 *  Released under the same license as the rest of the Cronopio SDK.
 */

#ifndef _CVM_CRON_COLLIDE_H
#define _CVM_CRON_COLLIDE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A rect — w and h are sizes (not maxs), so the rect covers
 * [x, x+w) × [y, y+h). All ints; the cart can use this for sprite
 * bounds, collision walls, hot regions, whatever. */
typedef struct {
    int32_t x, y, w, h;
} cron_rect_t;

/* AABB overlap by raw args. Returns 1 iff the two rectangles share at
 * least one pixel. Half-open intervals: a rect at (0,0,16,16) does NOT
 * touch a rect at (16,0,16,16) — they're adjacent. Negative w/h
 * silently return 0 (degenerate input shouldn't pretend to hit). */
static inline int cron_aabb_overlap(int ax, int ay, int aw, int ah,
                                    int bx, int by, int bw, int bh) {
    if (aw <= 0 || ah <= 0 || bw <= 0 || bh <= 0) return 0;
    if (ax + aw <= bx || bx + bw <= ax) return 0;
    if (ay + ah <= by || by + bh <= ay) return 0;
    return 1;
}

/* Same, taking rect structs. Equivalent — just nicer at call sites
 * when you already have rects. */
static inline int cron_aabb_overlap_r(const cron_rect_t *a, const cron_rect_t *b) {
    return cron_aabb_overlap(a->x, a->y, a->w, a->h,
                             b->x, b->y, b->w, b->h);
}

/* Point-in-rect. Half-open intervals as above. */
static inline int cron_aabb_contains(int px, int py,
                                     int rx, int ry, int rw, int rh) {
    return (px >= rx && px < rx + rw && py >= ry && py < ry + rh);
}

/* Compute the intersection of two rects into *out. Returns 1 if there is
 * any overlap (and *out is valid); 0 otherwise (*out left untouched).
 * Useful when you need to draw / scan the overlap region itself
 * (e.g. as the first cheap pass before a pixel-perfect check). */
static inline int cron_aabb_intersect(const cron_rect_t *a,
                                      const cron_rect_t *b,
                                      cron_rect_t *out) {
    if (!cron_aabb_overlap_r(a, b)) return 0;
    int x0 = a->x > b->x ? a->x : b->x;
    int y0 = a->y > b->y ? a->y : b->y;
    int x1 = (a->x + a->w) < (b->x + b->w) ? (a->x + a->w) : (b->x + b->w);
    int y1 = (a->y + a->h) < (b->y + b->h) ? (a->y + a->h) : (b->y + b->h);
    out->x = x0; out->y = y0;
    out->w = x1 - x0; out->h = y1 - y0;
    return 1;
}

/* Circle overlap by raw args. Cheaper than the AABB if you already have
 * squared radii at hand and your hit boxes are round (asteroids, balls,
 * bullets, planet sprites). Returns 1 iff the two circles overlap (or
 * touch — closed condition; switch to `<` if you want strict). */
static inline int cron_circ_overlap(int ax, int ay, int ar,
                                    int bx, int by, int br) {
    int dx = ax - bx, dy = ay - by;
    int rsum = ar + br;
    return (dx * dx + dy * dy) <= (rsum * rsum);
}

/* TODO when a port actually needs it: pixel-perfect collision. Sketch:
 *
 *   int cron_collide_pixel(const uint8_t *a_pix, int aw, int ah,
 *                          int ax, int ay, int a_colkey,
 *                          const uint8_t *b_pix, int bw, int bh,
 *                          int bx, int by, int b_colkey);
 *
 * Computes the AABB intersection; walks the overlap rect; returns 1 the
 * first time both samples are non-colkey. ~O(overlap_area). The cart
 * owns the pixel buffers and supplies them — no host state needed.
 *
 * Same shape but for rotated/scaled sprites would need access to the
 * blit math (rotate+scale inverse), or the cart can rasterise the
 * sprite into a temp mask buffer first via cron_blt_ex and read the FB.
 * Defer until we hit a real use case. */

#ifdef __cplusplus
}
#endif

#endif /* _CVM_CRON_COLLIDE_H */
