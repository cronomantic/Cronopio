#ifndef CRONOPIO_HOSTCFG_H
#define CRONOPIO_HOSTCFG_H

/* Desktop-host configuration: the keyboard / game-controller bindings for the
 * console's 8-button virtual pad, plus a little UI persistence (last browse
 * directory). Loaded at startup and rewritten whenever the system menu closes,
 * so remaps and the controller choice survive across runs.
 *
 * SDL-only: this lives in host/desktop, the common/ layer never sees SDL. */

#include <SDL.h>

/* The 12 logical pad buttons (SNES-style: d-pad, A/B/X/Y, L/R shoulders,
 * Start/Select), in the bit order the console expects. These indices MUST match
 * the CRON_BTN_* bit positions in the SDK (cronopio.h): the console pad word is
 * a mask of (1u << PAD_*) and the cart reads it as (1u << <bit>). */
enum {
    PAD_UP = 0, PAD_DOWN, PAD_LEFT, PAD_RIGHT,
    PAD_A, PAD_B, PAD_X, PAD_Y,
    PAD_L, PAD_R, PAD_START, PAD_SELECT,   /* SNES-style 12-button extension */
    PAD_BTN_COUNT
};

typedef struct {
    /* Keyboard scancode bound to each pad button, or SDL_SCANCODE_UNKNOWN. */
    SDL_Scancode key[PAD_BTN_COUNT];
    /* Game-controller button bound to each pad button, or
     * SDL_CONTROLLER_BUTTON_INVALID. The analog sticks are mapped to the d-pad
     * unconditionally in main.c (a fixed deadzone), so only buttons are stored. */
    SDL_GameControllerButton gbtn[PAD_BTN_COUNT];

    /* Preferred controller, by SDL joystick GUID string ("" = first opened). */
    char joy_guid[40];
    /* Last directory the file browser showed ("" = start in the cwd). */
    char last_dir[1024];

    /* Video. scale = integer window multiple of the 320x240 framebuffer (1..6);
     * fullscreen = desktop fullscreen (the cart is letterboxed 4:3); vsync =
     * present synced to the display. All persisted + settable on the CLI and in
     * the F1 menu's Video screen. */
    int scale;
    int fullscreen;
    int vsync;
} host_cfg_t;

#define HOSTCFG_SCALE_MIN 1
#define HOSTCFG_SCALE_MAX 6

/* Fill cfg with the built-in defaults (arrows+WASD-era layout: arrows -> d-pad,
 * Z/X/C/V -> A/B/X/Y; controller d-pad + A/B/X/Y face buttons). */
void hostcfg_defaults(host_cfg_t* cfg);

/* Load/save the `key=value` text file at `path`. Missing keys keep whatever is
 * already in cfg (so load defaults first, then overlay the file). Returns 0 on
 * success, -1 if the file could not be opened (load) or written (save). */
int  hostcfg_load(host_cfg_t* cfg, const char* path);
int  hostcfg_save(const host_cfg_t* cfg, const char* path);

/* Human-readable button name for the menu ("Up", "A", ...). */
const char* hostcfg_pad_name(int btn);

#endif /* CRONOPIO_HOSTCFG_H */
