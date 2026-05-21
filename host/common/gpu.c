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
    fb[y * CRONOPIO_SCREEN_W + x] = c->draw.pal_map[col & 0xFF];
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

/* Built-in 8x8 font ROM — printable ASCII 0x20..0x7F (96 glyphs).
 * Public-domain font8x8_basic (Marcel Sondaar / Daniel Hepper). Each glyph
 * is 8 rows; within a row byte, bit 0 (0x01) is the leftmost column and
 * bit 7 the rightmost. Index into the table is `ch - 0x20`. */
static const uint8_t font8x8[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* (space) */
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, /* ! */
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, /* " */
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, /* # */
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, /* $ */
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, /* % */
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, /* & */
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, /* ' */
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, /* ( */
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, /* ) */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* * */
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, /* + */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, /* , */
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, /* - */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, /* . */
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, /* / */
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, /* 0 */
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, /* 1 */
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, /* 2 */
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, /* 3 */
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, /* 4 */
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, /* 5 */
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, /* 6 */
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, /* 7 */
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, /* 8 */
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, /* 9 */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, /* : */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, /* ; */
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, /* < */
    {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, /* = */
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, /* > */
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, /* ? */
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, /* @ */
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, /* A */
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, /* B */
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, /* C */
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, /* D */
    {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00}, /* E */
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, /* F */
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, /* G */
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, /* H */
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* I */
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, /* J */
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, /* K */
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, /* L */
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, /* M */
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, /* N */
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, /* O */
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, /* P */
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, /* Q */
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, /* R */
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, /* S */
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* T */
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, /* U */
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* V */
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, /* W */
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, /* X */
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, /* Y */
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, /* Z */
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, /* [ */
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, /* \ */
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, /* ] */
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, /* ^ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* _ */
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, /* ` */
    {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00}, /* a */
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, /* b */
    {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, /* c */
    {0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00}, /* d */
    {0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00}, /* e */
    {0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00}, /* f */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F}, /* g */
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, /* h */
    {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, /* i */
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, /* j */
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, /* k */
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* l */
    {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00}, /* m */
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, /* n */
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, /* o */
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, /* p */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, /* q */
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, /* r */
    {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00}, /* s */
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, /* t */
    {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, /* u */
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* v */
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, /* w */
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, /* x */
    {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F}, /* y */
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, /* z */
    {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00}, /* { */
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, /* | */
    {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00}, /* } */
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00}, /* ~ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 0x7F */
};

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
    for (int j = 0; j < h; ++j) {
        int py = sy + j;
        if (py < 0 || py >= map_h_px) continue;
        int cy = py / CRONOPIO_TILE_SIZE, ty = py % CRONOPIO_TILE_SIZE;
        for (int i = 0; i < w; ++i) {
            int px = sx + i;
            if (px < 0 || px >= map_w_px) continue;
            int cx = px / CRONOPIO_TILE_SIZE, tx = px % CRONOPIO_TILE_SIZE;
            uint16_t cell = cells[cy * m->w + cx];
            if (cell == 0xFFFF) continue;            /* empty cell */
            int til_x = (cell % tpr) * CRONOPIO_TILE_SIZE + tx;
            int til_y = (cell / tpr) * CRONOPIO_TILE_SIZE + ty;
            if (til_x >= ib->w || til_y >= ib->h) continue;
            uint8_t s = tiles[til_y * ib->w + til_x];
            if (colkey >= 0 && s == (uint8_t)colkey) continue;
            put_px(c, fb, dx + i, dy + j, s);
        }
    }
}
