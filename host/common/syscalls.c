/* Cronopio syscall dispatch — wires the console state to CronoVM imports.
 *
 * Each handler matches `cvm_syscall_fn`: it reads its arguments from
 * regs[0..N-1] and writes the return value (if any) into regs[0]. Pointer
 * arguments arriving from the VM are guest heap offsets that we either
 *   - pass through to GPU primitives that already operate on `heap` + offset
 *   - or validate via cvm_heap_read / cvm_heap_write for bulk reads.
 *
 * The import-name convention follows CronoVM: every cart-facing symbol is
 * prefixed with `cvm_sys_`, which is what the translator emits into the
 * IMPORTS section when it sees a `cvm_sys_*` reference in the bitcode. */

#include "syscalls.h"

#include "cvm.h"
#include "cron_ogg.h"
#include "cron_module.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The user_data pointer passed through cvm_link points at this struct so
 * handlers have access to both the running image (for heap_read/write) and
 * the host-side console state. */
typedef struct {
    cronopio_console_t *c;
    struct cvm_image   *img;
} ctx_t;

/* Single static context — v0 runs one cart at a time. If we ever need to
 * embed multiple carts in one host this becomes per-image. */
static ctx_t g_ctx;

/* ------ helpers --------------------------------------------------------- */

static int sane_str(struct cvm_image *img, uint32_t addr, uint32_t len,
                    char *buf, size_t cap)
{
    if (len == 0) { buf[0] = '\0'; return 0; }
    if (len >= cap) len = (uint32_t)(cap - 1);
    if (cvm_heap_read(img, addr, buf, len) != CVM_OK) return -1;
    buf[len] = '\0';
    return 0;
}

/* ------ core ------------------------------------------------------------ */

static int sys_log(struct cvm_image *img, int32_t *r, void *ud) {
    (void)ud;
    uint32_t addr = (uint32_t)r[0];
    int32_t  len  = r[1];
    if (len < 0) len = 0;
    char buf[512];
    if (sane_str(img, addr, (uint32_t)len, buf, sizeof(buf)) != 0) return -1;
    /* sane_str clamps the copy to sizeof(buf)-1; mirror that here so we never
     * fwrite past what was actually read into buf. */
    if ((size_t)len >= sizeof(buf)) len = (int32_t)(sizeof(buf) - 1);
    /* Write raw: the cart controls newlines. Forcing a '\n' per call broke
     * progress output like printf(".") with no trailing newline (one dot per
     * line). fwrite by length so embedded NULs / no terminator are fine. */
    fwrite(buf, 1, (size_t)len, stderr);
    r[0] = 0;
    return 0;
}

static int sys_trace_i32(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img; (void)ud;
    fprintf(stderr, "[trace %d] %d\n", (int)r[0], (int)r[1]);
    r[0] = 0;
    return 0;
}

static int sys_set_frame(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    ctx_t *x = (ctx_t*)ud;
    /* CronoVM treats function pointers as FUNCS indices (CALLR reads R[A]
     * as the index). The cart writes one such value into R0 before this
     * SYSCALL, so we just record it. The platform loop will hand it to
     * cvm_call once per tick. */
    x->c->frame_fn_index = r[0];
    r[0] = 0;
    return 0;
}

static int sys_exit(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    ctx_t *x = (ctx_t*)ud;
    x->c->cart_exited  = 1;
    x->c->exit_status  = r[0];
    /* Trap the run; the host detects cart_exited and stops the platform
     * loop after the current frame. Trapping is the only way to make
     * cvm_run return from inside a syscall. */
    return CVM_E_SYSCALL_TRAP;
}

/* ------ system / time --------------------------------------------------- */

static int sys_time_ms(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    ctx_t *x = (ctx_t*)ud;
    /* boot_ms is the SDL_GetTicks-at-start sample stamped by the platform
     * shell; we compute "ms since boot" relative to that. */
    extern uint64_t cronopio_platform_ticks_ms(void);
    uint64_t now = cronopio_platform_ticks_ms();
    r[0] = (int32_t)(uint32_t)(now - x->c->boot_ms);
    return 0;
}

static int sys_frame_count(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    ctx_t *x = (ctx_t*)ud;
    r[0] = (int32_t)x->c->frame_count;
    return 0;
}

static int sys_random(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    ctx_t *x = (ctx_t*)ud;
    /* xorshift32 */
    uint32_t s = x->c->prng_state;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    if (s == 0) s = 1;
    x->c->prng_state = s;
    r[0] = (int32_t)s;
    return 0;
}

static int sys_seed(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    ctx_t *x = (ctx_t*)ud;
    x->c->prng_state = (uint32_t)r[0] ? (uint32_t)r[0] : 1u;
    r[0] = 0;
    return 0;
}

/* ------ display --------------------------------------------------------- */

static int sys_palette_set(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    ctx_t   *x   = (ctx_t*)ud;
    int32_t  i   = r[0];
    uint32_t rgb = (uint32_t)r[1];
    if ((unsigned)i >= CRONOPIO_PALETTE_SIZE || !x->c->regions_ok) { r[0] = 0; return 0; }
    uint8_t *p = x->img->heap + x->c->pal_offset + (uint32_t)i * 4u;
    p[0] = (uint8_t)(rgb       & 0xFF);
    p[1] = (uint8_t)((rgb>> 8) & 0xFF);
    p[2] = (uint8_t)((rgb>>16) & 0xFF);
    p[3] = 0;
    r[0] = 0;
    return 0;
}

static int sys_palette_get(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    ctx_t   *x = (ctx_t*)ud;
    int32_t  i = r[0];
    if ((unsigned)i >= CRONOPIO_PALETTE_SIZE || !x->c->regions_ok) { r[0] = 0; return 0; }
    const uint8_t *p = x->img->heap + x->c->pal_offset + (uint32_t)i * 4u;
    r[0] = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16));
    return 0;
}

static int sys_cls(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    ctx_t *x = (ctx_t*)ud;
    if (x->c->regions_ok)
        cron_gpu_cls(x->c, x->img->heap, (int)r[0]);
    r[0] = 0;
    return 0;
}

static int sys_pset(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    ctx_t *x = (ctx_t*)ud;
    if (x->c->regions_ok)
        cron_gpu_pset(x->c, x->img->heap, (int)r[0], (int)r[1], (int)r[2]);
    r[0] = 0;
    return 0;
}

static int sys_rect(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    ctx_t *x = (ctx_t*)ud;
    if (x->c->regions_ok)
        cron_gpu_rect(x->c, x->img->heap,
                      (int)r[0], (int)r[1], (int)r[2], (int)r[3], (int)r[4]);
    r[0] = 0;
    return 0;
}

static int sys_line(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    ctx_t *x = (ctx_t*)ud;
    if (x->c->regions_ok)
        cron_gpu_line(x->c, x->img->heap,
                      (int)r[0], (int)r[1], (int)r[2], (int)r[3], (int)r[4]);
    r[0] = 0;
    return 0;
}

static int sys_blit(struct cvm_image *img, int32_t *r, void *ud) {
    ctx_t *x = (ctx_t*)ud;
    uint32_t src_addr = (uint32_t)r[0];
    int32_t  sw = r[1], sh = r[2], dx = r[3], dy = r[4];
    if (!x->c->regions_ok || sw <= 0 || sh <= 0) { r[0] = 0; return 0; }
    /* Copy the source bitmap out through the bounds-checked accessor so an
     * over-large sw*sh from the cart can't run off the heap. */
    size_t n = (size_t)sw * (size_t)sh;
    uint8_t *tmp = (uint8_t*)malloc(n);
    if (!tmp) return -1;
    if (cvm_heap_read(img, src_addr, tmp, n) != CVM_OK) { free(tmp); return -1; }
    cron_gpu_blit_raw(x->c, x->img->heap, tmp, sw, sh, dx, dy);
    free(tmp);
    r[0] = 0;
    return 0;
}

static int sys_text(struct cvm_image *img, int32_t *r, void *ud) {
    ctx_t   *x   = (ctx_t*)ud;
    uint32_t sa  = (uint32_t)r[0];
    int32_t  len = r[1];
    if (!x->c->regions_ok || len <= 0) { r[0] = 0; return 0; }
    char buf[256];
    if (sane_str(img, sa, (uint32_t)len, buf, sizeof(buf)) != 0) return -1;
    cron_gpu_text(x->c, x->img->heap, buf, (int)strlen(buf),
                  (int)r[2], (int)r[3], (int)r[4]);
    r[0] = 0;
    return 0;
}

static int sys_present(struct cvm_image *img, int32_t *r, void *ud) {
    /* If the shell registered a present hook, flush now (lets a cart paint
     * during a blocking entry — e.g. a loading screen while D_DoomMain runs).
     * Otherwise a no-op: the shell blits + vsyncs after the frame fn returns. */
    (void)img;
    cronopio_console_t *c = ((ctx_t*)ud)->c;
    if (c->present_cb) c->present_cb(c->present_ud);
    r[0] = 0;
    return 0;
}

/* ------ extended graphics (0x100): sprites, tilemaps, shapes, state ----- */

#define X ((ctx_t*)ud)
#define GUARD if (!X->c->regions_ok) { r[0] = 0; return 0; }
#define HEAP (X->img->heap)

static int sys_image(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    cron_gpu_image(X->c, (int)r[0], (uint32_t)r[1], (int)r[2], (int)r[3], X->img->mem_size);
    r[0] = 0; return 0;
}
static int sys_tilemap(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    cron_gpu_tilemap(X->c, (int)r[0], (uint32_t)r[1], (int)r[2], (int)r[3], (int)r[4], X->img->mem_size);
    r[0] = 0; return 0;
}
static int sys_blt(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img; GUARD;
    cron_gpu_blt(X->c, HEAP, (int)r[0], (int)r[1], (int)r[2], (int)r[3], (int)r[4], (int)r[5], (int)r[6], (int)r[7]);
    r[0] = 0; return 0;
}
/* sys_blt_buf — buffer->buffer colour-key blit (native). 8 args, packed:
 *   r0 dst off | r1 dst_w<<16|dst_h | r2 dst_pitch | r3 src off | r4 src_pitch |
 *   r5 dx<<16|dy (signed i16) | r6 blt_w<<16|blt_h | r7 colkey */
static int sys_blt_buf(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img; GUARD;
    int dst_w = (int)((uint32_t)r[1] >> 16),  dst_h = (int)((uint32_t)r[1] & 0xFFFF);
    int dx    = (int)(int16_t)((uint32_t)r[5] >> 16);
    int dy    = (int)(int16_t)((uint32_t)r[5] & 0xFFFF);
    int blt_w = (int)((uint32_t)r[6] >> 16),  blt_h = (int)((uint32_t)r[6] & 0xFFFF);
    cron_gpu_blt_buf(X->c, HEAP, X->img->mem_size,
                     (uint32_t)r[0], dst_w, dst_h, (int)r[2],
                     (uint32_t)r[3], (int)r[4], dx, dy, blt_w, blt_h, (int)r[7], 0);
    r[0] = 0; return 0;
}
/* sys_blt_buf_blend — like sys_blt_buf but r7 packs the blend slot in the high
 * 16 bits and the colour-key (i16) in the low 16: r7 = blend_slot<<16 | (colkey
 * & 0xFFFF). Composites each written pixel through that 256x256 LUT. */
static int sys_blt_buf_blend(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img; GUARD;
    int dst_w = (int)((uint32_t)r[1] >> 16),  dst_h = (int)((uint32_t)r[1] & 0xFFFF);
    int dx    = (int)(int16_t)((uint32_t)r[5] >> 16);
    int dy    = (int)(int16_t)((uint32_t)r[5] & 0xFFFF);
    int blt_w = (int)((uint32_t)r[6] >> 16),  blt_h = (int)((uint32_t)r[6] & 0xFFFF);
    int colkey      = (int)(int16_t)((uint32_t)r[7] & 0xFFFF);
    int blend_slot  = (int)(((uint32_t)r[7] >> 16) & 0xFF);
    cron_gpu_blt_buf(X->c, HEAP, X->img->mem_size,
                     (uint32_t)r[0], dst_w, dst_h, (int)r[2],
                     (uint32_t)r[3], (int)r[4], dx, dy, blt_w, blt_h, colkey, blend_slot);
    r[0] = 0; return 0;
}
static int sys_bltm(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img; GUARD;
    cron_gpu_bltm(X->c, HEAP, (int)r[0], (int)r[1], (int)r[2], (int)r[3], (int)r[4], (int)r[5], (int)r[6], (int)r[7]);
    r[0] = 0; return 0;
}
/* sys_bltm_raster — 7 args (fits in R0..R6). sx/sy and w/h are packed as
 * i16 lo/hi pairs to keep the count under 8, mirroring cron_blt_ex's
 * srcpack/dimpack convention. The SDK inline wrapper does the packing. */
static int sys_bltm_raster(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img; GUARD;
    int16_t  sx_i = (int16_t)(r[3] & 0xFFFF), sy_i = (int16_t)(r[3] >> 16);
    int16_t  w_i  = (int16_t)(r[4] & 0xFFFF), h_i  = (int16_t)(r[4] >> 16);
    cron_gpu_bltm_raster(X->c, HEAP, (int)r[0], (int)r[1], (int)r[2],
                         sx_i, sy_i, w_i, h_i, (int)r[5], (uint32_t)r[6]);
    r[0] = 0; return 0;
}
/* sys_blt_flip — 7 args (R0..R6). sx/sy and w/h packed (mirror cron_blt_ex). */
static int sys_blt_flip(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img; GUARD;
    int16_t sx_i = (int16_t)(r[3] & 0xFFFF), sy_i = (int16_t)(r[3] >> 16);
    int16_t w_i  = (int16_t)(r[4] & 0xFFFF), h_i  = (int16_t)(r[4] >> 16);
    cron_gpu_blt_flip(X->c, HEAP, (int)r[0], (int)r[1], (int)r[2],
                      sx_i, sy_i, w_i, h_i, (int)r[5], (int)r[6]);
    r[0] = 0; return 0;
}
/* sys_blt_scale — 8 args (R0..R7). srcpack/dimpack as usual. */
static int sys_blt_scale(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img; GUARD;
    int16_t sx_i = (int16_t)(r[3] & 0xFFFF), sy_i = (int16_t)(r[3] >> 16);
    int16_t w_i  = (int16_t)(r[4] & 0xFFFF), h_i  = (int16_t)(r[4] >> 16);
    cron_gpu_blt_scale(X->c, HEAP, (int)r[0], (int)r[1], (int)r[2],
                       sx_i, sy_i, w_i, h_i, (int)r[5], (int)r[6], (int)r[7]);
    r[0] = 0; return 0;
}

/* sys_palette_bank — registers a 256-byte remap table as bank `slot`.
 * 2 args: (slot, offset). */
static int sys_palette_bank(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img; GUARD;
    cron_gpu_palette_bank(X->c, (int)r[0], (uint32_t)r[1], X->img->mem_size);
    r[0] = 0; return 0;
}
/* sys_tile_anim — registers a cron_tile_anim_t table for an image bank.
 * 3 args: (img_slot, table_offset, count). count==0 clears. */
static int sys_tile_anim(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img; GUARD;
    cron_gpu_tile_anim(X->c, (int)r[0], (uint32_t)r[1], (int)r[2]);
    r[0] = 0; return 0;
}
/* sys_blend_table — registers a 64 KB 256x256 blend LUT as slot 1..7.
 * 2 args: (slot, offset). */
static int sys_blend_table(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img; GUARD;
    cron_gpu_blend_table(X->c, (int)r[0], (uint32_t)r[1], X->img->mem_size);
    r[0] = 0; return 0;
}
/* sys_blend_set — sets the active blend slot. 0 disables. */
static int sys_blend_set(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img; GUARD;
    cron_gpu_blend_set(X->c, (int)r[0]);
    r[0] = 0; return 0;
}

/* sys_bltm_affine — 6 args (R0..R5). w/h packed as a single i16 pair. */
static int sys_bltm_affine(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img; GUARD;
    int16_t w_i = (int16_t)(r[3] & 0xFFFF), h_i = (int16_t)(r[3] >> 16);
    cron_gpu_bltm_affine(X->c, HEAP, (int)r[0], (int)r[1], (int)r[2],
                         w_i, h_i, (int)r[4], (uint32_t)r[5]);
    r[0] = 0; return 0;
}
static int sys_rectb(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img; GUARD;
    cron_gpu_rectb(X->c, HEAP, (int)r[0], (int)r[1], (int)r[2], (int)r[3], (int)r[4]);
    r[0] = 0; return 0;
}
static int sys_circ(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img; GUARD;
    cron_gpu_circ(X->c, HEAP, (int)r[0], (int)r[1], (int)r[2], (int)r[3]);
    r[0] = 0; return 0;
}
static int sys_circb(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img; GUARD;
    cron_gpu_circb(X->c, HEAP, (int)r[0], (int)r[1], (int)r[2], (int)r[3]);
    r[0] = 0; return 0;
}
static int sys_elli(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img; GUARD;
    cron_gpu_elli(X->c, HEAP, (int)r[0], (int)r[1], (int)r[2], (int)r[3], (int)r[4]);
    r[0] = 0; return 0;
}
static int sys_ellib(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img; GUARD;
    cron_gpu_ellib(X->c, HEAP, (int)r[0], (int)r[1], (int)r[2], (int)r[3], (int)r[4]);
    r[0] = 0; return 0;
}
static int sys_tri(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img; GUARD;
    cron_gpu_tri(X->c, HEAP, (int)r[0], (int)r[1], (int)r[2], (int)r[3], (int)r[4], (int)r[5], (int)r[6]);
    r[0] = 0; return 0;
}
static int sys_trib(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img; GUARD;
    cron_gpu_trib(X->c, HEAP, (int)r[0], (int)r[1], (int)r[2], (int)r[3], (int)r[4], (int)r[5], (int)r[6]);
    r[0] = 0; return 0;
}
static int sys_fill(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img; GUARD;
    cron_gpu_fill(X->c, HEAP, (int)r[0], (int)r[1], (int)r[2]);
    r[0] = 0; return 0;
}
static int sys_clip(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    cron_gpu_clip(X->c, (int)r[0], (int)r[1], (int)r[2], (int)r[3]);
    r[0] = 0; return 0;
}
static int sys_clip_reset(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img; cron_gpu_clip_reset(X->c); r[0] = 0; return 0;
}
static int sys_camera(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img; cron_gpu_camera(X->c, (int)r[0], (int)r[1]); r[0] = 0; return 0;
}
static int sys_camera_reset(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img; cron_gpu_camera(X->c, 0, 0); r[0] = 0; return 0;
}
static int sys_pal(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img; cron_gpu_pal(X->c, (int)r[0], (int)r[1]); r[0] = 0; return 0;
}
static int sys_pal_reset(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img; cron_gpu_pal_reset(X->c); r[0] = 0; return 0;
}

/* Rotozoom blit. sx/sy and w/h arrive packed (the SDK packs them) so the
 * call fits the 8-arg syscall ABI: src=(sx<<16)|sy, dim=(w<<16)|h. */
static int sys_blt_ex(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img; GUARD;
    uint32_t src = (uint32_t)r[3], dim = (uint32_t)r[4];
    int sx = (int)(src >> 16),  sy = (int)(src & 0xFFFF);
    int w  = (int)(dim >> 16),  h  = (int)(dim & 0xFFFF);
    cron_gpu_blt_ex(X->c, HEAP, (int)r[0], (int)r[1], (int)r[2],
                    sx, sy, w, h, (int)r[5], (int)r[6], (int)r[7]);
    r[0] = 0; return 0;
}

static int sys_cmap(struct cvm_image *img, int32_t *r, void *ud) {
    uint32_t ptr = (uint32_t)r[0];
    if (ptr == 0) { cron_gpu_cmap(X->c, 0, 0); r[0] = 0; return 0; }
    if ((uint64_t)ptr + 256u > img->mem_size) { cron_gpu_cmap(X->c, 0, 0); r[0] = 0; return 0; }
    cron_gpu_cmap(X->c, ptr, 1);
    r[0] = 0; return 0;
}

static int sys_tcol(struct cvm_image *img, int32_t *r, void *ud) {
    GUARD;
    uint32_t src = (uint32_t)r[3];
    uint32_t mask = (uint32_t)r[4];
    if ((uint64_t)src + (uint64_t)mask + 1u > img->mem_size) { r[0] = 0; return 0; }
    cron_gpu_tcol(X->c, HEAP, (int)r[0], (int)r[1], (int)r[2], src, (int)mask, r[5], r[6]);
    r[0] = 0; return 0;
}

static int sys_tcolm(struct cvm_image *img, int32_t *r, void *ud) {
    GUARD;
    uint32_t src  = (uint32_t)r[3];
    int32_t  y0   = r[1], y1 = r[2];
    int32_t  frac = r[4], step = r[5];
    /* Bound the source by the ACTUAL linear index span over [y0,y1] (index =
     * frac>>16 stepping by `step`), not by a power-of-two mask. Check both ends;
     * clipping only shrinks the range, so this is conservative. */
    int     n    = (y1 >= y0) ? (y1 - y0) : 0;
    int64_t i0   = (int64_t)((uint32_t)frac >> 16);
    int64_t i1   = (int64_t)((uint32_t)(frac + step * n) >> 16);
    int64_t imax = i0 > i1 ? i0 : i1;
    if (imax < 0 || (uint64_t)src + (uint64_t)imax + 1u > img->mem_size) { r[0] = 0; return 0; }
    cron_gpu_tcolm(X->c, HEAP, (int)r[0], y0, y1, src, frac, step);
    r[0] = 0; return 0;
}

static int sys_tspan(struct cvm_image *img, int32_t *r, void *ud) {
    GUARD;
    uint32_t src = (uint32_t)r[3];
    if ((uint64_t)src + 4096u > img->mem_size) { r[0] = 0; return 0; }
    cron_gpu_tspan(X->c, HEAP, (int)r[0], (int)r[1], (int)r[2], src, r[4], r[5], r[6], r[7]);
    r[0] = 0; return 0;
}

/* ------ 3D triangle submission ------------------------------------------ */

static int sys_zbuf(struct cvm_image *img, int32_t *r, void *ud) {
    uint32_t ptr = (uint32_t)r[0];
    uint64_t bytes = (uint64_t)CRONOPIO_SCREEN_W * CRONOPIO_SCREEN_H * 4u;
    if (ptr == 0 || (uint64_t)ptr + bytes > img->mem_size) { cron_gpu_zbuf(X->c, 0, 0); r[0] = 0; return 0; }
    cron_gpu_zbuf(X->c, ptr, 1);
    r[0] = 0; return 0;
}
static int sys_zclear(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img; cron_gpu_zclear(X->c, HEAP, r[0]); r[0] = 0; return 0;
}
static int sys_lightmap(struct cvm_image *img, int32_t *r, void *ud) {
    uint32_t ptr = (uint32_t)r[0]; int w = (int)r[1], h = (int)r[2];
    if (ptr == 0 || w <= 0 || h <= 0 ||
        (uint64_t)ptr + (uint64_t)w * h > img->mem_size) {
        cron_gpu_lightmap(X->c, 0, 0, 0, 0); r[0] = 0; return 0;
    }
    cron_gpu_lightmap(X->c, ptr, w, h, 1);
    r[0] = 0; return 0;
}
static int sys_turb(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img; int phase = (int)r[0], amp = (int)r[1];
    if (amp <= 0) { cron_gpu_turb(X->c, 0, 0, 0); r[0] = 0; return 0; }
    cron_gpu_turb(X->c, phase, amp, 1);
    r[0] = 0; return 0;
}
static int sys_colormap(struct cvm_image *img, int32_t *r, void *ud) {
    uint32_t ptr = (uint32_t)r[0]; int levels = (int)r[1];
    if (ptr == 0 || levels <= 0 ||
        (uint64_t)ptr + (uint64_t)levels * 256u > img->mem_size) {
        cron_gpu_colormap(X->c, 0, 0, 0); r[0] = 0; return 0;
    }
    cron_gpu_colormap(X->c, ptr, levels, 1);
    r[0] = 0; return 0;
}
static int sys_polys(struct cvm_image *img, int32_t *r, void *ud) {
    GUARD;
    uint32_t voff  = (uint32_t)r[1];
    int      count = (int)r[2];
    if (count < 3) { r[0] = 0; return 0; }
    uint64_t need = (uint64_t)count * CRONOPIO_VERT_BYTES;
    if ((uint64_t)voff + need > img->mem_size) { r[0] = 0; return 0; }
    cron_gpu_polys(X->c, HEAP, (int)r[0], voff, count, (int)r[3], (int)r[4]);
    r[0] = 0; return 0;
}
static int sys_mvp(struct cvm_image *img, int32_t *r, void *ud) {
    (void)ud;
    uint32_t off = (uint32_t)r[0];
    int      set = (int)r[1];
    if (!set || off == 0 || (uint64_t)off + 64u > img->mem_size) {
        cron_gpu_mvp(X->c, 0, 0); r[0] = 0; return 0;
    }
    /* matrix is 16 little-endian floats in cart memory — copy into a host
     * float[16] so the rasteriser doesn't read from cart memory each tri. */
    float mat[16];
    memcpy(mat, HEAP + off, sizeof mat);
    cron_gpu_mvp(X->c, mat, 1);
    r[0] = 0; return 0;
}
static int sys_xform_polys(struct cvm_image *img, int32_t *r, void *ud) {
    GUARD;
    uint32_t voff  = (uint32_t)r[1];
    int      count = (int)r[2];
    if (count < 3) { r[0] = 0; return 0; }
    uint64_t need = (uint64_t)count * CRONOPIO_WVERT_BYTES;
    if ((uint64_t)voff + need > img->mem_size) { r[0] = 0; return 0; }
    cron_gpu_xform_polys(X->c, HEAP, (int)r[0], voff, count, (int)r[3], (int)r[4]);
    r[0] = 0; return 0;
}

#undef X
#undef GUARD
#undef HEAP

/* ------ audio ----------------------------------------------------------- */

static int sys_snd_tone(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    ctx_t *x = (ctx_t*)ud;
    cron_apu_tone(x->c, (int)r[0], (int)r[1], (uint32_t)r[2], (int)r[3], (int)r[4]);
    r[0] = 0;
    return 0;
}

static int sys_snd_stop(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    ctx_t *x = (ctx_t*)ud;
    cron_apu_stop(x->c, (int)r[0]);
    r[0] = 0;
    return 0;
}

static int sys_sample(struct cvm_image *img, int32_t *r, void *ud) {
    ctx_t *x = (ctx_t*)ud;
    /* r: slot, ptr, len, rate, fmt(0=signed8,1=unsigned8) */
    cron_apu_sample(x->c, (int)r[0], (uint32_t)r[1], (uint32_t)r[2], (uint32_t)r[3], (int)r[4], img->mem_size);
    r[0] = 0;
    return 0;
}
static int sys_stream(struct cvm_image *img, int32_t *r, void *ud) {
    ctx_t *x = (ctx_t*)ud;
    r[0] = cron_apu_stream(x->c, (uint32_t)r[0], (int)r[1], img->mem_size);
    return 0;
}
static int sys_stream_free(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    r[0] = cron_apu_stream_free(((ctx_t*)ud)->c);
    return 0;
}
static int sys_pcm(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    ctx_t *x = (ctx_t*)ud;
    cron_apu_pcm(x->c, (int)r[0], (int)r[1], (uint32_t)r[2], (int)r[3], (int)r[4], (int)r[5]);
    r[0] = 0;
    return 0;
}
static int sys_pcm_params(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    ctx_t *x = (ctx_t*)ud;
    cron_apu_pcm_params(x->c, (int)r[0], (int)r[1], (int)r[2]);
    r[0] = 0;
    return 0;
}
static int sys_env(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    ctx_t *x = (ctx_t*)ud;
    cron_apu_env(x->c, (int)r[0], (int)r[1], (int)r[2], (int)r[3], (int)r[4]);
    r[0] = 0;
    return 0;
}
static int sys_note_off(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    ctx_t *x = (ctx_t*)ud;
    cron_apu_note_off(x->c, (int)r[0]);
    r[0] = 0;
    return 0;
}
static int sys_snd_master(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    ctx_t *x = (ctx_t*)ud;
    cron_apu_master(x->c, (int)r[0]);
    r[0] = 0;
    return 0;
}

/* --- MIDI + SoundFont synth (midisynth.c) ------------------------------- */

static int sys_sf2_load(struct cvm_image *img, int32_t *r, void *ud) {
    ctx_t *x = (ctx_t*)ud;
    uint32_t off = (uint32_t)r[0], len = (uint32_t)r[1];
    if (!x->c->heap || (uint64_t)off + len > img->mem_size) { r[0] = -1; return 0; }
    r[0] = cron_synth_load_mem(x->c->synth, x->c->heap + off, (int)len);
    return 0;
}
static int sys_sf2_free(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    cron_synth_free_slot(((ctx_t*)ud)->c->synth, (int)r[0]);
    r[0] = 0;
    return 0;
}
static int sys_midi_soundfont(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    cron_synth_select(((ctx_t*)ud)->c->synth, (int)r[0]);
    r[0] = 0;
    return 0;
}
static int sys_midi_send(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    ctx_t *x = (ctx_t*)ud;
    cron_synth_send(x->c->synth, (int)r[0], (int)r[1], (int)r[2]);
    r[0] = 0;
    return 0;
}
static int sys_midi_reset(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    cron_synth_reset(((ctx_t*)ud)->c->synth);
    r[0] = 0;
    return 0;
}
static int sys_midi_volume(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    cron_synth_volume(((ctx_t*)ud)->c->synth, (int)r[0]);
    r[0] = 0;
    return 0;
}

/* --- streaming OGG music (cron_ogg.c) --------------------------------- */

static int sys_ogg(struct cvm_image *img, int32_t *r, void *ud) {
    ctx_t *x = (ctx_t*)ud;
    uint32_t off = (uint32_t)r[0], len = (uint32_t)r[1];
    if (!x->c->heap || (uint64_t)off + len > img->mem_size) { r[0] = -1; return 0; }
    cron_ogg_play(x->c->ogg, x->c->heap + off, (int)len, (int)r[2]);
    r[0] = 0;
    return 0;
}
static int sys_ogg_stop(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    cron_ogg_stop(((ctx_t*)ud)->c->ogg);
    r[0] = 0;
    return 0;
}
static int sys_ogg_volume(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    cron_ogg_set_volume(((ctx_t*)ud)->c->ogg, (int)r[0]);
    r[0] = 0;
    return 0;
}

/* --- tracker-module music — MOD/S3M/XM/IT (cron_module.c, libxmp) ------- */

static int sys_module(struct cvm_image *img, int32_t *r, void *ud) {
    ctx_t *x = (ctx_t*)ud;
    uint32_t off = (uint32_t)r[0], len = (uint32_t)r[1];
    if (!x->c->heap || (uint64_t)off + len > img->mem_size) { r[0] = -1; return 0; }
    cron_module_play(x->c->module, x->c->heap + off, (int)len, (int)r[2]);
    r[0] = 0;
    return 0;
}
static int sys_module_stop(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    cron_module_stop(((ctx_t*)ud)->c->module);
    r[0] = 0;
    return 0;
}
static int sys_module_volume(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    cron_module_set_volume(((ctx_t*)ud)->c->module, (int)r[0]);
    r[0] = 0;
    return 0;
}
static int sys_module_set(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    cron_module_set(((ctx_t*)ud)->c->module, (int)r[0], (int)r[1]);
    r[0] = 0;
    return 0;
}

/* ------ input ----------------------------------------------------------- */

static int sys_pad(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    ctx_t *x = (ctx_t*)ud;
    r[0] = (int32_t)cron_input_pad(x->c, (int)r[0]);
    return 0;
}

static int sys_pad_pressed(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    ctx_t *x = (ctx_t*)ud;
    r[0] = (int32_t)cron_input_pad_pressed(x->c, (int)r[0]);
    return 0;
}

static int sys_pad_released(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    ctx_t *x = (ctx_t*)ud;
    r[0] = (int32_t)cron_input_pad_released(x->c, (int)r[0]);
    return 0;
}

static int sys_mouse(struct cvm_image *img, int32_t *r, void *ud) {
    ctx_t   *x = (ctx_t*)ud;
    uint32_t ax = (uint32_t)r[0];
    uint32_t ay = (uint32_t)r[1];
    int32_t  mx = x->c->mouse_x, my = x->c->mouse_y;
    if (ax) cvm_heap_write(img, ax, &mx, sizeof(mx));
    if (ay) cvm_heap_write(img, ay, &my, sizeof(my));
    r[0] = (int32_t)x->c->mouse_buttons;
    return 0;
}

/* Consume motion accumulated since the previous call (zeroes the accumulator
 * on read). Works in both absolute and relative mode — gives the cart raw
 * mouselook deltas in cart coords regardless. */
static int sys_mouse_delta(struct cvm_image *img, int32_t *r, void *ud) {
    ctx_t   *x = (ctx_t*)ud;
    uint32_t ax = (uint32_t)r[0];
    uint32_t ay = (uint32_t)r[1];
    int32_t  dx, dy;
    cron_input_consume_mouse_delta(x->c, &dx, &dy);
    if (ax) cvm_heap_write(img, ax, &dx, sizeof(dx));
    if (ay) cvm_heap_write(img, ay, &dy, sizeof(dy));
    r[0] = 0;
    return 0;
}

/* Consume accumulated wheel ticks (+ = wheel up / away from user). */
static int sys_mouse_wheel(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    ctx_t *x = (ctx_t*)ud;
    r[0] = cron_input_consume_mouse_wheel(x->c);
    return 0;
}

/* Show/hide the OS cursor. Idempotent; the desktop host syncs the SDL state
 * once per frame in sync_mouse_mode. Headless hosts ignore the flag. */
static int sys_cursor(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    ctx_t *x = (ctx_t*)ud;
    cron_input_set_cursor_visible(x->c, (int)r[0]);
    r[0] = 0;
    return 0;
}

/* Toggle SDL relative-mouse mode (mouselook): hides + locks the cursor,
 * deltas keep coming in via cron_mouse_delta. */
static int sys_mouse_relative(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    ctx_t *x = (ctx_t*)ud;
    cron_input_set_mouse_relative(x->c, (int)r[0]);
    r[0] = 0;
    return 0;
}

/* ------ persistence ----------------------------------------------------- */

static int sys_save_read(struct cvm_image *img, int32_t *r, void *ud) {
    ctx_t   *x   = (ctx_t*)ud;
    uint32_t dst = (uint32_t)r[0];
    int32_t  len = r[1];
    if (len < 0) { r[0] = 0; return 0; }
    /* Only the live bytes are available (what was last written / loaded). */
    if (len > (int32_t)x->c->save_len) len = (int32_t)x->c->save_len;
    if (len > 0 && cvm_heap_write(img, dst, x->c->save, (size_t)len) != CVM_OK) return -1;
    r[0] = len;
    return 0;
}

static int sys_save_write(struct cvm_image *img, int32_t *r, void *ud) {
    ctx_t   *x   = (ctx_t*)ud;
    uint32_t src = (uint32_t)r[0];
    int32_t  len = r[1];
    if (len < 0) { r[0] = 0; return 0; }
    /* Grow the region on demand (capped); clamp the write if we can't. */
    if (!cronopio_save_reserve(x->c, (uint32_t)len)) len = (int32_t)x->c->save_cap;
    if (len > 0 && cvm_heap_read(img, src, x->c->save, (size_t)len) != CVM_OK) return -1;
    x->c->save_len   = (uint32_t)len;
    x->c->save_dirty = 1;
    r[0] = len;
    return 0;
}

static int sys_save_size(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    r[0] = (int32_t)((ctx_t*)ud)->c->save_cap;   /* current capacity */
    return 0;
}

static int sys_save_used(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    r[0] = (int32_t)((ctx_t*)ud)->c->save_len;   /* live bytes (what read returns) */
    return 0;
}

static int sys_save_reserve(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    ctx_t *x = (ctx_t*)ud;
    int32_t n = r[0];
    if (n > 0) cronopio_save_reserve(x->c, (uint32_t)n);
    r[0] = (int32_t)x->c->save_cap;   /* capacity after the request */
    return 0;
}

/* ------ dispatch table -------------------------------------------------- */

typedef struct {
    const char    *name;
    cvm_syscall_fn fn;
} entry_t;

static const entry_t kSyscalls[] = {
    /* core */
    { "cvm_sys_cron_log",          sys_log          },
    { "cvm_sys_cron_trace_i32",    sys_trace_i32    },
    { "cvm_sys_cron_set_frame",    sys_set_frame    },
    { "cvm_sys_cron_exit",         sys_exit         },
    /* system/time */
    { "cvm_sys_cron_time_ms",      sys_time_ms      },
    { "cvm_sys_cron_frame_count",  sys_frame_count  },
    { "cvm_sys_cron_random",       sys_random       },
    { "cvm_sys_cron_seed",         sys_seed         },
    /* display */
    { "cvm_sys_cron_palette_set",  sys_palette_set  },
    { "cvm_sys_cron_palette_get",  sys_palette_get  },
    { "cvm_sys_cron_cls",          sys_cls          },
    { "cvm_sys_cron_pset",         sys_pset         },
    { "cvm_sys_cron_rect",         sys_rect         },
    { "cvm_sys_cron_line",         sys_line         },
    { "cvm_sys_cron_blit",         sys_blit         },
    { "cvm_sys_cron_text",         sys_text         },
    { "cvm_sys_cron_present",      sys_present      },
    /* audio */
    { "cvm_sys_cron_snd_tone",     sys_snd_tone     },
    { "cvm_sys_cron_snd_stop",     sys_snd_stop     },
    { "cvm_sys_cron_snd_master",   sys_snd_master   },
    { "cvm_sys_cron_sample",       sys_sample       },
    { "cvm_sys_cron_pcm",          sys_pcm          },
    { "cvm_sys_cron_pcm_params",   sys_pcm_params   },
    { "cvm_sys_cron_env",          sys_env          },
    { "cvm_sys_cron_note_off",     sys_note_off     },
    { "cvm_sys_cron_stream",       sys_stream       },
    { "cvm_sys_cron_stream_free",  sys_stream_free  },
    { "cvm_sys_cron_sf2_load",       sys_sf2_load       },
    { "cvm_sys_cron_sf2_free",       sys_sf2_free       },
    { "cvm_sys_cron_midi_soundfont", sys_midi_soundfont },
    { "cvm_sys_cron_midi_send",      sys_midi_send      },
    { "cvm_sys_cron_midi_reset",     sys_midi_reset     },
    { "cvm_sys_cron_midi_volume",    sys_midi_volume    },
    { "cvm_sys_cron_ogg",          sys_ogg          },
    { "cvm_sys_cron_ogg_stop",     sys_ogg_stop     },
    { "cvm_sys_cron_ogg_volume",   sys_ogg_volume   },
    { "cvm_sys_cron_module",        sys_module        },
    { "cvm_sys_cron_module_stop",   sys_module_stop   },
    { "cvm_sys_cron_module_volume", sys_module_volume },
    { "cvm_sys_cron_module_set",    sys_module_set    },
    /* input */
    { "cvm_sys_cron_pad",          sys_pad          },
    { "cvm_sys_cron_pad_pressed",  sys_pad_pressed  },
    { "cvm_sys_cron_pad_released", sys_pad_released },
    { "cvm_sys_cron_mouse",        sys_mouse        },
    { "cvm_sys_cron_mouse_delta",  sys_mouse_delta  },
    { "cvm_sys_cron_mouse_wheel",  sys_mouse_wheel  },
    { "cvm_sys_cron_cursor",       sys_cursor       },
    { "cvm_sys_cron_mouse_relative", sys_mouse_relative },
    /* persistence */
    { "cvm_sys_cron_save_read",    sys_save_read    },
    { "cvm_sys_cron_save_write",   sys_save_write   },
    { "cvm_sys_cron_save_size",    sys_save_size    },
    { "cvm_sys_cron_save_used",    sys_save_used    },
    { "cvm_sys_cron_save_reserve", sys_save_reserve },
    /* extended graphics: banks + sprites/tilemaps */
    { "cvm_sys_cron_image",        sys_image        },
    { "cvm_sys_cron_tilemap",      sys_tilemap      },
    { "cvm_sys_cron_blt",          sys_blt          },
    { "cvm_sys_cron_blt_buf",      sys_blt_buf      },
    { "cvm_sys_cron_blt_buf_blend", sys_blt_buf_blend },
    { "cvm_sys_cron_bltm",         sys_bltm         },
    { "cvm_sys_cron_bltm_raster",  sys_bltm_raster  },
    { "cvm_sys_cron_bltm_affine",  sys_bltm_affine  },
    { "cvm_sys_cron_blt_flip",     sys_blt_flip     },
    { "cvm_sys_cron_blt_scale",    sys_blt_scale    },
    { "cvm_sys_cron_palette_bank", sys_palette_bank },
    { "cvm_sys_cron_tile_anim",    sys_tile_anim    },
    { "cvm_sys_cron_blend_table",  sys_blend_table  },
    { "cvm_sys_cron_blend_set",    sys_blend_set    },
    /* extended graphics: shapes */
    { "cvm_sys_cron_rectb",        sys_rectb        },
    { "cvm_sys_cron_circ",         sys_circ         },
    { "cvm_sys_cron_circb",        sys_circb        },
    { "cvm_sys_cron_elli",         sys_elli         },
    { "cvm_sys_cron_ellib",        sys_ellib        },
    { "cvm_sys_cron_tri",          sys_tri          },
    { "cvm_sys_cron_trib",         sys_trib         },
    { "cvm_sys_cron_fill",         sys_fill         },
    /* extended graphics: draw state */
    { "cvm_sys_cron_clip",         sys_clip         },
    { "cvm_sys_cron_clip_reset",   sys_clip_reset   },
    { "cvm_sys_cron_camera",       sys_camera       },
    { "cvm_sys_cron_camera_reset", sys_camera_reset },
    { "cvm_sys_cron_pal",          sys_pal          },
    { "cvm_sys_cron_pal_reset",    sys_pal_reset    },
    /* extended graphics: rotozoom + software-3D rasteriser accelerators */
    { "cvm_sys_cron_blt_ex",       sys_blt_ex       },
    { "cvm_sys_cron_cmap",         sys_cmap         },
    { "cvm_sys_cron_tcol",         sys_tcol         },
    { "cvm_sys_cron_tcolm",        sys_tcolm        },
    { "cvm_sys_cron_tspan",        sys_tspan        },
    /* 3D triangle submission */
    { "cvm_sys_cron_zbuf",         sys_zbuf         },
    { "cvm_sys_cron_lightmap",     sys_lightmap     },
    { "cvm_sys_cron_turb",         sys_turb         },
    { "cvm_sys_cron_colormap",     sys_colormap     },
    { "cvm_sys_cron_zclear",       sys_zclear       },
    { "cvm_sys_cron_polys",        sys_polys        },
    { "cvm_sys_cron_mvp",          sys_mvp          },
    { "cvm_sys_cron_xform_polys",  sys_xform_polys  },
};

int cronopio_syscalls_install(struct cvm_image* img, cronopio_console_t* c) {
    g_ctx.img = img;
    g_ctx.c   = c;
    /* Capture the heap base so the audio thread can read PCM sample bytes
     * (the audio callback only receives the console pointer). */
    c->heap = img->heap;
    int unresolved = 0;
    const size_t n = sizeof(kSyscalls) / sizeof(kSyscalls[0]);
    for (size_t i = 0; i < n; ++i) {
        int rc = cvm_link(img, kSyscalls[i].name, kSyscalls[i].fn, &g_ctx);
        /* cvm_link returns CVM_E_NO_SUCH_IMPORT for syscalls the cart didn't
         * actually reference — that's normal, not an error. We only flag
         * something if linking fails for another reason. */
        if (rc != CVM_OK && rc != CVM_E_NO_SUCH_IMPORT) unresolved++;
    }
    return unresolved;
}

int cronopio_resolve_video_regions(struct cvm_image* img, cronopio_console_t* c) {
    uint32_t fb_off = 0, fb_size = 0, pal_off = 0, pal_size = 0;
    int rc;
    rc = cvm_image_get_region(img, CRONOPIO_FB_REGION,  &fb_off,  &fb_size);
    if (rc != CVM_OK || fb_size < CRONOPIO_FB_BYTES) return -1;
    rc = cvm_image_get_region(img, CRONOPIO_PAL_REGION, &pal_off, &pal_size);
    if (rc != CVM_OK || pal_size < CRONOPIO_PAL_BYTES) return -1;
    c->fb_offset  = fb_off;
    c->pal_offset = pal_off;
    c->regions_ok = 1;
    cronopio_console_seed_palette(img->heap, pal_off);
    return 0;
}
