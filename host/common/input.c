/* Input state — the platform shell pushes button bitmasks into here
 * each frame via cron_input_set_pad; syscalls read from the same fields. */

#include "console.h"

void cron_input_set_pad(cronopio_console_t* c, int player, uint32_t mask) {
    if ((unsigned)player >= CRONOPIO_PAD_COUNT) return;
    c->pad_cur[player] = mask;
}

uint32_t cron_input_pad(const cronopio_console_t* c, int player) {
    if ((unsigned)player >= CRONOPIO_PAD_COUNT) return 0;
    return c->pad_cur[player];
}

uint32_t cron_input_pad_pressed(const cronopio_console_t* c, int player) {
    if ((unsigned)player >= CRONOPIO_PAD_COUNT) return 0;
    return c->pad_cur[player] & ~c->pad_prev[player];
}

uint32_t cron_input_pad_released(const cronopio_console_t* c, int player) {
    if ((unsigned)player >= CRONOPIO_PAD_COUNT) return 0;
    return c->pad_prev[player] & ~c->pad_cur[player];
}
