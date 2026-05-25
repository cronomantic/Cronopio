/* Spinning textured cube — the 3D pipeline end to end.
 *
 * Exercises the cart-side maths (cronopio3d.h: perspective, lookat, rotation,
 * transform + near-clip + project) and the host triangle rasteriser
 * (cron_polys with affine texturing and a depth buffer). The cart builds a
 * checkerboard texture, registers it as image bank 0, and each frame submits
 * the cube's 12 triangles for the host to fill.
 *
 * Build with the CronoVM toolchain (CronopioCart.cmake does this):
 *   cvm-cc -I sdk/include --region=fb:76800:rw --region=pal:1024:rw \
 *          examples/cube/cube.c -o cube.bin
 * Run:  ./cronopio cube.bin
 */

#include <cronopio.h>
#include <cronopio3d.h>

volatile uint8_t  *CRON_FB  = 0;
volatile uint32_t *CRON_PAL = 0;

#define TEXSZ 16

/* The 8 corners of a unit cube and its 6 quad faces (4 corner indices each).
 * Winding is irrelevant here — the depth buffer resolves visibility, so we
 * draw every face and let nearer fragments win. */
static const float V[8][3] = {
    {-1,-1,-1}, { 1,-1,-1}, { 1, 1,-1}, {-1, 1,-1},
    {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1},
};
static const unsigned char F[6][4] = {
    {0,1,2,3},  /* -Z */  {5,4,7,6},  /* +Z */
    {4,0,3,7},  /* -X */  {1,5,6,2},  /* +X */
    {4,5,1,0},  /* -Y */  {3,2,6,7},  /* +Y */
};

static int32_t    zbuffer[CRON_SCREEN_W * CRON_SCREEN_H];
static uint8_t    tex[TEXSZ * TEXSZ];
static cron_vert_t batch[64];
static float       angle;

static cron_vec3 corner(int i) {
    return cron_v3(V[i][0], V[i][1], V[i][2]);
}

/* Append one quad face (corners a,b,c,d with texcoords spanning the texture)
 * as two near-clipped, projected triangles. Returns verts written. */
static int emit_face(cron_vert_t *out, const cron_mat4 *mvp, int f) {
    cron_vec3 p0 = corner(F[f][0]), p1 = corner(F[f][1]);
    cron_vec3 p2 = corner(F[f][2]), p3 = corner(F[f][3]);
    float T = (float)TEXSZ;
    /* per-corner {u, v, light}; pos is filled by cron_emit_tri */
    cron_cvert q0 = {{0,0,0,0}, 0, 0, 0}, q1 = {{0,0,0,0}, T, 0, 0};
    cron_cvert q2 = {{0,0,0,0}, T, T, 0}, q3 = {{0,0,0,0}, 0, T, 0};
    cron_cvert t012[3] = { q0, q1, q2 };
    cron_cvert t023[3] = { q0, q2, q3 };
    int n = 0;
    n += cron_emit_tri(out + n, mvp, p0, p1, p2, t012);
    n += cron_emit_tri(out + n, mvp, p0, p2, p3, t023);
    return n;
}

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

    int count = 0;
    for (int f = 0; f < 6; ++f)
        count += emit_face(batch + count, &mvp, f);

    cron_polys(CRON_POLY_TEX | CRON_POLY_PERSP | CRON_POLY_ZTEST, batch, count, 0, -1);

    angle += 0.02f;
}

int main(void) {
    if (cron_resolve_video() != 0) {
        cron_log("cube: fb/pal regions missing\n", 28);
        return 1;
    }

    /* Checkerboard texture with a bright border, in default-palette colours
     * (7 ~ white, 12 ~ blue, 10 ~ yellow border). */
    for (int y = 0; y < TEXSZ; ++y) {
        for (int x = 0; x < TEXSZ; ++x) {
            uint8_t c;
            if (x == 0 || y == 0 || x == TEXSZ - 1 || y == TEXSZ - 1)
                c = 10;
            else
                c = (((x >> 2) ^ (y >> 2)) & 1) ? 7 : 12;
            tex[y * TEXSZ + x] = c;
        }
    }
    cron_image(0, tex, TEXSZ, TEXSZ);
    cron_zbuf(zbuffer);

    cron_set_frame(frame);
    return 0;
}
