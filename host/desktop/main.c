/* Cronopio desktop/web host — SDL2 shell.
 *
 *   1. Slurp the .bin from disk, hand it to cvm_load.
 *   2. Resolve the fb/pal host regions and install Cronopio's syscalls.
 *   3. Run the cart's entry once (it registers a frame fn via cron_set_frame).
 *   4. Each 1/60 s: poll input, cvm_call the frame fn, blit FB, mix audio.
 *
 * On top of that bare runner sits a small in-app SYSTEM MENU (menu.c, toggled
 * with F1): load another cartridge, reset, rebind the pad to the keyboard, and
 * map a host game controller. While the menu is open the cart is paused
 * (its last frame stays on screen) and audio is silenced. Pad bindings and the
 * controller choice persist in cronopio.cfg next to the executable.
 *
 * The per-frame work lives in tick() so both the native blocking loop and
 * Emscripten's requestAnimationFrame-driven callback can share it. */

#include "console.h"
#include "syscalls.h"
#include "cvm.h"
#include "hostcfg.h"
#include "menu.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define SCALE 3

/* Exposed for sys_time_ms in host/common/syscalls.c — kept here so the
 * common layer doesn't pull SDL in. */
uint64_t cronopio_platform_ticks_ms(void) {
    return (uint64_t)SDL_GetTicks();
}

/* Everything the per-frame step and the system menu need. Lives in a single
 * static instance; a pointer is handed to the loop driver and menu callbacks. */
typedef struct {
    cronopio_console_t *console;
    struct cvm_image    img;          /* current cart image */
    uint8_t            *blob;         /* current cart bytes (owned) */
    char                cart_path[1024];
    int                 has_cart;

    SDL_Renderer       *ren;
    SDL_Texture        *tex;
    uint32_t           *rgba;
    SDL_AudioDeviceID   audio_dev;

    SDL_GameController *gc;            /* opened host controller, or NULL */
    char                joy_name[128];

    host_cfg_t          cfg;
    char                cfg_path[1024];
    menu_t             *menu;

    int                 running;
} app_t;

/* ---- input: build the 8-button pad from the configured keyboard + pad ---- */

static uint32_t build_pad(app_t* a, const Uint8* keys) {
    uint32_t m = 0;
    for (int b = 0; b < PAD_BTN_COUNT; ++b) {
        SDL_Scancode sc = a->cfg.key[b];
        if (sc != SDL_SCANCODE_UNKNOWN && keys[sc]) m |= (1u << b);
    }
    if (a->gc) {
        for (int b = 0; b < PAD_BTN_COUNT; ++b) {
            SDL_GameControllerButton gb = a->cfg.gbtn[b];
            if (gb != SDL_CONTROLLER_BUTTON_INVALID &&
                SDL_GameControllerGetButton(a->gc, gb)) m |= (1u << b);
        }
        /* Left analog stick also drives the d-pad (fixed deadzone). */
        const int dz = 16000;
        Sint16 ax = SDL_GameControllerGetAxis(a->gc, SDL_CONTROLLER_AXIS_LEFTX);
        Sint16 ay = SDL_GameControllerGetAxis(a->gc, SDL_CONTROLLER_AXIS_LEFTY);
        if (ax < -dz) m |= (1u << PAD_LEFT);
        if (ax >  dz) m |= (1u << PAD_RIGHT);
        if (ay < -dz) m |= (1u << PAD_UP);
        if (ay >  dz) m |= (1u << PAD_DOWN);
    }
    return m;
}

/* ---- controller (re)discovery ----------------------------------------- */

static void close_controller(app_t* a) {
    if (a->gc) { SDL_GameControllerClose(a->gc); a->gc = NULL; }
    a->joy_name[0] = '\0';
}

/* Open the configured controller (by GUID) or the first available one. */
static void open_controller(app_t* a) {
    close_controller(a);
    int n = SDL_NumJoysticks(), chosen = -1;
    for (int i = 0; i < n; ++i) {
        if (!SDL_IsGameController(i)) continue;
        if (a->cfg.joy_guid[0]) {
            char g[40];
            SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i), g, sizeof(g));
            if (!strcmp(g, a->cfg.joy_guid)) { chosen = i; break; }
        }
        if (chosen < 0) chosen = i;            /* first controller as fallback */
    }
    if (chosen >= 0) {
        a->gc = SDL_GameControllerOpen(chosen);
        if (a->gc) {
            const char* nm = SDL_GameControllerName(a->gc);
            snprintf(a->joy_name, sizeof(a->joy_name), "%s", nm ? nm : "Controller");
            SDL_Joystick* js = SDL_GameControllerGetJoystick(a->gc);
            SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(js),
                                      a->cfg.joy_guid, sizeof(a->cfg.joy_guid));
        }
    }
    if (a->menu) menu_set_joy_name(a->menu, a->gc ? a->joy_name : NULL);
}

static void audio_cb(void* userdata, Uint8* stream, int len) {
    cronopio_console_t* c = (cronopio_console_t*)userdata;
    int frames = len / (int)sizeof(int16_t) / 2;
    cronopio_console_mix(c, (int16_t*)stream, frames);
}

static uint8_t* slurp(const char* path, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    if (n < 0) { fclose(f); return NULL; }
    uint8_t* buf = (uint8_t*)malloc((size_t)n);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(buf); return NULL; }
    *out_len = (size_t)n;
    return buf;
}

/* Compose the current scene: cart texture + (optional) menu overlay. */
static void present_all(app_t* a) {
    SDL_RenderClear(a->ren);
    SDL_RenderCopy(a->ren, a->tex, NULL, NULL);
    if (menu_is_open(a->menu)) menu_render(a->menu);
    SDL_RenderPresent(a->ren);
}

/* Refresh the cart texture from the framebuffer region. */
static void refresh_tex(app_t* a) {
    cronopio_console_blit_rgba(a->console, a->img.heap, a->rgba);
    SDL_UpdateTexture(a->tex, NULL, a->rgba, CRONOPIO_SCREEN_W * 4);
}

/* Full standalone present (no menu) — used as the cron_present hook so a cart's
 * loading screen drawn during its blocking entry actually reaches the window. */
static void desktop_present_cb(void* ud) {
    app_t* a = (app_t*)ud;
    refresh_tex(a);
    SDL_RenderClear(a->ren);
    SDL_RenderCopy(a->ren, a->tex, NULL, NULL);
    SDL_RenderPresent(a->ren);
}

/* ---- cart load / reset (also the menu's load_cart/reset_cart callbacks) -- */

/* ---- per-cart save persistence ("memory card") ------------------------- */
/* The cart's save blob lives beside the cart as <cart>.sav. We load it before
 * running the cart (its libc reads the save region at boot) and write it back
 * whenever the cart marks it dirty, atomically (temp + rename). */

static void load_cart_save(app_t* a) {
    cronopio_console_t* c = a->console;
    c->save_len = 0;
    c->save_dirty = 0;
    cronopio_save_reserve(c, CRONOPIO_SAVE_DEFAULT);   /* baseline capacity */
    if (!a->cart_path[0]) return;
    char p[1100];
    snprintf(p, sizeof p, "%s.sav", a->cart_path);
    FILE* f = fopen(p, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz > 0) {
        cronopio_save_reserve(c, (uint32_t)sz);        /* grow to hold the file */
        c->save_len = (uint32_t)fread(c->save, 1, c->save_cap, f);
    }
    fclose(f);
}

static void persist_cart_save(app_t* a) {
    if (!a->has_cart || !a->console->save_dirty || !a->cart_path[0]) return;
    char p[1100], tmp[1112];
    snprintf(p,   sizeof p,   "%s.sav",     a->cart_path);
    snprintf(tmp, sizeof tmp, "%s.sav.tmp", a->cart_path);
    FILE* f = fopen(tmp, "wb");
    if (!f) return;
    size_t n  = a->console->save_len;
    int    ok = (n == 0) || (fwrite(a->console->save, 1, n, f) == n);
    if (fclose(f) != 0) ok = 0;
    if (ok) { remove(p); rename(tmp, p); }   /* swap in atomically-ish */
    else    { remove(tmp); }
    a->console->save_dirty = 0;
}

/* Load the cart at `path`, swapping out any current one. Returns 0 on success;
 * on failure the running cart (if any) is left untouched. Safe to call while
 * audio is live: the swap happens under the audio lock. */
static int app_load_cart(void* ud, const char* path) {
    app_t* a = (app_t*)ud;

    size_t   nlen = 0;
    uint8_t* nblob = slurp(path, &nlen);
    if (!nblob) return -1;

    struct cvm_image nimg;
    int rc = cvm_load(nblob, nlen, &nimg);
    if (rc != CVM_OK) {
        fprintf(stderr, "cvm_load(%s): %s\n", path, cvm_strerror(rc));
        free(nblob);
        return -1;
    }

    /* Flush the outgoing cart's save before tearing its console down. */
    persist_cart_save(a);

    /* Stop the audio thread touching the console while we tear it down. */
    if (a->audio_dev) SDL_LockAudioDevice(a->audio_dev);

    if (a->has_cart) {
        cvm_image_free(&a->img);
        free(a->blob);
    }
    /* Full console reset: drop the old synth, re-init (reloads the BIOS SF2),
     * then re-establish the host hooks console_init clears. */
    cron_synth_destroy(a->console->synth);
    cronopio_save_free(a->console);   /* free the outgoing cart's save buffer */
    cronopio_console_init(a->console);
    a->console->boot_ms    = SDL_GetTicks();
    a->console->present_cb = desktop_present_cb;
    a->console->present_ud = a;

    a->img      = nimg;
    a->blob     = nblob;
    a->has_cart = 1;
    snprintf(a->cart_path, sizeof(a->cart_path), "%s", path);

    if (cronopio_resolve_video_regions(&a->img, a->console) != 0)
        fprintf(stderr, "warning: cart declares no 'fb'/'pal' — drawing no-ops.\n");
    cronopio_syscalls_install(&a->img, a->console);

    /* Seed the save region from <cart>.sav before the cart's entry runs (its
     * libc reads the save blob at boot). */
    load_cart_save(a);

    if (a->audio_dev) SDL_UnlockAudioDevice(a->audio_dev);

    /* Run the cart's entry (registers the frame fn; a DOOM cart blocks here for
     * seconds loading the WAD, painting a loading screen via cron_present). */
    int32_t entry_ret = 0;
    rc = cvm_run(&a->img, &entry_ret);
    if (rc != CVM_OK && rc != CVM_E_SYSCALL_TRAP) {
        fprintf(stderr, "cart entry trapped: %s\n", cvm_strerror(rc));
        return -1;
    }
    refresh_tex(a);
    return 0;
}

static void app_reset_cart(void* ud) {
    app_t* a = (app_t*)ud;
    if (a->has_cart) {
        char path[1024];
        snprintf(path, sizeof(path), "%s", a->cart_path);
        app_load_cart(a, path);
    }
}

static void app_quit(void* ud) { ((app_t*)ud)->running = 0; }

/* ---- the per-frame step ------------------------------------------------ */

static void run_cart_frame(app_t* a) {
    cronopio_console_t* console = a->console;

    cronopio_console_begin_frame(console);
    const Uint8* keys = SDL_GetKeyboardState(NULL);
    /* The console exposes only the abstract pad to carts; the keyboard is just
     * one way to drive it on the desktop (remappable in the F1 menu). */
    console->pad_cur[0] = build_pad(a, keys);

    if (console->frame_fn_index > 0) {
        int32_t fret = 0;
        int rc = cvm_call(&a->img, (uint32_t)console->frame_fn_index, NULL, 0, &fret);
        if (rc != CVM_OK && rc != CVM_E_SYSCALL_TRAP) {
            fprintf(stderr, "frame trap: %s\n", cvm_strerror(rc));
            a->running = 0;
        }
    }
    refresh_tex(a);
    cronopio_console_end_frame(console);

    /* Write the save out if the cart touched it this frame. */
    persist_cart_save(a);
}

/* Directory to seed the file browser with: last used, else the executable's. */
static const char* browse_dir(app_t* a) {
    if (a->cfg.last_dir[0]) return a->cfg.last_dir;
    return ".";
}

static void toggle_menu(app_t* a) {
    if (menu_is_open(a->menu)) {
        if (a->has_cart) menu_close(a->menu);   /* can't resume with no cart */
    } else {
        menu_open(a->menu, browse_dir(a), a->gc ? a->joy_name : NULL, a->has_cart);
    }
}

static void tick(void* arg) {
    app_t* a = (app_t*)arg;
    int was_open = menu_is_open(a->menu);

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) { a->running = 0; continue; }

        if (ev.type == SDL_KEYDOWN && ev.key.keysym.scancode == SDL_SCANCODE_F1
            && ev.key.repeat == 0) {
            toggle_menu(a);
            continue;
        }
        if (ev.type == SDL_CONTROLLERDEVICEADDED ||
            ev.type == SDL_CONTROLLERDEVICEREMOVED) {
            open_controller(a);
            continue;
        }

        if (menu_is_open(a->menu)) {
            menu_handle_event(a->menu, &ev);   /* modal: it consumes input */
            continue;
        }

        /* Cart-running input not covered by the per-frame snapshot. */
        if (ev.type == SDL_MOUSEMOTION) {
            a->console->mouse_x = ev.motion.x / SCALE;
            a->console->mouse_y = ev.motion.y / SCALE;
        }
        if (ev.type == SDL_MOUSEBUTTONDOWN || ev.type == SDL_MOUSEBUTTONUP) {
            uint32_t bit = (ev.button.button == SDL_BUTTON_LEFT) ? 1u : 2u;
            if (ev.type == SDL_MOUSEBUTTONDOWN) a->console->mouse_buttons |=  bit;
            else                                a->console->mouse_buttons &= ~bit;
        }
    }

    int is_open = menu_is_open(a->menu);
    /* Silence audio while paused; save config when the menu just closed. */
    if (a->audio_dev) SDL_PauseAudioDevice(a->audio_dev, is_open ? 1 : 0);
    if (was_open && !is_open)
        hostcfg_save(&a->cfg, a->cfg_path);

    if (!is_open && a->has_cart && !a->console->cart_exited)
        run_cart_frame(a);
    present_all(a);

#ifdef __EMSCRIPTEN__
    if (!a->running || a->console->cart_exited)
        emscripten_cancel_main_loop();
#endif
}

int main(int argc, char** argv) {
    const char* cart_path = (argc >= 2) ? argv[1] : NULL;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* win = SDL_CreateWindow(
        "Cronopio",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        CRONOPIO_SCREEN_W * SCALE, CRONOPIO_SCREEN_H * SCALE,
        SDL_WINDOW_SHOWN);
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_Texture*  tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, CRONOPIO_SCREEN_W, CRONOPIO_SCREEN_H);

    static cronopio_console_t console;
    cronopio_console_init(&console);   /* loads the embedded default SoundFont */
    console.boot_ms = SDL_GetTicks();

    static app_t app;
    app.console  = &console;
    app.ren      = ren;
    app.tex      = tex;
    app.rgba     = (uint32_t*)malloc((size_t)CRONOPIO_FB_BYTES * 4);
    app.running  = 1;
    app.has_cart = 0;

    /* Config: defaults, then overlay cronopio.cfg from beside the executable. */
    hostcfg_defaults(&app.cfg);
    {
        char* base = SDL_GetBasePath();
        snprintf(app.cfg_path, sizeof(app.cfg_path), "%scronopio.cfg",
                 base ? base : "");
        hostcfg_load(&app.cfg, app.cfg_path);
        /* First run: seed the browser at the executable's directory. */
        if (!app.cfg.last_dir[0] && base)
            snprintf(app.cfg.last_dir, sizeof(app.cfg.last_dir), "%s", base);
        if (base) SDL_free(base);
    }

    /* Open audio at 22050 Hz stereo, ~1024-frame buffer. */
    SDL_AudioSpec want = {0}, got;
    want.freq     = CRONOPIO_AUDIO_HZ;
    want.format   = AUDIO_S16LSB;
    want.channels = 2;
    want.samples  = 1024;
    want.callback = audio_cb;
    want.userdata = &console;
    app.audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
    if (app.audio_dev) SDL_PauseAudioDevice(app.audio_dev, 0);

    /* System menu + host controller. */
    menu_host_t mh = { &app, app_load_cart, app_reset_cart, app_quit };
    app.menu = menu_create(ren, CRONOPIO_SCREEN_W * SCALE, CRONOPIO_SCREEN_H * SCALE,
                           &app.cfg, &mh);
    open_controller(&app);

    /* Boot: load the CLI cart if given, else open the file browser. */
    console.present_cb = desktop_present_cb;
    console.present_ud = &app;
    if (cart_path) {
        if (app_load_cart(&app, cart_path) != 0)
            menu_open(app.menu, browse_dir(&app), app.gc ? app.joy_name : NULL, 0);
    } else {
        menu_open(app.menu, browse_dir(&app), app.gc ? app.joy_name : NULL, 0);
    }

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop_arg(tick, &app, 0, 1);
#else
    while (app.running && !console.cart_exited)
        tick(&app);

    persist_cart_save(&app);   /* flush the save on exit */
    cronopio_save_free(&console);
    hostcfg_save(&app.cfg, app.cfg_path);
    if (app.has_cart) { cvm_image_free(&app.img); free(app.blob); }
    free(app.rgba);
    menu_destroy(app.menu);
    close_controller(&app);
#endif

    if (app.audio_dev) SDL_CloseAudioDevice(app.audio_dev);
    cron_synth_destroy(console.synth);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return console.exit_status;
}
