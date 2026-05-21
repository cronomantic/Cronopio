/* @NAME@ — a Cronopio sprite cartridge.
 *
 * Builds one 8x8 sprite into image bank 0 at boot, then blits it each frame
 * with colour-key transparency, moved by the d-pad. See docs/cartridge.md
 * "Sprites & tilemaps" for tilemaps, flipping and rotozoom.
 *
 * Build:  cronopio-cc main.c -o @NAME@.bin
 * Run:    cronopio @NAME@.bin   (move with arrows / WASD)
 */
#include <cronopio.h>

#define SPR 8

static uint8_t sprite[SPR * SPR];          /* the sprite's pixels       */
static int32_t px = 156, py = 116;         /* sprite position           */

void setup(void) {
    /* A little diamond: colour 0 is the transparent key. */
    for (int y = 0; y < SPR; ++y)
        for (int x = 0; x < SPR; ++x) {
            int d = (x < 4 ? 3 - x : x - 4) + (y < 4 ? 3 - y : y - 4);
            sprite[y * SPR + x] = (d < 3) ? 10 : (d < 4 ? 9 : 0);
        }
    cron_image(0, sprite, SPR, SPR);       /* register as image bank 0  */
}

void frame(void) {
    cron_cls(1);

    uint32_t pad = cron_pad(0);
    if (pad & CRON_BTN_LEFT)  px -= 2;
    if (pad & CRON_BTN_RIGHT) px += 2;
    if (pad & CRON_BTN_UP)    py -= 2;
    if (pad & CRON_BTN_DOWN)  py += 2;

    /* blt(bank, dst x/y, src x/y, w, h, colourkey). colourkey 0 = skip. */
    cron_blt(0, px, py, 0, 0, SPR, SPR, 0);

    static const char hi[] = "@NAME@ - move with the d-pad";
    cron_text(hi, (int32_t)sizeof(hi) - 1, 8, 8, 15);
}

CRONOPIO_CART_INIT(setup, frame)
