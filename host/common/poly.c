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

void cron_gpu_polys(cronopio_console_t* c, uint8_t* heap, int mode,
                    uint32_t verts_off, int count, int arg, int colkey) {
    if (count < 3) return;
    const uint8_t* vbase = heap + verts_off;
    uint8_t* fb = heap + c->fb_offset;
    const uint8_t* cm = c->cmap_set ? heap + c->cmap_offset : 0;

    int use_z = (mode & CRONOPIO_POLY_ZTEST) && c->zbuf_set;
    int32_t* zb = use_z ? (int32_t*)(heap + c->zbuf_offset) : 0;

    const uint8_t* tex = 0;
    int tw = 0, th = 0;
    if (mode & CRONOPIO_POLY_TEX) {
        if ((unsigned)arg >= CRONOPIO_IMAGE_SLOTS || !c->images[arg].used) return;
        tex = heap + c->images[arg].offset;
        tw  = c->images[arg].w;
        th  = c->images[arg].h;
    }
    int persp   = (mode & CRONOPIO_POLY_PERSP) && (mode & CRONOPIO_POLY_TEX);
    int gouraud = (mode & CRONOPIO_POLY_GOURAUD);
    int clamp   = (mode & CRONOPIO_POLY_CLAMP);   /* alias skins clamp; world wraps */
    int turb    = (mode & CRONOPIO_POLY_TURB) && (mode & CRONOPIO_POLY_TEX) && c->turb_set;
    int turb_phase = 0;
    if (turb) { turb_build(c->turb_amp); turb_phase = c->turb_phase; }

    /* A levels*256 colormap (cron_colormap) feeds two lit-texture modes below. */
    const uint8_t* colormap = 0; int cmlevels = 0;
    if (c->colormap_set) {
        colormap = heap + c->colormap_offset; cmlevels = c->colormap_levels;
        if (cmlevels <= 0) colormap = 0;
    }

    /* Per-texel lighting (Quake world): sample a small per-surface light grid
     * (lu/lv) for the colormap row. Needs TEX + grid + colormap. */
    const uint8_t* lm = 0; int lmw = 0, lmh = 0;
    int lightmap = (mode & CRONOPIO_POLY_LIGHTMAP) && (mode & CRONOPIO_POLY_TEX)
                 && c->lm_set && colormap;
    if (lightmap) {
        lm = heap + c->lm_offset; lmw = c->lm_w; lmh = c->lm_h;
        if (lmw <= 0 || lmh <= 0) lightmap = 0;
    }

    /* Per-vertex lit texture (Quake alias): interpolate the vertex .c as the
     * colormap row (Gouraud light) and remap the texel through it. TEX+GOURAUD
     * with a colormap bound, and no per-texel grid. */
    int texlit = (mode & CRONOPIO_POLY_GOURAUD) && (mode & CRONOPIO_POLY_TEX)
               && colormap && !lightmap;

    const int CX0 = c->draw.clip_x0, CY0 = c->draw.clip_y0;
    const int CX1 = c->draw.clip_x1, CY1 = c->draw.clip_y1;

    for (int t = 0; t + 3 <= count; t += 3) {
        vert_t a, b, d;
        rd_vert(vbase + (size_t)(t + 0) * CRONOPIO_VERT_BYTES, &a);
        rd_vert(vbase + (size_t)(t + 1) * CRONOPIO_VERT_BYTES, &b);
        rd_vert(vbase + (size_t)(t + 2) * CRONOPIO_VERT_BYTES, &d);

        int64_t area = edge(a.x, a.y, b.x, b.y, d.x, d.y);
        if (area == 0) continue;                     /* degenerate */
        float invarea = 1.0f / (float)area;

        int minx = a.x < b.x ? (a.x < d.x ? a.x : d.x) : (b.x < d.x ? b.x : d.x);
        int maxx = a.x > b.x ? (a.x > d.x ? a.x : d.x) : (b.x > d.x ? b.x : d.x);
        int miny = a.y < b.y ? (a.y < d.y ? a.y : d.y) : (b.y < d.y ? b.y : d.y);
        int maxy = a.y > b.y ? (a.y > d.y ? a.y : d.y) : (b.y > d.y ? b.y : d.y);
        if (minx < CX0) minx = CX0; if (maxx > CX1 - 1) maxx = CX1 - 1;
        if (miny < CY0) miny = CY0; if (maxy > CY1 - 1) maxy = CY1 - 1;
        if (minx > maxx || miny > maxy) continue;

        /* Perspective: pre-divide texcoords by w per vertex, interpolate
         * u/w, v/w, 1/w linearly, divide per pixel. */
        float iw0=0, iw1=0, iw2=0, uo0=0, uo1=0, uo2=0, vo0=0, vo1=0, vo2=0;
        float luo0=0, luo1=0, luo2=0, lvo0=0, lvo1=0, lvo2=0;
        if (persp) {
            iw0 = a.w ? 1.0f / (float)a.w : 0.0f;
            iw1 = b.w ? 1.0f / (float)b.w : 0.0f;
            iw2 = d.w ? 1.0f / (float)d.w : 0.0f;
            uo0 = a.u * iw0; uo1 = b.u * iw1; uo2 = d.u * iw2;
            vo0 = a.v * iw0; vo1 = b.v * iw1; vo2 = d.v * iw2;
            if (lightmap) {
                luo0 = a.lu * iw0; luo1 = b.lu * iw1; luo2 = d.lu * iw2;
                lvo0 = a.lv * iw0; lvo1 = b.lv * iw1; lvo2 = d.lv * iw2;
            }
        }

        for (int py = miny; py <= maxy; ++py) {
            int row = py * CRONOPIO_SCREEN_W;
            for (int px = minx; px <= maxx; ++px) {
                int64_t wa = edge(b.x, b.y, d.x, d.y, px, py);
                int64_t wb = edge(d.x, d.y, a.x, a.y, px, py);
                int64_t wc = edge(a.x, a.y, b.x, b.y, px, py);
                if (area > 0) { if (wa < 0 || wb < 0 || wc < 0) continue; }
                else          { if (wa > 0 || wb > 0 || wc > 0) continue; }

                float bA = (float)wa * invarea;
                float bB = (float)wb * invarea;
                float bC = (float)wc * invarea;
                int idx = row + px;

                int32_t z = 0;
                if (use_z) {
                    z = (int32_t)(bA * a.z + bB * b.z + bC * d.z);
                    if (z >= zb[idx]) continue;
                }

                int col;
                if (tex) {
                    float uf, vf, luf = 0, lvf = 0;
                    if (persp) {
                        float iwp = bA*iw0 + bB*iw1 + bC*iw2;
                        if (iwp == 0.0f) continue;
                        float rw = 1.0f / iwp;
                        uf = (bA*uo0 + bB*uo1 + bC*uo2) * rw;
                        vf = (bA*vo0 + bB*vo1 + bC*vo2) * rw;
                        if (lightmap) {
                            luf = (bA*luo0 + bB*luo1 + bC*luo2) * rw;
                            lvf = (bA*lvo0 + bB*lvo1 + bC*lvo2) * rw;
                        }
                    } else {
                        uf = bA*a.u + bB*b.u + bC*d.u;
                        vf = bA*a.v + bB*b.v + bC*d.v;
                        if (lightmap) {
                            luf = bA*a.lu + bB*b.lu + bC*d.lu;
                            lvf = bA*a.lv + bB*b.lv + bC*d.lv;
                        }
                    }
                    int tu = ((int)uf) >> 16;
                    int tv = ((int)vf) >> 16;
                    if (turb) {
                        /* Quake water/lava ripple: perturb each texel by a sine
                         * of the *other* coordinate (+ phase), period 128. */
                        int ou = tu, ov = tv;
                        tu += g_turbsin[(ov + turb_phase) & (TURB_CYCLE - 1)];
                        tv += g_turbsin[(ou + turb_phase) & (TURB_CYCLE - 1)];
                    }
                    if (clamp) {
                        /* edge clamp: a texel past the skin border samples the
                         * border, not the opposite edge (which gave alias
                         * models a "blue seam" when texcoords wrapped). */
                        if (tu < 0) tu = 0; else if (tu >= tw) tu = tw - 1;
                        if (tv < 0) tv = 0; else if (tv >= th) tv = th - 1;
                    } else {
                        tu %= tw; if (tu < 0) tu += tw;
                        tv %= th; if (tv < 0) tv += th;
                    }
                    uint8_t s = tex[tv * tw + tu];
                    if (colkey >= 0 && s == (uint8_t)colkey) continue;
                    if (lightmap) {
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
                        else if (lx >= lmw-1)  { lx = lx1 = lmw-1; fx = 0; }
                        else                     lx1 = lx + 1;
                        if (ly < 0)            { ly = ly1 = 0; fy = 0; }
                        else if (ly >= lmh-1)  { ly = ly1 = lmh-1; fy = 0; }
                        else                     ly1 = ly + 1;
                        int l00 = lm[ly *lmw + lx], l10 = lm[ly *lmw + lx1];
                        int l01 = lm[ly1*lmw + lx], l11 = lm[ly1*lmw + lx1];
                        int top = l00 + (((l10 - l00) * fx) >> 16);
                        int bot = l01 + (((l11 - l01) * fx) >> 16);
                        int li  = top + (((bot - top) * fy) >> 16);
                        if (li < 0) li = 0; else if (li >= cmlevels) li = cmlevels - 1;
                        col = colormap[li * 256 + s];
                    } else if (texlit) {
                        /* per-vertex light row (Gouraud) through the colormap */
                        int row = (int)(bA*a.c + bB*b.c + bC*d.c);
                        if (row < 0) row = 0; else if (row >= cmlevels) row = cmlevels - 1;
                        col = colormap[row * 256 + s];
                    } else {
                        col = cm ? cm[s] : s;
                    }
                } else if (gouraud) {
                    int ci = (int)(bA*a.c + bB*b.c + bC*d.c) & 0xFF;
                    col = cm ? cm[ci] : ci;
                } else {
                    col = arg & 0xFF;
                }

                fb[idx] = (uint8_t)col;
                if (use_z) zb[idx] = z;
            }
        }
    }
}
