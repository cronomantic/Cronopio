/* "collide" — AABB collision smoke test for sdk/include/cron_collide.h.
 *
 * Eight balls bounce around the screen on linear trajectories. Each frame
 * the cart runs cron_aabb_overlap on every pair; balls in contact flash
 * their colour (and exchange velocities, the classic 1-D elastic swap, so
 * the motion is visibly affected by the detection — not just a visual
 * highlight). The intent is to exercise the SDK helper end-to-end and
 * give a visible "this collision really fired" signal that survives
 * a headless frame grab.
 *
 * No syscalls beyond the standard drawing ones — collision is pure
 * cart-side library code (cron_collide.h is inline-only).
 */

#include <cronopio.h>
#include <cron_collide.h>

volatile uint8_t  *CRON_FB  = 0;
volatile uint32_t *CRON_PAL = 0;

#define N_BALLS  8
#define BALL_R   8
#define BALL_SZ  (BALL_R * 2)

typedef struct {
    int  x, y;     /* top-left of the ball's AABB (square hitbox) */
    int  vx, vy;   /* per-frame velocity, signed */
    int  col;      /* palette index — flashes briefly on collision */
    int  flash;    /* remaining frames the ball stays "lit" */
} ball_t;

static ball_t balls[N_BALLS];

/* Tiny deterministic PRNG so the start state is identical every run —
 * makes headless frame inspection comparable. */
static uint32_t rng = 0x12345u;
static int rand_in(int lo, int hi) {
    rng = rng * 1664525u + 1013904223u;
    return lo + (int)((rng >> 8) % (uint32_t)(hi - lo));
}

static void init_balls(void) {
    for (int i = 0; i < N_BALLS; ++i) {
        balls[i].x  = rand_in(BALL_R, CRON_SCREEN_W - BALL_SZ - BALL_R);
        balls[i].y  = rand_in(BALL_R, CRON_SCREEN_H - BALL_SZ - BALL_R);
        balls[i].vx = rand_in(-3, 4);
        balls[i].vy = rand_in(-3, 4);
        if (balls[i].vx == 0) balls[i].vx = 1;
        if (balls[i].vy == 0) balls[i].vy = 1;
        balls[i].col   = 1 + (i & 0xF);   /* indices 1..15, the palette body */
        balls[i].flash = 0;
    }
}

/* Tick: move + wall-bounce + pairwise AABB. */
static void tick(void) {
    /* Motion + wall bounce */
    for (int i = 0; i < N_BALLS; ++i) {
        ball_t *b = &balls[i];
        b->x += b->vx;
        b->y += b->vy;
        if (b->x < 0) { b->x = 0; b->vx = -b->vx; }
        if (b->y < 0) { b->y = 0; b->vy = -b->vy; }
        if (b->x + BALL_SZ > CRON_SCREEN_W) {
            b->x = CRON_SCREEN_W - BALL_SZ;  b->vx = -b->vx;
        }
        if (b->y + BALL_SZ > CRON_SCREEN_H) {
            b->y = CRON_SCREEN_H - BALL_SZ;  b->vy = -b->vy;
        }
        if (b->flash > 0) --b->flash;
    }
    /* Pairwise AABB hits. O(N^2) which is fine at N=8; for big N a grid
     * partition or sweep-and-prune lives in user code. */
    for (int i = 0; i < N_BALLS; ++i) {
        for (int j = i + 1; j < N_BALLS; ++j) {
            ball_t *a = &balls[i], *b = &balls[j];
            if (!cron_aabb_overlap(a->x, a->y, BALL_SZ, BALL_SZ,
                                   b->x, b->y, BALL_SZ, BALL_SZ)) continue;
            /* Visible signal: flash for 4 frames. */
            a->flash = 4;
            b->flash = 4;
            /* Crude separation: swap velocities (1-D elastic on equal
             * masses). Plus push them apart by 1 px along the overlap
             * axis so they don't lock together next frame. */
            int tx = a->vx; a->vx = b->vx; b->vx = tx;
            int ty = a->vy; a->vy = b->vy; b->vy = ty;
            /* Nudge along the dominant axis to break contact. */
            int dx = (a->x + BALL_SZ / 2) - (b->x + BALL_SZ / 2);
            int dy = (a->y + BALL_SZ / 2) - (b->y + BALL_SZ / 2);
            if ((dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy)) {
                if (dx > 0) { a->x += 1; b->x -= 1; }
                else        { a->x -= 1; b->x += 1; }
            } else {
                if (dy > 0) { a->y += 1; b->y -= 1; }
                else        { a->y -= 1; b->y += 1; }
            }
        }
    }
}

static void draw(void) {
    /* Dark blue background so flashed balls (yellow/white) pop. */
    for (int i = 0; i < CRON_SCREEN_W * CRON_SCREEN_H; ++i) CRON_FB[i] = 4;
    for (int i = 0; i < N_BALLS; ++i) {
        int col = balls[i].flash ? 15 : balls[i].col;
        cron_circ(balls[i].x + BALL_R, balls[i].y + BALL_R, BALL_R, col);
    }
    cron_text("CRON_COLLIDE", 12, 8, 8, 7);
}

static void frame(void) {
    tick();
    draw();
}

int main(void) {
    if (cron_resolve_video() != 0) {
        cron_log("collide: fb/pal regions missing\n", 32);
        return 1;
    }
    init_balls();
    cron_set_frame(frame);
    return 0;
}
