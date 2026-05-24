/* Headless Cronopio runner — no SDL, no window.
 *
 * Loads a cart, runs its entry, drives the frame fn N times, then prints a
 * histogram of the 8 bpp framebuffer (palette index -> pixel count). Used to
 * verify a cart renders the expected colours without opening a window.
 *
 *   headless cart.bin [frames]
 */
#include "console.h"
#include "syscalls.h"
#include "cvm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Defined in the SDL host; the common layer calls it for sys_time_ms. A frozen
 * clock makes time-driven carts (e.g. DOOM, whose title/demo state machine only
 * advances when I_GetTime() ticks) render nothing, so advance a virtual 60Hz
 * clock: the frame loop bumps g_frame_ms by ~16ms before each frame. */
static uint64_t g_frame_ms = 0;
uint64_t cronopio_platform_ticks_ms(void) { return g_frame_ms; }

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

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s cart.bin [frames]\n", argv[0]); return 1; }
    int frames = (argc >= 3) ? atoi(argv[2]) : 1;

    size_t blob_len = 0;
    uint8_t* blob = slurp(argv[1], &blob_len);
    if (!blob) return 1;

    struct cvm_image img;
    int rc = cvm_load(blob, blob_len, &img);
    if (rc != CVM_OK) { fprintf(stderr, "cvm_load: %s\n", cvm_strerror(rc)); return 1; }

    static cronopio_console_t console;
    cronopio_console_init(&console);
    if (cronopio_resolve_video_regions(&img, &console) != 0)
        fprintf(stderr, "warning: no fb/pal regions\n");
    cronopio_syscalls_install(&img, &console);

    /* Per-cart save ("memory card"): load <cart>.sav before the entry runs. */
    char savepath[1100];
    snprintf(savepath, sizeof savepath, "%s.sav", argv[1]);
    cronopio_save_reserve(&console, CRONOPIO_SAVE_DEFAULT);
    { FILE* sf = fopen(savepath, "rb");
      if (sf) { fseek(sf,0,SEEK_END); long sz=ftell(sf); fseek(sf,0,SEEK_SET);
                if (sz > 0) { cronopio_save_reserve(&console, (uint32_t)sz);
                              console.save_len = (uint32_t)fread(console.save, 1, console.save_cap, sf); }
                fclose(sf); } }

    int32_t ret = 0;
    rc = cvm_run(&img, &ret);
    if (rc != CVM_OK && rc != CVM_E_SYSCALL_TRAP) {
        fprintf(stderr, "entry trap: %s\n", cvm_strerror(rc)); return 1;
    }

    for (int f = 0; f < frames && !console.cart_exited; ++f) {
        g_frame_ms = (uint64_t)f * 1000u / 60u;   /* virtual 60Hz clock */
        cronopio_console_begin_frame(&console);
        if (console.frame_fn_index > 0) {
            int32_t fr = 0;
            rc = cvm_call(&img, (uint32_t)console.frame_fn_index, NULL, 0, &fr);
            if (rc != CVM_OK && rc != CVM_E_SYSCALL_TRAP) {
                fprintf(stderr, "frame %d trap: %s\n", f, cvm_strerror(rc)); return 1;
            }
        }
        cronopio_console_end_frame(&console);
    }

    /* Flush the save if the cart wrote to it (atomic: temp + rename). */
    if (console.save_dirty) {
        char tmp[1112]; snprintf(tmp, sizeof tmp, "%s.tmp", savepath);
        FILE* sf = fopen(tmp, "wb");
        if (sf) {
            int ok = (console.save_len == 0) ||
                     (fwrite(console.save, 1, console.save_len, sf) == console.save_len);
            if (fclose(sf) != 0) ok = 0;
            if (ok) { remove(savepath); rename(tmp, savepath); } else remove(tmp);
        }
    }

    /* Histogram of the framebuffer. */
    uint32_t hist[256]; memset(hist, 0, sizeof hist);
    const uint8_t* fb = img.heap + console.fb_offset;
    for (int i = 0; i < CRONOPIO_FB_BYTES; ++i) hist[fb[i]]++;

    printf("frames=%d fb_offset=%u\n", frames, console.fb_offset);
    int distinct = 0;
    for (int c = 0; c < 256; ++c)
        if (hist[c]) { printf("  color %3d : %6u px\n", c, hist[c]); distinct++; }
    printf("distinct colors: %d\n", distinct);

    /* Optional PPM screenshot: `headless cart.bin [frames] [out.ppm]`. Packs
     * the 8bpp framebuffer through the cart palette into RGB and writes a P6. */
    if (argc >= 4) {
        static uint32_t rgba[CRONOPIO_FB_BYTES];
        cronopio_console_blit_rgba(&console, img.heap, rgba);
        FILE* p = fopen(argv[3], "wb");
        if (p) {
            fprintf(p, "P6\n%d %d\n255\n", CRONOPIO_SCREEN_W, CRONOPIO_SCREEN_H);
            for (int i = 0; i < CRONOPIO_FB_BYTES; ++i) {
                uint32_t px = rgba[i];           /* 0xAARRGGBB */
                uint8_t rgb[3] = { (uint8_t)(px >> 16), (uint8_t)(px >> 8),
                                   (uint8_t)(px) };
                fwrite(rgb, 1, 3, p);
            }
            fclose(p);
            printf("wrote screenshot %s\n", argv[3]);
        }
    }

    cvm_image_free(&img);
    free(blob);
    return 0;
}
