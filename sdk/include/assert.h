/* <assert.h> — Cronopio SDK freestanding libc. See ctype.h for the rationale.
 *
 * On a failed assertion we log "file:line: expr" via cron_log and terminate
 * the cart with cron_exit(1). NDEBUG disables the check (the standard allows
 * assert.h to be re-included with a different NDEBUG, so there is no include
 * guard around the macro definition). */

#include <cronopio.h>

#undef assert

#ifdef NDEBUG

#define assert(expr) ((void)0)

#else

/* Helper lives in cron_sys.c so the per-call-site code stays tiny. */
void _cvm_assert_fail(const char *expr, const char *file, int line);

#define assert(expr) \
    ((expr) ? (void)0 : _cvm_assert_fail(#expr, __FILE__, __LINE__))

#endif /* NDEBUG */
