/* cron_music.c — host-side streaming OGG music (see cron_music.h).
 *
 * Threading: the cart thread prepares a fully-opened track and publishes it
 * through an atomic `pending` slot; the audio thread adopts it at the start of
 * a render block and owns it thereafter (it is the only thread that frees a
 * published track). Stop/volume are atomics. This keeps the decoder, which is
 * not thread-safe, touched by exactly one thread at a time. */

#include "cron_music.h"
#include "console.h"     /* CRONOPIO_AUDIO_HZ */

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "vendor/stb_vorbis.c"

#define SRCBUF_FRAMES 1024   /* source frames decoded per stb_vorbis pull */

typedef struct track {
    stb_vorbis* v;
    uint8_t*    data;    /* owned copy of the ogg bytes */
    int         loop;
    int         rate;    /* source sample rate */
} track_t;

typedef struct {
    /* cart-thread -> audio-thread handoff */
    _Atomic(track_t*) pending;
    atomic_int        stop_req;
    atomic_int        vol_q8;     /* 0..256 */

    /* audio-thread-only state */
    track_t* cur;
    int      ended;
    /* linear resampler: output frame = lerp(s0, s1, t), step = rate/out_rate */
    int      s0[2], s1[2];
    double   t;
    double   step;
    /* decoded-source buffer */
    int16_t  buf[SRCBUF_FRAMES * 2];
    int      buf_n, buf_i;
} music_t;

static void track_free(track_t* tk) {
    if (!tk) return;
    if (tk->v)    stb_vorbis_close(tk->v);
    if (tk->data) free(tk->data);
    free(tk);
}

void* cron_music_create(void) {
    music_t* m = (music_t*)calloc(1, sizeof(*m));
    if (m) {
        atomic_store(&m->pending, (track_t*)NULL);
        atomic_store(&m->stop_req, 0);
        atomic_store(&m->vol_q8, 256);
    }
    return m;
}

void cron_music_destroy(void* mm) {
    music_t* m = (music_t*)mm;
    if (!m) return;
    track_free(atomic_exchange(&m->pending, (track_t*)NULL));
    track_free(m->cur);
    free(m);
}

void cron_music_play(void* mm, const uint8_t* ogg, int len, int loop) {
    music_t* m = (music_t*)mm;
    if (!m || !ogg || len <= 0) return;

    track_t* tk = (track_t*)calloc(1, sizeof(*tk));
    if (!tk) return;
    tk->data = (uint8_t*)malloc((size_t)len);
    if (!tk->data) { free(tk); return; }
    memcpy(tk->data, ogg, (size_t)len);

    int err = 0;
    tk->v = stb_vorbis_open_memory(tk->data, len, &err, NULL);
    if (!tk->v) { track_free(tk); return; }
    tk->rate = stb_vorbis_get_info(tk->v).sample_rate;
    tk->loop = loop;

    /* Publish; free any prior pending we displace (audio thread never saw it). */
    track_free(atomic_exchange(&m->pending, tk));
}

void cron_music_stop(void* mm) {
    music_t* m = (music_t*)mm;
    if (m) atomic_store(&m->stop_req, 1);
}

void cron_music_set_volume(void* mm, int vol_q8) {
    music_t* m = (music_t*)mm;
    if (!m) return;
    if (vol_q8 < 0) vol_q8 = 0;
    if (vol_q8 > 256) vol_q8 = 256;
    atomic_store(&m->vol_q8, vol_q8);
}

/* Pull one source stereo frame (audio thread). Returns 0 at end-of-stream. */
static int pull_src(music_t* m, int out[2]) {
    if (!m->cur || !m->cur->v) { out[0] = out[1] = 0; return 0; }
    if (m->buf_i >= m->buf_n) {
        m->buf_n = stb_vorbis_get_samples_short_interleaved(
            m->cur->v, 2, m->buf, SRCBUF_FRAMES * 2);
        m->buf_i = 0;
        if (m->buf_n <= 0) {
            if (m->cur->loop) {
                stb_vorbis_seek_start(m->cur->v);
                m->buf_n = stb_vorbis_get_samples_short_interleaved(
                    m->cur->v, 2, m->buf, SRCBUF_FRAMES * 2);
                m->buf_i = 0;
            }
            if (m->buf_n <= 0) { out[0] = out[1] = 0; return 0; }
        }
    }
    out[0] = m->buf[m->buf_i * 2];
    out[1] = m->buf[m->buf_i * 2 + 1];
    m->buf_i++;
    return 1;
}

static void adopt(music_t* m, track_t* tk) {
    track_free(m->cur);
    m->cur   = tk;
    m->ended = 0;
    m->buf_n = m->buf_i = 0;
    m->t     = 0.0;
    m->step  = (double)tk->rate / (double)CRONOPIO_AUDIO_HZ;
    pull_src(m, m->s0);
    pull_src(m, m->s1);
}

void cron_music_render(void* mm, int16_t* dst, int frames) {
    music_t* m = (music_t*)mm;
    if (!m) { memset(dst, 0, (size_t)frames * 4); return; }

    track_t* p = atomic_exchange(&m->pending, (track_t*)NULL);
    if (p) adopt(m, p);

    if (atomic_exchange(&m->stop_req, 0)) {
        track_free(m->cur);
        m->cur = NULL;
    }

    int vol = atomic_load(&m->vol_q8);
    if (!m->cur || m->ended || vol <= 0) {
        memset(dst, 0, (size_t)frames * 4);
        return;
    }

    for (int i = 0; i < frames; ++i) {
        int l = m->s0[0] + (int)((m->s1[0] - m->s0[0]) * m->t);
        int r = m->s0[1] + (int)((m->s1[1] - m->s0[1]) * m->t);
        dst[i * 2]     = (int16_t)((l * vol) >> 8);
        dst[i * 2 + 1] = (int16_t)((r * vol) >> 8);

        m->t += m->step;
        while (m->t >= 1.0) {
            m->s0[0] = m->s1[0];
            m->s0[1] = m->s1[1];
            if (!pull_src(m, m->s1)) {
                m->ended = 1;          /* end (non-looping): fade to the last */
            }
            m->t -= 1.0;
        }
    }
}
