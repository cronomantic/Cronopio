#ifndef CRONOPIO_HOSTCFG_H
#define CRONOPIO_HOSTCFG_H

/* Desktop-host configuration: the keyboard / game-controller bindings for the
 * console's 8-button virtual pad, plus a little UI persistence (last browse
 * directory). Loaded at startup and rewritten whenever the system menu closes,
 * so remaps and the controller choice survive across runs.
 *
 * SDL-only: this lives in host/desktop, the common/ layer never sees SDL. */

#include <SDL.h>

/* The 8 logical pad buttons, in the bit order the console expects (matches the
 * historical hardcoded map_keys_to_pad: d-pad then A/B/X/Y). cron_input_set_pad
 * receives a mask of (1u << PAD_*). */
enum {
    PAD_UP = 0, PAD_DOWN, PAD_LEFT, PAD_RIGHT,
    PAD_A, PAD_B, PAD_X, PAD_Y,
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
} host_cfg_t;

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
