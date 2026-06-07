/* Drawing primitives + sprite/tilemap blitting — the convenience layer the
 * syscall dispatch calls. Everything writes into the framebuffer region of
 * the cart heap at c->fb_offset.
 *
 * All pixel writes route through put_px(), which applies the global draw
 * state: camera offset (subtracted from world coords), clip rect, and the
 * draw-time palette remap. cls() is the one exception — it clears the whole
 * framebuffer ignoring the draw state. Image/tilemap "banks" are thin
 * handles over bitmaps the cart owns (in RAM or ROM); blt/bltm read straight
 * from there. */

#include "console.h"

#include <stdint.h>
#include <math.h>

/* ---- state ------------------------------------------------------------- */

void cron_gpu_reset_state(cronopio_console_t* c) {
    c->draw.clip_x0 = 0;
    c->draw.clip_y0 = 0;
    c->draw.clip_x1 = CRONOPIO_SCREEN_W;
    c->draw.clip_y1 = CRONOPIO_SCREEN_H;
    c->draw.cam_x = 0;
    c->draw.cam_y = 0;
    for (int i = 0; i < 256; ++i) c->draw.pal_map[i] = (uint8_t)i;
}

void cron_gpu_clip(cronopio_console_t* c, int x, int y, int w, int h) {
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > CRONOPIO_SCREEN_W) x1 = CRONOPIO_SCREEN_W;
    if (y1 > CRONOPIO_SCREEN_H) y1 = CRONOPIO_SCREEN_H;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    c->draw.clip_x0 = x0; c->draw.clip_y0 = y0;
    c->draw.clip_x1 = x1; c->draw.clip_y1 = y1;
}
void cron_gpu_clip_reset(cronopio_console_t* c) {
    c->draw.clip_x0 = 0; c->draw.clip_y0 = 0;
    c->draw.clip_x1 = CRONOPIO_SCREEN_W; c->draw.clip_y1 = CRONOPIO_SCREEN_H;
}
void cron_gpu_camera(cronopio_console_t* c, int x, int y) {
    c->draw.cam_x = x; c->draw.cam_y = y;
}
void cron_gpu_pal(cronopio_console_t* c, int c0, int c1) {
    c->draw.pal_map[c0 & 0xFF] = (uint8_t)(c1 & 0xFF);
}
void cron_gpu_pal_reset(cronopio_console_t* c) {
    for (int i = 0; i < 256; ++i) c->draw.pal_map[i] = (uint8_t)i;
}

/* ---- the one place pixels are written --------------------------------- */

static inline uint8_t* fb_of(cronopio_console_t* c, uint8_t* heap) {
    return heap + c->fb_offset;
}

/* World-space write: apply camera, clip, palette remap. col is a raw index. */
static inline void put_px(cronopio_console_t* c, uint8_t* fb, int xw, int yw, int col) {
    int x = xw - c->draw.cam_x;
    int y = yw - c->draw.cam_y;
    if (x < c->draw.clip_x0 || x >= c->draw.clip_x1) return;
    if (y < c->draw.clip_y0 || y >= c->draw.clip_y1) return;
    int idx = y * CRONOPIO_SCREEN_W + x;
    uint8_t s = c->draw.pal_map[col & 0xFF];
    /* Blend: when slot > 0 and bound, output = table[src][dst]. The table
     * is 256*256 = 64 KB; cart builds it once per blend mode and registers
     * it via cron_blend_table. Default slot 0 is opaque (current behaviour);
     * the branch is well-predicted in non-blending code paths. */
    int bs = c->blend_active;
    if (bs > 0 && bs < CRONOPIO_BLEND_SLOTS && c->blend_tables[bs].used) {
        const uint8_t *heap = fb - c->fb_offset;
        const uint8_t *tbl  = heap + c->blend_tables[bs].offset;
        fb[idx] = tbl[(unsigned)s * 256u + fb[idx]];
    } else {
        fb[idx] = s;
    }
}

/* ---- basic primitives -------------------------------------------------- */

void cron_gpu_cls(cronopio_console_t* c, uint8_t* heap, int color) {
    uint8_t  v  = (uint8_t)(color & 0xFF);
    uint8_t* fb = fb_of(c, heap);
    for (int i = 0; i < CRONOPIO_FB_BYTES; ++i) fb[i] = v;
}

void cron_gpu_pset(cronopio_console_t* c, uint8_t* heap, int x, int y, int color) {
    put_px(c, fb_of(c, heap), x, y, color);
}

int cron_gpu_pget(cronopio_console_t* c, uint8_t* heap, int x, int y) {
    int sx = x - c->draw.cam_x, sy = y - c->draw.cam_y;
    if ((unsigned)sx >= CRONOPIO_SCREEN_W || (unsigned)sy >= CRONOPIO_SCREEN_H) return 0;
    return fb_of(c, heap)[sy * CRONOPIO_SCREEN_W + sx];
}

void cron_gpu_rect(cronopio_console_t* c, uint8_t* heap, int x, int y, int w, int h, int color) {
    if (w <= 0 || h <= 0) return;
    uint8_t* fb = fb_of(c, heap);
    for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx)
            put_px(c, fb, x + xx, y + yy, color);
}

void cron_gpu_rectb(cronopio_console_t* c, uint8_t* heap, int x, int y, int w, int h, int color) {
    if (w <= 0 || h <= 0) return;
    uint8_t* fb = fb_of(c, heap);
    for (int xx = 0; xx < w; ++xx) { put_px(c, fb, x+xx, y, color); put_px(c, fb, x+xx, y+h-1, color); }
    for (int yy = 0; yy < h; ++yy) { put_px(c, fb, x, y+yy, color); put_px(c, fb, x+w-1, y+yy, color); }
}

void cron_gpu_line(cronopio_console_t* c, uint8_t* heap, int x0, int y0, int x1, int y1, int color) {
    uint8_t* fb = fb_of(c, heap);
    int dx =  (x1 > x0 ? x1 - x0 : x0 - x1);
    int dy = -(y1 > y0 ? y1 - y0 : y0 - y1);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        put_px(c, fb, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* ---- circles / ellipses ----------------------------------------------- */

static void hspan(cronopio_console_t* c, uint8_t* fb, int xa, int xb, int y, int color) {
    if (xa > xb) { int t = xa; xa = xb; xb = t; }
    for (int x = xa; x <= xb; ++x) put_px(c, fb, x, y, color);
}

void cron_gpu_circ(cronopio_console_t* c, uint8_t* heap, int cx, int cy, int r, int color) {
    if (r < 0) return;
    uint8_t* fb = fb_of(c, heap);
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        hspan(c, fb, cx - x, cx + x, cy + y, color);
        hspan(c, fb, cx - x, cx + x, cy - y, color);
        hspan(c, fb, cx - y, cx + y, cy + x, color);
        hspan(c, fb, cx - y, cx + y, cy - x, color);
        y++;
        if (err < 0) err += 2*y + 1;
        else { x--; err += 2*(y - x) + 1; }
    }
}

void cron_gpu_circb(cronopio_console_t* c, uint8_t* heap, int cx, int cy, int r, int color) {
    if (r < 0) return;
    uint8_t* fb = fb_of(c, heap);
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        put_px(c, fb, cx+x, cy+y, color); put_px(c, fb, cx-x, cy+y, color);
        put_px(c, fb, cx+x, cy-y, color); put_px(c, fb, cx-x, cy-y, color);
        put_px(c, fb, cx+y, cy+x, color); put_px(c, fb, cx-y, cy+x, color);
        put_px(c, fb, cx+y, cy-x, color); put_px(c, fb, cx-y, cy-x, color);
        y++;
        if (err < 0) err += 2*y + 1;
        else { x--; err += 2*(y - x) + 1; }
    }
}

/* Ellipse inscribed in the (x,y,w,h) box. */
static void elli_impl(cronopio_console_t* c, uint8_t* fb, int x, int y, int w, int h, int color, int filled) {
    if (w <= 0 || h <= 0) return;
    /* Work in a centred coordinate frame with half-axes a,b. */
    int a = w - 1, b = h - 1;            /* full diameters in pixels - 1 */
    int cx2 = 2*x + a, cy2 = 2*y + b;    /* 2*center to stay integer for odd/even */
    /* Midpoint ellipse over the doubled grid. */
    long aa = (long)a*a, bb = (long)b*b;
    for (int dy = -b; dy <= b; dy += 2) {
        /* solve for dx: (dx/a)^2 + (dy/b)^2 <= 1 */
        long rhs = aa - (aa * (long)dy * dy) / (bb ? bb : 1);
        if (rhs < 0) continue;
        /* dx max ~ a*sqrt(rhs)/a ... integer sqrt */
        long lim = 0; while ((lim+1)*(lim+1) <= rhs) lim++;
        int cyp = (cy2 + dy) / 2;
        if (filled) {
            hspan(c, fb, (cx2 - (int)lim)/2, (cx2 + (int)lim)/2, cyp, color);
        } else {
            put_px(c, fb, (cx2 - (int)lim)/2, cyp, color);
            put_px(c, fb, (cx2 + (int)lim)/2, cyp, color);
        }
    }
}
void cron_gpu_elli (cronopio_console_t* c, uint8_t* heap, int x, int y, int w, int h, int color) {
    elli_impl(c, fb_of(c, heap), x, y, w, h, color, 1);
}
void cron_gpu_ellib(cronopio_console_t* c, uint8_t* heap, int x, int y, int w, int h, int color) {
    elli_impl(c, fb_of(c, heap), x, y, w, h, color, 0);
}

/* ---- triangles --------------------------------------------------------- */

void cron_gpu_trib(cronopio_console_t* c, uint8_t* heap, int x0,int y0,int x1,int y1,int x2,int y2,int color) {
    cron_gpu_line(c, heap, x0, y0, x1, y1, color);
    cron_gpu_line(c, heap, x1, y1, x2, y2, color);
    cron_gpu_line(c, heap, x2, y2, x0, y0, color);
}

void cron_gpu_tri(cronopio_console_t* c, uint8_t* heap, int x0,int y0,int x1,int y1,int x2,int y2,int color) {
    uint8_t* fb = fb_of(c, heap);
    /* sort by y: (x0,y0) top .. (x2,y2) bottom */
    if (y0 > y1) { int t; t=x0;x0=x1;x1=t; t=y0;y0=y1;y1=t; }
    if (y0 > y2) { int t; t=x0;x0=x2;x2=t; t=y0;y0=y2;y2=t; }
    if (y1 > y2) { int t; t=x1;x1=x2;x2=t; t=y1;y1=y2;y2=t; }
    if (y2 == y0) { hspan(c, fb, (x0<x1?(x0<x2?x0:x2):(x1<x2?x1:x2)),
                                 (x0>x1?(x0>x2?x0:x2):(x1>x2?x1:x2)), y0, color); return; }
    for (int y = y0; y <= y2; ++y) {
        /* xa on edge 0->2 (long edge); xb on 0->1 then 1->2 */
        int xa = x0 + (int)((long)(x2 - x0) * (y - y0) / (y2 - y0));
        int xb;
        if (y < y1 && y1 != y0)
            xb = x0 + (int)((long)(x1 - x0) * (y - y0) / (y1 - y0));
        else if (y2 != y1)
            xb = x1 + (int)((long)(x2 - x1) * (y - y1) / (y2 - y1));
        else
            xb = x1;
        hspan(c, fb, xa, xb, y, color);
    }
}

/* ---- flood fill -------------------------------------------------------- */

/* 4-way scanline-ish flood fill in screen space, bounded by the clip rect.
 * Uses an explicit stack sized to the framebuffer to stay non-recursive. */
void cron_gpu_fill(cronopio_console_t* c, uint8_t* heap, int x, int y, int color) {
    uint8_t* fb = fb_of(c, heap);
    int sx = x - c->draw.cam_x, sy = y - c->draw.cam_y;
    if (sx < c->draw.clip_x0 || sx >= c->draw.clip_x1) return;
    if (sy < c->draw.clip_y0 || sy >= c->draw.clip_y1) return;
    uint8_t target = fb[sy * CRONOPIO_SCREEN_W + sx];
    uint8_t repl   = c->draw.pal_map[color & 0xFF];
    if (target == repl) return;

    static int32_t stack[CRONOPIO_FB_BYTES];   /* worst case: every pixel once */
    int sp = 0;
    stack[sp++] = sy * CRONOPIO_SCREEN_W + sx;
    while (sp > 0) {
        int32_t p = stack[--sp];
        int px = p % CRONOPIO_SCREEN_W, py = p / CRONOPIO_SCREEN_W;
        if (px < c->draw.clip_x0 || px >= c->draw.clip_x1) continue;
        if (py < c->draw.clip_y0 || py >= c->draw.clip_y1) continue;
        if (fb[p] != target) continue;
        fb[p] = repl;
        if (sp <= CRONOPIO_FB_BYTES - 4) {
            stack[sp++] = p - 1;
            stack[sp++] = p + 1;
            stack[sp++] = p - CRONOPIO_SCREEN_W;
            stack[sp++] = p + CRONOPIO_SCREEN_W;
        }
    }
}

/* ---- text (8x8 font) --------------------------------------------------- */

/* Glyph ROM lives in font8x8.h, shared with the desktop system-menu overlay. */
#include "font8x8.h"

void cron_gpu_text(cronopio_console_t* c, uint8_t* heap,
                   const char* s, int len, int x, int y, int color) {
    uint8_t* fb = fb_of(c, heap);
    for (int i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 0x20 || ch > 0x7F) continue;
        const uint8_t* glyph = font8x8[ch - 0x20];
        int gx = x + i * 8;
        for (int row = 0; row < 8; ++row) {
            uint8_t bits = glyph[row];
            if (!bits) continue;
            for (int col = 0; col < 8; ++col)
                if (bits & (1u << col)) put_px(c, fb, gx + col, y + row, color);
        }
    }
}

/* Raw blit of an already-extracted host-side bitmap (the legacy cron_blit,
 * syscall 0x026). No bank, no colorkey; honours the draw state. */
void cron_gpu_blit_raw(cronopio_console_t* c, uint8_t* heap,
                       const uint8_t* src, int sw, int sh, int dx, int dy) {
    if (sw <= 0 || sh <= 0) return;
    uint8_t* fb = fb_of(c, heap);
    for (int j = 0; j < sh; ++j)
        for (int i = 0; i < sw; ++i)
            put_px(c, fb, dx + i, dy + j, src[j * sw + i]);
}

/* Image bank blit with variable scale + flip flags. No rotation — for the
 * rotation case use cron_blt_ex. scale_q16 is Q16.16 (0x10000 = 1.0). The
 * sprite is scaled around its top-left corner (dx, dy), so a scaled sprite
 * keeps that corner anchored — predictable for HUD / sprite positioning.
 * Nearest-neighbour sampling with a per-axis fixed-point step so each
 * destination pixel maps to one source texel.
 *
 * Cheaper than cron_blt_ex when rotation isn't needed (no trig, no AABB
 * extents, single pass per dest pixel). Faster path for SNES-style
 * "approaching boss", icon zoom, sprite breathing animations, etc. */
void cron_gpu_blt_scale(cronopio_console_t* c, uint8_t* heap, int img,
                        int dx, int dy, int sx, int sy, int w, int h,
                        int colkey, int scale_q16, int flags) {
    if ((unsigned)img >= CRONOPIO_IMAGE_SLOTS || !c->images[img].used) return;
    if (w <= 0 || h <= 0 || scale_q16 <= 0) return;
    cron_image_bank_t* b = &c->images[img];
    const uint8_t* src = heap + b->offset;
    uint8_t* fb = fb_of(c, heap);

    /* Destination rect size. Truncated to integer pixels; very small scales
     * may yield 0 — silently skip rather than draw a degenerate row/col. */
    int dw = (int)((int64_t)w * scale_q16 >> 16);
    int dh = (int)((int64_t)h * scale_q16 >> 16);
    if (dw <= 0 || dh <= 0) return;

    /* Per-dest-pixel source step in Q16.16. (w<<16)/dw is constant across
     * the loop; precompute so we never integer-divide in the hot path. */
    int32_t u_step = (int32_t)(((int64_t)w << 16) / dw);
    int32_t v_step = (int32_t)(((int64_t)h << 16) / dh);

    int hflip = (flags & 1);
    int vflip = (flags & 2);
    int32_t v0 = vflip ? ((int32_t)(h - 1) << 16) : 0;
    int32_t dv = vflip ? -v_step : v_step;

    int32_t v = v0;
    for (int j = 0; j < dh; ++j, v += dv) {
        int srcy_off = (int)(v >> 16);
        if (srcy_off < 0 || srcy_off >= h) continue;
        int srcy = sy + srcy_off;
        if (srcy < 0 || srcy >= b->h) continue;
        const uint8_t* row = src + srcy * b->w;
        int32_t u0 = hflip ? ((int32_t)(w - 1) << 16) : 0;
        int32_t du = hflip ? -u_step : u_step;
        int32_t u = u0;
        for (int i = 0; i < dw; ++i, u += du) {
            int srcx_off = (int)(u >> 16);
            if (srcx_off < 0 || srcx_off >= w) continue;
            int srcx = sx + srcx_off;
            if (srcx < 0 || srcx >= b->w) continue;
            uint8_t s = row[srcx];
            if (colkey >= 0 && s == (uint8_t)colkey) continue;
            put_px(c, fb, dx + i, dy + j, s);
        }
    }
}

/* Image bank blit with horizontal/vertical flip flags. Cheap fast path for
 * sprites that need to face left or stand on their head — no rotation, no
 * scale, just NN sampling with optional axis mirroring. Matches cron_blt
 * argument-wise except for the extra `flags` (CRON_BLT_HFLIP / VFLIP). */
void cron_gpu_blt_flip(cronopio_console_t* c, uint8_t* heap, int img,
                       int dx, int dy, int sx, int sy, int w, int h,
                       int colkey, int flags) {
    if ((unsigned)img >= CRONOPIO_IMAGE_SLOTS || !c->images[img].used) return;
    if (w <= 0 || h <= 0) return;
    cron_image_bank_t* b = &c->images[img];
    const uint8_t* src = heap + b->offset;
    uint8_t* fb = fb_of(c, heap);
    int hflip = (flags & 1);
    int vflip = (flags & 2);
    for (int j = 0; j < h; ++j) {
        int sj = vflip ? (h - 1 - j) : j;
        int srcy = sy + sj;
        if (srcy < 0 || srcy >= b->h) continue;
        const uint8_t* row = src + srcy * b->w;
        for (int i = 0; i < w; ++i) {
            int si = hflip ? (w - 1 - i) : i;
            int srcx = sx + si;
            if (srcx < 0 || srcx >= b->w) continue;
            uint8_t s = row[srcx];
            if (colkey >= 0 && s == (uint8_t)colkey) continue;
            put_px(c, fb, dx + i, dy + j, s);
        }
    }
}

/* ---- image / tilemap banks -------------------------------------------- */

int cron_gpu_image(cronopio_console_t* c, int slot, uint32_t offset, int w, int h, uint32_t mem_size) {
    if ((unsigned)slot >= CRONOPIO_IMAGE_SLOTS) return -1;
    if (w <= 0 || h <= 0) return -1;
    uint64_t end = (uint64_t)offset + (uint64_t)w * (uint64_t)h;
    if (end > mem_size) return -1;
    c->images[slot].offset = offset;
    c->images[slot].w = w;
    c->images[slot].h = h;
    c->images[slot].used = 1;
    return 0;
}

/* Register a 256-byte palette remap (input colour index → output index)
 * as bank `slot`. Bank 0 is reserved as the identity sentinel — binding
 * it is rejected. Returns 0 on success, -1 on invalid slot / OOB offset. */
int cron_gpu_palette_bank(cronopio_console_t* c, int slot, uint32_t offset,
                          uint32_t mem_size) {
    if (slot <= 0 || slot >= CRONOPIO_PAL_BANK_SLOTS) return -1;
    /* The bank must fit a full 256-byte remap table starting at `offset`. */
    if (offset > mem_size || mem_size - offset < 256u) return -1;
    c->pal_banks[slot].offset = offset;
    c->pal_banks[slot].used   = 1;
    return 0;
}

/* Tile-anim table entry — matches sdk/include/cronopio.h cron_tile_anim_t.
 * 16 bytes; the frames array (u16[num_frames]) lives separately at
 * heap+frames_offset. */
typedef struct {
    uint16_t src_tile;       /* the cell's tile-idx (post HFLIP/VFLIP mask) */
    uint16_t period_frames;  /* >= 1; how many host frames between advances */
    uint16_t num_frames;     /* >= 1 */
    uint16_t _pad;
    uint32_t frames_offset;  /* heap offset to u16[num_frames] */
} tile_anim_t;

int cron_gpu_tile_anim(cronopio_console_t* c, int img_slot,
                       uint32_t table_offset, int count) {
    if ((unsigned)img_slot >= CRONOPIO_IMAGE_SLOTS) return -1;
    if (count < 0) return -1;
    c->tile_anims[img_slot].table_offset = (count > 0) ? table_offset : 0;
    c->tile_anims[img_slot].count        = (count > 0) ? count : 0;
    return 0;
}

int cron_gpu_blend_table(cronopio_console_t* c, int slot, uint32_t offset,
                         uint32_t mem_size) {
    if (slot <= 0 || slot >= CRONOPIO_BLEND_SLOTS) return -1;
    /* 256*256 = 65536 bytes required at heap+offset. */
    if (offset > mem_size || mem_size - offset < 65536u) return -1;
    c->blend_tables[slot].offset = offset;
    c->blend_tables[slot].used   = 1;
    return 0;
}

void cron_gpu_blend_set(cronopio_console_t* c, int slot) {
    /* Reject out-of-range silently — staying opaque is safer than aborting.
     * Slot 0 always disables blending; slot N with .used == 0 also no-ops
     * (put_px's bound-check catches it). */
    if (slot < 0 || slot >= CRONOPIO_BLEND_SLOTS) slot = 0;
    c->blend_active = slot;
}

/* Lookup helper: given a tilemap cell's masked tile index, return the
 * substituted index from any matching anim, or the original. Hot-path
 * inline-able: returns fast when the bank has no anims. */
static inline int subst_tile(cronopio_console_t* c, uint8_t* heap,
                             int img_slot, int idx) {
    int n = c->tile_anims[img_slot].count;
    if (n <= 0) return idx;
    const tile_anim_t* anims =
        (const tile_anim_t*)(heap + c->tile_anims[img_slot].table_offset);
    for (int k = 0; k < n; ++k) {
        if (anims[k].src_tile != idx) continue;
        if (anims[k].num_frames == 0 || anims[k].period_frames == 0) return idx;
        uint32_t step = c->frame_count / anims[k].period_frames;
        int fi = (int)(step % anims[k].num_frames);
        const uint16_t* frames =
            (const uint16_t*)(heap + anims[k].frames_offset);
        return frames[fi] & 0x3FFFu;   /* in case the table embeds flip flags */
    }
    return idx;
}

int cron_gpu_tilemap(cronopio_console_t* c, int slot, uint32_t offset, int w, int h, int img, uint32_t mem_size) {
    if ((unsigned)slot >= CRONOPIO_TILEMAP_SLOTS) return -1;
    if (w <= 0 || h <= 0) return -1;
    if ((unsigned)img >= CRONOPIO_IMAGE_SLOTS) return -1;
    uint64_t end = (uint64_t)offset + (uint64_t)w * (uint64_t)h * 2u;   /* u16 cells */
    if (end > mem_size) return -1;
    c->tilemaps[slot].offset = offset;
    c->tilemaps[slot].w = w;
    c->tilemaps[slot].h = h;
    c->tilemaps[slot].img = img;
    c->tilemaps[slot].used = 1;
    return 0;
}

void cron_gpu_blt(cronopio_console_t* c, uint8_t* heap, int img,
                  int dx, int dy, int sx, int sy, int w, int h, int colkey) {
    if ((unsigned)img >= CRONOPIO_IMAGE_SLOTS || !c->images[img].used) return;
    cron_image_bank_t* b = &c->images[img];
    int fx = 0, fy = 0;
    if (w < 0) { w = -w; fx = 1; }
    if (h < 0) { h = -h; fy = 1; }
    if (w == 0 || h == 0) return;
    const uint8_t* src = heap + b->offset;
    uint8_t* fb = fb_of(c, heap);
    for (int j = 0; j < h; ++j) {
        int srcy = sy + (fy ? (h - 1 - j) : j);
        if (srcy < 0 || srcy >= b->h) continue;
        for (int i = 0; i < w; ++i) {
            int srcx = sx + (fx ? (w - 1 - i) : i);
            if (srcx < 0 || srcx >= b->w) continue;
            uint8_t s = src[srcy * b->w + srcx];
            if (colkey >= 0 && s == (uint8_t)colkey) continue;
            put_px(c, fb, dx + i, dy + j, s);
        }
    }
}

/* Buffer->buffer colour-key blit, native (see console.h). dst/src are heap
 * offsets; the whole accessed range of each is validated against mem_size so a
 * bad cart pointer can't read/write out of the cart image. */
void cron_gpu_blt_buf(cronopio_console_t* c, uint8_t* heap, uint32_t mem_size,
                      uint32_t dst, int dst_w, int dst_h, int dst_pitch,
                      uint32_t src, int src_pitch,
                      int dx, int dy, int blt_w, int blt_h, int colkey) {
    (void)c;
    if (blt_w <= 0 || blt_h <= 0 || dst_w <= 0 || dst_h <= 0) return;
    if (dst_pitch <= 0 || src_pitch <= 0) return;
    /* Validate the full byte extents touched in each buffer. dst spans rows
     * 0..dst_h-1 of dst_pitch; src spans rows 0..blt_h-1 of src_pitch (the cart
     * passes src already at its sub-origin, so blt_h rows is the read extent). */
    uint64_t dst_end = (uint64_t)dst + (uint64_t)(dst_h - 1) * (uint64_t)dst_pitch + (uint64_t)dst_w;
    uint64_t src_end = (uint64_t)src + (uint64_t)(blt_h - 1) * (uint64_t)src_pitch + (uint64_t)blt_w;
    if (dst_end > mem_size || src_end > mem_size) return;
    uint8_t*       d = heap + dst;
    const uint8_t* s = heap + src;
    for (int j = 0; j < blt_h; ++j) {
        int dyy = dy + j;
        if (dyy < 0 || dyy >= dst_h) continue;
        const uint8_t* srow = s + (size_t)j * (size_t)src_pitch;
        uint8_t*       drow = d + (size_t)dyy * (size_t)dst_pitch;
        for (int i = 0; i < blt_w; ++i) {
            int dxx = dx + i;
            if (dxx < 0 || dxx >= dst_w) continue;
            uint8_t px = srow[i];
            if (colkey >= 0 && px == (uint8_t)colkey) continue;
            drow[dxx] = px;
        }
    }
}

/* Full rotozoom + flip. `rotate_packed` is (rotate_deg & 0xFFFF) in low
 * 16 bits, (flags << 16) in high 16. Flags: CRON_BLT_HFLIP / VFLIP.
 * Flip is applied to the SOURCE sampling (mirror within the sprite's
 * own rect), then rotation + scale apply to the flipped source. */
void cron_gpu_blt_ex(cronopio_console_t* c, uint8_t* heap, int img,
                     int dx, int dy, int sx, int sy, int w, int h,
                     int colkey, int rotate_packed, int scale_q16) {
    if ((unsigned)img >= CRONOPIO_IMAGE_SLOTS || !c->images[img].used) return;
    if (w <= 0 || h <= 0 || scale_q16 == 0) return;
    cron_image_bank_t* b = &c->images[img];
    const uint8_t* src = heap + b->offset;
    uint8_t* fb = fb_of(c, heap);

    /* Unpack rotate (signed 16) + flags (high 16). */
    int rotate_deg = (int)(int16_t)(rotate_packed & 0xFFFF);
    int flags      = (rotate_packed >> 16) & 0xFFFF;
    int hflip      = (flags & 1);
    int vflip      = (flags & 2);

    float s   = (float)scale_q16 / 65536.0f;
    float ang = (float)rotate_deg * 3.14159265358979f / 180.0f;
    float ca = cosf(ang), sa = sinf(ang);
    float cxs = sx + w * 0.5f, cys = sy + h * 0.5f;   /* source centre */
    float cxd = dx + w * 0.5f, cyd = dy + h * 0.5f;   /* dest centre (world) */
    /* Half-extent of the scaled+rotated w×h box, to bound the dest scan. */
    float hw = w * 0.5f * s, hh = h * 0.5f * s;
    float extx = fabsf(ca)*hw + fabsf(sa)*hh;
    float exty = fabsf(sa)*hw + fabsf(ca)*hh;
    int x0 = (int)floorf(cxd - extx), x1 = (int)ceilf(cxd + extx);
    int y0 = (int)floorf(cyd - exty), y1 = (int)ceilf(cyd + exty);
    float inv = 1.0f / s;

    for (int Y = y0; Y <= y1; ++Y) {
        for (int X = x0; X <= x1; ++X) {
            float rx = (X + 0.5f) - cxd;
            float ry = (Y + 0.5f) - cyd;
            /* inverse rotate (by -ang) then unscale */
            float ux = ( ca*rx + sa*ry) * inv;
            float uy = (-sa*rx + ca*ry) * inv;
            int srcx = (int)floorf(cxs + ux);
            int srcy = (int)floorf(cys + uy);
            if (srcx < sx || srcx >= sx + w || srcy < sy || srcy >= sy + h) continue;
            /* Apply flip in the SOURCE rect (mirror around its centre). */
            if (hflip) srcx = (sx + w - 1) - (srcx - sx);
            if (vflip) srcy = (sy + h - 1) - (srcy - sy);
            if (srcx < 0 || srcx >= b->w || srcy < 0 || srcy >= b->h) continue;
            uint8_t px = src[srcy * b->w + srcx];
            if (colkey >= 0 && px == (uint8_t)colkey) continue;
            put_px(c, fb, X, Y, px);
        }
    }
}

/* Tile cell layout (uint16_t):
 *   0xFFFF         = empty cell (no draw)
 *   bit 15         = HFLIP
 *   bit 14         = VFLIP
 *   bits 13..0     = tile index in the image bank (0..16383)
 *
 * Backwards compatible with the original "all 16 bits are the index": old
 * tilemaps with values < 0x4000 land with HFLIP=0, VFLIP=0, tile index
 * unchanged. Old "empty" sentinel 0xFFFF still has all bits set so it
 * stays empty (the empty check is done before the flag/index split). */
#define CELL_EMPTY   0xFFFFu
#define CELL_HFLIP   0x8000u
#define CELL_VFLIP   0x4000u
#define CELL_IDX_MASK 0x3FFFu

void cron_gpu_bltm(cronopio_console_t* c, uint8_t* heap, int tm,
                   int dx, int dy, int sx, int sy, int w, int h, int colkey) {
    if ((unsigned)tm >= CRONOPIO_TILEMAP_SLOTS || !c->tilemaps[tm].used) return;
    cron_tilemap_bank_t* m = &c->tilemaps[tm];
    if ((unsigned)m->img >= CRONOPIO_IMAGE_SLOTS || !c->images[m->img].used) return;
    cron_image_bank_t* ib = &c->images[m->img];
    int tpr = ib->w / CRONOPIO_TILE_SIZE;            /* tiles per row in tileset */
    if (tpr <= 0) return;
    if (w <= 0 || h <= 0) return;
    const uint16_t* cells = (const uint16_t*)(heap + m->offset);
    const uint8_t*  tiles = heap + ib->offset;
    uint8_t* fb = fb_of(c, heap);
    int map_w_px = m->w * CRONOPIO_TILE_SIZE, map_h_px = m->h * CRONOPIO_TILE_SIZE;
    if (map_w_px <= 0 || map_h_px <= 0) return;
    /* The source coords wrap modulo the map size, so the tilemap tiles
     * infinitely — a scrolling background never runs off into blank. */
    for (int j = 0; j < h; ++j) {
        int py = (sy + j) % map_h_px;
        if (py < 0) py += map_h_px;
        int cy = py / CRONOPIO_TILE_SIZE, ty0 = py % CRONOPIO_TILE_SIZE;
        for (int i = 0; i < w; ++i) {
            int px = (sx + i) % map_w_px;
            if (px < 0) px += map_w_px;
            int cx = px / CRONOPIO_TILE_SIZE, tx0 = px % CRONOPIO_TILE_SIZE;
            uint16_t cell = cells[cy * m->w + cx];
            if (cell == CELL_EMPTY) continue;
            int tx = (cell & CELL_HFLIP) ? (CRONOPIO_TILE_SIZE - 1 - tx0) : tx0;
            int ty = (cell & CELL_VFLIP) ? (CRONOPIO_TILE_SIZE - 1 - ty0) : ty0;
            int idx = subst_tile(c, heap, m->img, cell & CELL_IDX_MASK);
            int til_x = (idx % tpr) * CRONOPIO_TILE_SIZE + tx;
            int til_y = (idx / tpr) * CRONOPIO_TILE_SIZE + ty;
            if (til_x >= ib->w || til_y >= ib->h) continue;
            uint8_t s = tiles[til_y * ib->w + til_x];
            if (colkey >= 0 && s == (uint8_t)colkey) continue;
            put_px(c, fb, dx + i, dy + j, s);
        }
    }
}

/* Per-line raster table entry — matches sdk/include/cronopio.h cron_raster_t.
 * 8 bytes, aligned for direct indexing. */
typedef struct {
    int16_t  scroll_x;
    int16_t  scroll_y;
    uint8_t  pal_offset;
    uint8_t  flags;       /* reserved */
    uint16_t pal_bank;    /* 0 = identity (no swap); 1..31 = palette bank slot */
} raster_entry_t;

/* Tilemap blit with per-scanline parameter overrides. Each scanline j of the
 * destination reads table[j] for its own scroll_x/scroll_y/pal_offset; the
 * sx/sy args become the baseline that overrides ADD to. Single syscall —
 * the host walks the table in this loop, no per-line VM round-trip. */
void cron_gpu_bltm_raster(cronopio_console_t* c, uint8_t* heap, int tm,
                          int dx, int dy, int sx, int sy, int w, int h,
                          int colkey, uint32_t table_off) {
    if ((unsigned)tm >= CRONOPIO_TILEMAP_SLOTS || !c->tilemaps[tm].used) return;
    cron_tilemap_bank_t* m = &c->tilemaps[tm];
    if ((unsigned)m->img >= CRONOPIO_IMAGE_SLOTS || !c->images[m->img].used) return;
    cron_image_bank_t* ib = &c->images[m->img];
    int tpr = ib->w / CRONOPIO_TILE_SIZE;
    if (tpr <= 0) return;
    if (w <= 0 || h <= 0) return;
    const uint16_t* cells = (const uint16_t*)(heap + m->offset);
    const uint8_t*  tiles = heap + ib->offset;
    const raster_entry_t* table = (const raster_entry_t*)(heap + table_off);
    uint8_t* fb = fb_of(c, heap);
    int map_w_px = m->w * CRONOPIO_TILE_SIZE, map_h_px = m->h * CRONOPIO_TILE_SIZE;
    if (map_w_px <= 0 || map_h_px <= 0) return;
    for (int j = 0; j < h; ++j) {
        /* Per-scanline parameter pull. Indexed by the DESTINATION y (dy+j)
         * so the cart can fill a 240-entry table covering the whole screen
         * once and reuse it regardless of where the layer happens to land. */
        int dyl = dy + j;
        const raster_entry_t r = (dyl >= 0 && dyl < CRONOPIO_SCREEN_H)
                               ? table[dyl]
                               : (raster_entry_t){0,0,0,0,0};
        int py = (sy + j + r.scroll_y) % map_h_px;
        if (py < 0) py += map_h_px;
        int cy = py / CRONOPIO_TILE_SIZE, ty0 = py % CRONOPIO_TILE_SIZE;
        int sx_line = sx + r.scroll_x;
        uint8_t pal_off = r.pal_offset;
        /* Resolve the palette bank for this line. bank 0 or out-of-range or
         * unbound = identity (no swap). */
        const uint8_t *pal_bank = NULL;
        if (r.pal_bank > 0 && r.pal_bank < CRONOPIO_PAL_BANK_SLOTS
            && c->pal_banks[r.pal_bank].used) {
            pal_bank = heap + c->pal_banks[r.pal_bank].offset;
        }
        for (int i = 0; i < w; ++i) {
            int px = (sx_line + i) % map_w_px;
            if (px < 0) px += map_w_px;
            int cx = px / CRONOPIO_TILE_SIZE, tx0 = px % CRONOPIO_TILE_SIZE;
            uint16_t cell = cells[cy * m->w + cx];
            if (cell == CELL_EMPTY) continue;
            int tx = (cell & CELL_HFLIP) ? (CRONOPIO_TILE_SIZE - 1 - tx0) : tx0;
            int ty = (cell & CELL_VFLIP) ? (CRONOPIO_TILE_SIZE - 1 - ty0) : ty0;
            int idx = subst_tile(c, heap, m->img, cell & CELL_IDX_MASK);
            int til_x = (idx % tpr) * CRONOPIO_TILE_SIZE + tx;
            int til_y = (idx / tpr) * CRONOPIO_TILE_SIZE + ty;
            if (til_x >= ib->w || til_y >= ib->h) continue;
            uint8_t s = tiles[til_y * ib->w + til_x];
            if (colkey >= 0 && s == (uint8_t)colkey) continue;
            /* Apply bank-remap first (full colour swap), then pal_offset
             * (additive shift on top — useful as a gradient over the remap). */
            if (pal_bank) s = pal_bank[s];
            put_px(c, fb, dx + i, dy + j, (uint8_t)(s + pal_off));
        }
    }
}

/* Per-line affine table entry — matches sdk/include/cronopio.h cron_affine_t.
 * 16 bytes: u/v at screen x=0, plus per-pixel du/dv increments. All Q16.16. */
typedef struct {
    int32_t u, v;     /* texture coord at screen x=0 (Q16.16) */
    int32_t du, dv;   /* increment per screen pixel (Q16.16) */
} affine_entry_t;

/* Affine ("Mode-7"-style) tilemap blit. For each destination scanline j:
 *   texel sampled at screen-x i = (u + i*du, v + i*dv)
 * where (u, v, du, dv) come from table[dy+j]. The texture coords wrap
 * modulo the tilemap size (so the "floor" extends to infinity). One
 * syscall covers the whole plane; the cart fills the table outside. */
void cron_gpu_bltm_affine(cronopio_console_t* c, uint8_t* heap, int tm,
                          int dx, int dy, int w, int h, int colkey,
                          uint32_t table_off) {
    if ((unsigned)tm >= CRONOPIO_TILEMAP_SLOTS || !c->tilemaps[tm].used) return;
    cron_tilemap_bank_t* m = &c->tilemaps[tm];
    if ((unsigned)m->img >= CRONOPIO_IMAGE_SLOTS || !c->images[m->img].used) return;
    cron_image_bank_t* ib = &c->images[m->img];
    int tpr = ib->w / CRONOPIO_TILE_SIZE;
    if (tpr <= 0) return;
    if (w <= 0 || h <= 0) return;
    const uint16_t* cells = (const uint16_t*)(heap + m->offset);
    const uint8_t*  tiles = heap + ib->offset;
    const affine_entry_t* table = (const affine_entry_t*)(heap + table_off);
    uint8_t* fb = fb_of(c, heap);
    int map_w_px = m->w * CRONOPIO_TILE_SIZE, map_h_px = m->h * CRONOPIO_TILE_SIZE;
    if (map_w_px <= 0 || map_h_px <= 0) return;
    for (int j = 0; j < h; ++j) {
        int dyl = dy + j;
        if (dyl < 0 || dyl >= CRONOPIO_SCREEN_H) continue;
        const affine_entry_t a = table[dyl];
        int32_t u = a.u, v = a.v;
        for (int i = 0; i < w; ++i, u += a.du, v += a.dv) {
            /* Q16.16 -> pixel coord; modulo wrap for "infinite floor". */
            int px = ((u >> 16) % map_w_px + map_w_px) % map_w_px;
            int py = ((v >> 16) % map_h_px + map_h_px) % map_h_px;
            int cx = px / CRONOPIO_TILE_SIZE, tx0 = px % CRONOPIO_TILE_SIZE;
            int cy = py / CRONOPIO_TILE_SIZE, ty0 = py % CRONOPIO_TILE_SIZE;
            uint16_t cell = cells[cy * m->w + cx];
            if (cell == CELL_EMPTY) continue;
            int tx = (cell & CELL_HFLIP) ? (CRONOPIO_TILE_SIZE - 1 - tx0) : tx0;
            int ty = (cell & CELL_VFLIP) ? (CRONOPIO_TILE_SIZE - 1 - ty0) : ty0;
            int idx = subst_tile(c, heap, m->img, cell & CELL_IDX_MASK);
            int til_x = (idx % tpr) * CRONOPIO_TILE_SIZE + tx;
            int til_y = (idx / tpr) * CRONOPIO_TILE_SIZE + ty;
            if (til_x >= ib->w || til_y >= ib->h) continue;
            uint8_t s = tiles[til_y * ib->w + til_x];
            if (colkey >= 0 && s == (uint8_t)colkey) continue;
            put_px(c, fb, dx + i, dyl, s);
        }
    }
}

/* ---- textured-rasteriser accelerators (software-3D fast path) --------- */

void cron_gpu_cmap(cronopio_console_t* c, uint32_t offset, int set) {
    c->cmap_offset = offset;
    c->cmap_set    = set;
}

void cron_gpu_tcol(cronopio_console_t* c, uint8_t* heap, int x, int y0, int y1,
                   uint32_t src_off, int mask, int32_t frac, int32_t step) {
    if (x < c->draw.clip_x0 || x >= c->draw.clip_x1) return;
    if (y0 < c->draw.clip_y0) { frac += step * (c->draw.clip_y0 - y0); y0 = c->draw.clip_y0; }
    if (y1 >= c->draw.clip_y1) y1 = c->draw.clip_y1 - 1;
    const uint8_t* src = heap + src_off;
    const uint8_t* cm  = c->cmap_set ? heap + c->cmap_offset : 0;
    uint8_t* fb = heap + c->fb_offset;
    uint32_t m = (uint32_t)mask;
    for (int y = y0; y <= y1; ++y) {
        uint8_t s = src[((uint32_t)frac >> 16) & m];
        fb[y * CRONOPIO_SCREEN_W + x] = cm ? cm[s] : s;
        frac += step;
    }
}

void cron_gpu_tcolm(cronopio_console_t* c, uint8_t* heap, int x, int y0, int y1,
                    uint32_t src_off, int32_t frac, int32_t step) {
    if (x < c->draw.clip_x0 || x >= c->draw.clip_x1) return;
    if (y0 < c->draw.clip_y0) { frac += step * (c->draw.clip_y0 - y0); y0 = c->draw.clip_y0; }
    if (y1 >= c->draw.clip_y1) y1 = c->draw.clip_y1 - 1;
    const uint8_t* src = heap + src_off;
    const uint8_t* cm  = c->cmap_set ? heap + c->cmap_offset : 0;
    uint8_t* fb = heap + c->fb_offset;
    for (int y = y0; y <= y1; ++y) {
        uint8_t s = src[(uint32_t)frac >> 16];   /* linear, no wrap */
        fb[y * CRONOPIO_SCREEN_W + x] = cm ? cm[s] : s;
        frac += step;
    }
}

void cron_gpu_tspan(cronopio_console_t* c, uint8_t* heap, int y, int x0, int x1,
                    uint32_t src_off, int32_t u, int32_t v, int32_t du, int32_t dv) {
    if (y < c->draw.clip_y0 || y >= c->draw.clip_y1) return;
    if (x0 < c->draw.clip_x0) { u += du * (c->draw.clip_x0 - x0); v += dv * (c->draw.clip_x0 - x0); x0 = c->draw.clip_x0; }
    if (x1 >= c->draw.clip_x1) x1 = c->draw.clip_x1 - 1;
    const uint8_t* src = heap + src_off;   /* 64x64 */
    const uint8_t* cm  = c->cmap_set ? heap + c->cmap_offset : 0;
    uint8_t* fb = heap + c->fb_offset;
    int row = y * CRONOPIO_SCREEN_W;
    for (int x = x0; x <= x1; ++x) {
        int idx = (int)((((uint32_t)v >> 16) & 63u) << 6) | (int)(((uint32_t)u >> 16) & 63u);
        uint8_t s = src[idx];
        fb[row + x] = cm ? cm[s] : s;
        u += du; v += dv;
    }
}
