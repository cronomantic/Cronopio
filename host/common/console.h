#ifndef CRONOPIO_CONSOLE_H
#define CRONOPIO_CONSOLE_H

#include <stdint.h>

#define CRONOPIO_SCREEN_W      320
#define CRONOPIO_SCREEN_H      240
#define CRONOPIO_PALETTE_SIZE  256
#define CRONOPIO_FPS            60
#define CRONOPIO_AUDIO_HZ    22050
#define CRONOPIO_AUDIO_CHANS     4
#define CRONOPIO_PAD_COUNT       2
#define CRONOPIO_SAVE_BYTES   1024

#define CRONOPIO_FB_BYTES      (CRONOPIO_SCREEN_W * CRONOPIO_SCREEN_H)  /* 76 800 */
#define CRONOPIO_PAL_BYTES     (CRONOPIO_PALETTE_SIZE * 4)              /* 128    */

/* Canonical region names for host-shared regions. The cart declares them via
 * cvm-cc flags (--region=fb:76800:rw --region=pal:128:rw); the host resolves
 * their heap offsets at load time with cvm_image_get_region. */
#define CRONOPIO_FB_REGION   "fb"
#define CRONOPIO_PAL_REGION  "pal"

typedef struct {
    int      wave;        /* 0 sine, 1 square, 2 triangle, 3 noise */
    uint32_t freq_mhz;
    int      vol;         /* 0..255           */
    int      pan;         /* -128..127        */
    uint32_t phase;
} cron_voice_t;

typedef struct {
    /* Heap-relative offsets of the fb/pal regions inside the CronoVM image.
     * Resolved once at cart load time. Zero/invalid means "not declared by
     * this cart" — the host still runs the cart but drawing syscalls
     * become no-ops and the screen stays black. */
    uint32_t fb_offset;
    uint32_t pal_offset;
    int      regions_ok;

    /* audio */
    cron_voice_t voices[CRONOPIO_AUDIO_CHANS];
    int          master_vol_q8;                  /* 0..256 */

    /* input snapshot */
    uint32_t pad_cur[CRONOPIO_PAD_COUNT];
    uint32_t pad_prev[CRONOPIO_PAD_COUNT];
    int32_t  mouse_x, mouse_y;
    uint32_t mouse_buttons;

    /* keyboard scancode bitmap (256 bits) — written by the platform shell. */
    uint8_t  keys[32];

    /* timing */
    uint64_t boot_ms;
    uint32_t frame_count;
    uint32_t prng_state;

    /* Cart-registered per-frame callback. The cart calls cron_set_frame(fn)
     * during its boot run; the syscall handler stores fn's FUNCS index here.
     * The platform shell invokes it via cvm_call once per 1/60 s tick. */
    int32_t  frame_fn_index;

    /* cart exit signal — set by cron_exit syscall or the platform shell. */
    int      cart_exited;
    int32_t  exit_status;

    /* persistence */
    uint8_t  save[CRONOPIO_SAVE_BYTES];
    int      save_dirty;
} cronopio_console_t;

void cronopio_console_init(cronopio_console_t* c);
void cronopio_console_begin_frame(cronopio_console_t* c);
void cronopio_console_end_frame(cronopio_console_t* c);

/* Initialise the palette region of a freshly-loaded cart's heap with the
 * default 32-colour palette. fb_region/pal_region are heap-relative
 * offsets resolved via cvm_image_get_region; both must already be valid.
 * The framebuffer region is left zeroed (loader already zero-fills it). */
void cronopio_console_seed_palette(uint8_t* heap, uint32_t pal_offset);

/* Pack the cart's 8 bpp framebuffer + palette (read from `heap` at the
 * region offsets stored in `c`) into a 32-bit RGBA buffer supplied by the
 * platform shell. `dst` must be SCREEN_W*SCREEN_H pixels. */
void cronopio_console_blit_rgba(const cronopio_console_t* c,
                                const uint8_t* heap, uint32_t* dst);

/* Render `frames` audio frames into a 16-bit signed stereo buffer
 * (length frames*2). */
void cronopio_console_mix(cronopio_console_t* c, int16_t* dst, int frames);

/* APU / input glue used by the syscall layer and the platform shell. */
void     cron_apu_tone  (cronopio_console_t* c, int ch, int wave,
                         uint32_t freq_mhz, int vol, int pan);
void     cron_apu_stop  (cronopio_console_t* c, int ch);
void     cron_apu_master(cronopio_console_t* c, int vol_q8);

void     cron_input_set_pad     (cronopio_console_t* c, int player, uint32_t mask);
uint32_t cron_input_pad         (const cronopio_console_t* c, int player);
uint32_t cron_input_pad_pressed (const cronopio_console_t* c, int player);
uint32_t cron_input_pad_released(const cronopio_console_t* c, int player);

/* GPU primitives operate on the framebuffer slice of `heap` starting at
 * fb_offset. They silently clip; out-of-range coords are ignored. */
void cron_gpu_cls  (uint8_t* heap, uint32_t fb_offset, int color);
void cron_gpu_pset (uint8_t* heap, uint32_t fb_offset, int x, int y, int color);
void cron_gpu_rect (uint8_t* heap, uint32_t fb_offset, int x, int y, int w, int h, int color);
void cron_gpu_line (uint8_t* heap, uint32_t fb_offset, int x0, int y0, int x1, int y1, int color);
void cron_gpu_blit (uint8_t* heap, uint32_t fb_offset,
                    const uint8_t* src, int sw, int sh, int dx, int dy);
void cron_gpu_text (uint8_t* heap, uint32_t fb_offset,
                    const char* s, int len, int x, int y, int color);

#endif
