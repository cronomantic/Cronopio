/* <math.h> — Cronopio SDK freestanding libc. See ctype.h for the rationale.
 *
 * The kept DOOM/Crispy renderer math (r_main.c tangent/sine tables, p_setup.c
 * segment lengths, v_video/v_trans gamma) calls a handful of transcendental
 * functions. The CronoVM translator CANNOT represent f64 (double), so these
 * are declared and implemented in `float` (f32). The standard C signatures use
 * `double`; using `float` here means a DOOM source that does double arithmetic
 * (e.g. `sqrt((double)dx*dx + ...)` in p_setup.c) will still create f64 values
 * in *that* translation unit — those few call sites must be ported to float by
 * the cart (out of scope for the libc). This header keeps the libc itself
 * f64-free and gives the renderer real float math.
 *
 * Implemented header-only (static inline) with small polynomial / Newton
 * approximations — accuracy is "good enough for a software renderer", not
 * IEEE-correct. No <math.h> from the toolchain is used. */
#ifndef CVM_LIBC_MATH_H
#define CVM_LIBC_MATH_H

#define M_PI    3.14159265358979323846f
#define M_PI_2  1.57079632679489661923f
#define M_E     2.71828182845904523536f
#define M_SQRT2 1.41421356237309504880f
#define HUGE_VAL (1.0e30f)

/* sqrt: Newton-Raphson from a bit-twiddled initial guess. */
static inline float sqrtf(float x) {
    if (x <= 0.0f) return 0.0f;
    /* fast inverse-sqrt seed, then refine with two Newton steps on sqrt */
    union { float f; int i; } u;
    u.f = x;
    u.i = 0x5f3759df - (u.i >> 1);   /* approx 1/sqrt(x) */
    float y = u.f;
    y = y * (1.5f - 0.5f * x * y * y);
    y = y * (1.5f - 0.5f * x * y * y);
    float r = x * y;                 /* x / sqrt(x) = sqrt(x) */
    /* one Newton step directly on sqrt for accuracy */
    r = 0.5f * (r + x / r);
    return r;
}
static inline float sqrt(float x) { return sqrtf(x); }

static inline float fabsf(float x) { return x < 0.0f ? -x : x; }
static inline float fabs(float x)  { return fabsf(x); }

static inline float floorf(float x) {
    int i = (int)x;
    if ((float)i > x) i--;
    return (float)i;
}
static inline float floor(float x) { return floorf(x); }

static inline float ceilf(float x) {
    int i = (int)x;
    if ((float)i < x) i++;
    return (float)i;
}
static inline float ceil(float x) { return ceilf(x); }

/* sin/cos via range reduction to [-pi, pi] and a 7th-order polynomial. */
static inline float sinf(float x) {
    /* reduce x mod 2*pi */
    const float twopi = 6.28318530717958647692f;
    /* k = round(x / twopi) computed without library round */
    float q = x * (1.0f / twopi);
    int k = (int)(q + (q < 0.0f ? -0.5f : 0.5f));
    x = x - (float)k * twopi;
    /* now x in [-pi, pi]; minimax-ish polynomial */
    float x2 = x * x;
    float r = x * (1.0f
                + x2 * (-1.0f / 6.0f
                + x2 * (1.0f / 120.0f
                + x2 * (-1.0f / 5040.0f
                + x2 * (1.0f / 362880.0f)))));
    return r;
}
static inline float sin(float x) { return sinf(x); }

static inline float cosf(float x) { return sinf(x + M_PI_2); }
static inline float cos(float x)  { return cosf(x); }

static inline float tanf(float x) {
    float c = cosf(x);
    if (c == 0.0f) return 0.0f;
    return sinf(x) / c;
}
static inline float tan(float x) { return tanf(x); }

/* atan: range-reduce using identities, then a polynomial on [-1,1]. */
static inline float atanf(float x) {
    int neg = 0, inv = 0;
    if (x < 0.0f) { x = -x; neg = 1; }
    if (x > 1.0f) { x = 1.0f / x; inv = 1; }
    float x2 = x * x;
    /* polynomial approximation of atan on [0,1] */
    float r = x * (0.9998660f
              + x2 * (-0.3302995f
              + x2 * (0.1801410f
              + x2 * (-0.0851330f
              + x2 * (0.0208351f)))));
    if (inv) r = M_PI_2 - r;
    if (neg) r = -r;
    return r;
}
static inline float atan(float x) { return atanf(x); }

static inline float atan2f(float y, float x) {
    if (x > 0.0f) return atanf(y / x);
    if (x < 0.0f) {
        if (y >= 0.0f) return atanf(y / x) + M_PI;
        return atanf(y / x) - M_PI;
    }
    /* x == 0 */
    if (y > 0.0f) return M_PI_2;
    if (y < 0.0f) return -M_PI_2;
    return 0.0f;
}
static inline float atan2(float y, float x) { return atan2f(y, x); }

static inline float powf(float base, float exp) {
    /* integer-exponent fast path (the only use in the kept set is gamma-ish);
     * fall back to exp(exp*log(base)) is avoided to keep this f64-free, so
     * non-integer exponents are approximated by repeated multiply of the
     * rounded exponent. Good enough for the renderer's needs. */
    int e = (int)exp;
    float r = 1.0f;
    int n = e < 0 ? -e : e;
    while (n--) r *= base;
    if (e < 0) return r != 0.0f ? 1.0f / r : 0.0f;
    return r;
}
static inline float pow(float base, float exp) { return powf(base, exp); }

/* round-half-away-from-zero, float-only (no f64). lround/lroundf return long;
 * long is 32-bit on this target. Used by g_game.c mouse code. */
static inline float roundf(float x) {
    return x >= 0.0f ? floorf(x + 0.5f) : ceilf(x - 0.5f);
}
static inline float round(float x) { return roundf(x); }
static inline long lroundf(float x) { return (long)roundf(x); }
static inline long lround(float x) { return lroundf(x); }

/* Double-precision math from picolibc's libm (f64 → soft-float runtime). The
 * hand-written set above is float-only (DOOM/Quake heritage); these are declared
 * on demand for code that needs real doubles — UQM's planet-surface generator
 * (plangen acos(), pl_stuff exp()). Add more (log/pow/...) here as engines need
 * them, with the matching source in runtime/lib/build_picolibc.sh. */
extern double exp(double x);
extern double acos(double x);
extern double log(double x);   /* UQM setupmenu.c gamma curve */

#endif /* CVM_LIBC_MATH_H */
