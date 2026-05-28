/* 2D capabilities demo — exercises the three new draw primitives:
 *
 *   cron_bltm           with the new tilemap u16 layout (HFLIP/VFLIP)
 *   cron_bltm_raster    with a per-scanline scroll wave (linescroll)
 *   cron_bltm_affine    with a Mode-7-style floor
 *   cron_blt_flip       sprite flipping per facing direction
 *
 * Layout (top to bottom on the 320x240 screen):
 *   y =   0..79   : sky tilemap with sine-wave horizontal linescroll
 *   y =  80..199  : Mode-7 perspective floor (one tilemap warped per-line)
 *   y = 200..239  : opaque ground band (cron_rect)
 *   plus: a small character sprite at the horizon, flipped H per facing
 *
 * Resources are built procedurally — no external assets, so the example
 * is self-contained and verifiable headless via PPM colour histograms.
 */

#include <cronopio.h>

volatile uint8_t  *CRON_FB  = 0;
volatile uint32_t *CRON_PAL = 0;

/* ---------------------------------------------------------------------- */
/* Resources */

#define TILE   8
#define SKYW   32    /* sky tilemap: 32 cells wide × 4 tall = 256x32 px */
#define SKYH   4
#define FLOORW 16    /* floor tilemap: 16x16 cells = 128x128 px (wraps) */
#define FLOORH 16
#define SPRSZ  16

/* Tileset: 8 distinct 8x8 tiles. One bank holds them all (sky uses 0..3,
 * floor uses 4..7). Sized 64x8 = 8 tiles across, 1 tile down. */
static uint8_t   tileset[TILE * 8 * TILE];      /* 64x8 */
static uint16_t  sky_map[SKYW * SKYH];
static uint16_t  floor_map[FLOORW * FLOORH];
static uint8_t   sprite_img[SPRSZ * SPRSZ];

static cron_raster_t raster_table[CRON_SCREEN_H];   /* sine-wave linescroll */
static cron_affine_t affine_table[CRON_SCREEN_H];   /* Mode-7 perspective */

/* ---------------------------------------------------------------------- */
/* Asset build */

static void build_tileset(void) {
    for (int t = 0; t < 8; ++t) {
        for (int y = 0; y < TILE; ++y) {
            for (int x = 0; x < TILE; ++x) {
                uint8_t c;
                switch (t) {
                    /* Sky tiles (clouds-ish): colours 1, 3 */
                    case 0: c = 1; break;
                    case 1: c = ((x + y) & 3) < 2 ? 1 : 3; break;
                    case 2: c = ((x ^ y) & 1) ? 3 : 1; break;
                    case 3: c = 3; break;
                    /* Floor tiles (checkerboard variants): colours 6, 8 */
                    case 4: c = 6; break;
                    case 5: c = ((x >> 1) ^ (y >> 1)) & 1 ? 8 : 6; break;
                    case 6: c = ((x + y) & 3) == 0 ? 14 : 6; break;
                    default: c = 8; break;
                }
                tileset[y * (TILE * 8) + (t * TILE + x)] = c;
            }
        }
    }
}

static void build_sky_map(void) {
    /* Use HFLIP on every other column to demonstrate the new flip bits.
     * Tiles cycle through 0..3 with HFLIP on odd x — even when the same
     * underlying tile, the visual alternates. */
    for (int y = 0; y < SKYH; ++y) {
        for (int x = 0; x < SKYW; ++x) {
            uint16_t idx = (uint16_t)((x + y) & 3);
            uint16_t flip = (x & 1) ? CRON_TILE_HFLIP : 0;
            sky_map[y * SKYW + x] = idx | flip;
        }
    }
}

static void build_floor_map(void) {
    /* Checkerboard of tiles 4..7. */
    for (int y = 0; y < FLOORH; ++y) {
        for (int x = 0; x < FLOORW; ++x) {
            int t = ((x + y) & 1) ? 5 : 6;
            floor_map[y * FLOORW + x] = (uint16_t)(t & CRON_TILE_IDX_MASK);
        }
    }
}

static void build_sprite(void) {
    /* A small "character" — palette index 0 is transparent (colkey 0).
     * Body in 11, eye/highlight in 7 so HFLIP is visible (asymmetric). */
    for (int y = 0; y < SPRSZ; ++y) {
        for (int x = 0; x < SPRSZ; ++x) {
            int dx = x - 7, dy = y - 8;
            int r2 = dx * dx + dy * dy;
            uint8_t c = 0;
            if (r2 < 49) c = 11;
            /* eye on right side (asymmetric -> flip is visible) */
            if (x >= 9 && x <= 11 && y >= 5 && y <= 7) c = 7;
            sprite_img[y * SPRSZ + x] = c;
        }
    }
}

/* ---------------------------------------------------------------------- */
/* Frame */

static int t_frames = 0;

/* Q16.16 fixed-point sin/cos approximation — small lookup, no libm. */
static const int16_t sin_q16[64] = {
    /* sin(2*pi*i/64) * 32767, i = 0..63 */
        0,  3211,  6392,  9512, 12539, 15446, 18204, 20787,
    23170, 25329, 27245, 28898, 30273, 31356, 32137, 32609,
    32767, 32609, 32137, 31356, 30273, 28898, 27245, 25329,
    23170, 20787, 18204, 15446, 12539,  9512,  6392,  3211,
        0, -3211, -6392, -9512,-12539,-15446,-18204,-20787,
   -23170,-25329,-27245,-28898,-30273,-31356,-32137,-32609,
   -32767,-32609,-32137,-31356,-30273,-28898,-27245,-25329,
   -23170,-20787,-18204,-15446,-12539, -9512, -6392, -3211,
};

static int isin(int phase) {
    return sin_q16[phase & 63];
}

static void fill_raster_wave(int phase) {
    /* Per-line scroll_x = sin(y/period + phase) * amplitude.
     * Reset everything else so unused fields are well-defined. */
    for (int y = 0; y < CRON_SCREEN_H; ++y) {
        int s = isin((y >> 2) + phase) >> 11;     /* /2048: amplitude ~16 px */
        raster_table[y].scroll_x  = (int16_t)s;
        raster_table[y].scroll_y  = 0;
        raster_table[y].pal_offset = 0;
        raster_table[y].flags     = 0;
        raster_table[y]._pad      = 0;
    }
}

static void fill_affine_floor(int cam_x) {
    /* SNES Mode-7 recipe (simplified). The floor is drawn from y=80 to
     * y=199, with y=80 being the horizon line. For each scanline y,
     * perspective foreshortens the texture:
     *   distance = focal / (y - horizon)
     * The texture moves with cam_x; we keep cam_y fixed at 0 for now.
     *
     * Implemented as: for each scanline j in 80..199, u/v at screen x=0
     * place the (camera_x, distance) point in tilemap space; du/dv span
     * one screen-pixel = (1/scale) texels in tilemap space.
     *
     * scale_q16 below is "texels per screen pixel along x", which for a
     * floor receding into the distance grows linearly with (y - horizon). */
    const int horizon = 80;
    const int focal = 100;     /* fake camera focal length in tiles */
    for (int y = 0; y < CRON_SCREEN_H; ++y) {
        /* Zero out lines outside the floor band so a stray draw is safe. */
        affine_table[y].u  = 0; affine_table[y].v  = 0;
        affine_table[y].du = 0; affine_table[y].dv = 0;
    }
    for (int y = horizon; y < CRON_SCREEN_H; ++y) {
        int row = y - horizon;
        if (row <= 0) row = 1;
        /* distance from camera, in tilemap pixels — bigger row → closer.
         * texels-per-screen-pixel scale = (FLOORW*TILE) / (row * focal) */
        int scale_num = FLOORW * TILE * 65536;     /* Q16.16 */
        int scale = scale_num / (row * focal);     /* texels/pixel, Q16.16 */
        /* v: tilemap-y, fixed per scanline (camera at z=0 looking down). */
        int v_tiles = (FLOORH * TILE * focal) / row;
        affine_table[y].u  = cam_x * scale - 160 * scale; /* centre screen at u origin */
        affine_table[y].v  = v_tiles << 16;
        affine_table[y].du = scale;
        affine_table[y].dv = 0;
    }
}

static void frame(void) {
    ++t_frames;

    /* --- Pre-clear FB to colour 0 (transparent for sprite) ----------- */
    cron_rect(0, 0, CRON_SCREEN_W, CRON_SCREEN_H, 0);

    /* --- Sky band with linescroll wave (uses cron_bltm_raster) ------- */
    fill_raster_wave(t_frames);
    cron_bltm_raster(0, /* tilemap slot 0 = sky */
                     0, 0,                              /* dx, dy */
                     t_frames << 0, 0,                  /* base scroll: slow drift */
                     CRON_SCREEN_W, 80,                 /* w, h */
                     -1,                                /* opaque */
                     raster_table);

    /* --- Mode-7 floor (uses cron_bltm_affine) ----------------------- */
    fill_affine_floor(t_frames * 4);
    cron_bltm_affine(1,                                 /* tilemap slot 1 = floor */
                     0, 80,                             /* dx, dy = below horizon */
                     CRON_SCREEN_W, 120,                /* w, h = floor band */
                     -1,
                     affine_table);

    /* --- Ground edge --- */
    cron_rect(0, 200, CRON_SCREEN_W, 40, 13);

    /* --- Character sprite, flipped H every 64 frames ---------------- */
    int facing_left = (t_frames & 64) != 0;
    int sx = 160 - SPRSZ / 2;
    int sy = 80 - SPRSZ;     /* stand right at the horizon */
    cron_blt_flip(0,         /* image slot */
                  sx, sy,
                  0, 0, SPRSZ, SPRSZ,
                  0,         /* colour key (index 0 transparent) */
                  facing_left ? CRON_BLT_HFLIP : 0);

    /* --- Approaching "boss": breathing scale ------------------------ */
    /* scale oscillates 1.0..3.0 over 64 frames via isin(phase) — gives a
     * smooth zoom in/out. Anchored at top-left, so it expands rightward
     * and downward. We compensate by shifting the dest position so the
     * centre stays at (40, 40). */
    int s_phase = (t_frames * 2) & 63;
    int s_raw   = isin(s_phase);                        /* -32767..32767 */
    /* Map to Q16.16 scale in 1.0..2.0 — note (s_raw + 32767) is already in
     * [0..65534], which is essentially 0..0x10000 in Q16.16, so we can
     * just add it as-is to 1.0. Avoids the 32-bit overflow that comes
     * with (sval * 0x10000). */
    int scale   = 0x10000 + (s_raw + 32767);            /* 1.0..1.9999 in Q16.16 */
    int scaled_sz = (SPRSZ * scale) >> 16;
    int boss_x = 40 - scaled_sz / 2;
    int boss_y = 40 - scaled_sz / 2;
    cron_blt_scale(0,                                   /* same sprite img */
                   boss_x, boss_y,
                   0, 0, SPRSZ, SPRSZ,
                   0,                                   /* colour key */
                   scale,
                   0);                                  /* no flip */

    /* --- HUD --- */
    cron_text("CRONOPIO 2D", 11, 8, 8, 7);
}

int main(void) {
    if (cron_resolve_video() != 0) {
        const char *msg = "2d: fb/pal regions missing\n";
        cron_log(msg, 26);
        return 1;
    }
    build_tileset();
    build_sky_map();
    build_floor_map();
    build_sprite();

    /* Register banks:
     *   image slot 0 = the sprite (16x16)
     *   image slot 1 = the tileset (64x8 — 8 tiles)
     *   tilemap slot 0 = sky (32x4, uses image bank 1)
     *   tilemap slot 1 = floor (16x16, uses image bank 1)
     */
    cron_image  (0, sprite_img,  SPRSZ, SPRSZ);
    cron_image  (1, tileset,     TILE * 8, TILE);
    cron_tilemap(0, sky_map,     SKYW,  SKYH,  1);
    cron_tilemap(1, floor_map,   FLOORW, FLOORH, 1);

    cron_set_frame(frame);
    return 0;
}
