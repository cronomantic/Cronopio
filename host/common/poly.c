/* 3D triangle rasteriser — the PSX / 486-Pentium-style submission layer.
 *
 * The cart does transform + projection + lighting in fixed point and hands
 * the host batches of screen-space triangles (a vertex array in cart memory);
 * this file fills them. Barycentric edge-function rasteriser with per-pixel
 * attribute interpolation: flat / Gouraud (light index through the active
 * cmap) / affine or perspective-correct texture from an image bank, with an
 * optional depth buffer.
 *
 * Interpolation uses host floats — the host is native code, so this is the
 * fast path; the cart's own 3D maths stays fixed-point. Writes honour the
 * clip rect (viewport) but, like tcol/tspan, bypass camera and the draw
 * palette (the cmap is the only colour remap). */

#include "console.h"

#include <stdint.h>
#include <math.h>
#include <string.h>

#define TURB_CYCLE 128   /* turbulence sine period (matches Quake's CYCLE) */

typedef struct { int32_t x, y, z, u, v, w, c, lu, lv; } vert_t;

/* Turbulence offset table: turbsin[i] in [0, 2*amp] texels, rebuilt when the
 * amplitude changes. Quake's water warp is s' = s + amp + amp*sin(theta). */
static int g_turbsin[TURB_CYCLE];
static int g_turb_amp_built = -1;
static void turb_build(int amp) {
    if (amp == g_turb_amp_built) return;
    for (int i = 0; i < TURB_CYCLE; i++)
        g_turbsin[i] = (int)(amp + sin(i * (2.0 * 3.14159265358979 / TURB_CYCLE)) * amp);
    g_turb_amp_built = amp;
}

static void rd_vert(const uint8_t* p, vert_t* o) {
    const int32_t* w = (const int32_t*)p;
    o->x = w[0]; o->y = w[1]; o->z = w[2];
    o->u = w[3]; o->v = w[4]; o->w = w[5]; o->c = w[6];
    o->lu = w[7]; o->lv = w[8];
}

/* Twice the signed area of triangle (a,b,c); >0 for one winding, <0 the
 * other. Also the edge function value of c against edge a->b. */
static int64_t edge(int ax, int ay, int bx, int by, int cx, int cy) {
    return (int64_t)(bx - ax) * (cy - ay) - (int64_t)(by - ay) * (cx - ax);
}

void cron_gpu_zbuf(cronopio_console_t* c, uint32_t offset, int set) {
    c->zbuf_offset = offset;
    c->zbuf_set    = set;
}

void cron_gpu_zclear(cronopio_console_t* c, uint8_t* heap, int32_t far) {
    if (!c->zbuf_set) return;
    int32_t* zb = (int32_t*)(heap + c->zbuf_offset);
    int n = CRONOPIO_SCREEN_W * CRONOPIO_SCREEN_H;
    for (int i = 0; i < n; ++i) zb[i] = far;
}

void cron_gpu_lightmap(cronopio_console_t* c, uint32_t offset, int w, int h, int set) {
    c->lm_offset = offset; c->lm_w = w; c->lm_h = h; c->lm_set = set;
}

void cron_gpu_colormap(cronopio_console_t* c, uint32_t offset, int levels, int set) {
    c->colormap_offset = offset; c->colormap_levels = levels; c->colormap_set = set;
}

void cron_gpu_turb(cronopio_console_t* c, int phase, int amp, int set) {
    c->turb_phase = phase; c->turb_amp = amp; c->turb_set = set;
}

/* Resolved per-draw rasteriser state: pointer to the texture / lightmap /
 * colormap, dimensions, flags. Filled by `resolve_state` (shared between
 * cron_gpu_polys and cron_gpu_xform_polys) and consumed by raster_tri. */
typedef struct {
    uint8_t*       fb;
    int32_t*       zb;
    const uint8_t* cm;
    const uint8_t* tex;
    int            tw, th;
    const uint8_t* colormap;
    int            cmlevels;
    const uint8_t* lm;
    int            lmw, lmh;
    int            use_z, persp, gouraud, clamp, turb, lightmap, texlit;
    int            turb_phase;
    int            arg, colkey;
    int            cx0, cy0, cx1, cy1;   /* viewport clip rect, screen-space */
} raster_ctx_t;

/* Resolve console state into a raster_ctx for a draw call. Returns 0 if the
 * mode is unrasterisable (e.g. a TEX draw against an unbound image slot). */
static int resolve_state(cronopio_console_t* c, uint8_t* heap, int mode,
                         int arg, int colkey, raster_ctx_t* x) {
    x->fb = heap + c->fb_offset;
    x->cm = c->cmap_set ? heap + c->cmap_offset : 0;
    x->use_z = (mode & CRONOPIO_POLY_ZTEST) && c->zbuf_set;
    x->zb = x->use_z ? (int32_t*)(heap + c->zbuf_offset) : 0;

    x->tex = 0; x->tw = x->th = 0;
    if (mode & CRONOPIO_POLY_TEX) {
        if ((unsigned)arg >= CRONOPIO_IMAGE_SLOTS || !c->images[arg].used) return 0;
        x->tex = heap + c->images[arg].offset;
        x->tw  = c->images[arg].w;
        x->th  = c->images[arg].h;
    }
    x->persp   = (mode & CRONOPIO_POLY_PERSP) && (mode & CRONOPIO_POLY_TEX);
    x->gouraud = (mode & CRONOPIO_POLY_GOURAUD);
    x->clamp   = (mode & CRONOPIO_POLY_CLAMP);
    x->turb    = (mode & CRONOPIO_POLY_TURB) && (mode & CRONOPIO_POLY_TEX) && c->turb_set;
    x->turb_phase = 0;
    if (x->turb) { turb_build(c->turb_amp); x->turb_phase = c->turb_phase; }

    x->colormap = 0; x->cmlevels = 0;
    if (c->colormap_set) {
        x->colormap = heap + c->colormap_offset;
        x->cmlevels = c->colormap_levels;
        if (x->cmlevels <= 0) x->colormap = 0;
    }
    x->lm = 0; x->lmw = x->lmh = 0;
    x->lightmap = (mode & CRONOPIO_POLY_LIGHTMAP) && (mode & CRONOPIO_POLY_TEX)
                && c->lm_set && x->colormap;
    if (x->lightmap) {
        x->lm = heap + c->lm_offset; x->lmw = c->lm_w; x->lmh = c->lm_h;
        if (x->lmw <= 0 || x->lmh <= 0) x->lightmap = 0;
    }
    x->texlit = (mode & CRONOPIO_POLY_GOURAUD) && (mode & CRONOPIO_POLY_TEX)
              && x->colormap && !x->lightmap;
    x->arg = arg; x->colkey = colkey;
    x->cx0 = c->draw.clip_x0; x->cy0 = c->draw.clip_y0;
    x->cx1 = c->draw.clip_x1; x->cy1 = c->draw.clip_y1;
    return 1;
}

/* Rasterise one screen-space triangle (a,b,d) into the framebuffer. The
 * per-pixel work — barycentric edges, optional z-test, affine/perspective
 * texture sample, lightmap or per-vertex Gouraud lit texturing, flat colour
 * — all flows from the same inner loop; resolve_state pre-bakes the choices. */
static void raster_tri(const raster_ctx_t* x, const vert_t* a, const vert_t* b, const vert_t* d) {
    int64_t area = edge(a->x, a->y, b->x, b->y, d->x, d->y);
    if (area == 0) return;                     /* degenerate */
    float invarea = 1.0f / (float)area;

    int minx = a->x < b->x ? (a->x < d->x ? a->x : d->x) : (b->x < d->x ? b->x : d->x);
    int maxx = a->x > b->x ? (a->x > d->x ? a->x : d->x) : (b->x > d->x ? b->x : d->x);
    int miny = a->y < b->y ? (a->y < d->y ? a->y : d->y) : (b->y < d->y ? b->y : d->y);
    int maxy = a->y > b->y ? (a->y > d->y ? a->y : d->y) : (b->y > d->y ? b->y : d->y);
    if (minx < x->cx0) minx = x->cx0; if (maxx > x->cx1 - 1) maxx = x->cx1 - 1;
    if (miny < x->cy0) miny = x->cy0; if (maxy > x->cy1 - 1) maxy = x->cy1 - 1;
    if (minx > maxx || miny > maxy) return;

    /* Perspective: pre-divide texcoords by w per vertex, interpolate
     * u/w, v/w, 1/w linearly, divide per pixel. */
    float iw0=0, iw1=0, iw2=0, uo0=0, uo1=0, uo2=0, vo0=0, vo1=0, vo2=0;
    float luo0=0, luo1=0, luo2=0, lvo0=0, lvo1=0, lvo2=0;
    if (x->persp) {
        iw0 = a->w ? 1.0f / (float)a->w : 0.0f;
        iw1 = b->w ? 1.0f / (float)b->w : 0.0f;
        iw2 = d->w ? 1.0f / (float)d->w : 0.0f;
        uo0 = a->u * iw0; uo1 = b->u * iw1; uo2 = d->u * iw2;
        vo0 = a->v * iw0; vo1 = b->v * iw1; vo2 = d->v * iw2;
        if (x->lightmap) {
            luo0 = a->lu * iw0; luo1 = b->lu * iw1; luo2 = d->lu * iw2;
            lvo0 = a->lv * iw0; lvo1 = b->lv * iw1; lvo2 = d->lv * iw2;
        }
    }

    for (int py = miny; py <= maxy; ++py) {
        int row = py * CRONOPIO_SCREEN_W;
        for (int px = minx; px <= maxx; ++px) {
            int64_t wa = edge(b->x, b->y, d->x, d->y, px, py);
            int64_t wb = edge(d->x, d->y, a->x, a->y, px, py);
            int64_t wc = edge(a->x, a->y, b->x, b->y, px, py);
            if (area > 0) { if (wa < 0 || wb < 0 || wc < 0) continue; }
            else          { if (wa > 0 || wb > 0 || wc > 0) continue; }

            float bA = (float)wa * invarea;
            float bB = (float)wb * invarea;
            float bC = (float)wc * invarea;
            int idx = row + px;

            int32_t z = 0;
            if (x->use_z) {
                z = (int32_t)(bA * a->z + bB * b->z + bC * d->z);
                if (z >= x->zb[idx]) continue;
            }

            int col;
            if (x->tex) {
                float uf, vf, luf = 0, lvf = 0;
                if (x->persp) {
                    float iwp = bA*iw0 + bB*iw1 + bC*iw2;
                    if (iwp == 0.0f) continue;
                    float rw = 1.0f / iwp;
                    uf = (bA*uo0 + bB*uo1 + bC*uo2) * rw;
                    vf = (bA*vo0 + bB*vo1 + bC*vo2) * rw;
                    if (x->lightmap) {
                        luf = (bA*luo0 + bB*luo1 + bC*luo2) * rw;
                        lvf = (bA*lvo0 + bB*lvo1 + bC*lvo2) * rw;
                    }
                } else {
                    uf = bA*a->u + bB*b->u + bC*d->u;
                    vf = bA*a->v + bB*b->v + bC*d->v;
                    if (x->lightmap) {
                        luf = bA*a->lu + bB*b->lu + bC*d->lu;
                        lvf = bA*a->lv + bB*b->lv + bC*d->lv;
                    }
                }
                int tu = ((int)uf) >> 16;
                int tv = ((int)vf) >> 16;
                if (x->turb) {
                    /* Quake water/lava ripple: perturb each texel by a sine
                     * of the *other* coordinate (+ phase), period 128. */
                    int ou = tu, ov = tv;
                    tu += g_turbsin[(ov + x->turb_phase) & (TURB_CYCLE - 1)];
                    tv += g_turbsin[(ou + x->turb_phase) & (TURB_CYCLE - 1)];
                }
                if (x->clamp) {
                    /* edge clamp: a texel past the skin border samples the
                     * border, not the opposite edge (which gave alias
                     * models a "blue seam" when texcoords wrapped). */
                    if (tu < 0) tu = 0; else if (tu >= x->tw) tu = x->tw - 1;
                    if (tv < 0) tv = 0; else if (tv >= x->th) tv = x->th - 1;
                } else {
                    tu %= x->tw; if (tu < 0) tu += x->tw;
                    tv %= x->th; if (tv < 0) tv += x->th;
                }
                uint8_t s = x->tex[tv * x->tw + tu];
                if (x->colkey >= 0 && s == (uint8_t)x->colkey) continue;
                if (x->lightmap) {
                    /* Bilinear sample of the per-surface light grid so the
                     * coarse 16-unit lumels gradate smoothly, matching the
                     * software renderer's block interpolation (NEAREST left
                     * the accel walls visibly blocky/inconsistent). The
                     * grid bytes are colormap rows, interpolated in Q16. */
                    int li_q = (int)luf, lv_q = (int)lvf;
                    int lx = li_q >> 16, ly = lv_q >> 16;
                    int fx = li_q & 0xFFFF, fy = lv_q & 0xFFFF;
                    int lx1, ly1;
                    if (lx < 0)            { lx = lx1 = 0; fx = 0; }
                    else if (lx >= x->lmw-1)  { lx = lx1 = x->lmw-1; fx = 0; }
                    else                     lx1 = lx + 1;
                    if (ly < 0)            { ly = ly1 = 0; fy = 0; }
                    else if (ly >= x->lmh-1)  { ly = ly1 = x->lmh-1; fy = 0; }
                    else                     ly1 = ly + 1;
                    int l00 = x->lm[ly *x->lmw + lx], l10 = x->lm[ly *x->lmw + lx1];
                    int l01 = x->lm[ly1*x->lmw + lx], l11 = x->lm[ly1*x->lmw + lx1];
                    int top = l00 + (((l10 - l00) * fx) >> 16);
                    int bot = l01 + (((l11 - l01) * fx) >> 16);
                    int li  = top + (((bot - top) * fy) >> 16);
                    if (li < 0) li = 0; else if (li >= x->cmlevels) li = x->cmlevels - 1;
                    col = x->colormap[li * 256 + s];
                } else if (x->texlit) {
                    /* per-vertex light row (Gouraud) through the colormap */
                    int row2 = (int)(bA*a->c + bB*b->c + bC*d->c);
                    if (row2 < 0) row2 = 0; else if (row2 >= x->cmlevels) row2 = x->cmlevels - 1;
                    col = x->colormap[row2 * 256 + s];
                } else {
                    col = x->cm ? x->cm[s] : s;
                }
            } else if (x->gouraud) {
                int ci = (int)(bA*a->c + bB*b->c + bC*d->c) & 0xFF;
                col = x->cm ? x->cm[ci] : ci;
            } else {
                col = x->arg & 0xFF;
            }

            x->fb[idx] = (uint8_t)col;
            if (x->use_z) x->zb[idx] = z;
        }
    }
}

void cron_gpu_polys(cronopio_console_t* c, uint8_t* heap, int mode,
                    uint32_t verts_off, int count, int arg, int colkey) {
    if (count < 3) return;
    raster_ctx_t x;
    if (!resolve_state(c, heap, mode, arg, colkey, &x)) return;
    const uint8_t* vbase = heap + verts_off;
    for (int t = 0; t + 3 <= count; t += 3) {
        vert_t a, b, d;
        rd_vert(vbase + (size_t)(t + 0) * CRONOPIO_VERT_BYTES, &a);
        rd_vert(vbase + (size_t)(t + 1) * CRONOPIO_VERT_BYTES, &b);
        rd_vert(vbase + (size_t)(t + 2) * CRONOPIO_VERT_BYTES, &d);
        raster_tri(&x, &a, &b, &d);
    }
}

/* ============================================================ */
/* cron_gpu_xform_polys: host-side transform / near-clip / project pipeline.
 *
 * Each input vertex is 8 little-endian floats (CRONOPIO_WVERT_BYTES):
 *   { x, y, z (world), u, v (texels), lu, lv (lumels), light (gouraud row) }
 *
 * Per source triangle the host:
 *   1. transforms the three world-space xyz by the bound MVP (4x4 row-major)
 *      into clip-space (x, y, z, w);
 *   2. clips against the near plane (z + w >= 0), Sutherland-Hodgman style —
 *      a clipped triangle becomes 0, 1 (triangle) or 2 (quad) screen tris
 *      with interpolated attrs at the crossings;
 *   3. perspective-divides each surviving vertex, maps NDC to the bound clip
 *      rect (the viewport), and quantises u/v/lu/lv to Q16.16 / light to int
 *      to match what cron_polys consumes;
 *   4. feeds the screen-space verts to raster_tri (same inner loop as
 *      cron_polys), one tri at a time.
 *
 * Same CRONOPIO_POLY_* mode flags + image / lightmap / colormap / turb
 * bindings as cron_polys; the only "extra" state is cron_mvp. */
typedef struct {
    float x, y, z, w;       /* clip space */
    float u, v;             /* texels */
    float lu, lv;           /* lumels */
    float light;            /* Gouraud row */
} cvert_t;

/* Read one world-space vertex from cart memory and transform by the MVP. */
static void wvert_to_clip(const uint8_t* p, const float* m, cvert_t* o) {
    float wx, wy, wz;
    memcpy(&wx, p +  0, 4);
    memcpy(&wy, p +  4, 4);
    memcpy(&wz, p +  8, 4);
    memcpy(&o->u,     p + 12, 4);
    memcpy(&o->v,     p + 16, 4);
    memcpy(&o->lu,    p + 20, 4);
    memcpy(&o->lv,    p + 24, 4);
    memcpy(&o->light, p + 28, 4);
    o->x = m[ 0]*wx + m[ 1]*wy + m[ 2]*wz + m[ 3];
    o->y = m[ 4]*wx + m[ 5]*wy + m[ 6]*wz + m[ 7];
    o->z = m[ 8]*wx + m[ 9]*wy + m[10]*wz + m[11];
    o->w = m[12]*wx + m[13]*wy + m[14]*wz + m[15];
}

/* Linear interpolation of a clip vertex at parameter t in [0,1]. */
static void cvert_lerp(cvert_t* o, const cvert_t* a, const cvert_t* b, float t) {
    o->x     = a->x     + (b->x     - a->x)     * t;
    o->y     = a->y     + (b->y     - a->y)     * t;
    o->z     = a->z     + (b->z     - a->z)     * t;
    o->w     = a->w     + (b->w     - a->w)     * t;
    o->u     = a->u     + (b->u     - a->u)     * t;
    o->v     = a->v     + (b->v     - a->v)     * t;
    o->lu    = a->lu    + (b->lu    - a->lu)    * t;
    o->lv    = a->lv    + (b->lv    - a->lv)    * t;
    o->light = a->light + (b->light - a->light) * t;
}

/* Clip one triangle against the near plane (z + w >= 0). Writes 0/3/6 verts
 * to `out` and returns the count — same shape as the cart's cron_clip_near. */
static int clip_near(const cvert_t in[3], cvert_t out[6]) {
    cvert_t poly[4];
    int np = 0;
    for (int i = 0; i < 3; ++i) {
        const cvert_t* cur = &in[i];
        const cvert_t* nxt = &in[(i + 1) % 3];
        float dc = cur->z + cur->w;
        float dn = nxt->z + nxt->w;
        int in_c = dc >= 0.0f, in_n = dn >= 0.0f;
        if (in_c && np < 4) poly[np++] = *cur;
        if (in_c != in_n) {
            float t = dc / (dc - dn);
            if (np < 4) cvert_lerp(&poly[np++], cur, nxt, t);
        }
    }
    if (np < 3) return 0;
    out[0]=poly[0]; out[1]=poly[1]; out[2]=poly[2];
    if (np == 3) return 3;
    out[3]=poly[0]; out[4]=poly[2]; out[5]=poly[3];
    return 6;
}

/* Saturate-cast a float to int32_t (matches the cart's cvm_f2i_sat_s so the
 * two pipelines agree on edge / overflow behaviour). */
static int32_t f2i_sat(float f) {
    if (f >= 2147483520.0f) return INT32_MAX;
    if (f <= -2147483520.0f) return INT32_MIN;
    return (int32_t)f;
}

/* Project one clipped vertex into the viewport-mapped vert_t consumed by
 * raster_tri. The viewport is the bound clip rect (g_vx,g_vy,g_vw,g_vh in
 * the cart). u/v/lu/lv become Q16.16; w carries the clip-w (Q16.16) for
 * perspective-correct sampling; light becomes an int colormap row. */
static void cvert_to_screen(vert_t* o, const cvert_t* c, int vx, int vy, int vw, int vh) {
    float iw = (c->w != 0.0f) ? 1.0f / c->w : 0.0f;
    float nx = c->x * iw, ny = c->y * iw, nz = c->z * iw;
    o->x = vx + f2i_sat((nx * 0.5f + 0.5f) * (float)vw);
    o->y = vy + f2i_sat((0.5f - ny * 0.5f) * (float)vh);
    o->z = f2i_sat(nz * 8388607.0f);
    o->u = f2i_sat(c->u * 65536.0f);
    o->v = f2i_sat(c->v * 65536.0f);
    o->w = f2i_sat(c->w * 65536.0f);
    o->c = f2i_sat(c->light);
    o->lu = f2i_sat(c->lu * 65536.0f);
    o->lv = f2i_sat(c->lv * 65536.0f);
}

void cron_gpu_mvp(cronopio_console_t* c, const float* mat, int set) {
    if (set && mat) memcpy(c->mvp, mat, sizeof c->mvp);
    c->mvp_set = set ? 1 : 0;
}

void cron_gpu_xform_polys(cronopio_console_t* d, uint8_t* heap, int mode,
                          uint32_t verts_off, int count, int arg, int colkey) {
    if (count < 3 || !d->mvp_set) return;
    raster_ctx_t x;
    if (!resolve_state(d, heap, mode, arg, colkey, &x)) return;
    const uint8_t* vbase = heap + verts_off;
    /* viewport = the bound clip rect; raster_tri also clips per pixel against
     * cx0..cx1/cy0..cy1, so out-of-viewport pixels are skipped anyway. */
    int vx = x.cx0, vy = x.cy0;
    int vw = x.cx1 - x.cx0, vh = x.cy1 - x.cy0;
    if (vw <= 0 || vh <= 0) return;

    for (int t = 0; t + 3 <= count; t += 3) {
        cvert_t tri[3], clipped[6];
        wvert_to_clip(vbase + (size_t)(t + 0) * CRONOPIO_WVERT_BYTES, d->mvp, &tri[0]);
        wvert_to_clip(vbase + (size_t)(t + 1) * CRONOPIO_WVERT_BYTES, d->mvp, &tri[1]);
        wvert_to_clip(vbase + (size_t)(t + 2) * CRONOPIO_WVERT_BYTES, d->mvp, &tri[2]);
        int n = clip_near(tri, clipped);
        if (n == 0) continue;
        vert_t sv[6];
        for (int i = 0; i < n; ++i) cvert_to_screen(&sv[i], &clipped[i], vx, vy, vw, vh);
        raster_tri(&x, &sv[0], &sv[1], &sv[2]);
        if (n == 6) raster_tri(&x, &sv[3], &sv[4], &sv[5]);
    }
}
