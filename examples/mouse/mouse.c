/* Mouse demo — exercises the whole mouse API: position, buttons (L/R/M/X1/X2),
 * relative motion, wheel, cursor hide, and SDL relative-mouse mode.
 *
 *   LEFT mouse  — paint a 3x3 splat under the cursor (in the current colour)
 *   RIGHT       — clear the canvas
 *   MIDDLE      — toggle relative-mouse mode (mouselook): the OS cursor is
 *                 hidden + locked; the cart-drawn cursor moves by accumulated
 *                 cron_mouse_delta deltas instead of cron_mouse's absolute.
 *   Wheel up/dn — cycle the paint colour (palette indices 1..31)
 *
 * The cart also hides the OS cursor on boot via cron_cursor(0) and draws its
 * own pixel cross — the canonical pattern for any cart that wants a custom
 * pointer. The HUD (top-left) shows the colour swatch, the absolute/relative
 * mode tag, and a 5-square button strip lit per button currently held.
 *
 * Build via CronopioCart.cmake (see CMakeLists.txt); run as:
 *
 *   ./cronopio mouse.bin
 */

#include <cronopio.h>

volatile uint8_t  *CRON_FB  = 0;
volatile uint32_t *CRON_PAL = 0;

static uint8_t  g_color     = 8;                       /* bright red to start */
static int32_t  g_ax        = CRON_SCREEN_W / 2;       /* virtual cursor in relative mode */
static int32_t  g_ay        = CRON_SCREEN_H / 2;
static int      g_relative  = 0;
static int      g_inited    = 0;
static uint32_t g_prev_btns = 0;

static void put_px(int32_t x, int32_t y, uint8_t c) {
    if ((uint32_t)x < (uint32_t)CRON_SCREEN_W && (uint32_t)y < (uint32_t)CRON_SCREEN_H)
        CRON_FB[y * CRON_SCREEN_W + x] = c;
}

static void clear_canvas(void) {
    for (int32_t i = 0; i < CRON_SCREEN_W * CRON_SCREEN_H; ++i) CRON_FB[i] = 0;
}

static void splat(int32_t x, int32_t y, uint8_t c) {
    for (int32_t dy = -1; dy <= 1; ++dy)
        for (int32_t dx = -1; dx <= 1; ++dx)
            put_px(x + dx, y + dy, c);
}

static void cursor_cross(int32_t x, int32_t y) {
    /* 5-pixel cross — white centre, black ring so it shows over any colour. */
    put_px(x, y, 7);
    put_px(x - 1, y, 7); put_px(x + 1, y, 7);
    put_px(x, y - 1, 7); put_px(x, y + 1, 7);
    put_px(x - 2, y, 0); put_px(x + 2, y, 0);
    put_px(x, y - 2, 0); put_px(x, y + 2, 0);
}

static void filled_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint8_t c) {
    for (int32_t yy = 0; yy < h; ++yy)
        for (int32_t xx = 0; xx < w; ++xx)
            put_px(x + xx, y + yy, c);
}

static void hud(uint32_t buttons) {
    /* HUD background panel so paint underneath doesn't bleed through. */
    filled_rect(0, 0, 96, 14, 5);                          /* dark grey */

    /* Colour swatch — solid 8x8 in the current paint colour, white border. */
    filled_rect(3, 3, 8, 8, g_color);
    for (int32_t i = 2; i <= 11; ++i) {
        put_px(i, 2, 7); put_px(i, 11, 7);
        put_px(2, i, 7); put_px(11, i, 7);
    }

    /* Mode tag: ABS or REL. */
    static const char abs_tag[] = "ABS";
    static const char rel_tag[] = "REL";
    if (g_relative) cron_text(rel_tag, 3, 16, 4, 7);
    else            cron_text(abs_tag, 3, 16, 4, 7);

    /* Button strip: 5 squares (L, R, M, X1, X2), lit green when held. */
    for (int b = 0; b < 5; ++b) {
        int32_t bx = 40 + b * 10;
        uint8_t col = (buttons & (1u << b)) ? 11 : 1;      /* green / dark blue */
        filled_rect(bx, 4, 6, 6, col);
    }
}

static void frame(void) {
    /* First-frame setup: hide the OS cursor (we draw our own) + black canvas. */
    if (!g_inited) {
        cron_cursor(0);
        clear_canvas();
        g_inited = 1;
    }

    int32_t  mx, my;
    uint32_t buttons = cron_mouse(&mx, &my);

    int32_t dx, dy;
    cron_mouse_delta(&dx, &dy);

    int32_t wheel = cron_mouse_wheel();

    /* Wheel → cycle paint colour by one index per tick (clang collapses
     * multi-tick `while` loops into llvm.usub.sat.i32 which the translator
     * doesn't lower — single-step wrap-around does the job for a demo). */
    if (wheel > 0) {
        int32_t nc = (int32_t)g_color + 1;
        if (nc > 31) nc = 1;
        g_color = (uint8_t)nc;
    } else if (wheel < 0) {
        int32_t nc = (int32_t)g_color - 1;
        if (nc < 1) nc = 31;
        g_color = (uint8_t)nc;
    }

    uint32_t pressed = buttons & ~g_prev_btns;

    /* RIGHT pressed → clear the canvas (HUD is redrawn after, so it survives). */
    if (pressed & 2) clear_canvas();

    /* MIDDLE pressed → toggle relative mode. Centre the virtual cursor on
     * each switch so it's not stuck at an edge after toggling. */
    if (pressed & 4) {
        g_relative = !g_relative;
        cron_mouse_relative(g_relative);
        g_ax = CRON_SCREEN_W / 2;
        g_ay = CRON_SCREEN_H / 2;
    }

    /* Where to draw the cursor: in relative mode integrate dx/dy ourselves
     * (the absolute position is meaningless while SDL has the cursor locked);
     * in absolute mode the host-mapped (mx,my) is canonical. */
    int32_t cx, cy;
    if (g_relative) {
        g_ax += dx; g_ay += dy;
        if (g_ax < 0) g_ax = 0; else if (g_ax >= CRON_SCREEN_W) g_ax = CRON_SCREEN_W - 1;
        if (g_ay < 0) g_ay = 0; else if (g_ay >= CRON_SCREEN_H) g_ay = CRON_SCREEN_H - 1;
        cx = g_ax; cy = g_ay;
    } else {
        cx = mx; cy = my;
    }

    /* LEFT held → paint a splat at the cursor. Persistent (lives in CRON_FB). */
    if (buttons & 1) splat(cx, cy, g_color);

    /* HUD on top of the canvas. */
    hud(buttons);

    /* Cursor on top of everything. */
    cursor_cross(cx, cy);

    g_prev_btns = buttons;
}

int main(void) {
    if (cron_resolve_video() != 0) {
        cron_log("mouse: fb/pal regions missing — built without --region?\n", 56);
        return 1;
    }
    cron_log("mouse demo starting\n", 20);
    cron_set_frame(frame);
    return 0;
}
