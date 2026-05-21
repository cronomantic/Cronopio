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
#define CRONOPIO_IMAGE_SLOTS     8
#define CRONOPIO_TILEMAP_SLOTS   8
#define CRONOPIO_TILE_SIZE       8

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

/* Global drawing state — clip rect, camera offset, draw-time palette remap.
 * Every primitive routes pixel writes through these (Pyxel-style). */
typedef struct {
    int     clip_x0, clip_y0, clip_x1, clip_y1;  /* screen-space [x0,x1) [y0,y1) */
    int     cam_x, cam_y;                          /* subtracted from world coords */
    uint8_t pal_map[256];                          /* draw color -> written color  */
} cron_draw_t;

/* An image bank is a thin handle over an 8bpp bitmap living in cart memory
 * (RAM or ROM): offset into the heap + dimensions. used==0 means unbound. */
typedef struct {
    uint32_t offset;
    int      w, h;
    int      used;
} cron_image_bank_t;

/* A tilemap bank is a grid of u16 cells (linear tile index, 0xFFFF = empty)
 * in cart memory, drawn from 8x8 tiles of image bank `img`. */
typedef struct {
    uint32_t offset;
    int      w, h;        /* in cells */
    int      img;         /* image-bank slot the tiles come from */
    int      used;
} cron_tilemap_bank_t;

typedef struct {
    /* Heap-relative offsets of the fb/pal regions inside the CronoVM image.
     * Resolved once at cart load time. Zero/invalid means "not declared by
     * this cart" — the host still runs the cart but drawing syscalls
     * become no-ops and the screen stays black. */
    uint32_t fb_offset;
    uint32_t pal_offset;
    int      regions_ok;

    /* graphics */
    cron_draw_t         draw;
    cron_image_bank_t   images[CRONOPIO_IMAGE_SLOTS];
    cron_tilemap_bank_t tilemaps[CRONOPIO_TILEMAP_SLOTS];

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

/* GPU primitives. All take the console (for fb offset + draw state: clip,
 * camera, palette remap) and the heap base. Coordinates are in world space;
 * the camera offset and clip rect are applied per pixel. cls is the one
 * exception: it clears the whole framebuffer, ignoring the draw state. */
void cron_gpu_reset_state(cronopio_console_t* c);   /* clip=full, cam=0, pal=identity */

void cron_gpu_cls  (cronopio_console_t* c, uint8_t* heap, int color);
void cron_gpu_pset (cronopio_console_t* c, uint8_t* heap, int x, int y, int color);
int  cron_gpu_pget (cronopio_console_t* c, uint8_t* heap, int x, int y);
void cron_gpu_rect (cronopio_console_t* c, uint8_t* heap, int x, int y, int w, int h, int color);
void cron_gpu_rectb(cronopio_console_t* c, uint8_t* heap, int x, int y, int w, int h, int color);
void cron_gpu_line (cronopio_console_t* c, uint8_t* heap, int x0, int y0, int x1, int y1, int color);
void cron_gpu_circ (cronopio_console_t* c, uint8_t* heap, int x, int y, int r, int color);
void cron_gpu_circb(cronopio_console_t* c, uint8_t* heap, int x, int y, int r, int color);
void cron_gpu_elli (cronopio_console_t* c, uint8_t* heap, int x, int y, int w, int h, int color);
void cron_gpu_ellib(cronopio_console_t* c, uint8_t* heap, int x, int y, int w, int h, int color);
void cron_gpu_tri  (cronopio_console_t* c, uint8_t* heap, int x0,int y0,int x1,int y1,int x2,int y2,int color);
void cron_gpu_trib (cronopio_console_t* c, uint8_t* heap, int x0,int y0,int x1,int y1,int x2,int y2,int color);
void cron_gpu_fill (cronopio_console_t* c, uint8_t* heap, int x, int y, int color);
void cron_gpu_text (cronopio_console_t* c, uint8_t* heap, const char* s, int len, int x, int y, int color);
void cron_gpu_blit_raw(cronopio_console_t* c, uint8_t* heap,
                       const uint8_t* src, int sw, int sh, int dx, int dy);

/* Draw-state setters (clamped/normalised here). */
void cron_gpu_clip  (cronopio_console_t* c, int x, int y, int w, int h);
void cron_gpu_clip_reset(cronopio_console_t* c);
void cron_gpu_camera(cronopio_console_t* c, int x, int y);
void cron_gpu_pal   (cronopio_console_t* c, int c0, int c1);
void cron_gpu_pal_reset(cronopio_console_t* c);

/* Image / tilemap banks (mem_size is the cart's addressable byte count, used
 * to bounds-check the registered region). Returns 0 on success, -1 if the
 * slot is out of range or the region escapes cart memory. */
int  cron_gpu_image  (cronopio_console_t* c, int slot, uint32_t offset, int w, int h, uint32_t mem_size);
int  cron_gpu_tilemap(cronopio_console_t* c, int slot, uint32_t offset, int w, int h, int img, uint32_t mem_size);

/* Blit a (sx,sy,w,h) region of image bank `img` to (dx,dy). colkey: a source
 * index to treat as transparent, or -1 for fully opaque. Negative w/h flip
 * horizontally/vertically (Pyxel convention). */
void cron_gpu_blt (cronopio_console_t* c, uint8_t* heap, int img,
                   int dx, int dy, int sx, int sy, int w, int h, int colkey);
/* Blit a pixel region (sx,sy,w,h) of tilemap `tm` to (dx,dy). colkey as blt. */
void cron_gpu_bltm(cronopio_console_t* c, uint8_t* heap, int tm,
                   int dx, int dy, int sx, int sy, int w, int h, int colkey);

#endif
