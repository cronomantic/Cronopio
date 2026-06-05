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

/* Unified input timeline (--input=FILE; --pad=FILE is a pad-only alias). One
 * directive per line: `<frame> [TOKENS...]`. Held state (pad buttons + mouse
 * buttons) comes from the latest directive with frame <= f; the cursor position
 * is the latest MOUSE <x> <y> <= f (sticky); WHEEL fires on its exact frame.
 *   pad:   UP DOWN LEFT RIGHT A B X Y L R START SELECT      (held)
 *   mouse: MOUSE <x> <y> | LB RB MB (buttons held) | WHEEL <n> (impulse)
 * This is the decided design (exult-input-model): ONE pad+mouse+wheel timeline. */
#define INPUT_MAX 8192
static struct { int frame; uint32_t pad, mbtn; int has_move, mx, my, wheel; }
    g_in[INPUT_MAX];
static int g_in_n = 0;

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

/* Mouse-button token -> bit value (1=L, 2=R, 4=M, matching cron_mouse). */
static uint32_t mouse_btn_token(const char* t) {
    if (streq_ci(t, "LB")) return 1u;
    if (streq_ci(t, "RB")) return 2u;
    if (streq_ci(t, "MB")) return 4u;
    return 0;
}

/* Parse a timeline script into g_in. pad_only=1 routes every token to the pad
 * parser (the --pad= alias: MOUSE/WHEEL/LB/RB/MB are not recognised there). */
static void load_input_script(const char* path, int pad_only) {
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
        uint32_t pad = 0, mbtn = 0;
        int has_move = 0, mx = 0, my = 0, wheel = 0;
        while ((tok = strtok(NULL, " \t\r\n")) != NULL) {
            uint32_t mb;
            if (!pad_only && streq_ci(tok, "MOUSE")) {
                char* a = strtok(NULL, " \t\r\n");
                char* b = strtok(NULL, " \t\r\n");
                if (a && b) { has_move = 1; mx = atoi(a); my = atoi(b); }
            } else if (!pad_only && streq_ci(tok, "WHEEL")) {
                char* a = strtok(NULL, " \t\r\n");
                if (a) wheel = atoi(a);
            } else if (!pad_only && (mb = mouse_btn_token(tok)) != 0) {
                mbtn |= mb;
            } else {
                pad |= pad_token(tok);
            }
        }
        if (g_in_n < INPUT_MAX) {
            g_in[g_in_n].frame = frame; g_in[g_in_n].pad = pad;
            g_in[g_in_n].mbtn = mbtn; g_in[g_in_n].has_move = has_move;
            g_in[g_in_n].mx = mx; g_in[g_in_n].my = my; g_in[g_in_n].wheel = wheel;
            ++g_in_n;
        }
    }
    fclose(f);
    fprintf(stderr, "[input] loaded %d directive(s) from %s\n", g_in_n, path);
}

/* Resolve the held input state at frame f: pad+buttons from the latest
 * directive <= f, cursor position from the latest MOUSE <= f (sticky). */
static void input_state_at(int f, uint32_t* pad, uint32_t* mbtn, int* mx, int* my) {
    int best = -1, bestmove = -1;
    *pad = 0; *mbtn = 0; *mx = 0; *my = 0;
    for (int i = 0; i < g_in_n; ++i) {
        if (g_in[i].frame <= f && g_in[i].frame >= best) {
            best = g_in[i].frame; *pad = g_in[i].pad; *mbtn = g_in[i].mbtn;
        }
        if (g_in[i].has_move && g_in[i].frame <= f && g_in[i].frame >= bestmove) {
            bestmove = g_in[i].frame; *mx = g_in[i].mx; *my = g_in[i].my;
        }
    }
}

/* Wheel impulse to deliver on exactly frame f (sum of any WHEEL directives there). */
static int wheel_at(int f) {
    int w = 0;
    for (int i = 0; i < g_in_n; ++i)
        if (g_in[i].frame == f) w += g_in[i].wheel;
    return w;
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
    /* [bp] Unbuffered stdout so cron_log breadcrumbs are flushed immediately —
     * a hang/kill otherwise loses the tail of the log (block buffering to a
     * redirected file). Harmless for normal runs. */
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) {
        fprintf(stderr, "usage: %s cart.bin [frames] [out.ppm] "
                        "[--input=script | --pad=script]\n", argv[0]);
        return 1;
    }
    int frames = 1;
    const char* ppmpath = NULL;
    for (int a = 2; a < argc; ++a) {
        if (strncmp(argv[a], "--input=", 8) == 0) {
            load_input_script(argv[a] + 8, /*pad_only*/ 0);
        } else if (strncmp(argv[a], "--pad=", 6) == 0) {
            load_input_script(argv[a] + 6, /*pad_only*/ 1);   /* pad-only alias */
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

    /* Optional live per-frame progress to stderr (CRON_HL_PROGRESS=1). The
     * frame number is printed BEFORE the frame fn runs, so if a frame stalls
     * (e.g. heavy one-time work that doesn't yield), you see it stuck on that
     * number — distinguishing a slow-but-progressing run from a hang. */
    int hl_progress = getenv("CRON_HL_PROGRESS") != NULL;
    for (int f = 0; f < frames && !console.cart_exited; ++f) {
        if (hl_progress) { fprintf(stderr, "[frame %d/%d]\n", f, frames); fflush(stderr); }
        g_frame_ms = (uint64_t)f * 1000u / 60u;   /* virtual 60Hz clock */
        cronopio_console_begin_frame(&console);
        /* Inject the scripted input timeline (after begin_frame so *_prev holds
         * the previous state → pad_pressed/mouse-edge detection works). */
        if (g_in_n) {
            uint32_t pad = 0, mbtn = 0;
            int mx = 0, my = 0;
            input_state_at(f, &pad, &mbtn, &mx, &my);
            cron_input_set_pad(&console, 0, pad);
            static int prev_mx = 0, prev_my = 0;
            cron_input_mouse_motion(&console, mx, my, mx - prev_mx, my - prev_my);
            prev_mx = mx; prev_my = my;
            cron_input_mouse_button(&console, 0, (mbtn & 1u) ? 1 : 0);   /* L */
            cron_input_mouse_button(&console, 1, (mbtn & 2u) ? 1 : 0);   /* R */
            cron_input_mouse_button(&console, 2, (mbtn & 4u) ? 1 : 0);   /* M */
            int w = wheel_at(f);
            if (w) cron_input_mouse_wheel(&console, w);
        }
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
