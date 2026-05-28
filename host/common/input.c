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

/* ----- mouse ------------------------------------------------------------- */

/* Push one motion event from the host: absolute pos in cart 320x240 coords
 * (clamped by the caller) and relative deltas in cart coords. Deltas are
 * ACCUMULATED until the cart reads them with cron_input_consume_mouse_delta. */
void cron_input_mouse_motion(cronopio_console_t* c, int abs_x, int abs_y,
                             int dx, int dy) {
    c->mouse_x = abs_x;
    c->mouse_y = abs_y;
    c->mouse_dx += dx;
    c->mouse_dy += dy;
}

/* Push a button edge: `button` is the bit index (0=L, 1=R, 2=M, 3=X1, 4=X2),
 * `down` is 1 for press / 0 for release. */
void cron_input_mouse_button(cronopio_console_t* c, int button, int down) {
    if ((unsigned)button >= 5) return;
    uint32_t bit = 1u << button;
    if (down) c->mouse_buttons |=  bit;
    else      c->mouse_buttons &= ~bit;
}

/* Accumulate wheel ticks (positive = wheel up / scroll away from user). */
void cron_input_mouse_wheel(cronopio_console_t* c, int ticks) {
    c->mouse_wheel += ticks;
}

/* Cart-controlled state: cursor visibility + relative mouse mode. The desktop
 * host syncs these to SDL each frame (SDL_ShowCursor / SDL_SetRelativeMouseMode);
 * the headless host ignores them. */
void cron_input_set_cursor_visible(cronopio_console_t* c, int show) {
    c->cursor_visible = show ? 1 : 0;
}
void cron_input_set_mouse_relative(cronopio_console_t* c, int enable) {
    c->mouse_relative = enable ? 1 : 0;
}

/* Consume-on-read helpers for the syscall layer: copy the accumulator out
 * and zero it, so each call reports motion since the previous call. */
void cron_input_consume_mouse_delta(cronopio_console_t* c, int32_t* dx, int32_t* dy) {
    if (dx) *dx = c->mouse_dx;
    if (dy) *dy = c->mouse_dy;
    c->mouse_dx = c->mouse_dy = 0;
}
int32_t cron_input_consume_mouse_wheel(cronopio_console_t* c) {
    int32_t w = c->mouse_wheel;
    c->mouse_wheel = 0;
    return w;
}
