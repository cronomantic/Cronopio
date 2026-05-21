/* Gouraud-lit spinning cube — diffuse lighting through the 3D pipeline.
 *
 * Per-vertex normals (the corner directions, so the cube shades as if
 * rounded) are rotated into world space and dotted with a fixed light to
 * get an intensity; that picks an index in a dark->bright palette ramp,
 * carried per vertex as cron_cvert.light. CRON_POLY_GOURAUD interpolates it
 * across each triangle (no cmap needed — the ramp indices ARE the colours).
 * The depth buffer resolves face visibility.
 *
 *   cvm-cc -I sdk/include --region=fb:76800:rw --region=pal:1024:rw \
 *          examples/lit/lit.c -o lit.bin
 */

#include <cronopio.h>
#include <cronopio3d.h>

volatile uint8_t  *CRON_FB  = 0;
volatile uint32_t *CRON_PAL = 0;

#define RAMP_BASE   16
#define RAMP_COUNT  48        /* palette indices 16..63 = the light ramp */

static const float V[8][3] = {
    {-1,-1,-1}, { 1,-1,-1}, { 1, 1,-1}, {-1, 1,-1},
    {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1},
};
static const unsigned char F[6][4] = {
    {0,1,2,3}, {5,4,7,6}, {4,0,3,7}, {1,5,6,2}, {4,5,1,0}, {3,2,6,7},
};

static int32_t     zbuffer[CRON_SCREEN_W * CRON_SCREEN_H];
static cron_vert_t batch[64];
static float       angle;

static cron_vec3 corner(int i) { return cron_v3(V[i][0], V[i][1], V[i][2]); }

static void frame(void) {
    cron_cls(0);
    cron_zclear(0x7FFFFFFF);

    cron_mat4 proj, view, rotx, roty, model, mv, mvp;
    cron_mat_perspective(&proj, CRON_PI * 0.5f,
                         (float)CRON_SCREEN_W / (float)CRON_SCREEN_H, 0.1f, 100.0f);
    cron_mat_lookat(&view, cron_v3(0, 0, 4), cron_v3(0, 0, 0), cron_v3(0, 1, 0));
    cron_mat_rot_y(&roty, angle);
    cron_mat_rot_x(&rotx, angle * 0.6f);
    cron_mat_mul(&model, &roty, &rotx);
    cron_mat_mul(&mv, &view, &model);
    cron_mat_mul(&mvp, &proj, &mv);

    /* Light from upper-right-front, fixed in world space. */
    cron_vec3 light = cron_v3(0.4f, 0.7f, 0.6f);

    /* Per-corner shade: rotate the corner's outward normal into world space,
     * Lambert against the light, lift by an ambient floor, map to the ramp. */
    float lit[8];
    for (int i = 0; i < 8; ++i) {
        cron_vec3 wn;
        cron_mat_dir(&wn, &model, corner(i));   /* corner dir = its normal */
        float intensity = 0.25f + 0.75f * cron_lambert(wn, light);
        lit[i] = (float)cron_shade(RAMP_BASE, RAMP_COUNT, intensity);
    }

    int count = 0;
    for (int f = 0; f < 6; ++f) {
        int i0 = F[f][0], i1 = F[f][1], i2 = F[f][2], i3 = F[f][3];
        cron_vec3 p0 = corner(i0), p1 = corner(i1), p2 = corner(i2), p3 = corner(i3);
        cron_cvert q0 = {{0,0,0,0}, 0, 0, lit[i0]};
        cron_cvert q1 = {{0,0,0,0}, 0, 0, lit[i1]};
        cron_cvert q2 = {{0,0,0,0}, 0, 0, lit[i2]};
        cron_cvert q3 = {{0,0,0,0}, 0, 0, lit[i3]};
        cron_cvert t012[3] = { q0, q1, q2 };
        cron_cvert t023[3] = { q0, q2, q3 };
        count += cron_emit_tri(batch + count, &mvp, p0, p1, p2, t012);
        count += cron_emit_tri(batch + count, &mvp, p0, p2, p3, t023);
    }
    cron_polys(CRON_POLY_GOURAUD | CRON_POLY_ZTEST, batch, count, 0, -1);

    angle += 0.02f;
}

int main(void) {
    if (cron_resolve_video() != 0) return 1;
    /* dark teal -> warm white light ramp at indices 16..63 */
    cron_palette_ramp(RAMP_BASE, RAMP_COUNT, 0x06141a, 0xffe8c0);
    cron_zbuf(zbuffer);
    cron_set_frame(frame);
    return 0;
}
