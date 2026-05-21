/* Host-side ProTracker MOD player.
 *
 * Parses a .mod blob living in the cart's memory (RAM or cart ROM) and drives
 * voices 0..n_channels-1 of the mixer (console.c). MOD samples are 8-bit
 * signed mono — exactly the voice PCM format — so playback is just pointing a
 * voice at the sample's bytes inside the blob and stepping by the Amiga
 * period. SFX use the voices above n_channels.
 *
 * Supported: 4/6/8-channel modules, sample sustain loops, and the common
 * effects Cxx (set volume), Fxx (speed/tempo), Bxx (position jump), Dxx
 * (pattern break), 9xx (sample offset), Axy (volume slide). Other effects are
 * currently ignored (note still plays). */

#include "console.h"

#include <string.h>

/* PAL Amiga clock / 2; playback Hz = AMIGA / period. */
#define MOD_AMIGA  3546895u

static uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

static void mod_recalc_tick(cron_mod_t* m) {
    if (m->bpm < 32) m->bpm = 125;
    m->samples_per_tick = (CRONOPIO_AUDIO_HZ * 5) / (m->bpm * 2);
}

static uint32_t period_to_step(int period) {
    if (period <= 0) return 0;
    /* step (Q16.16) = (AMIGA/period) / output_rate */
    return (uint32_t)(((uint64_t)MOD_AMIGA << 16)
                      / ((uint64_t)period * (uint64_t)CRONOPIO_AUDIO_HZ));
}

int cron_mod_play(cronopio_console_t* c, uint32_t off, uint32_t len, int loop) {
    cron_mod_t* m = &c->mod;
    if (!c->heap || len < 1084u) return -1;
    const uint8_t* base = c->heap + off;

    /* channel count from the signature at offset 1080 */
    const uint8_t* sig = base + 1080;
    int nch = 0;
    if (!memcmp(sig, "M.K.", 4) || !memcmp(sig, "M!K!", 4) ||
        !memcmp(sig, "FLT4", 4) || !memcmp(sig, "4CHN", 4)) nch = 4;
    else if (!memcmp(sig, "6CHN", 4)) nch = 6;
    else if (!memcmp(sig, "8CHN", 4) || !memcmp(sig, "FLT8", 4)) nch = 8;
    else return -1;
    if (nch > CRONOPIO_MOD_MAXCHAN) return -1;

    m->n_channels = nch;

    /* 31 sample headers at offset 20, 30 bytes each. Sample PCM follows the
     * patterns; we accumulate offsets after counting patterns. */
    int      song_len = base[950];
    if (song_len < 1 || song_len > 128) return -1;
    m->song_len = song_len;
    for (int i = 0; i < 128; ++i) m->order[i] = base[952 + i];

    int n_patterns = 0;
    for (int i = 0; i < 128; ++i) if (m->order[i] >= n_patterns) n_patterns = m->order[i] + 1;

    m->pattern_off = off + 1084u;
    uint32_t pat_bytes = (uint32_t)n_patterns * 64u * (uint32_t)nch * 4u;
    uint32_t cursor = m->pattern_off + pat_bytes;          /* sample data start */

    for (int i = 0; i < CRONOPIO_MOD_SAMPLES; ++i) {
        const uint8_t* h = base + 20 + i * 30;
        uint32_t slen   = (uint32_t)be16(h + 22) * 2u;
        uint8_t  vol    = h[25]; if (vol > 64) vol = 64;
        uint32_t rstart = (uint32_t)be16(h + 26) * 2u;
        uint32_t rlen   = (uint32_t)be16(h + 28) * 2u;
        m->smp_off[i]       = cursor;
        m->smp_len[i]       = slen;
        m->smp_vol[i]       = vol;
        m->smp_loopstart[i] = rstart;
        m->smp_looplen[i]   = (rlen > 2u) ? rlen : 0u;       /* <=1 word = no loop */
        cursor += slen;
    }
    if (cursor > off + len) return -1;                      /* blob too short */

    /* reset playback */
    for (int ch = 0; ch < CRONOPIO_MOD_MAXCHAN; ++ch) {
        m->ch_sample[ch] = 0; m->ch_period[ch] = 0; m->ch_vol[ch] = 0;
        m->ch_fx[ch] = 0; m->ch_fxp[ch] = 0;
    }
    m->pos = 0; m->row = 0; m->tick = 0;
    m->speed = 6; m->bpm = 125;
    m->pat_break = -1; m->pos_jump = -1;
    m->loop_song = loop ? 1 : 0;
    mod_recalc_tick(m);
    m->sample_counter = 1;     /* tick on the next mixed sample */
    m->playing = 1;
    return 0;
}

void cron_mod_stop(cronopio_console_t* c) {
    cron_mod_t* m = &c->mod;
    if (!m->playing) return;
    m->playing = 0;
    for (int ch = 0; ch < m->n_channels; ++ch)
        c->voices[ch].env_stage = CRON_ENV_RELEASE;     /* fade out */
}

/* Process one pattern row: read each channel's cell, trigger notes, latch
 * effects. */
static void mod_row(cronopio_console_t* c) {
    cron_mod_t* m = &c->mod;
    int pat = m->order[m->pos];
    const uint8_t* rowp = c->heap + m->pattern_off
                        + (uint32_t)pat * 64u * (uint32_t)m->n_channels * 4u
                        + (uint32_t)m->row * (uint32_t)m->n_channels * 4u;

    for (int ch = 0; ch < m->n_channels; ++ch) {
        const uint8_t* cell = rowp + ch * 4;
        int sample = (cell[0] & 0xF0) | (cell[2] >> 4);
        int period = ((cell[0] & 0x0F) << 8) | cell[1];
        int fx     = cell[2] & 0x0F;
        int fxp    = cell[3];
        m->ch_fx[ch]  = (uint8_t)fx;
        m->ch_fxp[ch] = (uint8_t)fxp;

        if (sample > 0 && sample <= CRONOPIO_MOD_SAMPLES) {
            m->ch_sample[ch] = sample - 1;
            m->ch_vol[ch]    = m->smp_vol[sample - 1];
        }
        if (period > 0) {
            int s = m->ch_sample[ch];
            m->ch_period[ch] = period;
            cron_voice_t* v = &c->voices[ch];
            v->mode          = 1;
            v->pcm_off       = m->smp_off[s];
            v->pcm_len       = m->smp_len[s];
            v->pcm_loopstart = m->smp_loopstart[s];
            v->pcm_looplen   = m->smp_looplen[s];
            v->pcm_pos       = (fx == 0x9) ? ((uint32_t)fxp * 256u) << 16 : 0u;
            v->pcm_step      = period_to_step(period);
            v->pan           = (ch & 1) ? 80 : -80;     /* Amiga L/R/R/L-ish */
            v->has_env       = 0;
            v->env_stage     = CRON_ENV_SUSTAIN;
            v->env_level     = 255 << 8;
            v->active        = (v->pcm_len > 0);
        }

        /* row-time effects */
        switch (fx) {
            case 0xC: m->ch_vol[ch] = fxp > 64 ? 64 : fxp; break;
            case 0xF: if (fxp < 32) { m->speed = fxp ? fxp : m->speed; }
                      else { m->bpm = fxp; mod_recalc_tick(m); } break;
            case 0xB: m->pos_jump  = fxp; break;
            case 0xD: m->pat_break = (fxp >> 4) * 10 + (fxp & 0x0F); break;
            default: break;
        }
        c->voices[ch].vol = m->ch_vol[ch] * 255 / 64;
    }
}

/* Per-tick effects on ticks 1..speed-1. */
static void mod_fx_tick(cronopio_console_t* c) {
    cron_mod_t* m = &c->mod;
    for (int ch = 0; ch < m->n_channels; ++ch) {
        int fx = m->ch_fx[ch], fxp = m->ch_fxp[ch];
        if (fx == 0xA) {                       /* volume slide */
            int up = fxp >> 4, dn = fxp & 0x0F;
            m->ch_vol[ch] += up - dn;
            if (m->ch_vol[ch] < 0)  m->ch_vol[ch] = 0;
            if (m->ch_vol[ch] > 64) m->ch_vol[ch] = 64;
            c->voices[ch].vol = m->ch_vol[ch] * 255 / 64;
        }
    }
}

void cron_mod_tick(cronopio_console_t* c) {
    cron_mod_t* m = &c->mod;
    if (!m->playing) return;

    if (m->tick == 0) mod_row(c);
    else              mod_fx_tick(c);

    if (++m->tick >= m->speed) {
        m->tick = 0;
        int njump = m->pos_jump, nbreak = m->pat_break;
        m->pos_jump = m->pat_break = -1;
        int next_pos, next_row;
        if (njump >= 0)       { next_pos = njump;        next_row = (nbreak >= 0) ? nbreak : 0; }
        else if (nbreak >= 0) { next_pos = m->pos + 1;   next_row = nbreak; }
        else {
            next_row = m->row + 1; next_pos = m->pos;
            if (next_row >= 64) { next_row = 0; next_pos = m->pos + 1; }
        }
        if (next_row >= 64) next_row = 0;
        if (next_pos >= m->song_len) {
            if (m->loop_song) next_pos = 0;
            else { cron_mod_stop(c); return; }
        }
        m->pos = next_pos; m->row = next_row;
    }
}
