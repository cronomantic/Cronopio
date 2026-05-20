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
#define CRON_PAL_SIZE   32
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

extern uint32_t cvm_sys_cron_pad         (int32_t player);
extern uint32_t cvm_sys_cron_pad_pressed (int32_t player);
extern uint32_t cvm_sys_cron_pad_released(int32_t player);
extern int32_t  cvm_sys_cron_key         (int32_t scancode);
extern uint32_t cvm_sys_cron_mouse       (int32_t* out_x, int32_t* out_y);

extern int32_t  cvm_sys_cron_save_read   (uint8_t* dst, int32_t len);
extern int32_t  cvm_sys_cron_save_write  (const uint8_t* src, int32_t len);

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

static inline uint32_t cron_pad         (int32_t p)                       { return cvm_sys_cron_pad(p); }
static inline uint32_t cron_pad_pressed (int32_t p)                       { return cvm_sys_cron_pad_pressed(p); }
static inline uint32_t cron_pad_released(int32_t p)                       { return cvm_sys_cron_pad_released(p); }
static inline int32_t  cron_key         (int32_t s)                       { return cvm_sys_cron_key(s); }
static inline uint32_t cron_mouse       (int32_t* x, int32_t* y)          { return cvm_sys_cron_mouse(x, y); }

static inline int32_t  cron_save_read   (uint8_t* d, int32_t n)           { return cvm_sys_cron_save_read(d, n); }
static inline int32_t  cron_save_write  (const uint8_t* s, int32_t n)     { return cvm_sys_cron_save_write(s, n); }

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

#endif
