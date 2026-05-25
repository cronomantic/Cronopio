/* 2D demo — tilemap background + a movable sprite (the Pyxel-style layer).
 *
 * Exercises image banks, tilemaps, blt with colour-key transparency and
 * flipping, bltm scrolling, and a HUD with shapes/text. Resources are built
 * procedurally at boot (no external assets).
 *
 *   Arrows / WASD move the sprite; the background scrolls on its own.
 */

#include <cronopio.h>

volatile uint8_t  *CRON_FB  = 0;
volatile uint32_t *CRON_PAL = 0;

#define TILE   8
#define TILES  4                     /* tiles in the tileset */
#define MAPW   64                    /* tilemap size in cells */
#define MAPH   64
#define SPRSZ  16

static uint8_t  tileset[TILE * TILES * TILE];   /* 32x8: TILES tiles across */
static uint16_t tilemap[MAPW * MAPH];           /* u16 cells */
static uint8_t  sprite[SPRSZ * SPRSZ];          /* 16x16, index 0 = transparent */

static int  px = 152, py = 112, facing = 1;
static int  scroll;

static void build_assets(void) {
    /* tileset: 4 distinct 8x8 tiles in palette colours 1,3,5 + a checker */
    for (int t = 0; t < TILES; ++t)
        for (int y = 0; y < TILE; ++y)
            for (int x = 0; x < TILE; ++x) {
                uint8_t c;
                switch (t) {
                    case 0: c = 1; break;                                  /* solid */
                    case 1: c = 3; break;
                    case 2: c = ((x ^ y) & 1) ? 5 : 1; break;              /* checker */
                    default: c = (x==0||y==0||x==TILE-1||y==TILE-1)?6:0; break; /* framed */
                }
                tileset[(y) * (TILE*TILES) + (t*TILE + x)] = c;
            }
    /* tilemap: a varied pattern */
    for (int y = 0; y < MAPH; ++y)
        for (int x = 0; x < MAPW; ++x)
            tilemap[y*MAPW + x] = (uint16_t)(((x*3 + y*5) >> 1) & (TILES - 1));

    /* sprite: a round-ish blob (index 0 transparent), facing right */
    for (int y = 0; y < SPRSZ; ++y)
        for (int x = 0; x < SPRSZ; ++x) {
            int dx = x - 8, dy = y - 8;
            int r2 = dx*dx + dy*dy;
            uint8_t c = 0;
            if (r2 < 49)      c = 8;        /* body */
            else if (r2 < 64) c = 7;        /* outline */
            if (y < 8 && x > 9 && x < 13 && r2 < 49) c = 0; /* a notch = an eye */
            sprite[y*SPRSZ + x] = c;
        }
}

static void frame(void) {
    uint32_t p = cron_pad(0);
    if (p & CRON_BTN_LEFT)  { px -= 2; facing = -1; }
    if (p & CRON_BTN_RIGHT) { px += 2; facing =  1; }
    if (p & CRON_BTN_UP)    py -= 2;
    if (p & CRON_BTN_DOWN)  py += 2;
    if (px < 0) px = 0; if (px > CRON_SCREEN_W - SPRSZ) px = CRON_SCREEN_W - SPRSZ;
    if (py < 0) py = 0; if (py > CRON_SCREEN_H - SPRSZ) py = CRON_SCREEN_H - SPRSZ;
    scroll++;

    cron_cls(0);
    /* scrolling tilemap background — bltm wraps the source modulo the map
     * size, so the full 512px map scrolls past endlessly. */
    cron_bltm(0, 0, 0, scroll & (MAPW*TILE - 1), (scroll >> 1) & (MAPH*TILE - 1),
              CRON_SCREEN_W, CRON_SCREEN_H, -1);
    /* the sprite, colour-keyed on 0, h-flipped when facing left */
    cron_blt(1, px, py, 0, 0, facing > 0 ? SPRSZ : -SPRSZ, SPRSZ, 0);
    /* HUD */
    cron_rectb(2, 2, 96, 12, 6);
    cron_text("CRONOPIO 2D", 11, 6, 4, 7);
    cron_circ(300, 16, 6, 9);
}

int main(void) {
    if (cron_resolve_video() != 0) return 1;
    build_assets();
    cron_image(0, tileset, TILE * TILES, TILE);   /* tileset image bank */
    cron_image(1, sprite,  SPRSZ, SPRSZ);         /* sprite image bank */
    cron_tilemap(0, tilemap, MAPW, MAPH, 0);      /* tilemap drawn from bank 0 */
    cron_set_frame(frame);
    return 0;
}
