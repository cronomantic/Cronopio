/* "Hello, Cronopio" — minimal cartridge.
 *
 * Demonstrates the two valid drawing styles:
 *   1. direct framebuffer writes (the fast path)
 *   2. helper syscalls (cron_cls, cron_rect, cron_text)
 *
 * Build with the CronoVM toolchain (or via CronopioCart.cmake which adds
 * the right --region flags automatically):
 *
 *   cvm-cc -I <repo>/sdk/include \
 *          --heap-reserve=1M --stack-reserve=64K \
 *          --region=fb:76800:rw --region=pal:128:rw \
 *          hello.c -o hello.bin
 *
 * Run with the Cronopio host:
 *
 *   ./cronopio hello.bin
 *
 * Controls (from the desktop shell): arrows / WASD = D-pad, Z/X/C/V = A/B/X/Y,
 * Esc closes the window. */

#include <cronopio.h>

/* CRON_FB / CRON_PAL are declared volatile-pointer in cronopio.h; reserve
 * their definition here so the cart has the storage cron_resolve_video()
 * writes into. */
volatile uint8_t  *CRON_FB  = 0;
volatile uint32_t *CRON_PAL = 0;

static int32_t t;

static void frame(void) {
    /* Background — direct memory-mapped framebuffer writes. */
    for (int y = 0; y < CRON_SCREEN_H; ++y) {
        uint8_t c = (uint8_t)(((y >> 3) + (t >> 2)) & 0x1F);
        for (int x = 0; x < CRON_SCREEN_W; ++x) {
            CRON_FB[y * CRON_SCREEN_W + x] = c;
        }
    }

    /* A bouncing box — drawn through the helper syscall. */
    int32_t bx = (int32_t)((t * 3) % (CRON_SCREEN_W - 32));
    int32_t by = (int32_t)((t * 2) % (CRON_SCREEN_H - 32));
    cron_rect(bx, by, 32, 32, 7);

    /* A label. */
    static const char msg[] = "HELLO, CRONOPIO";
    cron_text(msg, (int32_t)sizeof(msg) - 1, 8, 8, 15);

    /* Press A to beep. */
    if (cron_pad_pressed(0) & CRON_BTN_A) {
        cron_snd_tone(0, CRON_WAVE_SQUARE, 440000, 80, 0);
    }
    if (cron_pad_released(0) & CRON_BTN_A) {
        cron_snd_stop(0);
    }

    t++;
}

int main(void) {
    if (cron_resolve_video() != 0) {
        cron_log("hello: fb/pal regions missing — built without --region?\n", 56);
        return 1;
    }
    cron_log("hello cart starting\n", 20);
    cron_set_frame(frame);
    return 0;
}
