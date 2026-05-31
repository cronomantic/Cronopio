/* cron_module.c — host-side tracker-module music via libxmp (see cron_module.h).
 *
 * Threading mirrors cron_ogg.c: the cart thread fully opens a module (libxmp
 * context + loaded module + started player) and publishes it through an atomic
 * `pending` slot; the audio thread adopts it at the start of a render block and
 * owns it thereafter (it is the only thread that frees a published track).
 * Stop/volume are atomics. libxmp renders directly at the output rate, so —
 * unlike the Ogg path — there is no resampler here. */

#include "cron_module.h"
#include "console.h"     /* CRONOPIO_AUDIO_HZ */

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "xmp.h"

typedef struct mtrack {
    xmp_context ctx;     /* created, module loaded, player started */
    int         loop;    /* repeat forever when set */
} mtrack_t;

typedef struct {
    /* cart-thread -> audio-thread handoff */
    _Atomic(mtrack_t*) pending;
    atomic_int         stop_req;
    atomic_int         vol_q8;    /* 0..256 */

    /* audio-thread-only state */
    mtrack_t* cur;
    int       ended;
} module_t;

static void mtrack_free(mtrack_t* tk) {
    if (!tk) return;
    if (tk->ctx) {
        xmp_end_player(tk->ctx);
        xmp_release_module(tk->ctx);
        xmp_free_context(tk->ctx);
    }
    free(tk);
}

void* cron_module_create(void) {
    module_t* m = (module_t*)calloc(1, sizeof(*m));
    if (m) {
        atomic_store(&m->pending, (mtrack_t*)NULL);
        atomic_store(&m->stop_req, 0);
        atomic_store(&m->vol_q8, 256);
    }
    return m;
}

void cron_module_destroy(void* mm) {
    module_t* m = (module_t*)mm;
    if (!m) return;
    mtrack_free(atomic_exchange(&m->pending, (mtrack_t*)NULL));
    mtrack_free(m->cur);
    free(m);
}

void cron_module_play(void* mm, const uint8_t* mod, int len, int loop) {
    module_t* m = (module_t*)mm;
    if (!m || !mod || len <= 0) return;

    xmp_context ctx = xmp_create_context();
    if (!ctx) return;
    if (xmp_load_module_from_memory(ctx, mod, (long)len) != 0) {
        xmp_free_context(ctx);
        return;
    }
    if (xmp_start_player(ctx, CRONOPIO_AUDIO_HZ, 0) != 0) {  /* format 0 = S16 stereo */
        xmp_release_module(ctx);
        xmp_free_context(ctx);
        return;
    }
    /* Quality: cubic-spline interpolation + the lowpass DSP (libxmp's effects). */
    xmp_set_player(ctx, XMP_PLAYER_INTERP, XMP_INTERP_SPLINE);
    xmp_set_player(ctx, XMP_PLAYER_DSP, XMP_DSP_ALL);

    mtrack_t* tk = (mtrack_t*)calloc(1, sizeof(*tk));
    if (!tk) {
        xmp_end_player(ctx); xmp_release_module(ctx); xmp_free_context(ctx);
        return;
    }
    tk->ctx = ctx;
    tk->loop = loop;

    /* Publish; free any prior pending we displace (audio thread never saw it). */
    mtrack_free(atomic_exchange(&m->pending, tk));
}

void cron_module_stop(void* mm) {
    module_t* m = (module_t*)mm;
    if (m) atomic_store(&m->stop_req, 1);
}

void cron_module_set_volume(void* mm, int vol_q8) {
    module_t* m = (module_t*)mm;
    if (!m) return;
    if (vol_q8 < 0)   vol_q8 = 0;
    if (vol_q8 > 256) vol_q8 = 256;
    atomic_store(&m->vol_q8, vol_q8);
}

static void adopt(module_t* m, mtrack_t* tk) {
    mtrack_free(m->cur);
    m->cur   = tk;
    m->ended = 0;
}

void cron_module_render(void* mm, int16_t* dst, int frames) {
    module_t* m = (module_t*)mm;
    if (!m) { memset(dst, 0, (size_t)frames * 4); return; }

    mtrack_t* p = atomic_exchange(&m->pending, (mtrack_t*)NULL);
    if (p) adopt(m, p);

    if (atomic_exchange(&m->stop_req, 0)) {
        mtrack_free(m->cur);
        m->cur = NULL;
    }

    int vol = atomic_load(&m->vol_q8);
    if (!m->cur || m->ended || vol <= 0) {
        memset(dst, 0, (size_t)frames * 4);
        return;
    }

    /* loop==0 (cart wants repeat) -> 0 disables libxmp's loop counter (plays
     * forever); loop set -> pass 1 so it stops after one play-through, filling
     * silence and returning -XMP_END. dst is S16 stereo => frames*4 bytes. */
    int rc = xmp_play_buffer(m->cur->ctx, dst, frames * 4, m->cur->loop ? 0 : 1);
    if (rc != 0) m->ended = 1;   /* -XMP_END / -XMP_ERROR_STATE: module finished */

    if (vol < 256) {
        for (int i = 0; i < frames * 2; ++i)
            dst[i] = (int16_t)(((int)dst[i] * vol) >> 8);
    }
}
