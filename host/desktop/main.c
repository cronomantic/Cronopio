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
#include "cron_music.h"
#include "syscalls.h"
#include "cvm.h"
#include "hostcfg.h"
#include "menu.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define HOST_MKDIR(p) _mkdir(p)
#else
#define HOST_MKDIR(p) mkdir((p), 0777)
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

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

    SDL_Window         *win;
    SDL_Renderer       *ren;
    SDL_Texture        *tex;
    uint32_t           *rgba;
    SDL_Rect            dst;          /* where the 320x240 cart is drawn (4:3, letterboxed) */
    SDL_AudioDeviceID   audio_dev;

    SDL_GameController *gc;            /* opened host controller, or NULL */
    char                joy_name[128];

    host_cfg_t          cfg;
    char                cfg_path[1024];
    char                base_path[1024];   /* host exe dir (trailing sep), for relative save_dir */
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

/* Recompute a->dst: the 320x240 cart drawn as large as fits inside the current
 * window/output while keeping its 4:3 aspect (letterboxed). In a windowed scale*
 * window this is the whole window; in fullscreen it centres with bars. Also
 * tells the menu the current output size so its overlay lays out correctly. */
static void compute_dst(app_t* a) {
    int ow = CRONOPIO_SCREEN_W, oh = CRONOPIO_SCREEN_H;
    SDL_GetRendererOutputSize(a->ren, &ow, &oh);
    if (ow < 1) ow = 1; if (oh < 1) oh = 1;
    /* integer-ish fit preserving 4:3 (CRONOPIO_SCREEN_W:H) */
    int sw = CRONOPIO_SCREEN_W, sh = CRONOPIO_SCREEN_H;
    int s_w = ow / sw, s_h = oh / sh;
    int s = s_w < s_h ? s_w : s_h;          /* largest integer scale that fits */
    if (s < 1) {                             /* window smaller than 320x240: fit-scale */
        a->dst.w = (ow * sh <= oh * sw) ? ow : oh * sw / sh;
        a->dst.h = (ow * sh <= oh * sw) ? ow * sh / sw : oh;
    } else {
        a->dst.w = sw * s; a->dst.h = sh * s;
    }
    a->dst.x = (ow - a->dst.w) / 2;
    a->dst.y = (oh - a->dst.h) / 2;
    if (a->menu) menu_set_size(a->menu, ow, oh);
}

/* Apply the video settings in a->cfg to the live window/renderer: window size
 * (windowed), fullscreen toggle, and vsync. Recomputes the present rect. */
static void apply_video(app_t* a) {
    int sc = a->cfg.scale;
    if (sc < HOSTCFG_SCALE_MIN) sc = HOSTCFG_SCALE_MIN;
    if (sc > HOSTCFG_SCALE_MAX) sc = HOSTCFG_SCALE_MAX;
    a->cfg.scale = sc;

    SDL_SetWindowFullscreen(a->win, a->cfg.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    if (!a->cfg.fullscreen) {
        SDL_SetWindowSize(a->win, CRONOPIO_SCREEN_W * sc, CRONOPIO_SCREEN_H * sc);
        SDL_SetWindowPosition(a->win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
#if SDL_VERSION_ATLEAST(2, 0, 18)
    SDL_RenderSetVSync(a->ren, a->cfg.vsync ? 1 : 0);   /* live vsync toggle */
#endif
    compute_dst(a);
}

/* The menu's apply_video callback (menu_host_t). */
static void app_apply_video(void* ud) { apply_video((app_t*)ud); }

/* Compose the current scene: cart texture + (optional) menu overlay. */
static void present_all(app_t* a) {
    SDL_RenderClear(a->ren);
    SDL_RenderCopy(a->ren, a->tex, NULL, &a->dst);
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
    SDL_RenderCopy(a->ren, a->tex, NULL, &a->dst);
    SDL_RenderPresent(a->ren);
}

/* ---- cart load / reset (also the menu's load_cart/reset_cart callbacks) -- */

/* ---- per-cart save persistence ("memory card") ------------------------- */
/* The cart's save blob is <name>.sav in the configured save folder (cfg.save_dir;
 * "" = beside the cartridge). We load it before running the cart (its libc reads
 * the save region at boot) and write it back when the cart marks it dirty,
 * atomically (temp + rename). */

/* Build the .sav path for the current cart into `out`. With no save_dir it is
 * "<cart>.sav" beside the cartridge; otherwise "<dir>/<cartname>.sav", where a
 * relative dir is taken under the host's base_path and the dir is created. */
static void cart_save_path(app_t* a, char* out, size_t cap) {
    if (!a->cfg.save_dir[0]) {
        snprintf(out, cap, "%s.sav", a->cart_path);
        return;
    }
    /* cart filename (after the last path separator) */
    const char* name = a->cart_path;
    for (const char* p = a->cart_path; *p; ++p)
        if (*p == '/' || *p == '\\') name = p + 1;
    /* absolute (/, \, or "X:") used verbatim; relative resolved under base_path */
    const char* sd = a->cfg.save_dir;
    int absolute = sd[0] == '/' || sd[0] == '\\' || (sd[0] && sd[1] == ':');
    char dir[1200];
    if (absolute) snprintf(dir, sizeof dir, "%s", sd);
    else          snprintf(dir, sizeof dir, "%s%s", a->base_path, sd);
    HOST_MKDIR(dir);   /* ensure it exists (ignore EEXIST) */
    snprintf(out, cap, "%s/%s.sav", dir, name);
}

static void load_cart_save(app_t* a) {
    cronopio_console_t* c = a->console;
    c->save_len = 0;
    c->save_dirty = 0;
    cronopio_save_reserve(c, CRONOPIO_SAVE_DEFAULT);   /* baseline capacity */
    if (!a->cart_path[0]) return;
    char p[1300];
    cart_save_path(a, p, sizeof p);
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
    char p[1300], tmp[1312];
    cart_save_path(a, p, sizeof p);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
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

    /* Reject anything that isn't a Cronopio cartridge (must declare the fb/pal
     * regions) BEFORE tearing down the running cart — otherwise we'd run a
     * non-cart image and likely crash. Old cart is left untouched on failure. */
    {
        uint32_t off, sz;
        if (cvm_image_get_region(&nimg, CRONOPIO_FB_REGION,  &off, &sz) != CVM_OK
            || sz < CRONOPIO_FB_BYTES
            || cvm_image_get_region(&nimg, CRONOPIO_PAL_REGION, &off, &sz) != CVM_OK
            || sz < CRONOPIO_PAL_BYTES) {
            fprintf(stderr, "%s: not a Cronopio cartridge (no fb/pal region)\n", path);
            cvm_image_free(&nimg);
            free(nblob);
            return -1;
        }
        /* Strict: require a valid integrity seal (magic + crc32). Rejects
         * unsealed or corrupt/tampered files. */
        if (cvm_seal_check(nblob, nlen) != 1) {
            fprintf(stderr, "%s: bad or missing cartridge seal (corrupt / not sealed)\n", path);
            cvm_image_free(&nimg);
            free(nblob);
            return -1;
        }
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
    cron_music_destroy(a->console->music);
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

        if (ev.type == SDL_WINDOWEVENT &&
            (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
             ev.window.event == SDL_WINDOWEVENT_RESIZED)) {
            compute_dst(a);   /* re-letterbox after a fullscreen/size change */
            continue;
        }

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

        /* Cart-running input not covered by the per-frame snapshot. Map window
         * pixels to the 320x240 cart through the letterboxed present rect. */
        if (ev.type == SDL_MOUSEMOTION) {
            int mx = (a->dst.w > 0) ? (ev.motion.x - a->dst.x) * CRONOPIO_SCREEN_W / a->dst.w : 0;
            int my = (a->dst.h > 0) ? (ev.motion.y - a->dst.y) * CRONOPIO_SCREEN_H / a->dst.h : 0;
            if (mx < 0) mx = 0; else if (mx >= CRONOPIO_SCREEN_W) mx = CRONOPIO_SCREEN_W - 1;
            if (my < 0) my = 0; else if (my >= CRONOPIO_SCREEN_H) my = CRONOPIO_SCREEN_H - 1;
            a->console->mouse_x = mx;
            a->console->mouse_y = my;
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
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    static app_t app;
    app.console  = NULL;
    app.rgba     = (uint32_t*)malloc((size_t)CRONOPIO_FB_BYTES * 4);
    app.running  = 1;
    app.has_cart = 0;
    app.menu     = NULL;

    /* Config: defaults, then overlay cronopio.cfg from beside the executable. */
    hostcfg_defaults(&app.cfg);
    {
        char* base = SDL_GetBasePath();
        snprintf(app.base_path, sizeof(app.base_path), "%s", base ? base : "");
        snprintf(app.cfg_path, sizeof(app.cfg_path), "%scronopio.cfg",
                 app.base_path);
        hostcfg_load(&app.cfg, app.cfg_path);
        /* First run: seed the browser at the executable's directory. */
        if (!app.cfg.last_dir[0] && base)
            snprintf(app.cfg.last_dir, sizeof(app.cfg.last_dir), "%s", base);
        if (base) SDL_free(base);
    }

    /* CLI: the first bare arg is the cart; flags override the saved cfg.
     *   --scale=N (1..6)  --fullscreen / --windowed  --vsync / --no-vsync
     *   --saves=DIR ("" or omit = beside the cartridge)                     */
    const char* cart_path = NULL;
    for (int i = 1; i < argc; ++i) {
        const char* s = argv[i];
        if (!strncmp(s, "--scale=", 8))      app.cfg.scale = atoi(s + 8);
        else if (!strcmp(s, "--fullscreen")) app.cfg.fullscreen = 1;
        else if (!strcmp(s, "--windowed"))   app.cfg.fullscreen = 0;
        else if (!strcmp(s, "--vsync"))      app.cfg.vsync = 1;
        else if (!strcmp(s, "--no-vsync"))   app.cfg.vsync = 0;
        else if (!strncmp(s, "--saves=", 8))
            snprintf(app.cfg.save_dir, sizeof(app.cfg.save_dir), "%s", s + 8);
        else if (s[0] != '-' && !cart_path)  cart_path = s;
    }
    if (app.cfg.scale < HOSTCFG_SCALE_MIN) app.cfg.scale = HOSTCFG_SCALE_MIN;
    if (app.cfg.scale > HOSTCFG_SCALE_MAX) app.cfg.scale = HOSTCFG_SCALE_MAX;

    SDL_Window* win = SDL_CreateWindow(
        "Cronopio",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        CRONOPIO_SCREEN_W * app.cfg.scale, CRONOPIO_SCREEN_H * app.cfg.scale,
        SDL_WINDOW_SHOWN | (app.cfg.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    Uint32 rflags = SDL_RENDERER_ACCELERATED | (app.cfg.vsync ? SDL_RENDERER_PRESENTVSYNC : 0);
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, rflags);
    SDL_Texture*  tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, CRONOPIO_SCREEN_W, CRONOPIO_SCREEN_H);

    static cronopio_console_t console;
    cronopio_console_init(&console);   /* loads the embedded default SoundFont */
    console.boot_ms = SDL_GetTicks();

    app.console  = &console;
    app.win      = win;
    app.ren      = ren;
    app.tex      = tex;

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
    menu_host_t mh = { &app, app_load_cart, app_reset_cart, app_quit, app_apply_video };
    int ow = CRONOPIO_SCREEN_W * app.cfg.scale, oh = CRONOPIO_SCREEN_H * app.cfg.scale;
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    app.menu = menu_create(ren, ow, oh, &app.cfg, &mh);
    apply_video(&app);   /* finalise window/vsync + compute the present rect */
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
    cron_music_destroy(console.music);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return console.exit_status;
}
