/* Cronopio 3D — header-only fixed-pipeline maths for the cart side.
 *
 * The cart transforms / projects / lights in floating point (CronoVM's f32
 * is single-instruction — FADD/FMUL/FDIV/FSQRT — and the intended path for
 * "simple 3D"), clips against the near plane, then submits screen-space
 * triangles to the host via cron_polys(). The retro look comes from the
 * 8 bpp framebuffer and affine texturing, not the maths precision.
 *
 * Conventions:
 *   - Right-handed world; camera looks down -Z. Row-major 4x4 matrices,
 *     point transform p' = M * (x,y,z,1).
 *   - Perspective matrix maps the near plane to clip z = -w (GL style); the
 *     near clip plane is therefore z + w >= 0.
 *   - Angles are radians. cron_sin/cron_cos are polynomial approximations.
 *
 * Small structs are passed/returned by value only in `inline` helpers (clang
 * -O1 scalarises them away); the heavy routines use out-pointers to keep the
 * translator off aggregate ABI paths. */

#ifndef CRONOPIO3D_H
#define CRONOPIO3D_H

#include <cronopio.h>
#include "cvm_intrin.h"   /* cvm_fsqrt, cvm_f2i_sat_s — provided by cvm-cc */

#define CRON_PI    3.14159265358979f
#define CRON_2PI   6.28318530717959f

typedef struct { float x, y, z; }       cron_vec3;
typedef struct { float x, y, z, w; }    cron_vec4;
typedef struct { float m[16]; }         cron_mat4;   /* row-major: m[r*4+c] */

/* A clip-space vertex carrying the attributes the rasteriser needs, so the
 * near-clip can interpolate them at the crossing. */
typedef struct {
    cron_vec4 pos;     /* clip space (post-MVP, pre-divide) */
    float     u, v;    /* texels */
    float     light;   /* gouraud light/index */
    float     lu, lv;  /* lightmap texels (CRON_POLY_LIGHTMAP) */
} cron_cvert;

/* ---- vec3 (inline; scalarised by SROA) -------------------------------- */

static inline cron_vec3 cron_v3(float x, float y, float z) { cron_vec3 r; r.x=x; r.y=y; r.z=z; return r; }
static inline cron_vec3 cron_v3_add  (cron_vec3 a, cron_vec3 b) { return cron_v3(a.x+b.x, a.y+b.y, a.z+b.z); }
static inline cron_vec3 cron_v3_sub  (cron_vec3 a, cron_vec3 b) { return cron_v3(a.x-b.x, a.y-b.y, a.z-b.z); }
static inline cron_vec3 cron_v3_scale(cron_vec3 a, float s)     { return cron_v3(a.x*s, a.y*s, a.z*s); }
static inline float     cron_v3_dot  (cron_vec3 a, cron_vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline cron_vec3 cron_v3_cross(cron_vec3 a, cron_vec3 b) {
    return cron_v3(a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x);
}
static inline float     cron_v3_len  (cron_vec3 a) { return cvm_fsqrt(cron_v3_dot(a, a)); }
static inline cron_vec3 cron_v3_norm (cron_vec3 a) {
    float l = cron_v3_len(a);
    if (l > 0.0001f) { float i = 1.0f / l; return cron_v3_scale(a, i); }
    return a;
}

/* ---- sin / cos (range-reduced degree-7 polynomial) -------------------- */

__attribute__((noinline))
static float cron_sin(float x) {
    /* reduce x to [-pi, pi] */
    float k = x * (1.0f / CRON_2PI);
    k = (float)cvm_f2i_sat_s(k >= 0.0f ? k + 0.5f : k - 0.5f);   /* round */
    x = x - k * CRON_2PI;
    float x2 = x * x;
    /* sin(x) ~= x - x^3/6 + x^5/120 - x^7/5040 */
    return x * (1.0f + x2 * (-0.16666667f + x2 * (0.0083333333f + x2 * (-0.00019841270f))));
}
static inline float cron_cos(float x) { return cron_sin(x + CRON_PI * 0.5f); }

/* ---- mat4 (out-pointer; out must not alias the inputs) ---------------- */

__attribute__((noinline))
static void cron_mat_identity(cron_mat4* o) {
    for (int i = 0; i < 16; ++i) o->m[i] = 0.0f;
    o->m[0] = o->m[5] = o->m[10] = o->m[15] = 1.0f;
}

__attribute__((noinline))
static void cron_mat_mul(cron_mat4* o, const cron_mat4* a, const cron_mat4* b) {
    cron_mat4 t;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            t.m[r*4+c] = a->m[r*4+0]*b->m[0*4+c] + a->m[r*4+1]*b->m[1*4+c]
                       + a->m[r*4+2]*b->m[2*4+c] + a->m[r*4+3]*b->m[3*4+c];
    *o = t;
}

__attribute__((noinline))
static void cron_mat_translate(cron_mat4* o, float x, float y, float z) {
    cron_mat_identity(o); o->m[3] = x; o->m[7] = y; o->m[11] = z;
}
__attribute__((noinline))
static void cron_mat_scale(cron_mat4* o, float x, float y, float z) {
    cron_mat_identity(o); o->m[0] = x; o->m[5] = y; o->m[10] = z;
}
__attribute__((noinline))
static void cron_mat_rot_x(cron_mat4* o, float a) {
    float s = cron_sin(a), c = cron_cos(a);
    cron_mat_identity(o); o->m[5]=c; o->m[6]=-s; o->m[9]=s; o->m[10]=c;
}
__attribute__((noinline))
static void cron_mat_rot_y(cron_mat4* o, float a) {
    float s = cron_sin(a), c = cron_cos(a);
    cron_mat_identity(o); o->m[0]=c; o->m[2]=s; o->m[8]=-s; o->m[10]=c;
}
__attribute__((noinline))
static void cron_mat_rot_z(cron_mat4* o, float a) {
    float s = cron_sin(a), c = cron_cos(a);
    cron_mat_identity(o); o->m[0]=c; o->m[1]=-s; o->m[4]=s; o->m[5]=c;
}

/* Perspective projection. fovy in radians, aspect = w/h. Maps the camera
 * frustum to clip space with near at z=-w. */
__attribute__((noinline))
static void cron_mat_perspective(cron_mat4* o, float fovy, float aspect,
                                 float near, float far) {
    float f = 1.0f / cron_sin(fovy * 0.5f) * cron_cos(fovy * 0.5f); /* cot(fovy/2) */
    for (int i = 0; i < 16; ++i) o->m[i] = 0.0f;
    o->m[0]  = f / aspect;
    o->m[5]  = f;
    o->m[10] = (far + near) / (near - far);
    o->m[11] = (2.0f * far * near) / (near - far);
    o->m[14] = -1.0f;
}

/* Look-at view matrix. eye/target/up are cron_vec3. */
__attribute__((noinline))
static void cron_mat_lookat(cron_mat4* o, cron_vec3 eye, cron_vec3 target, cron_vec3 up) {
    cron_vec3 f = cron_v3_norm(cron_v3_sub(target, eye));   /* forward */
    cron_vec3 s = cron_v3_norm(cron_v3_cross(f, up));       /* right   */
    cron_vec3 u = cron_v3_cross(s, f);                      /* true up */
    o->m[0]=s.x;  o->m[1]=s.y;  o->m[2]=s.z;  o->m[3]=-cron_v3_dot(s, eye);
    o->m[4]=u.x;  o->m[5]=u.y;  o->m[6]=u.z;  o->m[7]=-cron_v3_dot(u, eye);
    o->m[8]=-f.x; o->m[9]=-f.y; o->m[10]=-f.z;o->m[11]= cron_v3_dot(f, eye);
    o->m[12]=0;   o->m[13]=0;   o->m[14]=0;   o->m[15]=1.0f;
}

/* Transform a direction (w=0) by m's upper-left 3x3 — for normals and light
 * vectors. Correct for the rotation/uniform-scale matrices these helpers
 * build (no shear); for a general matrix you'd want the inverse-transpose. */
static inline void cron_mat_dir(cron_vec3* o, const cron_mat4* m, cron_vec3 d) {
    float x = m->m[0]*d.x + m->m[1]*d.y + m->m[2]*d.z;
    float y = m->m[4]*d.x + m->m[5]*d.y + m->m[6]*d.z;
    float z = m->m[8]*d.x + m->m[9]*d.y + m->m[10]*d.z;
    o->x = x; o->y = y; o->z = z;
}

/* ---- lighting --------------------------------------------------------- */

/* Lambert diffuse term: dot(normalize(n), normalize(to_light)), clamped to
 * [0,1]. `to_light` points from the surface toward the light. */
static inline float cron_lambert(cron_vec3 n, cron_vec3 to_light) {
    float d = cron_v3_dot(cron_v3_norm(n), cron_v3_norm(to_light));
    return d < 0.0f ? 0.0f : d;
}

/* Outward unit normal of triangle (a,b,c), CCW winding. */
static inline cron_vec3 cron_tri_normal(cron_vec3 a, cron_vec3 b, cron_vec3 c) {
    return cron_v3_norm(cron_v3_cross(cron_v3_sub(b, a), cron_v3_sub(c, a)));
}

/* Build a `count`-entry palette gradient from `dark` to `bright`
 * (0x00RRGGBB) at palette indices [base, base+count). Drawing GOURAUD with
 * cron_cvert.light set to a ramp index (and no cmap) then gives smooth
 * shading: interpolated light selects the gradient colour directly. */
static inline void cron_palette_ramp(int base, int count,
                                     uint32_t dark, uint32_t bright) {
    for (int i = 0; i < count; ++i) {
        float t = (count > 1) ? (float)i / (float)(count - 1) : 0.0f;
        int r = (int)((float)((dark>>16)&0xFF) * (1.0f - t) + (float)((bright>>16)&0xFF) * t);
        int g = (int)((float)((dark>> 8)&0xFF) * (1.0f - t) + (float)((bright>> 8)&0xFF) * t);
        int b = (int)((float)((dark    )&0xFF) * (1.0f - t) + (float)((bright    )&0xFF) * t);
        cron_palette_set(base + i, ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b);
    }
}

/* Map a 0..1 intensity to a ramp index in [base, base+count). */
static inline int cron_shade(int base, int count, float intensity) {
    if (intensity < 0.0f) intensity = 0.0f;
    if (intensity > 1.0f) intensity = 1.0f;
    int i = base + (int)(intensity * (float)(count - 1) + 0.5f);
    return i;
}

/* Transform a point (w=1) by m into clip space. */
__attribute__((noinline))
static void cron_mat_point(cron_vec4* o, const cron_mat4* m, cron_vec3 p) {
    o->x = m->m[0]*p.x + m->m[1]*p.y + m->m[2]*p.z  + m->m[3];
    o->y = m->m[4]*p.x + m->m[5]*p.y + m->m[6]*p.z  + m->m[7];
    o->z = m->m[8]*p.x + m->m[9]*p.y + m->m[10]*p.z + m->m[11];
    o->w = m->m[12]*p.x + m->m[13]*p.y + m->m[14]*p.z + m->m[15];
}

/* ---- near-plane clip + project ---------------------------------------- */

/* Linear interpolation of a clip vertex at parameter t in [0,1]. */
__attribute__((noinline))
static void cron_cvert_lerp(cron_cvert* o, const cron_cvert* a, const cron_cvert* b, float t) {
    o->pos.x = a->pos.x + (b->pos.x - a->pos.x) * t;
    o->pos.y = a->pos.y + (b->pos.y - a->pos.y) * t;
    o->pos.z = a->pos.z + (b->pos.z - a->pos.z) * t;
    o->pos.w = a->pos.w + (b->pos.w - a->pos.w) * t;
    o->u     = a->u     + (b->u     - a->u)     * t;
    o->v     = a->v     + (b->v     - a->v)     * t;
    o->light = a->light + (b->light - a->light) * t;
    o->lu    = a->lu    + (b->lu    - a->lu)    * t;
    o->lv    = a->lv    + (b->lv    - a->lv)    * t;
}

/* Clip triangle `in[3]` against the near plane (z + w >= 0). Writes the
 * resulting fan (0, 3 or 6 vertices = 0/1/2 triangles) to `out` and returns
 * the vertex count. Standard Sutherland-Hodgman: a triangle cut by a plane
 * yields a 3- or 4-gon, triangulated as a fan. */
__attribute__((noinline))
static int cron_clip_near(const cron_cvert in[3], cron_cvert out[6]) {
    cron_cvert poly[4];
    int np = 0;
    for (int i = 0; i < 3; ++i) {
        const cron_cvert* cur = &in[i];
        const cron_cvert* nxt = &in[(i + 1) % 3];
        float dc = cur->pos.z + cur->pos.w;
        float dn = nxt->pos.z + nxt->pos.w;
        int in_c = dc >= 0.0f, in_n = dn >= 0.0f;
        if (in_c) poly[np++] = *cur;
        if (in_c != in_n) {
            float t = dc / (dc - dn);
            if (np < 4) cron_cvert_lerp(&poly[np++], cur, nxt, t);
        }
    }
    if (np < 3) return 0;
    /* fan: (0,1,2) and, for a quad, (0,2,3) */
    out[0]=poly[0]; out[1]=poly[1]; out[2]=poly[2];
    if (np == 3) return 3;
    out[3]=poly[0]; out[4]=poly[2]; out[5]=poly[3];
    return 6;
}

/* Perspective-divide a clip vertex and map to the 320x240 viewport, filling
 * a cron_vert_t for cron_polys. z is mapped to a 23-bit depth (nearer =
 * smaller); w carries the clip w (Q16.16) for perspective-correct texturing;
 * u/v become Q16.16 texels; c is the light index. */
__attribute__((noinline))
static void cron_to_screen(cron_vert_t* o, const cron_cvert* cv) {
    float iw = (cv->pos.w != 0.0f) ? 1.0f / cv->pos.w : 0.0f;
    float ndc_x = cv->pos.x * iw;
    float ndc_y = cv->pos.y * iw;
    float ndc_z = cv->pos.z * iw;
    o->x = cvm_f2i_sat_s((ndc_x * 0.5f + 0.5f) * (float)CRON_SCREEN_W);
    o->y = cvm_f2i_sat_s((0.5f - ndc_y * 0.5f) * (float)CRON_SCREEN_H);
    o->z = cvm_f2i_sat_s(ndc_z * 8388607.0f);
    o->u = cvm_f2i_sat_s(cv->u * 65536.0f);
    o->v = cvm_f2i_sat_s(cv->v * 65536.0f);
    o->w = cvm_f2i_sat_s(cv->pos.w * 65536.0f);
    o->c = cvm_f2i_sat_s(cv->light);
    o->lu = cvm_f2i_sat_s(cv->lu * 65536.0f);
    o->lv = cvm_f2i_sat_s(cv->lv * 65536.0f);
}

/* High-level: transform one source triangle by `mvp`, near-clip it, and
 * append the resulting screen-space vertices (0, 3 or 6) to `out`. Returns
 * the count appended. `tc` holds per-vertex {u,v,light} (texels / index). */
__attribute__((noinline))
static int cron_emit_tri(cron_vert_t* out, const cron_mat4* mvp,
                         cron_vec3 p0, cron_vec3 p1, cron_vec3 p2,
                         const cron_cvert* attr /* attr[3]: only u,v,light read */) {
    cron_cvert tri[3], clipped[6];
    cron_mat_point(&tri[0].pos, mvp, p0); tri[0].u=attr[0].u; tri[0].v=attr[0].v; tri[0].light=attr[0].light; tri[0].lu=attr[0].lu; tri[0].lv=attr[0].lv;
    cron_mat_point(&tri[1].pos, mvp, p1); tri[1].u=attr[1].u; tri[1].v=attr[1].v; tri[1].light=attr[1].light; tri[1].lu=attr[1].lu; tri[1].lv=attr[1].lv;
    cron_mat_point(&tri[2].pos, mvp, p2); tri[2].u=attr[2].u; tri[2].v=attr[2].v; tri[2].light=attr[2].light; tri[2].lu=attr[2].lu; tri[2].lv=attr[2].lv;
    int n = cron_clip_near(tri, clipped);
    for (int i = 0; i < n; ++i) cron_to_screen(&out[i], &clipped[i]);
    return n;
}

/* ---- quads ------------------------------------------------------------ */

/* Expand a quad (4 screen-space verts, CW or CCW) into two triangles in
 * `out` (6 verts: 0,1,2 and 0,2,3). */
static inline void cron_quad(cron_vert_t out[6], const cron_vert_t* a,
                             const cron_vert_t* b, const cron_vert_t* c,
                             const cron_vert_t* d) {
    out[0]=*a; out[1]=*b; out[2]=*c;
    out[3]=*a; out[4]=*c; out[5]=*d;
}

/* Back-face test in screen space (2D cross product of the first triangle).
 * Returns >0 for CCW, <0 for CW, 0 degenerate — cull whichever your winding
 * convention treats as back-facing. */
static inline int cron_facing(const cron_vert_t* a, const cron_vert_t* b, const cron_vert_t* c) {
    return (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
}

#endif
