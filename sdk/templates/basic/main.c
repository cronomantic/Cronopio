/* @NAME@ — a Cronopio cartridge.
 *
 * Build:  cronopio-cc main.c -o @NAME@.bin   (or: cmake -B build && cmake --build build)
 * Run:    cronopio @NAME@.bin
 */
#include <cronopio.h>

static int32_t t;

void setup(void) {
    static const char boot[] = "@NAME@ booting\n";
    cron_log(boot, (int32_t)sizeof(boot) - 1);
}

void frame(void) {
    cron_cls(1);                                    /* clear to colour 1   */
    int32_t x = (t * 2) % (CRON_SCREEN_W - 32);
    cron_rect(x, 100, 32, 32, 7);                   /* a moving box        */
    static const char hi[] = "@NAME@";
    cron_text(hi, (int32_t)sizeof(hi) - 1, 8, 8, 15);
    if (cron_pad(0) & CRON_BTN_A) cron_cls(8);      /* hold A to flash     */
    t++;
}

CRONOPIO_CART_INIT(setup, frame)
