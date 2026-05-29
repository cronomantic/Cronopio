/* Headless Cronopio runner — no SDL, no window.
 *
 * Loads a cart, runs its entry, drives the frame fn N times, then prints a
 * histogram of the 8 bpp framebuffer (palette index -> pixel count). Used to
 * verify a cart renders the expected colours without opening a window.
 *
 *   headless cart.bin [frames] [out.ppm] [--pad=script.txt]
 *
 * --pad scripts the gamepad so interactive carts (menus, gameplay) can be
 * driven + verified headless. The script is one directive per line:
 *
 *     <frame> <TOKEN> [TOKEN...]      # pad = OR of these buttons AT <frame>,
 *     <frame> NONE                    #   held until the next directive
 *     # comment
 *
 * TOKENs: UP DOWN LEFT RIGHT A B X Y L R START SELECT (case-insensitive).
 * Example (tap DOWN at frame 30, SELECT at 90):
 *     30 DOWN
 *     31 NONE
 *     90 A
 *     91 NONE
 */
#include "console.h"
#include "syscalls.h"
#include "cvm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- optional scripted gamepad input (--pad=FILE) ---- */
/* Bit layout mirrors the SDK enum CRON_BTN_* in sdk/include/cronopio.h. */
#define PAD_BIT_UP    (1u<<0)
#define PAD_BIT_DOWN  (1u<<1)
#define PAD_BIT_LEFT  (1u<<2)
#define PAD_BIT_RIGHT (1u<<3)
#define PAD_BIT_A     (1u<<4)
#define PAD_BIT_B     (1u<<5)
#define PAD_BIT_X     (1u<<6)
#define PAD_BIT_Y     (1u<<7)
#define PAD_BIT_L     (1u<<8)
#define PAD_BIT_R     (1u<<9)
#define PAD_BIT_START (1u<<10)
#define PAD_BIT_SEL   (1u<<11)

#define PAD_MAX_DIRECTIVES 8192
static struct { int frame; uint32_t mask; } g_pad[PAD_MAX_DIRECTIVES];
static int g_pad_n = 0;

static int streq_ci(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b) {
        int ca = (*a >= 'a' && *a <= 'z') ? *a - 32 : *a;
        int cb = (*b >= 'a' && *b <= 'z') ? *b - 32 : *b;
        if (ca != cb) return 0;
    }
    return *a == *b;
}

static uint32_t pad_token(const char* t) {
    if (streq_ci(t, "UP"))    return PAD_BIT_UP;
    if (streq_ci(t, "DOWN"))  return PAD_BIT_DOWN;
    if (streq_ci(t, "LEFT"))  return PAD_BIT_LEFT;
    if (streq_ci(t, "RIGHT")) return PAD_BIT_RIGHT;
    if (streq_ci(t, "A"))     return PAD_BIT_A;
    if (streq_ci(t, "B"))     return PAD_BIT_B;
    if (streq_ci(t, "X"))     return PAD_BIT_X;
    if (streq_ci(t, "Y"))     return PAD_BIT_Y;
    if (streq_ci(t, "L"))     return PAD_BIT_L;
    if (streq_ci(t, "R"))     return PAD_BIT_R;
    if (streq_ci(t, "START")) return PAD_BIT_START;
    if (streq_ci(t, "SELECT"))return PAD_BIT_SEL;
    if (streq_ci(t, "NONE"))  return 0;
    fprintf(stderr, "warning: --pad: unknown token '%s'\n", t);
    return 0;
}

static void load_pad_script(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) { perror(path); return; }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;
        char* tok = strtok(p, " \t\r\n");
        if (!tok) continue;
        int frame = atoi(tok);
        uint32_t mask = 0;
        while ((tok = strtok(NULL, " \t\r\n")) != NULL)
            mask |= pad_token(tok);
        if (g_pad_n < PAD_MAX_DIRECTIVES) {
            g_pad[g_pad_n].frame = frame;
            g_pad[g_pad_n].mask = mask;
            ++g_pad_n;
        }
    }
    fclose(f);
    fprintf(stderr, "[pad] loaded %d directive(s) from %s\n", g_pad_n, path);
}

/* The pad mask in effect at frame f = the last directive with frame <= f. */
static uint32_t pad_mask_at(int f) {
    uint32_t mask = 0;
    int best = -1;
    for (int i = 0; i < g_pad_n; ++i) {
        if (g_pad[i].frame <= f && g_pad[i].frame >= best) {
            best = g_pad[i].frame;
            mask = g_pad[i].mask;
        }
    }
    return mask;
}

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
    if (argc < 2) {
        fprintf(stderr, "usage: %s cart.bin [frames] [out.ppm] [--pad=script]\n", argv[0]);
        return 1;
    }
    int frames = 1;
    const char* ppmpath = NULL;
    for (int a = 2; a < argc; ++a) {
        if (strncmp(argv[a], "--pad=", 6) == 0) {
            load_pad_script(argv[a] + 6);
        } else if (strstr(argv[a], ".ppm")) {
            ppmpath = argv[a];
        } else {
            frames = atoi(argv[a]);
        }
    }

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
        /* Inject scripted pad input (after begin_frame so pad_prev holds the
         * previous mask → pad_pressed edge detection works). */
        if (g_pad_n)
            cron_input_set_pad(&console, 0, pad_mask_at(f));
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
    if (ppmpath) {
        static uint32_t rgba[CRONOPIO_FB_BYTES];
        cronopio_console_blit_rgba(&console, img.heap, rgba);
        FILE* p = fopen(ppmpath, "wb");
        if (p) {
            fprintf(p, "P6\n%d %d\n255\n", CRONOPIO_SCREEN_W, CRONOPIO_SCREEN_H);
            for (int i = 0; i < CRONOPIO_FB_BYTES; ++i) {
                uint32_t px = rgba[i];           /* 0xAARRGGBB */
                uint8_t rgb[3] = { (uint8_t)(px >> 16), (uint8_t)(px >> 8),
                                   (uint8_t)(px) };
                fwrite(rgb, 1, 3, p);
            }
            fclose(p);
            printf("wrote screenshot %s\n", ppmpath);
        }
    }

    cvm_image_free(&img);
    free(blob);
    return 0;
}
