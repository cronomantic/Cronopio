/* Cronopio cartridge SDK — host ABI as C functions.
 *
 * Include this header from your cartridge sources. The CronoVM translator
 * recognises every `cvm_sys_*` symbol the binary references and adds it to
 * the IMPORTS section; the host (host/common/syscalls.c) binds the matching
 * handler by name at load time. There is no separate stub-emission step.
 *
 * Memory map (resolved at startup via cvm_sys_get_region):
 *   "fb"    framebuffer    320*240 bytes, 8 bpp indexed
 *   "pal"   palette        32 * u32 (0x00RRGGBB), little-endian
 *
 * Use cron_resolve_video() in your main() before drawing — that's what
 * fills CRON_FB / CRON_PAL with the actual heap pointers. */

#ifndef CRONOPIO_H
#define CRONOPIO_H

#include <stdint.h>

#define CRON_SCREEN_W  320
#define CRON_SCREEN_H  240
#define CRON_PAL_SIZE  256
#define CRON_SAVE_BYTES 1024

/* Gamepad bits. */
enum {
    CRON_BTN_UP    = 1u << 0,
    CRON_BTN_DOWN  = 1u << 1,
    CRON_BTN_LEFT  = 1u << 2,
    CRON_BTN_RIGHT = 1u << 3,
    CRON_BTN_A     = 1u << 4,
    CRON_BTN_B     = 1u << 5,
    CRON_BTN_X     = 1u << 6,
    CRON_BTN_Y     = 1u << 7,
};

enum { CRON_WAVE_SINE = 0, CRON_WAVE_SQUARE = 1, CRON_WAVE_TRI = 2, CRON_WAVE_NOISE = 3 };

/* CronoVM built-in: resolve a host-shared region's offset. Returns the
 * heap-relative offset or -1 if the binary didn't declare it. The cart's
 * --region= flags (passed to cvm-cc) line up with these names. */
extern int32_t cvm_sys_get_region(const char* name);

/* CronoVM built-ins: the read-only cartridge ROM baked in with --rom=FILE.
 * cvm_sys_rom_base() is the heap offset of the blob (treat as a pointer);
 * cvm_sys_rom_size() its length in bytes (0 if the cart carries no ROM). */
extern int32_t cvm_sys_rom_base(void);
extern int32_t cvm_sys_rom_size(void);

/* ---------------- Host syscalls (cvm_sys_ prefix → IMPORTS section) ----- */

extern void     cvm_sys_cron_log         (const char* msg, int32_t len);
extern void     cvm_sys_cron_trace_i32   (int32_t tag, int32_t value);
extern void     cvm_sys_cron_set_frame   (void (*frame_fn)(void));
extern void     cvm_sys_cron_exit        (int32_t status);

extern uint32_t cvm_sys_cron_time_ms     (void);
extern uint32_t cvm_sys_cron_frame_count (void);
extern uint32_t cvm_sys_cron_random      (void);
extern void     cvm_sys_cron_seed        (uint32_t seed);

extern void     cvm_sys_cron_palette_set (int32_t idx, uint32_t rgb);
extern uint32_t cvm_sys_cron_palette_get (int32_t idx);
extern void     cvm_sys_cron_cls         (int32_t color);
extern void     cvm_sys_cron_pset        (int32_t x, int32_t y, int32_t color);
extern void     cvm_sys_cron_rect        (int32_t x, int32_t y, int32_t w, int32_t h, int32_t color);
extern void     cvm_sys_cron_line        (int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t color);
extern void     cvm_sys_cron_blit        (const uint8_t* src, int32_t sw, int32_t sh, int32_t dx, int32_t dy);
extern void     cvm_sys_cron_text        (const char* s, int32_t len, int32_t x, int32_t y, int32_t color);
extern void     cvm_sys_cron_present     (void);

extern void     cvm_sys_cron_snd_tone    (int32_t ch, int32_t wave, int32_t freq_mhz, int32_t vol, int32_t pan);
extern void     cvm_sys_cron_snd_stop    (int32_t ch);
extern void     cvm_sys_cron_snd_master  (int32_t vol_q8);
extern void     cvm_sys_cron_sample      (int32_t slot, const void* ptr, int32_t len, int32_t rate, int32_t fmt);
extern int32_t  cvm_sys_cron_stream       (const int16_t* frames, int32_t nframes);
extern int32_t  cvm_sys_cron_stream_free  (void);
extern void     cvm_sys_cron_pcm         (int32_t voice, int32_t sample, int32_t pitch_q16, int32_t vol, int32_t pan, int32_t loop);
extern void     cvm_sys_cron_env         (int32_t voice, int32_t attack_ms, int32_t decay_ms, int32_t sustain, int32_t release_ms);
extern void     cvm_sys_cron_note_off    (int32_t voice);
extern int32_t  cvm_sys_cron_mod_play     (const void* mod, int32_t len, int32_t loop);
extern void     cvm_sys_cron_mod_stop     (void);

extern uint32_t cvm_sys_cron_pad         (int32_t player);
extern uint32_t cvm_sys_cron_pad_pressed (int32_t player);
extern uint32_t cvm_sys_cron_pad_released(int32_t player);
extern int32_t  cvm_sys_cron_key         (int32_t scancode);
extern uint32_t cvm_sys_cron_mouse       (int32_t* out_x, int32_t* out_y);

extern int32_t  cvm_sys_cron_save_read   (uint8_t* dst, int32_t len);
extern int32_t  cvm_sys_cron_save_write  (const uint8_t* src, int32_t len);

/* Extended graphics (0x100): banks, sprites, tilemaps, shapes, draw state. */
extern void     cvm_sys_cron_image       (int32_t slot, const uint8_t* ptr, int32_t w, int32_t h);
extern void     cvm_sys_cron_tilemap     (int32_t slot, const uint16_t* ptr, int32_t w, int32_t h, int32_t img);
extern void     cvm_sys_cron_blt         (int32_t img, int32_t dx, int32_t dy, int32_t sx, int32_t sy, int32_t w, int32_t h, int32_t colkey);
extern void     cvm_sys_cron_bltm        (int32_t tm, int32_t dx, int32_t dy, int32_t sx, int32_t sy, int32_t w, int32_t h, int32_t colkey);
extern void     cvm_sys_cron_rectb       (int32_t x, int32_t y, int32_t w, int32_t h, int32_t color);
extern void     cvm_sys_cron_circ        (int32_t x, int32_t y, int32_t r, int32_t color);
extern void     cvm_sys_cron_circb       (int32_t x, int32_t y, int32_t r, int32_t color);
extern void     cvm_sys_cron_elli        (int32_t x, int32_t y, int32_t w, int32_t h, int32_t color);
extern void     cvm_sys_cron_ellib       (int32_t x, int32_t y, int32_t w, int32_t h, int32_t color);
extern void     cvm_sys_cron_tri         (int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t color);
extern void     cvm_sys_cron_trib        (int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t color);
extern void     cvm_sys_cron_fill        (int32_t x, int32_t y, int32_t color);
extern void     cvm_sys_cron_clip        (int32_t x, int32_t y, int32_t w, int32_t h);
extern void     cvm_sys_cron_clip_reset  (void);
extern void     cvm_sys_cron_camera      (int32_t x, int32_t y);
extern void     cvm_sys_cron_camera_reset(void);
extern void     cvm_sys_cron_pal         (int32_t c0, int32_t c1);
extern void     cvm_sys_cron_pal_reset   (void);
/* blt_ex receives sx/sy and w/h packed (the cron_blt_ex wrapper packs them). */
extern void     cvm_sys_cron_blt_ex      (int32_t img, int32_t dx, int32_t dy, int32_t srcpack, int32_t dimpack, int32_t colkey, int32_t rotate, int32_t scale_q16);
extern void     cvm_sys_cron_cmap        (const uint8_t* ptr);
extern void     cvm_sys_cron_tcol        (int32_t x, int32_t y0, int32_t y1, const uint8_t* src, int32_t mask, int32_t frac, int32_t step);
extern void     cvm_sys_cron_tspan       (int32_t y, int32_t x0, int32_t x1, const uint8_t* src, int32_t u, int32_t v, int32_t du, int32_t dv);

/* 3D triangle submission (0x120). */
extern void     cvm_sys_cron_zbuf        (int32_t* zbuffer);
extern void     cvm_sys_cron_zclear      (int32_t far);
extern void     cvm_sys_cron_polys       (int32_t mode, const void* verts, int32_t count, int32_t arg, int32_t colkey);

/* ---------------- User-facing aliases (the `cron_*` names) -------------- */

static inline void     cron_log         (const char* m, int32_t n)        { cvm_sys_cron_log(m, n); }
static inline void     cron_trace_i32   (int32_t t, int32_t v)            { cvm_sys_cron_trace_i32(t, v); }
static inline void     cron_set_frame   (void (*fn)(void))                { cvm_sys_cron_set_frame(fn); }
static inline void     cron_exit        (int32_t s)                       { cvm_sys_cron_exit(s); }

static inline uint32_t cron_time_ms     (void)                            { return cvm_sys_cron_time_ms(); }
static inline uint32_t cron_frame_count (void)                            { return cvm_sys_cron_frame_count(); }
static inline uint32_t cron_random      (void)                            { return cvm_sys_cron_random(); }
static inline void     cron_seed        (uint32_t s)                      { cvm_sys_cron_seed(s); }

static inline void     cron_palette_set (int32_t i, uint32_t c)           { cvm_sys_cron_palette_set(i, c); }
static inline uint32_t cron_palette_get (int32_t i)                       { return cvm_sys_cron_palette_get(i); }
static inline void     cron_cls         (int32_t c)                       { cvm_sys_cron_cls(c); }
static inline void     cron_pset        (int32_t x, int32_t y, int32_t c) { cvm_sys_cron_pset(x, y, c); }
static inline void     cron_rect        (int32_t x, int32_t y, int32_t w, int32_t h, int32_t c) { cvm_sys_cron_rect(x, y, w, h, c); }
static inline void     cron_line        (int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t c) { cvm_sys_cron_line(x0, y0, x1, y1, c); }
static inline void     cron_blit        (const uint8_t* s, int32_t sw, int32_t sh, int32_t dx, int32_t dy) { cvm_sys_cron_blit(s, sw, sh, dx, dy); }
static inline void     cron_text        (const char* s, int32_t n, int32_t x, int32_t y, int32_t c) { cvm_sys_cron_text(s, n, x, y, c); }
static inline void     cron_present     (void)                            { cvm_sys_cron_present(); }

static inline void     cron_snd_tone    (int32_t ch, int32_t w, int32_t f, int32_t v, int32_t p) { cvm_sys_cron_snd_tone(ch, w, f, v, p); }
static inline void     cron_snd_stop    (int32_t ch)                      { cvm_sys_cron_snd_stop(ch); }
static inline void     cron_snd_master  (int32_t v)                       { cvm_sys_cron_snd_master(v); }
/* Register an 8-bit *signed* mono PCM sample (in RAM or ROM) as bank `slot`. */
static inline void     cron_sample      (int32_t slot, const int8_t* ptr, int32_t len, int32_t rate) { cvm_sys_cron_sample(slot, ptr, len, rate, 0); }
/* Register an 8-bit *unsigned* mono PCM sample (e.g. a DOOM DMX DS* lump,
 * played straight from cart ROM) as bank `slot`. */
static inline void     cron_sample_u8   (int32_t slot, const uint8_t* ptr, int32_t len, int32_t rate) { cvm_sys_cron_sample(slot, ptr, len, rate, 1); }
/* Play sample bank `s` on voice `v`. pitch 0x10000 = native rate. */
static inline void     cron_pcm         (int32_t v, int32_t s, int32_t pitch_q16, int32_t vol, int32_t pan, int32_t loop) { cvm_sys_cron_pcm(v, s, pitch_q16, vol, pan, loop); }
#define CRON_PITCH_1X  0x10000
/* ADSR envelope (ms; sustain 0..255) applied to the next trigger on voice v. */
static inline void     cron_env         (int32_t v, int32_t a_ms, int32_t d_ms, int32_t sustain, int32_t r_ms) { cvm_sys_cron_env(v, a_ms, d_ms, sustain, r_ms); }
static inline void     cron_note_off    (int32_t v)                       { cvm_sys_cron_note_off(v); }
/* Play a ProTracker .mod (in RAM or cart ROM) on voices 0..n_channels-1.
 * Returns 0 on success, -1 if not a recognised MOD. */
static inline int32_t  cron_mod_play     (const void* mod, int32_t len, int32_t loop) { return cvm_sys_cron_mod_play(mod, len, loop); }
static inline void     cron_mod_stop     (void)                           { cvm_sys_cron_mod_stop(); }

/* Streaming PCM: push `nframes` 16-bit signed *stereo* frames (interleaved
 * L,R) into the host's playback ring; returns frames actually queued.
 * cron_stream_free() returns how many frames the ring can accept now. Use
 * it for music a cart renders itself — e.g. a DOOM MUS through the port's
 * own OPL2 emulator — or any streamed audio. */
static inline int32_t  cron_stream       (const int16_t* frames, int32_t nframes) { return cvm_sys_cron_stream(frames, nframes); }
static inline int32_t  cron_stream_free  (void)                           { return cvm_sys_cron_stream_free(); }

static inline uint32_t cron_pad         (int32_t p)                       { return cvm_sys_cron_pad(p); }
static inline uint32_t cron_pad_pressed (int32_t p)                       { return cvm_sys_cron_pad_pressed(p); }
static inline uint32_t cron_pad_released(int32_t p)                       { return cvm_sys_cron_pad_released(p); }
static inline int32_t  cron_key         (int32_t s)                       { return cvm_sys_cron_key(s); }
static inline uint32_t cron_mouse       (int32_t* x, int32_t* y)          { return cvm_sys_cron_mouse(x, y); }

static inline int32_t  cron_save_read   (uint8_t* d, int32_t n)           { return cvm_sys_cron_save_read(d, n); }
static inline int32_t  cron_save_write  (const uint8_t* s, int32_t n)     { return cvm_sys_cron_save_write(s, n); }

/* Extended graphics ---------------------------------------------------*/
/* Register an 8bpp sprite sheet (in RAM or ROM) as image bank `slot`. */
static inline void     cron_image       (int32_t slot, const uint8_t* ptr, int32_t w, int32_t h) { cvm_sys_cron_image(slot, ptr, w, h); }
/* Register a w*h grid of u16 tile indices (0xFFFF = empty) as tilemap
 * `slot`, drawing 8x8 tiles from image bank `img`. */
static inline void     cron_tilemap     (int32_t slot, const uint16_t* ptr, int32_t w, int32_t h, int32_t img) { cvm_sys_cron_tilemap(slot, ptr, w, h, img); }
/* Blit (sx,sy,w,h) of image bank `img` to (dx,dy). colkey<0 = opaque;
 * negative w/h flip. */
static inline void     cron_blt         (int32_t img, int32_t dx, int32_t dy, int32_t sx, int32_t sy, int32_t w, int32_t h, int32_t colkey) { cvm_sys_cron_blt(img, dx, dy, sx, sy, w, h, colkey); }
static inline void     cron_bltm        (int32_t tm, int32_t dx, int32_t dy, int32_t sx, int32_t sy, int32_t w, int32_t h, int32_t colkey) { cvm_sys_cron_bltm(tm, dx, dy, sx, sy, w, h, colkey); }
static inline void     cron_rectb       (int32_t x, int32_t y, int32_t w, int32_t h, int32_t c) { cvm_sys_cron_rectb(x, y, w, h, c); }
static inline void     cron_circ        (int32_t x, int32_t y, int32_t r, int32_t c) { cvm_sys_cron_circ(x, y, r, c); }
static inline void     cron_circb       (int32_t x, int32_t y, int32_t r, int32_t c) { cvm_sys_cron_circb(x, y, r, c); }
static inline void     cron_elli        (int32_t x, int32_t y, int32_t w, int32_t h, int32_t c) { cvm_sys_cron_elli(x, y, w, h, c); }
static inline void     cron_ellib       (int32_t x, int32_t y, int32_t w, int32_t h, int32_t c) { cvm_sys_cron_ellib(x, y, w, h, c); }
static inline void     cron_tri         (int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t c) { cvm_sys_cron_tri(x0, y0, x1, y1, x2, y2, c); }
static inline void     cron_trib        (int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t c) { cvm_sys_cron_trib(x0, y0, x1, y1, x2, y2, c); }
static inline void     cron_fill        (int32_t x, int32_t y, int32_t c) { cvm_sys_cron_fill(x, y, c); }
static inline void     cron_clip        (int32_t x, int32_t y, int32_t w, int32_t h) { cvm_sys_cron_clip(x, y, w, h); }
static inline void     cron_clip_reset  (void) { cvm_sys_cron_clip_reset(); }
static inline void     cron_camera      (int32_t x, int32_t y) { cvm_sys_cron_camera(x, y); }
static inline void     cron_camera_reset(void) { cvm_sys_cron_camera_reset(); }
static inline void     cron_pal         (int32_t c0, int32_t c1) { cvm_sys_cron_pal(c0, c1); }
static inline void     cron_pal_reset   (void) { cvm_sys_cron_pal_reset(); }

/* Rotozoom blit: scale is Q16.16 (0x10000 = 1.0), rotate in degrees
 * clockwise, both around the sprite centre (placed at dx+w/2, dy+h/2). */
#define CRON_SCALE_1X  (0x10000)
static inline void     cron_blt_ex      (int32_t img, int32_t dx, int32_t dy,
                                         int32_t sx, int32_t sy, int32_t w, int32_t h,
                                         int32_t colkey, int32_t rotate, int32_t scale_q16) {
    cvm_sys_cron_blt_ex(img, dx, dy,
                        (int32_t)(((uint32_t)sx << 16) | ((uint32_t)sy & 0xFFFFu)),
                        (int32_t)(((uint32_t)w  << 16) | ((uint32_t)h  & 0xFFFFu)),
                        colkey, rotate, scale_q16);
}

/* Software-3D rasteriser accelerators (DOOM-style). cron_cmap sets the
 * active 256-byte light/colormap (NULL = identity); tcol/tspan write
 * cmap[texel] honouring the clip rect (camera and pal do not apply). */
static inline void     cron_cmap        (const uint8_t* ptr) { cvm_sys_cron_cmap(ptr); }
/* Vertical textured column: rows [y0,y1] at screen x; src is (mask+1) bytes
 * (mask = texture_height-1, power of two); frac/step are Q16.16. */
static inline void     cron_tcol        (int32_t x, int32_t y0, int32_t y1, const uint8_t* src, int32_t mask, int32_t frac, int32_t step) { cvm_sys_cron_tcol(x, y0, y1, src, mask, frac, step); }
/* Horizontal textured span over a 64x64 source: cols [x0,x1] at screen y;
 * (u,v) Q16.16 advance by (du,dv). */
static inline void     cron_tspan       (int32_t y, int32_t x0, int32_t x1, const uint8_t* src, int32_t u, int32_t v, int32_t du, int32_t dv) { cvm_sys_cron_tspan(y, x0, x1, src, u, v, du, dv); }

/* 3D triangle submission. Fill a cron_vert_t array, then cron_polys() draws
 * count/3 triangles. Transform/project/light in fixed point (Q16.16) on the
 * cart; the host rasterises. See docs/syscalls.md. */
typedef struct {
    int32_t x, y;   /* screen pixels */
    int32_t z;      /* depth (CRON_POLY_ZTEST: nearer = smaller) */
    int32_t u, v;   /* texcoords, Q16.16 texels (CRON_POLY_TEX) */
    int32_t w;      /* perspective depth (CRON_POLY_PERSP) */
    int32_t c;      /* gouraud light/index (CRON_POLY_GOURAUD) */
} cron_vert_t;

enum {
    CRON_POLY_FLAT    = 0,
    CRON_POLY_GOURAUD = 1 << 0,   /* interpolate vertex .c through the cmap */
    CRON_POLY_TEX     = 1 << 1,   /* affine texture from image bank `arg`    */
    CRON_POLY_PERSP   = 1 << 2,   /* perspective-correct texture (uses .w)   */
    CRON_POLY_ZTEST   = 1 << 3,   /* depth test/write the bound z-buffer     */
};

/* Bind a 320*240 int32 depth buffer (NULL disables). */
static inline void     cron_zbuf         (int32_t* zb) { cvm_sys_cron_zbuf(zb); }
/* Clear the bound z-buffer to `far` (e.g. 0x7FFFFFFF). */
static inline void     cron_zclear       (int32_t far) { cvm_sys_cron_zclear(far); }
/* Draw count/3 triangles. arg = flat colour, or image bank when TEX; colkey
 * = transparent texel or -1. */
static inline void     cron_polys        (int32_t mode, const cron_vert_t* verts, int32_t count, int32_t arg, int32_t colkey) { cvm_sys_cron_polys(mode, verts, count, arg, colkey); }

/* Video pointers — populated by cron_resolve_video(). Until that is called
 * they are NULL; reading/writing through them then would crash, so always
 * call cron_resolve_video() in your main() before drawing. */
extern volatile uint8_t  *CRON_FB;
extern volatile uint32_t *CRON_PAL;

/* Resolve the "fb" and "pal" host regions and store them in CRON_FB /
 * CRON_PAL. Returns 0 on success, -1 if either region is missing (which
 * means the build is missing the --region flags). */
static inline int cron_resolve_video(void) {
    int32_t fb  = cvm_sys_get_region("fb");
    int32_t pal = cvm_sys_get_region("pal");
    if (fb < 0 || pal < 0) return -1;
    CRON_FB  = (volatile uint8_t  *)(uintptr_t)fb;
    CRON_PAL = (volatile uint32_t *)(uintptr_t)pal;
    return 0;
}

/* Read-only cartridge ROM (assets baked in with cvm-cc --rom=FILE). Returns
 * a pointer into the cart's address space, NULL if no ROM was bundled. Pair
 * with cron_rom_size() for the length. A DOOM port points its WAD reader
 * here. */
static inline const uint8_t* cron_rom(void) {
    if (cvm_sys_rom_size() == 0) return 0;
    return (const uint8_t*)(uintptr_t)cvm_sys_rom_base();
}
static inline uint32_t cron_rom_size(void) {
    return (uint32_t)cvm_sys_rom_size();
}

/* ---------------- Cartridge entry point ------------------------------------
 *
 * The host expects a cart to: define storage for CRON_FB / CRON_PAL, resolve
 * the video regions, and register a per-frame callback from main(). That is
 * the same five lines in every cart, so these macros write them for you.
 *
 * Use ONE of them in exactly one source file:
 *
 *   #include <cronopio.h>
 *   void frame(void) { ... }      // called at 60 Hz
 *   CRONOPIO_CART(frame)          // <- generates main() + FB/PAL storage
 *
 * or, when you have one-time setup to run before the first frame:
 *
 *   void setup(void) { ... }      // called once at boot
 *   void frame(void) { ... }
 *   CRONOPIO_CART_INIT(setup, frame)
 *
 * If a cart needs full control of main() (e.g. it draws without a frame
 * callback, or wants a custom error path) it can still define CRON_FB /
 * CRON_PAL and main() by hand instead of using these — see examples/hello. */

/* Storage the macros and a hand-written main() share. Defining these in the
 * header (guarded so it lands in exactly one TU) is what removes the
 * boilerplate; a hand-rolled main() that wants them defines CRONOPIO_NO_MAIN
 * is *not* needed — it simply declares them itself as before. */
#define CRONOPIO_CART_STORAGE \
    volatile uint8_t  *CRON_FB  = 0; \
    volatile uint32_t *CRON_PAL = 0;

#define CRONOPIO_CART_INIT(setup_fn, frame_fn)                          \
    CRONOPIO_CART_STORAGE                                               \
    int main(void) {                                                   \
        if (cron_resolve_video() != 0) {                               \
            static const char _e[] =                                   \
                "cart: fb/pal regions missing — build via cronopio-cc " \
                "or CronopioCart.cmake\n";                             \
            cron_log(_e, (int32_t)sizeof(_e) - 1);                     \
            return 1;                                                  \
        }                                                              \
        (setup_fn)();                                                  \
        cron_set_frame(frame_fn);                                      \
        return 0;                                                      \
    }

/* Frame-only variant: no setup pass. */
static inline void cron__noop_setup(void) { }
#define CRONOPIO_CART(frame_fn) CRONOPIO_CART_INIT(cron__noop_setup, frame_fn)

#endif
