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

/* Defined in the SDL host; the common layer calls it for sys_time_ms. */
uint64_t cronopio_platform_ticks_ms(void) { return 0; }

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

    int32_t ret = 0;
    rc = cvm_run(&img, &ret);
    if (rc != CVM_OK && rc != CVM_E_SYSCALL_TRAP) {
        fprintf(stderr, "entry trap: %s\n", cvm_strerror(rc)); return 1;
    }

    for (int f = 0; f < frames && !console.cart_exited; ++f) {
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

    /* Histogram of the framebuffer. */
    uint32_t hist[256]; memset(hist, 0, sizeof hist);
    const uint8_t* fb = img.heap + console.fb_offset;
    for (int i = 0; i < CRONOPIO_FB_BYTES; ++i) hist[fb[i]]++;

    printf("frames=%d fb_offset=%u\n", frames, console.fb_offset);
    int distinct = 0;
    for (int c = 0; c < 256; ++c)
        if (hist[c]) { printf("  color %3d : %6u px\n", c, hist[c]); distinct++; }
    printf("distinct colors: %d\n", distinct);

    cvm_image_free(&img);
    free(blob);
    return 0;
}
