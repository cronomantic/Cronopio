#ifndef CRONOPIO_MENU_H
#define CRONOPIO_MENU_H

/* The desktop host's in-app system menu — an overlay drawn on top of the cart
 * framebuffer (independent of the cart's 8bpp palette: it renders at window
 * resolution with an SDL glyph atlas). Toggled with F1; while open the cart is
 * paused by the caller. Screens:
 *
 *   Main      -> Load cartridge / Reset / Controls / Joystick / Resume / Quit
 *   Browse    -> a *.bin file picker rooted at a directory
 *   Controls  -> rebind each of the 8 pad buttons to a keyboard key
 *   Joystick  -> rebind each of the 8 pad buttons to a controller button
 *
 * It is navigable by both keyboard and game controller. The menu owns no cart
 * state: it asks the host to load / reset / quit through the callback table. */

#include "hostcfg.h"
#include <SDL.h>

/* Callbacks the menu uses to act on the host. `ud` is opaque host context. */
typedef struct {
    void* ud;
    int  (*load_cart)(void* ud, const char* path); /* 0 = ok, <0 = failed */
    void (*reset_cart)(void* ud);
    void (*quit)(void* ud);
    void (*apply_video)(void* ud);   /* re-apply cfg.scale/fullscreen/vsync live */
} menu_host_t;

typedef struct menu menu_t;     /* opaque; allocate one with menu_create */

menu_t* menu_create(SDL_Renderer* ren, int win_w, int win_h,
                    host_cfg_t* cfg, const menu_host_t* host);
void    menu_destroy(menu_t* m);

int  menu_is_open(const menu_t* m);

/* Open the menu. start_dir seeds the file browser; joy_name is the connected
 * controller's display name (NULL if none). has_cart gates Resume/Reset. */
void menu_open (menu_t* m, const char* start_dir, const char* joy_name, int has_cart);
void menu_close(menu_t* m);

/* Tell the menu a controller (un)plugged so its Joystick screen stays current. */
void menu_set_joy_name(menu_t* m, const char* name);

/* Tell the menu the current output size (window pixels) so its overlay lays out
 * correctly after a window resize / fullscreen toggle. */
void menu_set_size(menu_t* m, int win_w, int win_h);

/* Feed an SDL event while open. Returns 1 if the menu consumed it. */
int  menu_handle_event(menu_t* m, const SDL_Event* ev);

/* Draw the overlay (call after the cart frame has been blitted, before
 * SDL_RenderPresent). */
void menu_render(menu_t* m);

#endif /* CRONOPIO_MENU_H */
