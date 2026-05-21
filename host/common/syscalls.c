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
    fputs(buf, stderr);
    if (len > 0 && buf[len-1] != '\n') fputc('\n', stderr);
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
    /* No-op in the callback-driven model — the platform shell blits + vsyncs
     * after the frame fn returns. Kept as a syscall so cart code that wants
     * to flush mid-frame compiles. */
    (void)img; (void)ud;
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
static int sys_bltm(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img; GUARD;
    cron_gpu_bltm(X->c, HEAP, (int)r[0], (int)r[1], (int)r[2], (int)r[3], (int)r[4], (int)r[5], (int)r[6], (int)r[7]);
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

static int sys_tspan(struct cvm_image *img, int32_t *r, void *ud) {
    GUARD;
    uint32_t src = (uint32_t)r[3];
    if ((uint64_t)src + 4096u > img->mem_size) { r[0] = 0; return 0; }
    cron_gpu_tspan(X->c, HEAP, (int)r[0], (int)r[1], (int)r[2], src, r[4], r[5], r[6], r[7]);
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

static int sys_snd_master(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    ctx_t *x = (ctx_t*)ud;
    cron_apu_master(x->c, (int)r[0]);
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

static int sys_key(struct cvm_image *img, int32_t *r, void *ud) {
    (void)img;
    ctx_t *x = (ctx_t*)ud;
    int32_t code = r[0];
    if ((unsigned)code >= 256) { r[0] = 0; return 0; }
    r[0] = (x->c->keys[code >> 3] >> (code & 7)) & 1;
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

/* ------ persistence ----------------------------------------------------- */

static int sys_save_read(struct cvm_image *img, int32_t *r, void *ud) {
    ctx_t   *x   = (ctx_t*)ud;
    uint32_t dst = (uint32_t)r[0];
    int32_t  len = r[1];
    if (len < 0) { r[0] = 0; return 0; }
    if (len > (int32_t)CRONOPIO_SAVE_BYTES) len = (int32_t)CRONOPIO_SAVE_BYTES;
    if (cvm_heap_write(img, dst, x->c->save, (size_t)len) != CVM_OK) return -1;
    r[0] = len;
    return 0;
}

static int sys_save_write(struct cvm_image *img, int32_t *r, void *ud) {
    ctx_t   *x   = (ctx_t*)ud;
    uint32_t src = (uint32_t)r[0];
    int32_t  len = r[1];
    if (len < 0) { r[0] = 0; return 0; }
    if (len > (int32_t)CRONOPIO_SAVE_BYTES) len = (int32_t)CRONOPIO_SAVE_BYTES;
    if (cvm_heap_read(img, src, x->c->save, (size_t)len) != CVM_OK) return -1;
    x->c->save_dirty = 1;
    r[0] = len;
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
    /* input */
    { "cvm_sys_cron_pad",          sys_pad          },
    { "cvm_sys_cron_pad_pressed",  sys_pad_pressed  },
    { "cvm_sys_cron_pad_released", sys_pad_released },
    { "cvm_sys_cron_key",          sys_key          },
    { "cvm_sys_cron_mouse",        sys_mouse        },
    /* persistence */
    { "cvm_sys_cron_save_read",    sys_save_read    },
    { "cvm_sys_cron_save_write",   sys_save_write   },
    /* extended graphics: banks + sprites/tilemaps */
    { "cvm_sys_cron_image",        sys_image        },
    { "cvm_sys_cron_tilemap",      sys_tilemap      },
    { "cvm_sys_cron_blt",          sys_blt          },
    { "cvm_sys_cron_bltm",         sys_bltm         },
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
    { "cvm_sys_cron_tspan",        sys_tspan        },
};

int cronopio_syscalls_install(struct cvm_image* img, cronopio_console_t* c) {
    g_ctx.img = img;
    g_ctx.c   = c;
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
