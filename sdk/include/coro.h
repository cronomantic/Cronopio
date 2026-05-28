/*
 *  coro.h — Cooperative coroutines for the Cronopio cart.
 *
 *  Single new VM primitive (opcode CVM_OP_CORO_SWAP, 0x3C) plus a cart-side
 *  init + scheduler library. Lets the cart implement any cooperative model
 *  it needs (a libco-style scheduler, Lua-style yield-with-value generators,
 *  async/await desugaring …) on top of three small functions:
 *
 *    cron_coro_init  — set up a fresh coroutine context on a user-owned stack
 *    cron_coro_swap  — save current context and resume another one
 *    cron_coro_yield — convenience: swap back to whoever last resumed us
 *
 *  Stack layout: a coroutine runs on whatever buffer the cart provides via
 *  coro->stack_lo / coro->stack_sz. Stacks grow downward (CronoVM ABI), so
 *  the initial SP is `stack_lo + stack_sz - 4`, with a deliberate "trap PC"
 *  pre-pushed at that slot so a runaway RET (from a fn that returns without
 *  swapping out and without a resumer) faults via CVM_E_BAD_PC instead of
 *  silently halting the cart via CVM_RET_SENTINEL.
 *
 *  The opcode reads + writes the first 16 bytes of the cron_coro_t struct
 *  (the {pc, sp, dest, status} record). Everything past offset 16 is
 *  cart-managed.
 */

#ifndef _CVM_CORO_H
#define _CVM_CORO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Coroutine status — first word past the saved-context triple. The opcode
 * reads this on the target to decide whether to do a FUNCS lookup
 * (FRESH → first-run trampoline launch) and writes it on the source/target. */
enum {
    CORO_FRESH     = 0,
    CORO_RUNNING   = 1,
    CORO_SUSPENDED = 2,
    CORO_DEAD      = 3
};

/* The full struct.
 *
 *   offset 0..15  — VM-managed context (do not write directly).
 *   offset 16..   — cart-managed; fn/arg/stack_lo/stack_sz are inputs to
 *                   cron_coro_init, resumer is set by cron_coro_swap. */
typedef struct cron_coro {
    /* VM-managed. */
    uint32_t _pc;
    uint32_t _sp;
    uint32_t _dest;
    uint32_t status;

    /* Cart-managed (caller fills before cron_coro_init). */
    void   (*fn)(void *arg);
    void    *arg;
    void    *stack_lo;
    uint32_t stack_sz;

    /* Cart-managed (set by cron_coro_swap to the most recent resumer). */
    struct cron_coro *resumer;
} cron_coro_t;

/* Raw opcode wrapper — lowers to CVM_OP_CORO_SWAP. Don't call this directly
 * unless you really mean it; cron_coro_swap() below wraps it to also set
 * to->resumer = from. Recognised by name in the translator.
 *
 * `returns_twice` is the same compiler hint setjmp uses: it tells clang that
 * control may return via a different path than the obvious one (here: the
 * resumer's swap-back rather than a normal return), so values live across
 * the call must be spilled to memory. Without this attribute clang reuses
 * caller-saved regs through the swap, and CORO_SWAP's "R[to.dest] = from"
 * clobber on resume corrupts them. */
extern void __cvm_coro_swap_raw(cron_coro_t *from, cron_coro_t *to)
    __attribute__((__returns_twice__));

/* Default trampoline — invoked on the new stack the first time a coroutine
 * runs. Sets status, calls coro->fn(coro->arg), then on return marks
 * status=DEAD and swaps back to coro->resumer. If there is no resumer,
 * deliberately traps. The cart may supply its own trampoline by overriding
 * cron_coro_t.fn with a value other than what its scheduler expects — but
 * for the common case, cron_coro_init below points at this one. */
void __cron_coro_trampoline(cron_coro_t *self);

/* ---------------------------------------------------------------------- */
/* Public API */

/* Prepare `coro` for first execution.
 *
 * REQUIRES coro->fn, coro->arg (may be NULL), coro->stack_lo, coro->stack_sz
 * all set by the caller. Recommend stack_sz >= 8 KB; UQM-style hosts use
 * 64 KB. Stack alignment: 4-byte minimum (CronoVM word size).
 *
 * Writes the VM-managed context such that the first
 * `cron_coro_swap(*, coro)` lands in __cron_coro_trampoline(coro), which
 * runs coro->fn(coro->arg) on the new stack. Sets coro->status = CORO_FRESH.
 *
 * Idempotent if status was already FRESH. Calling on a RUNNING/SUSPENDED
 * coroutine corrupts its saved context — don't. */
static inline void cron_coro_init(cron_coro_t *coro) {
    uint8_t *top = (uint8_t *)coro->stack_lo + coro->stack_sz - 4;
    /* Trap sentinel: any out-of-range PC will fault on DISPATCH() */
    *(uint32_t *)top = 0xFFFFFFFEu;
    coro->_pc      = (uint32_t)(uintptr_t)&__cron_coro_trampoline;
    coro->_sp      = (uint32_t)(uintptr_t)top;
    coro->_dest    = 0;  /* arg0 reg per CronoVM ABI */
    coro->status   = CORO_FRESH;
    coro->resumer  = (cron_coro_t *)0;
}

/* Cooperative swap.
 *
 * Saves the calling context into `from` (status = CORO_SUSPENDED) and
 * resumes `to`. Sets to->resumer = from before the actual swap so the
 * trampoline (or any cart code that walks the chain) can find its way back.
 *
 * Returns when somebody swaps back to `from`. `from` may be a freshly
 * zero-initialised cron_coro_t — its first word becomes the saved PC, and
 * its stack ptr/dest/status are populated on first save (no init needed
 * for the "main" coroutine that's the cart's pre-existing call stack). */
static inline void cron_coro_swap(cron_coro_t *from, cron_coro_t *to) {
    to->resumer = from;
    __cvm_coro_swap_raw(from, to);
}

/* Convenience: yield back to our resumer. */
static inline void cron_coro_yield(cron_coro_t *self) {
    if (self->resumer) cron_coro_swap(self, self->resumer);
}

#ifdef __cplusplus
}
#endif

#endif /* _CVM_CORO_H */
