/* @NAME@ — a Cronopio 3D cartridge: a spinning textured cube.
 *
 * The cart-side maths live in cronopio3d.h (perspective/lookat/rotation,
 * near-clip, project); the host rasterises the submitted triangles with a
 * depth buffer. See docs/3d.md for lighting (Gouraud) and the full pipeline.
 *
 * Build:  cronopio-cc main.c -o @NAME@.bin
 * Run:    cronopio @NAME@.bin
 */
#include <cronopio.h>
#include <cronopio3d.h>

#define TEXSZ 16

/* 8 cube corners and 6 quad faces (corner indices). The depth buffer resolves
 * visibility, so winding doesn't matter — draw every face. */
static const float V[8][3] = {
    {-1,-1,-1}, { 1,-1,-1}, { 1, 1,-1}, {-1, 1,-1},
    {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1},
};
static const unsigned char F[6][4] = {
    {0,1,2,3}, {5,4,7,6}, {4,0,3,7}, {1,5,6,2}, {4,5,1,0}, {3,2,6,7},
};

static int32_t     zbuffer[CRON_SCREEN_W * CRON_SCREEN_H];
static uint8_t     tex[TEXSZ * TEXSZ];
static cron_vert_t batch[64];
static float       angle;

static cron_vec3 corner(int i) { return cron_v3(V[i][0], V[i][1], V[i][2]); }

/* Append one quad face as two near-clipped, projected triangles. */
static int emit_face(cron_vert_t *out, const cron_mat4 *mvp, int f) {
    cron_vec3 p0 = corner(F[f][0]), p1 = corner(F[f][1]);
    cron_vec3 p2 = corner(F[f][2]), p3 = corner(F[f][3]);
    float T = (float)TEXSZ;
    cron_cvert q0 = {{0,0,0,0}, 0, 0, 0}, q1 = {{0,0,0,0}, T, 0, 0};
    cron_cvert q2 = {{0,0,0,0}, T, T, 0}, q3 = {{0,0,0,0}, 0, T, 0};
    cron_cvert t012[3] = { q0, q1, q2 };
    cron_cvert t023[3] = { q0, q2, q3 };
    int n = 0;
    n += cron_emit_tri(out + n, mvp, p0, p1, p2, t012);
    n += cron_emit_tri(out + n, mvp, p0, p2, p3, t023);
    return n;
}

void setup(void) {
    /* Checkerboard texture with a bright border. */
    for (int y = 0; y < TEXSZ; ++y)
        for (int x = 0; x < TEXSZ; ++x) {
            uint8_t c;
            if (x == 0 || y == 0 || x == TEXSZ - 1 || y == TEXSZ - 1) c = 10;
            else c = (((x >> 2) ^ (y >> 2)) & 1) ? 7 : 12;
            tex[y * TEXSZ + x] = c;
        }
    cron_image(0, tex, TEXSZ, TEXSZ);
    cron_zbuf(zbuffer);
}

void frame(void) {
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

    cron_polys(CRON_POLY_TEX | CRON_POLY_ZTEST, batch, count, 0, -1);
    angle += 0.02f;
}

CRONOPIO_CART_INIT(setup, frame)
