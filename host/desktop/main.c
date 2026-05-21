/* Cronopio desktop/web host — SDL2 shell.
 *
 *   1. Slurp the .bin from disk, hand it to cvm_load.
 *   2. Resolve the fb/pal host regions and install Cronopio's syscalls.
 *   3. Run the cart's entry once (it registers a frame fn via cron_set_frame).
 *   4. Each 1/60 s: poll input, cvm_call the frame fn, blit FB, mix audio.
 *
 * The per-frame work lives in frame_step() so both the native blocking
 * loop and Emscripten's requestAnimationFrame-driven callback can share it.
 * Under Emscripten there is no blocking main loop — emscripten_set_main_loop
 * calls frame_step() once per browser animation frame. */

#include "console.h"
#include "syscalls.h"
#include "cvm.h"

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

/* Everything the per-frame step needs. Lives on main()'s stack; a pointer
 * is handed to the loop driver. */
typedef struct {
    cronopio_console_t *console;
    struct cvm_image   *img;
    SDL_Renderer       *ren;
    SDL_Texture        *tex;
    uint32_t           *rgba;
    int                 running;
} app_t;

static uint32_t map_keys_to_pad(const Uint8* keys) {
    uint32_t m = 0;
    if (keys[SDL_SCANCODE_UP]    || keys[SDL_SCANCODE_W]) m |= 1u << 0;
    if (keys[SDL_SCANCODE_DOWN]  || keys[SDL_SCANCODE_S]) m |= 1u << 1;
    if (keys[SDL_SCANCODE_LEFT]  || keys[SDL_SCANCODE_A]) m |= 1u << 2;
    if (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D]) m |= 1u << 3;
    if (keys[SDL_SCANCODE_Z]     || keys[SDL_SCANCODE_J]) m |= 1u << 4; /* A */
    if (keys[SDL_SCANCODE_X]     || keys[SDL_SCANCODE_K]) m |= 1u << 5; /* B */
    if (keys[SDL_SCANCODE_C]     || keys[SDL_SCANCODE_L]) m |= 1u << 6; /* X */
    if (keys[SDL_SCANCODE_V]     || keys[SDL_SCANCODE_I]) m |= 1u << 7; /* Y */
    return m;
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

/* One frame: poll input, run the cart's frame fn, blit, present. Called by
 * the native while-loop and by emscripten_set_main_loop. */
static void frame_step(void* arg) {
    app_t* a = (app_t*)arg;
    cronopio_console_t* console = a->console;

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) a->running = 0;
        if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) a->running = 0;
        if (ev.type == SDL_MOUSEMOTION) {
            console->mouse_x = ev.motion.x / SCALE;
            console->mouse_y = ev.motion.y / SCALE;
        }
        if (ev.type == SDL_MOUSEBUTTONDOWN || ev.type == SDL_MOUSEBUTTONUP) {
            uint32_t bit = (ev.button.button == SDL_BUTTON_LEFT) ? 1u : 2u;
            if (ev.type == SDL_MOUSEBUTTONDOWN) console->mouse_buttons |=  bit;
            else                                console->mouse_buttons &= ~bit;
        }
    }

    cronopio_console_begin_frame(console);
    const Uint8* keys = SDL_GetKeyboardState(NULL);
    console->pad_cur[0] = map_keys_to_pad(keys);
    /* Snapshot scancodes for cron_key (low 256 cover standard keyboards). */
    for (int i = 0; i < 32; ++i) {
        uint8_t byte = 0;
        for (int b = 0; b < 8; ++b)
            if (keys[(i<<3) + b]) byte |= (uint8_t)(1u << b);
        console->keys[i] = byte;
    }

    if (console->frame_fn_index > 0) {
        int32_t fret = 0;
        int rc = cvm_call(a->img, (uint32_t)console->frame_fn_index, NULL, 0, &fret);
        if (rc != CVM_OK && rc != CVM_E_SYSCALL_TRAP) {
            fprintf(stderr, "frame trap: %s\n", cvm_strerror(rc));
            a->running = 0;
        }
    }

    cronopio_console_blit_rgba(console, a->img->heap, a->rgba);
    SDL_UpdateTexture(a->tex, NULL, a->rgba, CRONOPIO_SCREEN_W * 4);
    SDL_RenderClear(a->ren);
    SDL_RenderCopy(a->ren, a->tex, NULL, NULL);
    SDL_RenderPresent(a->ren);

    cronopio_console_end_frame(console);

#ifdef __EMSCRIPTEN__
    if (!a->running || console->cart_exited)
        emscripten_cancel_main_loop();
#endif
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s cartridge.bin\n", argv[0]);
        return 1;
    }
    const char* cart_path = argv[1];

    size_t   blob_len = 0;
    uint8_t* blob     = slurp(cart_path, &blob_len);
    if (!blob) return 1;

    struct cvm_image img;
    int rc = cvm_load(blob, blob_len, &img);
    if (rc != CVM_OK) {
        fprintf(stderr, "cvm_load: %s\n", cvm_strerror(rc));
        free(blob);
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        cvm_image_free(&img); free(blob);
        return 1;
    }

    SDL_Window*   win = SDL_CreateWindow(
        "Cronopio",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        CRONOPIO_SCREEN_W * SCALE, CRONOPIO_SCREEN_H * SCALE,
        SDL_WINDOW_SHOWN);
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_Texture*  tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, CRONOPIO_SCREEN_W, CRONOPIO_SCREEN_H);

    static cronopio_console_t console;
    cronopio_console_init(&console);
    console.boot_ms = SDL_GetTicks();

    if (cronopio_resolve_video_regions(&img, &console) != 0) {
        fprintf(stderr,
                "warning: cart did not declare 'fb' and 'pal' regions — "
                "drawing syscalls will no-op.\n");
    }
    cronopio_syscalls_install(&img, &console);

    /* Open audio at 22050 Hz stereo, ~1024-frame buffer. */
    SDL_AudioSpec want = {0}, got;
    want.freq     = CRONOPIO_AUDIO_HZ;
    want.format   = AUDIO_S16LSB;
    want.channels = 2;
    want.samples  = 1024;
    want.callback = audio_cb;
    want.userdata = &console;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
    if (dev) SDL_PauseAudioDevice(dev, 0);

    /* Run the cart's entry. It is expected to call cron_set_frame(fn) and
     * return; we then drive that fn ourselves every tick. This call is
     * short (no blocking), so it needs no ASYNCIFY under Emscripten. */
    int32_t entry_ret = 0;
    rc = cvm_run(&img, &entry_ret);
    if (rc != CVM_OK && rc != CVM_E_SYSCALL_TRAP) {
        fprintf(stderr, "cart entry trapped: %s\n", cvm_strerror(rc));
        goto teardown;
    }

    static app_t app;
    app.console = &console;
    app.img     = &img;
    app.ren     = ren;
    app.tex     = tex;
    app.rgba    = (uint32_t*)malloc((size_t)CRONOPIO_FB_BYTES * 4);
    app.running = 1;

#ifdef __EMSCRIPTEN__
    /* No blocking loop in the browser: hand the step to the event loop.
     * fps=0 → drive from requestAnimationFrame (matches the display, ~60 Hz);
     * simulate_infinite_loop=1 → this call does not return, so teardown
     * below is unreachable on web (the page lifecycle reclaims everything). */
    emscripten_set_main_loop_arg(frame_step, &app, 0, 1);
#else
    while (app.running && !console.cart_exited)
        frame_step(&app);

    free(app.rgba);
#endif

teardown:
    if (dev) SDL_CloseAudioDevice(dev);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    cvm_image_free(&img);
    free(blob);
    return console.exit_status;
}
