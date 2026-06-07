/* Headless Cronopio runner with a self-time profiler.
 *
 * Same frame-driving harness as headless.c, but built against a cvm.c
 * compiled with -DCVM_PROFILE. After running N frames it dumps the functions
 * that consumed the most interpreter instructions (self time, exclusive of
 * callees). Function names come from the "<cart>.sym" sidecar emitted by
 * cvm-translate when built with CVM_SYMS set (otherwise indices only).
 *
 *   headless_prof cart.bin [frames] [top] [warmup]
 *
 * `warmup` frames run before the counters are zeroed, so one-time startup
 * cost (DEH parsing, level setup, table init) is excluded and what remains is
 * steady-state per-frame work.
 *
 * Built standalone (not via CMake) — one clang invocation over a profiler-
 * enabled cvm.c plus all of host/common/. `-std=gnu23` is required because
 * bios_sf2.c embeds the default SoundFont with C23 #embed, and TinySoundFont
 * (used by midisynth.c) is included from external/. Since the audio subsystem
 * landed, host/common/ ALSO needs the stb_vorbis implementation TU (cron_ogg.c
 * includes it HEADER_ONLY) and a link against libxmp (cron_module.c), so add
 * stb_vorbis.c and libxmp.a (the bare host/common/*.c line no longer links):
 *
 *   CR=<cronopio-root>; CVM=$CR/external/CronoVM
 *   clang -O2 -std=gnu23 -DCVM_PROFILE -DLIBXMP_STATIC \
 *     -I $CVM/include -I $CR/host/common -I $CR/external/TinySoundFont \
 *     -I $CR/external/libxmp/include \
 *     $CVM/src/cvm.c $CR/host/common/*.c $CR/host/external/stb_vorbis.c \
 *     $CR/tools/headless/headless_prof.c $CR/build/libxmp/libxmp.a \
 *     -o cronopio-headless-prof.exe -lm
 *
 * Then build the cart with CVM_SYMS=1 so cvm-translate emits <cart>.bin.sym.
 */
#include "console.h"
#include "syscalls.h"
#include "cvm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Provided by the profiler-enabled cvm.c (see CVM_PROFILE in src/cvm.c). */
extern uint64_t *cvm_prof_counts;
extern uint32_t  cvm_prof_len;
extern uint64_t  cvm_prof_total;
extern uint32_t  cvm_prof_watch;
extern uint64_t *cvm_prof_caller;
extern uint64_t  cvm_prof_cap;   /* debug: stop after N instructions (0 = off) */
void cvm_profile_reset(uint32_t func_count);

/* Virtual 60Hz clock — DOOM's state machine only advances when I_GetTime
 * ticks; the frame loop bumps this before each frame. Mirrors headless.c. */
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

/* Load "<cart>.sym" (index<TAB>entry<TAB>name) into a name table indexed by
 * FUNCS index. Returns a malloc'd char*[count] (entries may be NULL) or NULL. */
static char** load_syms(const char* cart_path, uint32_t count) {
    char sympath[1024];
    snprintf(sympath, sizeof sympath, "%s.sym", cart_path);
    FILE* f = fopen(sympath, "rb");
    if (!f) { fprintf(stderr, "(no %s — showing indices only)\n", sympath); return NULL; }
    char** names = (char**)calloc(count ? count : 1, sizeof(char*));
    if (!names) { fclose(f); return NULL; }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        int idx; unsigned off; char name[256];
        /* "%d\t%u\t%255[^\n]" — name may contain anything but newline. */
        if (sscanf(line, "%d\t%u\t%255[^\n]", &idx, &off, name) >= 3 &&
            idx >= 0 && (uint32_t)idx < count) {
            free(names[idx]);
            names[idx] = strdup(name);
        }
    }
    fclose(f);
    return names;
}

typedef struct { uint32_t idx; uint64_t count; } prof_row;

static int by_count_desc(const void* a, const void* b) {
    uint64_t ca = ((const prof_row*)a)->count, cb = ((const prof_row*)b)->count;
    return (ca < cb) ? 1 : (ca > cb) ? -1 : 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s cart.bin [frames] [top]\n", argv[0]); return 1; }
    int frames = (argc >= 3) ? atoi(argv[2]) : 1;
    int top    = (argc >= 4) ? atoi(argv[3]) : 40;
    int warmup = (argc >= 5) ? atoi(argv[4]) : 0;
    if (warmup >= frames) warmup = frames > 0 ? frames - 1 : 0;

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

    cvm_profile_reset(img.func_count);
    { const char* w = getenv("CVM_PROF_WATCH"); if (w) cvm_prof_watch = (uint32_t)strtoul(w, NULL, 0); }

    /* [debug] Apply the instruction cap BEFORE the entry too, so a hang inside
     * the cart's global ctors / setup() (before any frame, e.g. an init loop)
     * is broken out and the dump shows the spinning function. (The post-entry
     * set below also stands for runaway loops inside a frame.) */
    { const char* c = getenv("CVM_PROF_CAP"); if (c) cvm_prof_cap = strtoull(c, NULL, 0); }

    int32_t ret = 0;
    rc = cvm_run(&img, &ret);
    if (rc != CVM_OK && rc != CVM_E_SYSCALL_TRAP) {
        fprintf(stderr, "entry trap: %s\n", cvm_strerror(rc)); return 1;
    }

    /* Debug: cap instructions per the env, set AFTER the (finite) entry so a
     * runaway loop inside a frame breaks out and the dump shows the spinner. */
    { const char* c = getenv("CVM_PROF_CAP"); if (c) cvm_prof_cap = strtoull(c, NULL, 0); }

    for (int f = 0; f < frames && !console.cart_exited; ++f) {
        if (f == warmup) cvm_profile_reset(img.func_count);  /* drop startup */
        g_frame_ms = (uint64_t)f * 1000u / 60u;
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

    /* Rank functions by self-instruction count. */
    char** names = load_syms(argv[1], img.func_count);
    prof_row* rows = (prof_row*)malloc(sizeof(prof_row) * (cvm_prof_len ? cvm_prof_len : 1));
    uint32_t nrows = 0;
    for (uint32_t i = 0; i < cvm_prof_len; ++i)
        if (cvm_prof_counts[i]) { rows[nrows].idx = i; rows[nrows].count = cvm_prof_counts[i]; nrows++; }
    qsort(rows, nrows, sizeof(prof_row), by_count_desc);

    double total = (double)(cvm_prof_total ? cvm_prof_total : 1);
    printf("\n=== profile: %d measured frames (after %d warmup), "
           "%llu instructions, %u functions hit ===\n",
           frames - warmup, warmup, (unsigned long long)cvm_prof_total, nrows);
    printf("  %5s  %14s  %6s  %s\n", "idx", "instructions", "self%", "function");
    int shown = (top > 0 && (uint32_t)top < nrows) ? top : (int)nrows;
    double cum = 0;
    for (int i = 0; i < shown; ++i) {
        double pct = 100.0 * (double)rows[i].count / total;
        cum += pct;
        const char* nm = (names && names[rows[i].idx]) ? names[rows[i].idx] : "<?>";
        printf("  %5u  %14llu  %5.2f%%  %s\n", rows[i].idx,
               (unsigned long long)rows[i].count, pct, nm);
    }
    printf("  (top %d = %.1f%% of all instructions)\n", shown, cum);

    /* Caller attribution for the watched function, if any. */
    if (cvm_prof_watch < cvm_prof_len && cvm_prof_caller) {
        const char* wn = (names && names[cvm_prof_watch]) ? names[cvm_prof_watch] : "<?>";
        uint64_t wtot = 0;
        for (uint32_t i = 0; i < cvm_prof_len; ++i) wtot += cvm_prof_caller[i];
        printf("\n--- callers of [%u] %s  (%llu calls total) ---\n",
               cvm_prof_watch, wn, (unsigned long long)wtot);
        prof_row* cr = (prof_row*)malloc(sizeof(prof_row) * (cvm_prof_len ? cvm_prof_len : 1));
        uint32_t cn = 0;
        for (uint32_t i = 0; i < cvm_prof_len; ++i)
            if (cvm_prof_caller[i]) { cr[cn].idx = i; cr[cn].count = cvm_prof_caller[i]; cn++; }
        qsort(cr, cn, sizeof(prof_row), by_count_desc);
        uint32_t cshow = cn < 15 ? cn : 15;
        for (uint32_t i = 0; i < cshow; ++i) {
            const char* nm = (names && names[cr[i].idx]) ? names[cr[i].idx] : "<?>";
            printf("  %5u  %14llu calls  %s\n", cr[i].idx,
                   (unsigned long long)cr[i].count, nm);
        }
        free(cr);
    }

    free(rows);
    if (names) { for (uint32_t i = 0; i < img.func_count; ++i) free(names[i]); free(names); }
    cvm_image_free(&img);
    free(blob);
    return 0;
}
