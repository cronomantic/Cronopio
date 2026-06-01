/* cron_sys.c — Cronopio SDK machine port + platform layer (NOT a libc).
 *
 * picolibc is the C library now (see runtime/lib/build_picolibc.sh +
 * libc-libcxx-decision): it owns the standard string/mem/ctype/stdlib/numeric
 * surface AND the canonical malloc/free/calloc/realloc. This file is what an
 * embedder must supply on top of picolibc — the "machine port" — PLUS the
 * Cronopio-specific functions that are not part of any standard libc:
 *   - the machine port picolibc needs: `errno` and `sbrk` (over the cron heap);
 *   - process control over cron syscalls: exit / abort / atexit / assert;
 *   - stdio + the FS layer (printf/fopen/dirent/stat/...) routed to cron
 *     syscalls and the cart ROM — DEFERRED to picolibc tinystdio in a later
 *     phase; for now the cron-routed implementations stay here;
 *   - the handful of classifiers/strings picolibc's CURATED build does not
 *     compile (isblank/iscntrl/isprint/isgraph/ispunct, strcasecmp/strncasecmp/
 *     strndup, strerror, strtod/atof, srand/rand, getenv);
 *   - the Cronopio-specific TUNED allocator under cron_* names (cron_malloc &c.)
 *     — kept available; the canonical allocator is picolibc's (see below).
 *
 * Hard target constraints (see the SDK docs):
 *   - The translator (cvm-translate) REJECTS i64 and f64 SSA values. So this
 *     file performs NO 64-bit integer arithmetic and uses NO `double`.
 *     int/long are 32-bit (ILP32); `float` (f32) is fine. Anything that the
 *     C standard types as `double` (atof, strtod) is deliberately weakened to
 *     avoid producing an f64 value — see the notes on those functions.
 *   - Loop-heavy helpers are marked noinline so an inlined copy at every call
 *     site does not blow the translator's 254-register budget (same reason the
 *     reference allocator's free-list walks are noinline).
 *
 * NOTE on comments: never write the asterisk-slash sequence inside a block
 * comment — it closes the comment early. So we say "mem and str", not the
 * glob form.
 *
 * The cart must link picolibc.bc alongside this file (the cart build scripts do
 * so). Console output and termination route through the Cronopio syscalls in
 * <cronopio.h>. There is no host filesystem: FILE I/O is stubbed/RAM-backed so
 * ports link, but bundled assets are read from the cartridge ROM (cron_rom). */

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

#include <cronopio.h>
#include "cvm_alloc.h"

#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <locale.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

/* ---------------- errno ------------------------------------------------- */

int errno = 0;


/* ============================== ctype.h ================================= */
/* picolibc owns isdigit/isupper/islower/isalpha/isalnum/isspace/isxdigit +
 * tolower/toupper (its ctype table). Only the classifiers picolibc's curated
 * build does NOT compile are kept here (trivial ASCII/C-locale range checks).
 * isalnum below resolves to picolibc's. */
int isblank(int c)  { return c == ' ' || c == '\t'; }
int iscntrl(int c)  { return (c >= 0 && c < 0x20) || c == 0x7f; }
int isprint(int c)  { return c >= 0x20 && c < 0x7f; }
int isgraph(int c)  { return c > 0x20 && c < 0x7f; }
int ispunct(int c)  { return isgraph(c) && !isalnum(c); }

/* ============================== string.h =============================== */
/* picolibc owns the standard mem and str surface: memcpy/memmove/memset/memcmp/
 * memchr, strcpy/strncpy/strcat/strncat, strcmp/strncmp, strlen/strnlen,
 * strchr/strrchr/strstr/strdup, strtok/strspn/strcspn/strpbrk. (picolibc's
 * mem routines map to the same llvm.mem* intrinsics, so they still become the
 * VM's single-op MEMCPY/MEMSET/MEMMOVE — no perf loss vs the old byte loops.)
 * Only the functions picolibc's curated build does NOT compile stay here; their
 * calls to tolower/strnlen/malloc/memcpy resolve to picolibc. */

int strcasecmp(const char *a, const char *b) {
    int ca, cb;
    do {
        ca = tolower((unsigned char)*a++);
        cb = tolower((unsigned char)*b++);
    } while (ca && ca == cb);
    return ca - cb;
}

int strncasecmp(const char *a, const char *b, size_t n) {
    int ca = 0, cb = 0;
    while (n--) {
        ca = tolower((unsigned char)*a++);
        cb = tolower((unsigned char)*b++);
        if (ca != cb || ca == 0) return ca - cb;
    }
    return 0;
}

char *strndup(const char *s, size_t n) {
    size_t len = strnlen(s, n);
    char *p = (char *)malloc(len + 1);
    if (p) {
        memcpy(p, s, len);
        p[len] = '\0';
    }
    return p;
}

char *strerror(int errnum) {
    (void)errnum;
    return (char *)"error";
}

/* strerror_r (POSIX/XSI form: fill buf, return 0). picolibc routes its own
 * strerror_r through the _user_strerror hook the translator rejects, so the
 * machine port supplies it. Needed by libc++ <system_error>/<locale> in a C++
 * cart; unreferenced by the plain-C DOOM/Quake path. */
int strerror_r(int errnum, char *buf, size_t buflen) {
    (void)errnum;
    const char *m = "error";
    size_t i = 0;
    if (buflen) { for (; m[i] && i + 1 < buflen; ++i) buf[i] = m[i]; buf[i] = 0; }
    return 0;
}

/* ============================== stdlib.h =============================== */
/* picolibc owns abs/labs, atoi/atol, strtol/strtoul, qsort/bsearch. The malloc
 * family is CONFIGURABLE per cart (see the two modes below): by default it is
 * picolibc's (sbrk-backed); with -DCRON_LIBC_TUNED_MALLOC it is the Cronopio
 * tuned O(1)-free allocator. Kept here regardless: strtod/atof + rand/getenv/
 * exit/abort/assert (not in picolibc's curated build). */

/* The array-size overflow helper is shared by both allocator configs below. */
static __attribute__((noinline))
size_t _cvm_array_bytes(size_t nmemb, size_t size) {
    /* Multiply, then verify by dividing the product back (overflow check). */
    volatile size_t total = nmemb * size;
    size_t t = total;
    if (size != 0 && (t / size) != nmemb) return 0;   /* overflowed */
    return t;
}

#ifdef CRON_LIBC_TUNED_MALLOC
/* ---- CANONICAL malloc = the Cronopio TUNED allocator -------------------- *
 * This cart selects the tuned O(1)-free allocator (cvm_alloc.h) as the standard
 * malloc family. picolibc.bc MUST be built WITHOUT its own malloc family
 * (build_picolibc.sh --no-malloc) so there is exactly ONE allocator on the cron
 * heap and no sbrk is needed (the allocator carves cvm_sys_heap_* directly).
 * Chosen for UQM: its 10559-entry ZIP content mount keeps many small blocks
 * live, where the tuned O(1) free beats picolibc nano-malloc's O(n) free
 * (~1s vs ~9s mount). See docs/architecture.md for the full trade-off. */
void *malloc(size_t size) {
    if (size == 0) size = 1;
    return cvm_malloc((int)size);
}
void free(void *ptr) { cvm_free(ptr); }
void *calloc(size_t nmemb, size_t size) {
    if ((nmemb != 0) && (size != 0)) {
        size_t total = _cvm_array_bytes(nmemb, size);
        if (total == 0) return NULL;        /* overflow */
        void *p = malloc(total);
        if (p) memset(p, 0, total);
        return p;
    }
    return malloc(1);                       /* zero count/size: 1-byte block */
}
void *realloc(void *ptr, size_t size) {
    if (ptr == NULL) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }
    /* payload size of the existing block: (whole-block-size & ~1) - header */
    int32_t whole = *(int32_t *)((char *)ptr - 4) & ~1;
    size_t oldsz = (size_t)(whole - 4);
    void *np = malloc(size);
    if (!np) return NULL;
    size_t copy = oldsz < size ? oldsz : size;
    memcpy(np, ptr, copy);
    free(ptr);
    return np;
}
#else
/* ---- CANONICAL malloc = picolibc's (sbrk-backed); tuned stays as cron_* -- *
 * Default config. picolibc.bc owns malloc/free/calloc/realloc; this file
 * supplies the sole OS hook picolibc needs — sbrk over the cron heap. The tuned
 * allocator is kept available under cron_* names (dormant; DCE'd unless the cart
 * calls it). A cart must NOT mix cron_malloc with picolibc malloc — both draw
 * from the same cron heap (cvm_sys_heap_*). To make the tuned allocator the
 * cart's standard malloc instead, build with -DCRON_LIBC_TUNED_MALLOC (and
 * picolibc.bc with --no-malloc), as build_uqm.sh does. */
/* sbrk: picolibc's malloc grows the heap by calling sbrk(). Hand out the region
 * reserved by --heap-reserve (located by cvm_sys_heap_start/size) linearly;
 * (void*)-1 + errno=ENOMEM on exhaustion is the contract picolibc expects. */
void *sbrk(ptrdiff_t incr);
void *sbrk(ptrdiff_t incr) {
    static int   inited = 0;
    static char *base, *brk, *end;
    if (!inited) {
        base = brk = (char *)(size_t)cvm_sys_heap_start();
        end  = base + (size_t)cvm_sys_heap_size();
        inited = 1;
    }
    char *prev = brk;
    if (incr > 0) {
        if ((size_t)(end - brk) < (size_t)incr) { errno = ENOMEM; return (void *)-1; }
    } else if (incr < 0) {
        if ((size_t)(brk - base) < (size_t)(-incr)) { errno = ENOMEM; return (void *)-1; }
    }
    brk += incr;
    return prev;
}

void *cron_malloc(size_t size);
void  cron_free(void *ptr);
void *cron_calloc(size_t nmemb, size_t size);
void *cron_realloc(void *ptr, size_t size);

void *cron_malloc(size_t size) {
    if (size == 0) size = 1;
    return cvm_malloc((int)size);
}
void *cron_calloc(size_t nmemb, size_t size) {
    if ((nmemb != 0) && (size != 0)) {
        size_t total = _cvm_array_bytes(nmemb, size);
        if (total == 0) return NULL;        /* overflow */
        void *p = cron_malloc(total);
        if (p) memset(p, 0, total);
        return p;
    }
    return cron_malloc(1);                   /* zero count/size: 1-byte block */
}
void cron_free(void *ptr) { cvm_free(ptr); }
void *cron_realloc(void *ptr, size_t size) {
    if (ptr == NULL) return cron_malloc(size);
    if (size == 0) { cron_free(ptr); return NULL; }
    int32_t whole = *(int32_t *)((char *)ptr - 4) & ~1;
    size_t oldsz = (size_t)(whole - 4);
    void *np = cron_malloc(size);
    if (!np) return NULL;
    size_t copy = oldsz < size ? oldsz : size;
    memcpy(np, ptr, copy);
    cron_free(ptr);
    return np;
}
#endif  /* CRON_LIBC_TUNED_MALLOC */

/* strtol/strtoul/atoi/atol are picolibc's now (its versions set errno on
 * overflow rather than saturating — the standard behaviour). */

/* atof / strtod return `double` (f64) by their C signatures. The translator
 * REJECTS any function whose return type is f64 — even a trivial `return 0`,
 * because the IR-level return type is still f64 (it scans every defined
 * function, not just reachable ones). So we CANNOT define them here without
 * making the whole module untranslatable.
 *
 * They are therefore guarded out by default. The headers still DECLARE them
 * (DOOM's m_config.c / i_musicpack.c reference atof), so those translation
 * units compile; but the few call sites that consume the double result
 * (`(float)atof(str)`, `atof(p) * rate`) themselves produce f64 in DOOM's own
 * code and must be float-ported by the cart anyway. Once that float port is
 * in place — i.e. atof is reachable only through a float-returning shim — a
 * build can define CVM_LIBC_ENABLE_F64 to pull in these bodies (and must then
 * link cvm_float64.h software emulation, or accept the f64 rejection).
 *
 * Net: by default this libc translates cleanly; atof/strtod are link-time
 * unresolved only if actually called, which flags the call sites for porting.*/
#ifdef CVM_LIBC_ENABLE_F64
/* Real string->double. f64 arithmetic (soft runtime) is fine here — these are
 * cold paths (config/entity parsing). The whitespace test is written as ranges,
 * not a char==... chain, so clang doesn't fold it into an i24 bit-test the
 * translator rejects (same gotcha as fscanf's sf_isws). */
double strtod(const char *nptr, char **endptr) {
    const char *s = nptr;
    while ((*s >= 9 && *s <= 13) || *s == 32) s++;   /* leading whitespace */
    int neg = 0;
    if (*s == '+' || *s == '-') { neg = (*s == '-'); s++; }

    double val = 0.0;
    while (*s >= '0' && *s <= '9') { val = val * 10.0 + (double)(*s - '0'); s++; }
    if (*s == '.') {
        s++;
        double frac = 0.1;
        while (*s >= '0' && *s <= '9') {
            val += (double)(*s - '0') * frac;
            frac *= 0.1;
            s++;
        }
    }
    if (*s == 'e' || *s == 'E') {
        s++;
        int eneg = 0;
        if (*s == '+' || *s == '-') { eneg = (*s == '-'); s++; }
        int exp = 0;
        while (*s >= '0' && *s <= '9') { exp = exp * 10 + (*s - '0'); s++; }
        double p = 1.0;
        for (int k = 0; k < exp; k++) p *= 10.0;
        if (eneg) val /= p; else val *= p;
    }
    if (endptr) *endptr = (char *)s;
    return neg ? -val : val;
}
double atof(const char *nptr) { return strtod(nptr, NULL); }
#endif

/* qsort + bsearch are picolibc's now. */

/* rand / srand — 32-bit xorshift. RAND_MAX is 0x7fffffff, so we mask the top
 * bit off. */
static uint32_t _cvm_rng = 1u;

void srand(unsigned int seed) { _cvm_rng = seed ? (uint32_t)seed : 1u; }

int rand(void) {
    uint32_t x = _cvm_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    _cvm_rng = x;
    return (int)(x & 0x7fffffffu);
}

char *getenv(const char *name) { (void)name; return NULL; }

void exit(int status) {
    cron_exit(status);
    for (;;) { }   /* cron_exit terminates the cart; loop satisfies noreturn */
}

void abort(void) {
    static const char msg[] = "abort\n";
    cron_log(msg, (int32_t)sizeof(msg) - 1);
    cron_exit(1);
    for (;;) { }
}

void atexit_unsupported(void) { }

/* assert() support helper (declared in <assert.h>): format the failed
 * expression with snprintf (picolibc) and log it, then terminate the cart. */
void _cvm_assert_fail(const char *expr, const char *file, int line) {
    char buf[256];
    int n = snprintf(buf, sizeof buf,
                     "assertion failed: %s, file %s, line %d\n", expr, file, line);
    int emit = n < (int)sizeof buf ? n : (int)sizeof buf - 1;
    cron_log(buf, emit);
    cron_exit(1);
    for (;;) { }
}

/* ============================== stdio.h ================================ *
 * The stdio IMPLEMENTATION is picolibc's tinystdio (printf/scanf/FILE/fopen, in
 * picolibc.bc built --with-stdio). This file provides the POSIX backend it sits
 * on -- open/read/write/lseek/close over the RAM-FS + ROM (below) -- plus the
 * one math hook its float formatter needs. The RAM-FS is a small persisted
 * "memory card"; the baked --rom blob backs a read-only file (cron_rom_mount).*/

/* tinystdio's float formatter bit-tests doubles via __isnand (isnan(double)). */
int __isnand(double x) { return x != x; }

/* ---- the persisted RAM filesystem (backs fopen-ed files) --------------- */
#define RAMFS_MAX    24
#define RAMFS_NAME   96
#define RAMFS_MAGIC  0x31534643u   /* 'C','F','S','1' little-endian */

typedef struct { int used; char name[RAMFS_NAME]; uint8_t *data; uint32_t len, cap; } ramfile_t;
static ramfile_t        g_fs[RAMFS_MAX];

/* A cart can expose its baked --rom blob (cron_rom) as a single read-only file
 * at a chosen path: fopen(path, "r"/"rb") then returns a ROM-backed handle whose
 * reads come straight from ROM (only requested bytes copied — no duplication of
 * a large blob). Used e.g. to serve a game data archive without a host FS. */
static const char *g_rom_path;
void cron_rom_mount(const char *path) { g_rom_path = path; }
static int              g_fs_loaded = 0;

static uint32_t cvm_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static void cvm_wr32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}

/* Normalise a path for matching: drop leading "./" and "/", drop a trailing
 * "/", collapse "."/""/"/" to the empty root prefix. The flat RAM-FS has no
 * notion of cwd, so a leading slash is meaningless — callers (e.g. UQM's uio,
 * which uses absolute "/content/..." paths) must compare equal to the
 * relative names files were created/mounted under. Writes <= RAMFS_NAME-1. */
static void cvm_norm_path(const char *p, char *out) {
    if (!p) { out[0] = 0; return; }
    while (p[0] == '.' && p[1] == '/') p += 2;   /* "./" */
    while (p[0] == '/') ++p;                       /* leading slashes */
    size_t n = strlen(p);
    while (n > 0 && p[n-1] == '/') --n;            /* trailing slash */
    if (n == 1 && p[0] == '.') n = 0;              /* bare "." -> root */
    if (n >= RAMFS_NAME) n = RAMFS_NAME - 1;
    memcpy(out, p, n); out[n] = 0;
}

/* Path equality up to normalisation ("/content/x" == "content/x" == "./content/x"). */
static int cvm_path_eq(const char *a, const char *b) {
    char na[RAMFS_NAME], nb[RAMFS_NAME];
    cvm_norm_path(a, na); cvm_norm_path(b, nb);
    return strcmp(na, nb) == 0;
}

/* Deserialise the FS from the persisted save blob (once, lazily). */
static void ramfs_load(void) {
    if (g_fs_loaded) return;
    g_fs_loaded = 1;
    int32_t used = cron_save_used();
    if (used < 8) return;
    uint8_t *buf = (uint8_t *)malloc((size_t)used);
    if (!buf) return;
    int32_t got = cron_save_read(buf, used);
    if (got >= 8 && cvm_rd32(buf) == RAMFS_MAGIC) {
        uint32_t count = cvm_rd32(buf + 4), o = 8;
        for (uint32_t i = 0; i < count && i < RAMFS_MAX; ++i) {
            if (o + 6 > (uint32_t)got) break;
            uint32_t nl = (uint32_t)buf[o] | ((uint32_t)buf[o+1] << 8); o += 2;
            if (o + nl + 4 > (uint32_t)got) break;
            uint32_t nc = nl < RAMFS_NAME ? nl : RAMFS_NAME - 1;
            memcpy(g_fs[i].name, buf + o, nc); g_fs[i].name[nc] = 0; o += nl;
            uint32_t dl = cvm_rd32(buf + o); o += 4;
            if (o + dl > (uint32_t)got) break;
            g_fs[i].data = (uint8_t *)malloc(dl ? dl : 1);
            if (!g_fs[i].data) break;
            if (dl) memcpy(g_fs[i].data, buf + o, dl);
            g_fs[i].len = dl; g_fs[i].cap = dl ? dl : 1; g_fs[i].used = 1;
            o += dl;
        }
    }
    free(buf);
}

/* Serialise the whole FS back into the save blob (host persists it to disk). */
static void ramfs_flush(void) {
    uint32_t total = 8, n = 0;
    for (int i = 0; i < RAMFS_MAX; ++i)
        if (g_fs[i].used) { total += 2 + (uint32_t)strlen(g_fs[i].name) + 4 + g_fs[i].len; n++; }
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return;
    cvm_wr32(buf, RAMFS_MAGIC); cvm_wr32(buf + 4, n);
    uint32_t o = 8;
    for (int i = 0; i < RAMFS_MAX; ++i) if (g_fs[i].used) {
        uint32_t nl = (uint32_t)strlen(g_fs[i].name);
        buf[o] = (uint8_t)nl; buf[o+1] = (uint8_t)(nl >> 8); o += 2;
        memcpy(buf + o, g_fs[i].name, nl); o += nl;
        cvm_wr32(buf + o, g_fs[i].len); o += 4;
        if (g_fs[i].len) memcpy(buf + o, g_fs[i].data, g_fs[i].len);
        o += g_fs[i].len;
    }
    cron_save_write(buf, (int32_t)total);
    free(buf);
}

static int ramfs_find(const char *name) {
    for (int i = 0; i < RAMFS_MAX; ++i)
        if (g_fs[i].used && cvm_path_eq(g_fs[i].name, name)) return i;
    return -1;
}
static int ramfs_create(const char *name) {
    for (int i = 0; i < RAMFS_MAX; ++i) if (!g_fs[i].used) {
        g_fs[i].used = 1; snprintf(g_fs[i].name, RAMFS_NAME, "%s", name);
        g_fs[i].data = NULL; g_fs[i].len = 0; g_fs[i].cap = 0;
        return i;
    }
    return -1;
}
static int ramfile_grow(ramfile_t *f, uint32_t need) {
    if (need <= f->cap) return 0;
    uint32_t nc = f->cap ? f->cap : 256;
    while (nc < need) nc <<= 1;
    uint8_t *nd = (uint8_t *)realloc(f->data, nc);
    if (!nd) return -1;
    f->data = nd; f->cap = nc;
    return 0;
}

/* ---- POSIX fd backend over the RAM-FS + ROM ---------------------------- *
 * tinystdio's FILE layer (fopen/fread/fwrite/fseek/...) sits on these. Each fd
 * is a {RAM-FS slot OR ROM, position, write-flag} descriptor. fd 0/1/2 are the
 * standard streams: writes to 1/2 go to cron_log, reads return EOF. O_* values
 * match picolibc's (sdk/include/fcntl.h), so the flags tinystdio passes are
 * interpreted correctly. close() of a written file flushes the RAM-FS. */
#define CVM_FD_BASE 3
#define CVM_FD_MAX  16
typedef struct { int used; int slot; uint32_t pos; int writing; int is_rom; } cvm_fd_t;
static cvm_fd_t g_fd[CVM_FD_MAX];

static cvm_fd_t *fd_get(int fd) {
    int i = fd - CVM_FD_BASE;
    if (i < 0 || i >= CVM_FD_MAX || !g_fd[i].used) return NULL;
    return &g_fd[i];
}

int open(const char *path, int flags, ...) {
    if (!path) { errno = ENOENT; return -1; }
    ramfs_load();
    int writing = (flags & (O_WRONLY | O_RDWR)) != 0;

    /* ROM-backed read-only file (the baked --rom blob at the mounted path). */
    if (!writing && g_rom_path && cron_rom_size() > 0 && cvm_path_eq(path, g_rom_path)) {
        for (int i = 0; i < CVM_FD_MAX; ++i) if (!g_fd[i].used) {
            g_fd[i].used = 1; g_fd[i].slot = -1; g_fd[i].pos = 0;
            g_fd[i].writing = 0; g_fd[i].is_rom = 1;
            return CVM_FD_BASE + i;
        }
        errno = ENOENT; return -1;
    }

    int slot = ramfs_find(path);
    if (!writing && slot < 0) { errno = ENOENT; return -1; }
    if (writing) {
        if (slot < 0) slot = ramfs_create(path);
        if (slot < 0) { errno = ENOENT; return -1; }
        if (flags & O_TRUNC) g_fs[slot].len = 0;
    }
    for (int i = 0; i < CVM_FD_MAX; ++i) if (!g_fd[i].used) {
        g_fd[i].used = 1; g_fd[i].slot = slot;
        g_fd[i].pos = ((flags & O_APPEND) && slot >= 0) ? g_fs[slot].len : 0;
        g_fd[i].writing = writing; g_fd[i].is_rom = 0;
        return CVM_FD_BASE + i;
    }
    errno = ENOENT; return -1;
}

ssize_t read(int fd, void *buf, size_t count) {
    cvm_fd_t *d = fd_get(fd);
    if (!d) return (fd >= 0 && fd <= 2) ? 0 : -1;   /* stdin -> EOF */
    const uint8_t *src; uint32_t len;
    if (d->is_rom) { src = cron_rom(); len = cron_rom_size(); }
    else { ramfile_t *f = &g_fs[d->slot]; src = f->data; len = f->len; }
    uint32_t avail = (d->pos < len) ? (len - d->pos) : 0;
    size_t got = count < avail ? count : avail;
    if (got) { memcpy(buf, src + d->pos, got); d->pos += (uint32_t)got; }
    return (ssize_t)got;
}

ssize_t write(int fd, const void *buf, size_t count) {
    if (fd == 1 || fd == 2) { cron_log((const char *)buf, (int32_t)count); return (ssize_t)count; }
    cvm_fd_t *d = fd_get(fd);
    if (!d || d->is_rom) return -1;
    ramfile_t *f = &g_fs[d->slot];
    if (ramfile_grow(f, d->pos + (uint32_t)count) != 0) { errno = ENOMEM; return -1; }
    memcpy(f->data + d->pos, buf, count);
    d->pos += (uint32_t)count;
    if (d->pos > f->len) f->len = d->pos;
    return (ssize_t)count;
}

off_t lseek(int fd, off_t offset, int whence) {
    cvm_fd_t *d = fd_get(fd);
    if (!d) return -1;
    off_t len = d->is_rom ? (off_t)cron_rom_size() : (off_t)g_fs[d->slot].len;
    off_t base = (whence == SEEK_CUR) ? (off_t)d->pos : (whence == SEEK_END) ? len : 0;
    off_t np = base + offset;
    if (np < 0) { errno = 22; return -1; }   /* EINVAL */
    d->pos = (uint32_t)np;
    return np;
}

int close(int fd) {
    cvm_fd_t *d = fd_get(fd);
    if (!d) return -1;
    if (d->writing) ramfs_flush();
    d->used = 0;
    return 0;
}

/* Locale + strftime stubs. C carts (DOOM/Quake) link picolibc built
 * --with-stdio, which omits these, so the machine port supplies trivial "C"
 * locale stubs. A C++ iostream/locale cart (e.g. Exult) instead links picolibc
 * --with-locale, which provides the REAL setlocale/localeconv/strftime — so the
 * cart's build defines CRON_SYS_LIBC_HAS_LOCALE to suppress these duplicates
 * (otherwise llvm-link reports "symbol multiply defined"). */
#ifndef CRON_SYS_LIBC_HAS_LOCALE
char *setlocale(int category, const char *locale) {
    (void)category; (void)locale;
    return (char *)"C";
}
struct lconv *localeconv(void) {
    static char dp[] = ".";
    static char empty[] = "";
    static struct lconv lc;
    lc.decimal_point = dp;
    lc.thousands_sep = empty;
    lc.grouping = empty;
    lc.int_curr_symbol = empty;
    lc.currency_symbol = empty;
    lc.mon_decimal_point = empty;
    lc.mon_thousands_sep = empty;
    lc.mon_grouping = empty;
    lc.positive_sign = empty;
    lc.negative_sign = empty;
    return &lc;
}
#endif /* !CRON_SYS_LIBC_HAS_LOCALE */

int remove(const char *path) {
    ramfs_load();
    int s = ramfs_find(path);
    if (s < 0) { errno = ENOENT; return -1; }
    free(g_fs[s].data);
    g_fs[s].used = 0; g_fs[s].data = NULL; g_fs[s].len = g_fs[s].cap = 0;
    ramfs_flush();
    return 0;
}

int rename(const char *oldp, const char *newp) {
    ramfs_load();
    int s = ramfs_find(oldp);
    if (s < 0) { errno = ENOENT; return -1; }
    int d = ramfs_find(newp);            /* replace any existing destination */
    if (d >= 0 && d != s) { free(g_fs[d].data); g_fs[d].used = 0; g_fs[d].data = NULL; }
    snprintf(g_fs[s].name, RAMFS_NAME, "%s", newp);
    ramfs_flush();
    return 0;
}

/* ===================== dirent.h — directory enumeration ================ */
/* The RAM-FS stores files by full path and the ROM blob is one file at
 * g_rom_path. Directories are implicit: opendir()/readdir() synthesise a
 * listing by walking every stored path (plus the ROM file) and emitting the
 * immediate child component under the queried directory, de-duplicated.
 * Faithful enough for ports that scan a directory (UQM's uio stdio fs). */

#include <dirent.h>

/* cvm_norm_path / cvm_path_eq are defined up in the RAM-FS section. */

#define CVM_DIR_DEDUP 64
struct __cvm_DIR {
    char prefix[RAMFS_NAME];          /* normalised dir, "" = root */
    int  next;                        /* next g_fs[] index to scan */
    int  rom_done;                    /* ROM file already considered */
    int  n_emitted;
    char emitted[CVM_DIR_DEDUP][RAMFS_NAME];
    struct dirent ent;                /* storage for the non-_r readdir() */
};

/* Does normalised `nm` live directly or indirectly under normalised
 * `prefix`? If so, copy its immediate child component into `child` and set
 * *is_dir (a child with a further '/' after it is a subdirectory). */
static int cvm_dir_child(const char *prefix, const char *nm,
                         char *child, int *is_dir) {
    const char *rem;
    if (prefix[0] == 0) {
        rem = nm;
    } else {
        size_t pl = strlen(prefix);
        if (strncmp(nm, prefix, pl) != 0 || nm[pl] != '/') return 0;
        rem = nm + pl + 1;
    }
    if (rem[0] == 0) return 0;        /* the directory itself */
    size_t k = 0;
    while (rem[k] && rem[k] != '/') ++k;
    if (k == 0 || k >= RAMFS_NAME) return 0;
    memcpy(child, rem, k); child[k] = 0;
    *is_dir = (rem[k] == '/');
    return 1;
}

DIR *opendir(const char *path) {
    ramfs_load();
    DIR *d = (DIR *)malloc(sizeof *d);
    if (!d) { errno = ENOMEM; return NULL; }
    cvm_norm_path(path, d->prefix);
    d->next = 0; d->rom_done = 0; d->n_emitted = 0;
    return d;
}

int readdir_r(DIR *d, struct dirent *entry, struct dirent **result) {
    if (!d) { errno = EBADF; if (result) *result = NULL; return EBADF; }
    char child[RAMFS_NAME]; int is_dir;
    for (;;) {
        const char *nm = NULL;
        char rom_norm[RAMFS_NAME];
        if (d->next < RAMFS_MAX) {
            int i = d->next++;
            if (!g_fs[i].used) continue;
            nm = g_fs[i].name;
        } else if (!d->rom_done) {
            d->rom_done = 1;
            if (g_rom_path && cron_rom_size() > 0) {
                cvm_norm_path(g_rom_path, rom_norm);
                nm = rom_norm;
            } else continue;
        } else {
            if (result) *result = NULL;     /* end of directory */
            return 0;
        }
        char nm_norm[RAMFS_NAME];
        cvm_norm_path(nm, nm_norm);
        if (!cvm_dir_child(d->prefix, nm_norm, child, &is_dir)) continue;
        /* de-dup (subdirs appear once even if many files live under them) */
        int dup = 0;
        for (int e = 0; e < d->n_emitted; ++e)
            if (strcmp(d->emitted[e], child) == 0) { dup = 1; break; }
        if (dup) continue;
        if (d->n_emitted < CVM_DIR_DEDUP)
            snprintf(d->emitted[d->n_emitted++], RAMFS_NAME, "%s", child);
        entry->d_ino = 1;
        entry->d_type = is_dir ? DT_DIR : DT_REG;
        snprintf(entry->d_name, sizeof entry->d_name, "%s", child);
        if (result) *result = entry;
        return 0;
    }
}

struct dirent *readdir(DIR *d) {
    if (!d) { errno = EBADF; return NULL; }
    struct dirent *res = NULL;
    if (readdir_r(d, &d->ent, &res) != 0) return NULL;
    return res;
}

void rewinddir(DIR *d) {
    if (d) { d->next = 0; d->rom_done = 0; d->n_emitted = 0; }
}

int closedir(DIR *d) {
    if (!d) { errno = EBADF; return -1; }
    free(d);
    return 0;
}

/* ============================== sys/stat.h ============================= */
/* No real perms; report regular files (RAM-FS + ROM) and implicit dirs. */

int stat(const char *path, struct stat *buf) {
    if (!path || !buf) { errno = EFAULT; return -1; }
    ramfs_load();
    memset(buf, 0, sizeof *buf);

    int slot = ramfs_find(path);
    if (slot >= 0) {
        buf->st_mode = S_IFREG | 0644;
        buf->st_size = (off_t)g_fs[slot].len;
        return 0;
    }
    if (g_rom_path && cron_rom_size() > 0 && cvm_path_eq(path, g_rom_path)) {
        buf->st_mode = S_IFREG | 0644;
        buf->st_size = (off_t)cron_rom_size();
        return 0;
    }
    /* A directory exists iff some stored path (or the ROM file) lives under
     * it. The empty/root prefix always exists. */
    char prefix[RAMFS_NAME]; cvm_norm_path(path, prefix);
    char child[RAMFS_NAME]; int is_dir;
    int exists = (prefix[0] == 0);
    for (int i = 0; !exists && i < RAMFS_MAX; ++i) {
        if (!g_fs[i].used) continue;
        char nm[RAMFS_NAME]; cvm_norm_path(g_fs[i].name, nm);
        if (cvm_dir_child(prefix, nm, child, &is_dir)) exists = 1;
    }
    if (!exists && g_rom_path && cron_rom_size() > 0) {
        char nm[RAMFS_NAME]; cvm_norm_path(g_rom_path, nm);
        if (cvm_dir_child(prefix, nm, child, &is_dir)) exists = 1;
    }
    if (exists) { buf->st_mode = S_IFDIR | 0755; return 0; }
    errno = ENOENT;
    return -1;
}

/* Directories are implicit (a path prefix), so creating one always
 * "succeeds" — there is nothing to allocate until a file is written under it. */
int mkdir(const char *path, unsigned int mode) {
    (void)path; (void)mode;
    return 0;
}

/* Removing an implicit directory is a no-op success (the entries under it, if
 * any, are removed by unlink()ing the files themselves). */
int rmdir(const char *path) { (void)path; return 0; }

/* fstat over the fd table: an open fd is always a regular file (RAM-FS or
 * ROM-backed); report its size. */
int fstat(int fd, struct stat *buf) {
    cvm_fd_t *d = fd_get(fd);
    if (!d || !buf) { errno = EBADF; return -1; }
    memset(buf, 0, sizeof *buf);
    buf->st_mode = S_IFREG | 0644;
    buf->st_size = d->is_rom ? (off_t)cron_rom_size()
                             : (off_t)g_fs[d->slot].len;
    return 0;
}

/* ============================== unistd.h =============================== */

int access(const char *path, int mode) {
    /* Existence/readability only (no real perms). Delegate to stat() so the
     * ROM-backed file and implicit directories are recognised, not just
     * RAM-FS entries — uio's stdio fs probes files with access() before
     * opening them. */
    (void)mode;
    struct stat st;
    return stat(path, &st);   /* sets errno = ENOENT on miss */
}
int unlink(const char *path) { return remove(path); }
char *getcwd(char *buf, size_t size) {
    if (buf && size) buf[0] = '\0';
    return buf;
}

/* ============================== time.h ================================= */
/* No real-time clock wired to the libc: time() returns 0 (epoch). The host
 * clock is available via cron_time_ms() in <cronopio.h> for game timing. */

time_t time(time_t *t) {
    if (t) *t = 0;
    return 0;
}

clock_t clock(void) { return 0; }

static struct tm _cvm_tm;   /* static storage for localtime/gmtime */

struct tm *localtime(const time_t *timep) {
    (void)timep;
    memset(&_cvm_tm, 0, sizeof _cvm_tm);
    _cvm_tm.tm_year = 70;   /* 1970 */
    _cvm_tm.tm_mday = 1;
    return &_cvm_tm;
}

struct tm *gmtime(const time_t *timep) { return localtime(timep); }

/* No-op tzset (the console runs on GMT — there is no TZ environment). picolibc's
 * real strftime (linked when a C++ locale cart builds picolibc --with-locale)
 * calls tzset; the --with-stdio C path uses cron_sys.c's own strftime stub and
 * never references it, so this is inert for DOOM/Quake. */
void tzset(void) { }

#ifndef CRON_SYS_LIBC_HAS_LOCALE   /* picolibc --with-locale provides the real one */
size_t strftime(char *s, size_t max, const char *format, const struct tm *tm) {
    (void)format; (void)tm;
    if (max == 0) return 0;
    s[0] = '\0';   /* empty string — DOOM tolerates an empty timestamp */
    return 0;
}
#endif

char *asctime(const struct tm *tm) {
    (void)tm;
    static char buf[] = "Thu Jan  1 00:00:00 1970\n";
    return buf;
}

char *ctime(const time_t *timep) {
    (void)timep;
    return asctime(&_cvm_tm);
}

/* ====================================================================== */
/* Coroutines — trampoline for the default cron_coro_init.                 */
/* The actual context-swap primitive lives in the VM (opcode CVM_OP_CORO_  */
/* SWAP = 0x3C); the translator lowers a call to __cvm_coro_swap_raw to    */
/* that opcode, so no body is needed for it here.                         */

#include <coro.h>

void __cron_coro_trampoline(cron_coro_t *self) {
    /* CORO_SWAP already marked us RUNNING on the first swap-in; redundant
     * but explicit. Then run the user's fn, mark dead, hand control back. */
    self->status = CORO_RUNNING;
    self->fn(self->arg);
    self->status = CORO_DEAD;
    if (self->resumer) {
        cron_coro_swap(self, self->resumer);
    }
    /* If there is no resumer, fall off the stack — RET pops the trap
     * sentinel that cron_coro_init planted at SP top, which faults with
     * CVM_E_BAD_PC. The intent is a clean trap so a buggy scheduler is
     * obvious; reaching this point silently would be worse. */
}
