/* <cron_arena.h> — Cronopio SDK game-oriented arena allocator (header-only).
 *
 * A bump allocator over a caller-provided block. The intended pattern for a
 * game cart:
 *
 *     static cron_arena_t arena;
 *     void *block = malloc(8 * 1024 * 1024);     // one big reservation
 *     cron_arena_init(&arena, block, 8*1024*1024);
 *
 *     // per level: bump-allocate everything, then drop it all at once
 *     size_t lvl = cron_arena_mark(&arena);
 *     thing_t *things = cron_arena_alloc(&arena, n * sizeof *things);
 *     ... use the level ...
 *     cron_arena_reset_to(&arena, lvl);          // frees the level in O(1)
 *
 * Allocations are 8-byte aligned and never individually freed — you reset to
 * a saved mark (or all the way back) to reclaim. This is far cheaper than a
 * general free-list when a game's lifetimes nest by frame or level.
 *
 * Note: a DOOM-style port does NOT use this directly — it hands one big block
 * to its own zone allocator (Z_Malloc). The arena is for new carts that want
 * a simple, fast, game-shaped allocator.
 *
 * Constraints: header-only static inline, no i64/double, no syscalls. */
#ifndef CRON_ARENA_H
#define CRON_ARENA_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char  *base;   /* start of the wrapped block */
    size_t cap;    /* total bytes in the block    */
    size_t used;   /* bytes handed out so far     */
} cron_arena_t;

/* Wrap a caller-provided block (e.g. one big cvm_malloc / malloc). */
static inline void cron_arena_init(cron_arena_t *a, void *buf, size_t cap) {
    a->base = (char *)buf;
    a->cap  = cap;
    a->used = 0;
}

/* Bump-allocate `n` bytes, 8-byte aligned. Returns NULL when the block is
 * full (the caller should size its reservation so this does not happen). */
static inline void *cron_arena_alloc(cron_arena_t *a, size_t n) {
    /* align the current offset up to 8 bytes */
    size_t off = (a->used + 7u) & ~(size_t)7u;
    /* overflow / capacity check (32-bit, no i64) */
    if (off > a->cap || n > a->cap - off) return (void *)0;
    void *p = a->base + off;
    a->used = off + n;
    return p;
}

/* Record the current high-water mark for a later reset_to. */
static inline size_t cron_arena_mark(const cron_arena_t *a) {
    return a->used;
}

/* Roll the arena back to a previously recorded mark, freeing everything
 * allocated since in O(1). */
static inline void cron_arena_reset_to(cron_arena_t *a, size_t mark) {
    if (mark <= a->cap) a->used = mark;
}

/* Drop every allocation. */
static inline void cron_arena_reset(cron_arena_t *a) {
    a->used = 0;
}

#endif /* CRON_ARENA_H */
