/* cvm_libc.c — Cronopio SDK freestanding C library implementation.
 *
 * Carts run on CronoVM with no host OS and no hosted libc. This file
 * implements the slice of the C standard library that ports written for a
 * hosted environment (DOOM / Crispy Doom) need to compile and run: the
 * <ctype.h>, <string.h>, <stdlib.h> and <stdio.h> declarations shipped in
 * sdk/include, plus the small POSIX-ish stubs (<sys/stat.h>, <time.h>,
 * <unistd.h>) those ports include.
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
 * Memory comes from cvm_alloc (the cart heap); console output and termination
 * route through the Cronopio syscalls in <cronopio.h>. There is no filesystem:
 * FILE I/O is stubbed so ports link, but bundled assets must be read from the
 * cartridge ROM (cron_rom), not fopen(). */

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
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

/* ---------------- errno ------------------------------------------------- */

int errno = 0;

/* Varargs-free formatter cores, defined in the stdio section below. Declared
 * here so earlier users (e.g. the assert helper) can call them. */
int cvm_vsnprintf_buf(char *str, size_t size, const char *fmt, const void *args);
int cvm_vsscanf(const char *str, const char *fmt, va_list ap);

/* ============================== ctype.h ================================= */
/* ASCII / C locale. All trivial range checks; no tables to keep them small. */

int isdigit(int c)  { return c >= '0' && c <= '9'; }
int isupper(int c)  { return c >= 'A' && c <= 'Z'; }
int islower(int c)  { return c >= 'a' && c <= 'z'; }
int isalpha(int c)  { return isupper(c) || islower(c); }
int isalnum(int c)  { return isalpha(c) || isdigit(c); }
int isspace(int c)  { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isblank(int c)  { return c == ' ' || c == '\t'; }
int iscntrl(int c)  { return (c >= 0 && c < 0x20) || c == 0x7f; }
int isprint(int c)  { return c >= 0x20 && c < 0x7f; }
int isgraph(int c)  { return c > 0x20 && c < 0x7f; }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int ispunct(int c)  { return isgraph(c) && !isalnum(c); }

int tolower(int c)  { return isupper(c) ? c + ('a' - 'A') : c; }
int toupper(int c)  { return islower(c) ? c - ('a' - 'A') : c; }

/* ============================== string.h =============================== */

/* __builtin_mem* lower to the llvm.mem* intrinsics in the -emit-llvm IR, which
 * cvm-translate turns into the VM's single-instruction MEMCPY/MEMSET/MEMMOVE
 * opcodes (one host memcpy/memset per call) instead of a per-byte CVM loop.
 * The builtin is the compiler primitive, not a call to these symbols, so there
 * is no self-recursion. Profiling DOOM showed the byte-loop memset at ~44% of
 * all in-level interpreter instructions; this collapses it to ~1 op/call. */
void *memcpy(void *dst, const void *src, size_t n) {
    __builtin_memcpy(dst, src, n);
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    __builtin_memmove(dst, src, n);
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    __builtin_memset(dst, c, n);
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    while (n--) {
        if (*pa != *pb) return (int)*pa - (int)*pb;
        pa++; pb++;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    unsigned char v = (unsigned char)c;
    while (n--) {
        if (*p == v) return (void *)p;
        p++;
    }
    return NULL;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++) != '\0') { }
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    char *d = dst;
    while (n && (*d = *src) != '\0') { d++; src++; n--; }
    while (n--) *d++ = '\0';
    return dst;
}

char *strcat(char *dst, const char *src) {
    char *d = dst;
    while (*d) d++;
    while ((*d++ = *src++) != '\0') { }
    return dst;
}

char *strncat(char *dst, const char *src, size_t n) {
    char *d = dst;
    while (*d) d++;
    while (n && (*src != '\0')) { *d++ = *src++; n--; }
    *d = '\0';
    return dst;
}

int strcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && (*a == *b)) { a++; b++; n--; }
    if (n == 0) return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

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

size_t strlen(const char *s) {
    /* Walk a separate pointer and counter, incrementing both INSIDE the loop
     * body. A counter returned straight from a loop-header phi (`while(s[n])
     * n++; return n;`) is miscomputed by one by the current translator; this
     * body-increment form translates correctly. */
    size_t n = 0;
    const char *p = s;
    while (*p != '\0') {
        p++;
        n++;
    }
    return n;
}

size_t strnlen(const char *s, size_t n) {
    /* Body-increment counter (see strlen) to avoid the translator's
     * header-phi off-by-one. */
    size_t i = 0;
    const char *p = s;
    while (i < n && *p != '\0') { p++; i++; }
    return i;
}

char *strchr(const char *s, int c) {
    char ch = (char)c;
    for (;;) {
        if (*s == ch) return (char *)s;
        if (*s == '\0') return NULL;
        s++;
    }
}

char *strrchr(const char *s, int c) {
    char ch = (char)c;
    const char *last = NULL;
    for (;;) {
        if (*s == ch) last = s;
        if (*s == '\0') break;
        s++;
    }
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle) {
    if (*needle == '\0') return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && (*h == *n)) { h++; n++; }
        if (*n == '\0') return (char *)haystack;
    }
    return NULL;
}

char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
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

/* file-local saveptr — strtok is not reentrant by design. */
static char *_cvm_strtok_save;

char *strtok(char *str, const char *delim) {
    char *s = str ? str : _cvm_strtok_save;
    if (!s) return NULL;
    /* skip leading delimiters */
    while (*s && strchr(delim, *s)) s++;
    if (*s == '\0') { _cvm_strtok_save = NULL; return NULL; }
    char *tok = s;
    /* find end of token */
    while (*s && !strchr(delim, *s)) s++;
    if (*s) { *s = '\0'; _cvm_strtok_save = s + 1; }
    else    { _cvm_strtok_save = NULL; }
    return tok;
}

char *strerror(int errnum) {
    (void)errnum;
    return (char *)"error";
}

/* ============================== stdlib.h =============================== */

void *malloc(size_t size) {
    if (size == 0) size = 1;
    return cvm_malloc((int)size);
}

/* noinline so clang cannot pattern-match the (multiply + overflow check) idiom
 * and lower it to llvm.umul.with.overflow.i32, which the translator does not
 * implement. The division test runs in a separate function from the multiply.*/
static __attribute__((noinline))
size_t _cvm_array_bytes(size_t nmemb, size_t size) {
    /* Multiply first, then verify by dividing the product back. Checking
     * AFTER the multiply (rather than guarding with MAX/size before it) avoids
     * the (guard + multiply) idiom that clang folds to
     * llvm.umul.with.overflow.i32 — an intrinsic the translator cannot lower.
     * `total` is volatile so the multiply genuinely happens before the check.*/
    volatile size_t total = nmemb * size;
    size_t t = total;
    if (size != 0 && (t / size) != nmemb) return 0;   /* overflowed */
    return t;
}

void *calloc(size_t nmemb, size_t size) {
    if ((nmemb != 0) && (size != 0)) {
        size_t total = _cvm_array_bytes(nmemb, size);
        if (total == 0) return NULL;        /* overflow */
        void *p = malloc(total);
        if (p) memset(p, 0, total);
        return p;
    }
    /* zero element count or size: allocate a 1-byte block per convention */
    return malloc(1);
}

void free(void *ptr) {
    cvm_free(ptr);
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

int abs(int n)   { return n < 0 ? -n : n; }
long labs(long n) { return n < 0 ? -n : n; }

/* Shared integer parser. Reads into an unsigned 32-bit accumulator and tracks
 * sign separately, so no 64-bit math is required. Saturates on overflow. */
static __attribute__((noinline))
unsigned long _cvm_strtox(const char *s, char **endptr, int base,
                          int *negout, int sgned) {
    const char *start = s;
    int neg = 0;
    unsigned long acc = 0;
    int any = 0;
    unsigned long cutoff;

    while (isspace((unsigned char)*s)) s++;

    if (*s == '+' || *s == '-') { neg = (*s == '-'); s++; }

    if ((base == 0 || base == 16) && s[0] == '0' &&
        (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        base = 16;
    } else if (base == 0 && s[0] == '0') {
        base = 8;
    } else if (base == 0) {
        base = 10;
    }

    /* saturation limits */
    if (sgned)
        cutoff = neg ? 0x80000000ul : 0x7ffffffful;
    else
        cutoff = 0xfffffffful;

    for (;;) {
        int c = (unsigned char)*s;
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        else break;
        if (d >= base) break;

        /* overflow check: acc*base + d > cutoff ? */
        if (acc > (cutoff - (unsigned long)d) / (unsigned long)base) {
            acc = cutoff;       /* saturate; keep scanning the digits */
        } else {
            acc = acc * (unsigned long)base + (unsigned long)d;
        }
        any = 1;
        s++;
    }

    if (endptr) *endptr = (char *)(any ? s : start);
    if (negout) *negout = neg;
    return acc;
}

long strtol(const char *s, char **endptr, int base) {
    int neg = 0;
    unsigned long v = _cvm_strtox(s, endptr, base, &neg, 1);
    if (neg) {
        if (v >= 0x80000000ul) return (long)0x80000000ul; /* INT_MIN */
        return -(long)v;
    }
    if (v > 0x7ffffffful) return (long)0x7ffffffful;
    return (long)v;
}

unsigned long strtoul(const char *s, char **endptr, int base) {
    int neg = 0;
    unsigned long v = _cvm_strtox(s, endptr, base, &neg, 0);
    if (neg) return (unsigned long)(-(long)v);
    return v;
}

int atoi(const char *s) { return (int)strtol(s, NULL, 10); }
long atol(const char *s) { return strtol(s, NULL, 10); }

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
double atof(const char *nptr) { (void)nptr; return 0; }
double strtod(const char *nptr, char **endptr) {
    if (endptr) *endptr = (char *)nptr;
    return 0;
}
#endif

/* qsort — simple recursive quicksort with a median-of-first pivot, falling
 * back to insertion sort for small partitions. Element swap is byte-wise so
 * any element size works. The partition loop is noinline (register budget). */
static void _cvm_swap(char *a, char *b, size_t size) {
    while (size--) {
        char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

static __attribute__((noinline))
void _cvm_qsort(char *base, size_t n, size_t size,
                int (*cmp)(const void *, const void *)) {
    while (n > 1) {
        if (n < 8) {
            /* insertion sort for small runs */
            for (size_t i = 1; i < n; i++) {
                size_t j = i;
                while (j > 0 &&
                       cmp(base + (j - 1) * size, base + j * size) > 0) {
                    _cvm_swap(base + (j - 1) * size, base + j * size, size);
                    j--;
                }
            }
            return;
        }
        /* partition around the first element */
        char *pivot = base;
        size_t i = 1, j = n;
        for (;;) {
            while (i < n && cmp(base + i * size, pivot) < 0) i++;
            do { j--; } while (j > 0 && cmp(base + j * size, pivot) > 0);
            if (i >= j) break;
            _cvm_swap(base + i * size, base + j * size, size);
            i++;
        }
        _cvm_swap(pivot, base + j * size, size);
        /* recurse on the smaller half, loop on the larger (bounded depth) */
        size_t left = j;
        size_t right = n - j - 1;
        if (left < right) {
            _cvm_qsort(base, left, size, cmp);
            base = base + (j + 1) * size;
            n = right;
        } else {
            _cvm_qsort(base + (j + 1) * size, right, size, cmp);
            n = left;
        }
    }
}

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *)) {
    if (nmemb < 2 || size == 0) return;
    _cvm_qsort((char *)base, nmemb, size, compar);
}

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

/* assert() support helper (declared in <assert.h>): log the failed expression
 * with its file and line, then terminate the cart. Uses the varargs-free
 * formatter core (cvm_vsnprintf_buf) so it translates without va_start. */
void _cvm_assert_fail(const char *expr, const char *file, int line) {
    char buf[256];
    /* argument block for "%s, file %s, line %d": three 4-byte slots */
    const void *args[3];
    args[0] = expr;
    args[1] = file;
    *(int *)&args[2] = line;
    int n = cvm_vsnprintf_buf(buf, sizeof buf,
                              "assertion failed: %s, file %s, line %d\n", args);
    int emit = n < (int)sizeof buf ? n : (int)sizeof buf - 1;
    cron_log(buf, emit);
    cron_exit(1);
    for (;;) { }
}

/* ============================== stdio.h ================================ */

/* FILE handles. The standard streams are NULL (see below) and the printf family
 * routes to cron_log regardless. A non-NULL FILE* is a handle into the RAM
 * filesystem (a small persisted "memory card", see the file-I/O section): every
 * fopen()'d file lives in RAM and is serialised to the host save blob on close.
 * Bundled read-only assets still come from the cartridge ROM (cron_rom). */
struct _CVM_FILE { int slot; unsigned int pos; int writing; int eofbit; };
/* The translator only serialises pointer globals when the initialiser is NULL
 * (it cannot relocate the address of another global, nor an inttoptr constant
 * expr). So the three standard streams are NULL pointers. That is fine here:
 * the printf/puts family route to cron_log regardless of the stream, and the
 * kept DOOM sources never compare a stream against stdout/stderr nor
 * dereference one (verified by grep). A port that truly needs distinct stream
 * identities should set these from main() at runtime to the addresses of
 * static FILE objects. */
FILE *stdin  = NULL;
FILE *stdout = NULL;
FILE *stderr = NULL;

/* ---- core formatter ---------------------------------------------------- */

/* Output sink for cvm_vfmt: writes into a caller buffer of capacity `cap`
 * (NUL-terminated when room), counting every byte that *would* be written so
 * snprintf can report the full length like the C standard requires. */
typedef struct {
    char  *buf;
    size_t cap;     /* including the NUL slot */
    size_t len;     /* bytes written so far (capped to cap-1) */
    size_t total;   /* bytes that would have been written (uncapped) */
} _cvm_sink;

static __attribute__((noinline))
void _sink_putc(_cvm_sink *s, char c) {
    if (s->buf && s->cap && s->len + 1 < s->cap) {
        s->buf[s->len++] = c;
    }
    s->total++;
}

/* Emit an unsigned value in the given base. digits buffer is built in reverse.
 * Used for d/i/u/x/X/o/p. No 64-bit math. */
static void _sink_uint(_cvm_sink *s, unsigned long val, int base, int upper,
                       int width, int prec, int flags, int neg);

/* flag bits */
#define FL_MINUS   0x01   /* '-' left-justify           */
#define FL_PLUS    0x02   /* '+' always sign            */
#define FL_SPACE   0x04   /* ' ' space before positive  */
#define FL_HASH    0x08   /* '#' alternate form         */
#define FL_ZERO    0x10   /* '0' zero pad               */

static __attribute__((noinline))
void _sink_pad(_cvm_sink *s, char c, int n) {
    while (n-- > 0) _sink_putc(s, c);
}

static __attribute__((noinline))
void _sink_uint(_cvm_sink *s, unsigned long val, int base, int upper,
                       int width, int prec, int flags, int neg) {
    char tmp[32];
    const char *digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;

    if (val == 0) {
        if (prec != 0) tmp[n++] = '0';   /* prec 0 with value 0 => no digits */
    } else {
        while (val) {
            tmp[n++] = digs[val % (unsigned long)base];
            val /= (unsigned long)base;
        }
    }

    /* precision: minimum number of digits */
    int zeros = 0;
    if (prec > n) zeros = prec - n;

    /* sign / prefix length */
    char sign = 0;
    if (neg) sign = '-';
    else if (flags & FL_PLUS) sign = '+';
    else if (flags & FL_SPACE) sign = ' ';

    int prefixlen = 0;
    char prefix0 = 0, prefix1 = 0;
    if ((flags & FL_HASH) && base == 16 && (n > 0)) {
        prefix0 = '0';
        prefix1 = upper ? 'X' : 'x';
        prefixlen = 2;
    } else if ((flags & FL_HASH) && base == 8 && (zeros == 0) &&
               (n == 0 || tmp[n - 1] != '0')) {
        /* octal alternate form: ensure a leading 0 */
        zeros = (zeros < 1) ? 1 : zeros;
    }

    int bodylen = n + zeros + (sign ? 1 : 0) + prefixlen;
    int padlen = width > bodylen ? width - bodylen : 0;

    /* zero padding only when not left-justified and no explicit precision */
    int zeropad = 0;
    if ((flags & FL_ZERO) && !(flags & FL_MINUS) && prec < 0) {
        zeropad = padlen;
        padlen = 0;
    }

    if (!(flags & FL_MINUS)) _sink_pad(s, ' ', padlen);
    if (sign) _sink_putc(s, sign);
    if (prefixlen) { _sink_putc(s, prefix0); _sink_putc(s, prefix1); }
    _sink_pad(s, '0', zeropad);
    _sink_pad(s, '0', zeros);
    while (n > 0) _sink_putc(s, tmp[--n]);
    if (flags & FL_MINUS) _sink_pad(s, ' ', padlen);
}

static __attribute__((noinline))
void _sink_str(_cvm_sink *s, const char *str, int width, int prec,
                      int flags) {
    if (!str) str = "(null)";
    /* Body-increment counter (see strlen): the header-phi form
     * `while (str[len] ...) len++;` is miscounted by one by the translator. */
    int len = 0;
    {
        const char *p = str;
        while (*p != '\0' && (prec < 0 || len < prec)) { p++; len++; }
    }
    int padlen = width > len ? width - len : 0;
    if (!(flags & FL_MINUS)) _sink_pad(s, ' ', padlen);
    for (int i = 0; i < len; i++) _sink_putc(s, str[i]);
    if (flags & FL_MINUS) _sink_pad(s, ' ', padlen);
}

/* Minimal float formatter (f32 only — no double). Default precision 6. No
 * round-to-even; truncating-with-carry is good enough for the rare float the
 * port prints. Handles sign, integer part, and `prec` fractional digits.
 * Magnitudes beyond ~2e9 lose precision (32-bit integer split) but DOOM does
 * not print large floats. */
static __attribute__((noinline))
void _sink_float(_cvm_sink *s, float val, int width, int prec,
                        int flags, int upper) {
    (void)upper;
    if (prec < 0) prec = 6;

    int neg = 0;
    if (val < 0.0f) { neg = 1; val = -val; }

    /* split into integer and fractional parts using 32-bit ints */
    unsigned long ipart = (unsigned long)val;
    float frac = val - (float)ipart;

    /* scale the fraction to `prec` digits with rounding */
    float scale = 1.0f;
    for (int i = 0; i < prec; i++) scale *= 10.0f;
    unsigned long fpart = (unsigned long)(frac * scale + 0.5f);
    /* carry from rounding the fraction up to a whole unit */
    {
        unsigned long lim = (unsigned long)(scale + 0.5f);
        if (prec > 0 && fpart >= lim) { fpart -= lim; ipart += 1; }
    }

    /* render integer part (reversed) */
    char ibuf[16];
    int in = 0;
    if (ipart == 0) ibuf[in++] = '0';
    else while (ipart) { ibuf[in++] = (char)('0' + (int)(ipart % 10)); ipart /= 10; }

    /* render fractional part forward */
    char fbuf[16];
    int fn = 0;
    if (prec > 0) {
        unsigned long div = (unsigned long)(scale + 0.5f);
        for (int i = 0; i < prec && fn < (int)sizeof(fbuf); i++) {
            div /= 10;
            unsigned long dig = div ? (fpart / div) % 10 : fpart % 10;
            fbuf[fn++] = (char)('0' + (int)dig);
        }
    }

    char sign = 0;
    if (neg) sign = '-';
    else if (flags & FL_PLUS) sign = '+';
    else if (flags & FL_SPACE) sign = ' ';

    int bodylen = in + (prec > 0 ? prec + 1 : 0) + (sign ? 1 : 0);
    int padlen = width > bodylen ? width - bodylen : 0;
    int zeropad = 0;
    if ((flags & FL_ZERO) && !(flags & FL_MINUS)) { zeropad = padlen; padlen = 0; }

    if (!(flags & FL_MINUS)) _sink_pad(s, ' ', padlen);
    if (sign) _sink_putc(s, sign);
    _sink_pad(s, '0', zeropad);
    while (in > 0) _sink_putc(s, ibuf[--in]);
    if (prec > 0) {
        _sink_putc(s, '.');
        for (int i = 0; i < fn; i++) _sink_putc(s, fbuf[i]);
    }
    if (flags & FL_MINUS) _sink_pad(s, ' ', padlen);
}

/* The formatter core. noinline: it is large and called from many wrappers;
 * inlining it everywhere would blow the register budget. */
static __attribute__((noinline))
int cvm_vfmt(char *out, size_t cap, const char *fmt, va_list ap) {
    _cvm_sink s;
    s.buf = out; s.cap = cap; s.len = 0; s.total = 0;

    while (*fmt) {
        if (*fmt != '%') { _sink_putc(&s, *fmt++); continue; }
        fmt++;  /* past '%' */

        /* flags */
        int flags = 0;
        for (;;) {
            if      (*fmt == '-') flags |= FL_MINUS;
            else if (*fmt == '+') flags |= FL_PLUS;
            else if (*fmt == ' ') flags |= FL_SPACE;
            else if (*fmt == '#') flags |= FL_HASH;
            else if (*fmt == '0') flags |= FL_ZERO;
            else break;
            fmt++;
        }

        /* width */
        int width = 0;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) { flags |= FL_MINUS; width = -width; }
            fmt++;
        } else {
            while (isdigit((unsigned char)*fmt)) width = width * 10 + (*fmt++ - '0');
        }

        /* precision */
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; if (prec < 0) prec = -1; }
            else { prec = 0; while (isdigit((unsigned char)*fmt)) prec = prec * 10 + (*fmt++ - '0'); }
        }

        /* length modifiers — parsed and (mostly) ignored on a 32-bit target.
         * We must still CONSUME the right amount from the va_list. `ll`/`L`/`j`
         * push a 64-bit arg, but we MUST NOT do `va_arg(ap, long long)`: that
         * emits an i64 load the translator rejects. Instead we consume the two
         * 32-bit stack slots of the 64-bit arg with two `va_arg(ap, unsigned)`
         * and format only the low 32 bits (little-endian: low half first). No
         * i64 value is ever created. DOOM does not rely on the high half of a
         * %lld. */
        int len_ll = 0;     /* 1 if a 64-bit arg must be consumed */
        if (*fmt == 'h') { fmt++; if (*fmt == 'h') fmt++; }
        else if (*fmt == 'l') { fmt++; if (*fmt == 'l') { fmt++; len_ll = 1; } }
        else if (*fmt == 'L') { fmt++; len_ll = 1; }
        else if (*fmt == 'z') { fmt++; }
        else if (*fmt == 'j') { fmt++; len_ll = 1; }
        else if (*fmt == 't') { fmt++; }

        char conv = *fmt;
        if (conv == '\0') break;
        fmt++;

        switch (conv) {
        case 'd':
        case 'i': {
            long v;
            if (len_ll) {
                /* consume the 64-bit arg as two 32-bit slots; keep the low
                 * half (little-endian). Sign comes from the low half too,
                 * which is correct for the small values DOOM passes. */
                unsigned lo = va_arg(ap, unsigned);
                (void)va_arg(ap, unsigned);   /* discard high half */
                v = (long)(int)lo;
            } else {
                v = va_arg(ap, int);
            }
            int neg = 0;
            unsigned long uv;
            if (v < 0) { neg = 1; uv = (unsigned long)(-(v)); }
            else uv = (unsigned long)v;
            _sink_uint(&s, uv, 10, 0, width, prec, flags, neg);
            break;
        }
        case 'u': {
            unsigned long v;
            if (len_ll) { v = (unsigned long)va_arg(ap, unsigned); (void)va_arg(ap, unsigned); }
            else        { v = (unsigned long)va_arg(ap, unsigned int); }
            _sink_uint(&s, v, 10, 0, width, prec, flags, 0);
            break;
        }
        case 'x':
        case 'X': {
            unsigned long v;
            if (len_ll) { v = (unsigned long)va_arg(ap, unsigned); (void)va_arg(ap, unsigned); }
            else        { v = (unsigned long)va_arg(ap, unsigned int); }
            _sink_uint(&s, v, 16, conv == 'X', width, prec, flags, 0);
            break;
        }
        case 'o': {
            unsigned long v;
            if (len_ll) { v = (unsigned long)va_arg(ap, unsigned); (void)va_arg(ap, unsigned); }
            else        { v = (unsigned long)va_arg(ap, unsigned int); }
            _sink_uint(&s, v, 8, 0, width, prec, flags, 0);
            break;
        }
        case 'p': {
            void *ptr = va_arg(ap, void *);
            unsigned long v = (unsigned long)(uintptr_t)ptr;
            _sink_putc(&s, '0'); _sink_putc(&s, 'x');
            _sink_uint(&s, v, 16, 0, 0, -1, 0, 0);
            break;
        }
        case 'c': {
            char ch = (char)va_arg(ap, int);
            int padlen = width > 1 ? width - 1 : 0;
            if (!(flags & FL_MINUS)) _sink_pad(&s, ' ', padlen);
            _sink_putc(&s, ch);
            if (flags & FL_MINUS) _sink_pad(&s, ' ', padlen);
            break;
        }
        case 's': {
            const char *str = va_arg(ap, const char *);
            _sink_str(&s, str, width, prec, flags);
            break;
        }
        case 'f': case 'F':
        case 'g': case 'G':
        case 'e': case 'E': {
            /* Floating-point conversions are WEAKENED. A varargs float is
             * promoted to `double` (f64) by the C ABI, so reading it would
             * require an f64 va_arg load — which the translator rejects. We
             * therefore consume the 8-byte slot as two 32-bit words WITHOUT
             * forming an f64 value, and emit a zero placeholder honouring the
             * precision. DOOM almost never prints floats (uncapped framerate
             * is disabled), so this is acceptable; see _sink_float() below for
             * a real f32 formatter a cart can wire up once its float printing
             * goes through a float-typed wrapper instead of varargs double. */
            (void)va_arg(ap, unsigned);
            (void)va_arg(ap, unsigned);
            _sink_float(&s, 0.0f, width, prec, flags,
                        (conv == 'F' || conv == 'G' || conv == 'E'));
            break;
        }
        case 'n': {
            int *p = va_arg(ap, int *);
            if (p) *p = (int)s.total;
            break;
        }
        case '%':
            _sink_putc(&s, '%');
            break;
        default:
            /* unknown conversion: emit literally */
            _sink_putc(&s, '%');
            _sink_putc(&s, conv);
            break;
        }
    }

    if (s.buf && s.cap) {
        size_t term = s.len < s.cap ? s.len : s.cap - 1;
        s.buf[term] = '\0';
    }
    return (int)s.total;
}

/* ---- va_list cores (ALWAYS translatable) ------------------------------
 *
 * IMPORTANT TOOLCHAIN NOTE. The CronoVM translator does not implement the
 * `llvm.va_start` intrinsic (varargs are "not in the subset" — see
 * docs/translator.md). A C function that uses `va_start` (i.e. any `f(...)`
 * with `...`) therefore CANNOT be translated, and the LLVM `expand-variadics`
 * pass does not lower it for the i386-elf target either (that pass only
 * supports a few backends). So the variadic entry points (snprintf, printf,
 * sprintf, fprintf, sscanf) are guarded out by default — their mere
 * *definition* in the module would make the whole cart untranslatable, even
 * if never called.
 *
 * What IS always available and translates cleanly:
 *   - the va_list-taking cores below (vsnprintf/vsprintf/vprintf/vfprintf):
 *     on this target `va_list` is a plain `char*`, and `va_arg` lowers to
 *     pointer arithmetic + loads (no intrinsic), so these are fine; and
 *   - cvm_vsnprintf_buf(): a non-varargs helper that formats from a caller-
 *     built argument buffer. A DOOM-style port wraps its variadic call sites
 *     with these (or builds the buffer) so nothing in the cart needs varargs.
 *
 * The real variadic wrappers (printf/snprintf/sprintf/sscanf) are compiled
 * below: the CronoVM translator now lowers va_start (a variadic callee takes
 * all its args on the stack, so the va_list walks them), so C-ellipsis
 * functions translate and run. */

int vsnprintf(char *str, size_t size, const char *fmt, va_list ap) {
    return cvm_vfmt(str, size, fmt, ap);
}

int vsprintf(char *str, const char *fmt, va_list ap) {
    /* unbounded: use a very large cap. Callers must size their buffer. */
    return cvm_vfmt(str, (size_t)0x7fffffff, fmt, ap);
}

/* Format from a caller-provided argument buffer (the i386 vararg block: each
 * argument occupies a 4-byte slot, 8 bytes for a 64-bit/double value, in
 * declaration order, little-endian). This is the varargs-free path a cart
 * uses to print without C ellipsis. */
int cvm_vsnprintf_buf(char *str, size_t size, const char *fmt, const void *args) {
    va_list ap = (va_list)(void *)(uintptr_t)args;   /* va_list is char* here */
    return cvm_vfmt(str, size, fmt, ap);
}

/* Console output: format into a stack buffer and hand the bytes to cron_log.
 * Long output is truncated to the buffer (1 KiB). */
static __attribute__((noinline))
int _cvm_log_vfmt(const char *fmt, va_list ap) {
    char buf[1024];
    int n = cvm_vfmt(buf, sizeof buf, fmt, ap);
    int emit = n < (int)sizeof buf ? n : (int)sizeof buf - 1;
    cron_log(buf, emit);
    return n;
}

int vprintf(const char *fmt, va_list ap) {
    return _cvm_log_vfmt(fmt, ap);
}

int vfprintf(FILE *stream, const char *fmt, va_list ap) {
    if (!stream) return _cvm_log_vfmt(fmt, ap);   /* std stream -> console */
    /* RAM-FS file: format then write the bytes into the file. */
    char buf[1024];
    va_list ap2; va_copy(ap2, ap);
    int n = cvm_vfmt(buf, sizeof buf, fmt, ap);
    if (n < 0) { va_end(ap2); return n; }
    if ((size_t)n < sizeof buf) {
        fwrite(buf, 1, (size_t)n, stream);
    } else {                                       /* didn't fit: size exactly */
        char *big = (char *)malloc((size_t)n + 1);
        if (big) { cvm_vfmt(big, (size_t)n + 1, fmt, ap2); fwrite(big, 1, (size_t)n, stream); free(big); }
    }
    va_end(ap2);
    return n;
}

/* ---- variadic wrappers (printf family; va_start works on this target) -- */

int snprintf(char *str, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = cvm_vfmt(str, size, fmt, ap);
    va_end(ap);
    return r;
}

int sprintf(char *str, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = cvm_vfmt(str, (size_t)0x7fffffff, fmt, ap);
    va_end(ap);
    return r;
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = _cvm_log_vfmt(fmt, ap);
    va_end(ap);
    return r;
}

int fprintf(FILE *stream, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(stream, fmt, ap);
    va_end(ap);
    return r;
}

int puts(const char *s) {
    size_t n = strlen(s);
    cron_log(s, (int32_t)n);
    cron_log("\n", 1);
    return (int)n + 1;
}

int fputs(const char *s, FILE *stream) {
    size_t n = strlen(s);
    if (stream) fwrite(s, 1, n, stream);     /* RAM-FS file */
    else        cron_log(s, (int32_t)n);     /* std -> console */
    return (int)n;
}

int fputc(int c, FILE *stream) {
    char ch = (char)c;
    if (stream) fwrite(&ch, 1, 1, stream);   /* RAM-FS file */
    else        cron_log(&ch, 1);            /* std -> console */
    return (unsigned char)c;
}

int putchar(int c) {
    char ch = (char)c;
    cron_log(&ch, 1);
    return (unsigned char)c;
}

/* ---- minimal sscanf (DOOM uses it for config parsing) ------------------ */
/* Supports the conversions the kept sources need: %d %i %u %x %s %c and
 * whitespace skipping. No width except %s reads a token. Returns the number
 * of fields assigned. Float conversions are not supported (would need f64).
 *
 * Like the printf family, the variadic `sscanf` is guarded out (it needs
 * va_start). The va_list core `cvm_vsscanf` is always available and a cart
 * can call it with a manually built pointer-argument buffer. */
int cvm_vsscanf(const char *str, const char *fmt, va_list ap) {
    int assigned = 0;
    const char *s = str;

    while (*fmt) {
        if (isspace((unsigned char)*fmt)) {
            fmt++;
            while (isspace((unsigned char)*s)) s++;
            continue;
        }
        if (*fmt != '%') {
            if (*s != *fmt) break;
            s++; fmt++;
            continue;
        }
        fmt++;  /* past % */
        int suppress = 0;
        if (*fmt == '*') { suppress = 1; fmt++; }
        /* ignore width/length modifiers */
        while (isdigit((unsigned char)*fmt)) fmt++;
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z' || *fmt == 'L') fmt++;

        char conv = *fmt;
        if (conv == '\0') break;
        fmt++;

        if (conv == 'c') {
            if (*s == '\0') break;
            if (!suppress) { char *p = va_arg(ap, char *); *p = *s; assigned++; }
            s++;
            continue;
        }

        while (isspace((unsigned char)*s)) s++;
        if (*s == '\0') break;

        if (conv == 's') {
            char *out = suppress ? NULL : va_arg(ap, char *);
            int wrote = 0;
            while (*s && !isspace((unsigned char)*s)) {
                if (out) *out++ = *s;
                s++; wrote++;
            }
            if (out) *out = '\0';
            if (!suppress && wrote) assigned++;
            else if (!wrote) break;
            continue;
        }

        if (conv == 'd' || conv == 'i' || conv == 'u' ||
            conv == 'x' || conv == 'X' || conv == 'o') {
            char *end;
            int base = (conv == 'x' || conv == 'X') ? 16 :
                       (conv == 'o') ? 8 :
                       (conv == 'i') ? 0 : 10;
            const char *before = s;
            if (conv == 'u') {
                unsigned long v = strtoul(s, &end, base);
                if (end == before) break;
                if (!suppress) { unsigned *p = va_arg(ap, unsigned *); *p = (unsigned)v; assigned++; }
            } else {
                long v = strtol(s, &end, base);
                if (end == before) break;
                if (!suppress) { int *p = va_arg(ap, int *); *p = (int)v; assigned++; }
            }
            s = end;
            continue;
        }

        if (conv == '%') {
            if (*s != '%') break;
            s++;
            continue;
        }
        /* unsupported conversion: stop */
        break;
    }

    return assigned;
}

int sscanf(const char *str, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = cvm_vsscanf(str, fmt, ap);
    va_end(ap);
    return r;
}

/* ---- RAM filesystem ("memory card") ----------------------------------- */
/* The console has no host filesystem, but a cart gets a small PERSISTED blob
 * (the save region — see <cronopio.h> cron_save_*). We expose it to ports as a
 * tiny in-RAM filesystem: every fopen()'d file lives in a heap buffer here,
 * loaded from the save blob on first use and serialised back on close. This is
 * how a ported engine's file-based saves (DOOM's savegames) persist with no
 * engine changes. Bounded by cron_save_size(); NOT a general filesystem.
 * Bundled read-only assets still come from the cartridge ROM (cron_rom).
 *
 * std streams (stdin/stdout/stderr) are NULL; the printf family + a NULL stream
 * route to cron_log. A non-NULL FILE* is always a RAM-FS handle. */

#define RAMFS_MAX    24
#define RAMFS_NAME   96
#define RAMFS_MAGIC  0x31534643u   /* 'C','F','S','1' little-endian */

typedef struct { int used; char name[RAMFS_NAME]; uint8_t *data; uint32_t len, cap; } ramfile_t;
static ramfile_t        g_fs[RAMFS_MAX];
static struct _CVM_FILE g_fh[RAMFS_MAX];
static int              g_fh_used[RAMFS_MAX];
static int              g_fs_loaded = 0;

static uint32_t cvm_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static void cvm_wr32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
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
        if (g_fs[i].used && strcmp(g_fs[i].name, name) == 0) return i;
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

/* ---- stdio over the RAM filesystem ------------------------------------- */

FILE *fopen(const char *path, const char *mode) {
    if (!path || !mode) { errno = ENOENT; return NULL; }
    ramfs_load();
    int write = (mode[0] == 'w' || mode[0] == 'a');
    int slot  = ramfs_find(path);
    if (!write && slot < 0) { errno = ENOENT; return NULL; }
    if (write) {
        if (slot < 0) slot = ramfs_create(path);
        if (slot < 0) { errno = ENOENT; return NULL; }
        if (mode[0] == 'w') g_fs[slot].len = 0;   /* truncate */
    }
    int h = -1;
    for (int i = 0; i < RAMFS_MAX; ++i) if (!g_fh_used[i]) { h = i; break; }
    if (h < 0) { errno = ENOENT; return NULL; }
    g_fh_used[h] = 1;
    g_fh[h].slot    = slot;
    g_fh[h].pos     = (mode[0] == 'a') ? g_fs[slot].len : 0;
    g_fh[h].writing = write;
    g_fh[h].eofbit  = 0;
    return &g_fh[h];
}

int fclose(FILE *stream) {
    if (!stream) return 0;            /* std stream */
    if (stream->writing) ramfs_flush();
    int h = (int)(stream - g_fh);
    if (h >= 0 && h < RAMFS_MAX) g_fh_used[h] = 0;
    return 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!stream || !ptr || size == 0 || nmemb == 0) return 0;
    ramfile_t *f = &g_fs[stream->slot];
    size_t want = size * nmemb;
    uint32_t avail = (stream->pos < f->len) ? (f->len - stream->pos) : 0;
    size_t got = want < avail ? want : avail;
    if (got) { memcpy(ptr, f->data + stream->pos, got); stream->pos += (uint32_t)got; }
    if (got < want) stream->eofbit = 1;
    return got / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (size == 0 || nmemb == 0) return 0;
    size_t total = size * nmemb;
    if (!stream) { cron_log((const char *)ptr, (int32_t)total); return nmemb; }  /* std */
    ramfile_t *f = &g_fs[stream->slot];
    if (ramfile_grow(f, stream->pos + (uint32_t)total) != 0) return 0;
    memcpy(f->data + stream->pos, ptr, total);
    stream->pos += (uint32_t)total;
    if (stream->pos > f->len) f->len = stream->pos;
    return nmemb;
}

int fseek(FILE *stream, long offset, int whence) {
    if (!stream) return -1;
    ramfile_t *f = &g_fs[stream->slot];
    long base = (whence == SEEK_CUR) ? (long)stream->pos
              : (whence == SEEK_END) ? (long)f->len : 0;
    long np = base + offset;
    if (np < 0) return -1;
    stream->pos = (uint32_t)np;
    stream->eofbit = 0;
    return 0;
}

long ftell(FILE *stream) { return stream ? (long)stream->pos : -1; }

int fflush(FILE *stream) {
    if (stream && stream->writing) ramfs_flush();
    return 0;
}

char *fgets(char *s, int size, FILE *stream) {
    if (!stream || !s || size <= 0) return NULL;
    ramfile_t *f = &g_fs[stream->slot];
    int i = 0;
    while (i < size - 1 && stream->pos < f->len) {
        char c = (char)f->data[stream->pos++];
        s[i++] = c;
        if (c == '\n') break;
    }
    if (i == 0) { stream->eofbit = 1; return NULL; }
    s[i] = '\0';
    return s;
}

int fgetc(FILE *stream) {
    if (!stream) return EOF;
    ramfile_t *f = &g_fs[stream->slot];
    if (stream->pos >= f->len) { stream->eofbit = 1; return EOF; }
    return (int)f->data[stream->pos++];
}

int ungetc(int c, FILE *stream) {
    if (!stream || c == EOF || stream->pos == 0) return EOF;
    stream->pos--;
    stream->eofbit = 0;
    return c;
}

/* ---- POSIX fd-level I/O (open/read/write/close/lseek) ---------------------
 * A thin fd table over fopen(): fds 0/1/2 are the standard streams (writes go
 * to cron_log), fd N>=CVM_FD_BASE maps to a RAM-FS FILE*. Surfaced by ports
 * that use low-level fd I/O (e.g. Quake's console log via open/write). */
#define CVM_FD_BASE 3
#define CVM_FD_MAX  16
static FILE *g_fd[CVM_FD_MAX];

static FILE *fd_file(int fd) {
    int i = fd - CVM_FD_BASE;
    if (i < 0 || i >= CVM_FD_MAX) return NULL;
    return g_fd[i];
}

int open(const char *path, int flags, ...) {
    const char *mode;
    if (flags & O_WRONLY)    mode = (flags & O_APPEND) ? "ab" : "wb";
    else if (flags & O_RDWR) mode = (flags & O_APPEND) ? "a+b"
                                  : ((flags & O_TRUNC) ? "w+b" : "r+b");
    else                     mode = "rb";
    FILE *fp = fopen(path, mode);
    if (!fp) return -1;
    for (int i = 0; i < CVM_FD_MAX; i++) {
        if (!g_fd[i]) { g_fd[i] = fp; return CVM_FD_BASE + i; }
    }
    fclose(fp);
    return -1;
}

ssize_t write(int fd, const void *buf, size_t count) {
    if (fd == 1 || fd == 2) { cron_log((const char *)buf, (int32_t)count); return (ssize_t)count; }
    FILE *fp = fd_file(fd);
    if (!fp) return -1;
    return (ssize_t)fwrite(buf, 1, count, fp);
}

ssize_t read(int fd, void *buf, size_t count) {
    FILE *fp = fd_file(fd);
    if (!fp) return -1;
    return (ssize_t)fread(buf, 1, count, fp);
}

int close(int fd) {
    int i = fd - CVM_FD_BASE;
    if (i < 0 || i >= CVM_FD_MAX || !g_fd[i]) return -1;
    fclose(g_fd[i]);
    g_fd[i] = NULL;
    return 0;
}

off_t lseek(int fd, off_t offset, int whence) {
    FILE *fp = fd_file(fd);
    if (!fp) return -1;
    if (fseek(fp, (long)offset, whence) != 0) return -1;
    return (off_t)ftell(fp);
}

/* fscanf over a RAM-FS file. Supports the directives ports actually use for
 * text config: whitespace (skips a run), literal chars, %% , and
 * %[*][width](s|d|i|u|x|[set]) — enough for crispy's "%79s %99[^\n]" config
 * lines. Reads via fgetc/ungetc; no console input (NULL stream -> EOF). */
/* Whitespace test. Written as an explicit range so clang does NOT fold the
 * c==' '||c=='\t'||... chain into a 24-bit bitmask test (the ws codes span
 * 9..32 = 24 values), which the translator rejects (no i24). 9..13 = \t\n\v\f\r,
 * 32 = space. */
static int sf_isws(int c) { return (c >= 9 && c <= 13) || c == 32; }

int fscanf(FILE *stream, const char *fmt, ...) {
    if (!stream) return -1;
    va_list ap; va_start(ap, fmt);
    int assigned = 0, saw_input = 0, c;
    const char *f = fmt;
    while (*f) {
        if (sf_isws(*f)) {                       /* whitespace: skip input ws */
            do { c = fgetc(stream); } while (sf_isws(c));
            if (c != EOF) ungetc(c, stream);
            f++;
            continue;
        }
        if (*f != '%') {                         /* literal: must match */
            c = fgetc(stream);
            if (c != (unsigned char)*f) { if (c != EOF) ungetc(c, stream); goto done; }
            saw_input = 1; f++;
            continue;
        }
        /* conversion */
        f++;
        int suppress = 0; if (*f == '*') { suppress = 1; f++; }
        int width = 0, have_w = 0;
        while (*f >= '0' && *f <= '9') { width = width*10 + (*f - '0'); have_w = 1; f++; }
        char conv = *f ? *f++ : 0;

        if (conv == '%') {
            c = fgetc(stream);
            if (c != '%') { if (c != EOF) ungetc(c, stream); goto done; }
            saw_input = 1; continue;
        }
        if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x') {
            do { c = fgetc(stream); } while (sf_isws(c));
            int base = (conv == 'x') ? 16 : 10, neg = 0, got = 0;
            long val = 0;
            if ((c == '-' || c == '+') && conv != 'u' && conv != 'x') { neg = (c=='-'); c = fgetc(stream); }
            int n = 0;
            while (c != EOF && (!have_w || n < width)) {
                int d;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (base == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (base == 16 && c >= 'A' && c <= 'F') d = c - 'A' + 10;
                else break;
                val = val*base + d; got = 1; saw_input = 1; n++;
                c = fgetc(stream);
            }
            if (c != EOF) ungetc(c, stream);
            if (!got) goto done;
            if (neg) val = -val;
            if (!suppress) { *va_arg(ap, int*) = (int)val; assigned++; }
            continue;
        }
        if (conv == 's' || conv == '[') {
            int negate = 0;
            const char *set0 = 0, *set1 = 0;
            if (conv == '[') {
                if (*f == '^') { negate = 1; f++; }
                set0 = f;
                while (*f && *f != ']') f++;
                set1 = f;
                if (*f == ']') f++;
            } else {
                do { c = fgetc(stream); } while (sf_isws(c));   /* %s skips leading ws */
                goto have_first;          /* c already holds the first char */
            }
            c = fgetc(stream);
        have_first:;
            char *out = suppress ? 0 : va_arg(ap, char*);
            int n = 0;
            for (;;) {
                if (c == EOF) break;
                int match;
                if (conv == 's') match = !sf_isws(c);
                else { int in = 0; for (const char *s = set0; s < set1; ++s) if ((unsigned char)*s == c) { in = 1; break; } match = negate ? !in : in; }
                if (!match || (have_w && n >= width)) break;
                if (out) out[n] = (char)c;
                n++; saw_input = 1;
                c = fgetc(stream);
            }
            if (c != EOF) ungetc(c, stream);
            if (out) out[n] = '\0';
            if (conv == 's' && n == 0) goto done;   /* %s requires >=1 char */
            if (!suppress) assigned++;
            continue;
        }
        goto done;   /* unsupported conversion */
    }
done:
    va_end(ap);
    if (assigned == 0 && !saw_input) return -1;   /* EOF / nothing matched */
    return assigned;
}

/* ---- <locale.h>: fixed "C" locale ------------------------------------- */
#include <locale.h>
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

int feof(FILE *stream) { return stream ? stream->eofbit : 1; }

int ferror(FILE *stream) { (void)stream; return 0; }

void clearerr(FILE *stream) { if (stream) stream->eofbit = 0; }

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

void perror(const char *s) {
    if (s && *s) {
        cron_log(s, (int32_t)strlen(s));
        cron_log(": ", 2);
    }
    static const char e[] = "error\n";
    cron_log(e, (int32_t)sizeof(e) - 1);
}

/* ============================== sys/stat.h ============================= */
/* No filesystem: stat reports failure, mkdir does nothing. */

int stat(const char *path, struct stat *buf) {
    (void)path; (void)buf;
    errno = ENOENT;
    return -1;
}

int mkdir(const char *path, unsigned int mode) {
    (void)path; (void)mode;
    errno = ENOENT;
    return -1;
}

/* ============================== unistd.h =============================== */

int access(const char *path, int mode) {
    (void)mode; ramfs_load();
    if (path && ramfs_find(path) >= 0) return 0;
    errno = ENOENT; return -1;
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

size_t strftime(char *s, size_t max, const char *format, const struct tm *tm) {
    (void)format; (void)tm;
    if (max == 0) return 0;
    s[0] = '\0';   /* empty string — DOOM tolerates an empty timestamp */
    return 0;
}

char *asctime(const struct tm *tm) {
    (void)tm;
    static char buf[] = "Thu Jan  1 00:00:00 1970\n";
    return buf;
}

char *ctime(const time_t *timep) {
    (void)timep;
    return asctime(&_cvm_tm);
}
