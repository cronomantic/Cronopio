#include "console.h"

#include <string.h>
#include <stdio.h>

/* Default (BIOS) SoundFont filename, looked up relative to the working dir at
 * console init. The platform shell may load a better path via
 * cron_synth_load_default. Ships beside the executable like SDL2.dll. */
#define CRONOPIO_DEFAULT_SF2  "GeneralUser-GS.sf2"

/* A pleasant 32-colour starter set occupies indices 0..31; seed_palette
 * fills 32..255 with a grayscale ramp so all 256 entries are valid for a
 * do-nothing cart. Cartridges (DOOM, etc.) overwrite the whole palette at
 * runtime by writing to the palette region. */
static const uint32_t default_palette32[32] = {
    0x000000, 0x1d2b53, 0x7e2553, 0x008751, 0xab5236, 0x5f574f, 0xc2c3c7, 0xfff1e8,
    0xff004d, 0xffa300, 0xffec27, 0x00e436, 0x29adff, 0x83769c, 0xff77a8, 0xffccaa,
    0x291814, 0x111d35, 0x422136, 0x125359, 0x742f29, 0x49333b, 0xa28879, 0xf3ef7d,
    0xbe1250, 0xff6c24, 0xa8e72e, 0x00b543, 0x065ab5, 0x754665, 0xff6e59, 0xff9d81,
};

void cronopio_console_init(cronopio_console_t* c) {
    memset(c, 0, sizeof(*c));
    c->master_vol_q8   = 256;
    c->prng_state      = 0xC0FFEE01u;
    c->frame_fn_index  = -1;
    cron_gpu_reset_state(c);   /* clip=full screen, camera=0, pal=identity */

    /* Bring up the MIDI+SoundFont synth and load the default (BIOS) bank. A
     * missing default font is non-fatal: MIDI music stays silent until a cart
     * supplies its own SoundFont via cron_sf2_load. */
    c->synth = cron_synth_create();
    if (c->synth && cron_synth_load_default(c->synth, CRONOPIO_DEFAULT_SF2) != 0) {
        fprintf(stderr, "[cronopio] default SoundFont '%s' not found; "
                        "MIDI music silent until a cart loads one\n",
                CRONOPIO_DEFAULT_SF2);
    }
}

void cronopio_console_begin_frame(cronopio_console_t* c) {
    memcpy(c->pad_prev, c->pad_cur, sizeof(c->pad_prev));
}

void cronopio_console_end_frame(cronopio_console_t* c) {
    c->frame_count++;
}

void cronopio_console_seed_palette(uint8_t* heap, uint32_t pal_offset) {
    uint8_t* pal = heap + pal_offset;
    for (int i = 0; i < CRONOPIO_PALETTE_SIZE; ++i) {
        uint32_t v;
        if (i < 32) {
            v = default_palette32[i];
        } else {
            /* Grayscale ramp across the remaining 224 entries. */
            uint32_t g = (uint32_t)(((i - 32) * 255) / (CRONOPIO_PALETTE_SIZE - 32 - 1));
            v = (g << 16) | (g << 8) | g;
        }
        pal[i*4 + 0] = (uint8_t)(v       & 0xFF);
        pal[i*4 + 1] = (uint8_t)((v>> 8) & 0xFF);
        pal[i*4 + 2] = (uint8_t)((v>>16) & 0xFF);
        pal[i*4 + 3] = 0;
    }
}

void cronopio_console_blit_rgba(const cronopio_console_t* c,
                                const uint8_t* heap, uint32_t* dst) {
    if (!c->regions_ok) {
        memset(dst, 0, (size_t)CRONOPIO_FB_BYTES * 4);
        return;
    }
    const uint8_t* fb  = heap + c->fb_offset;
    const uint8_t* pal = heap + c->pal_offset;

    uint32_t cache[CRONOPIO_PALETTE_SIZE];
    for (int i = 0; i < CRONOPIO_PALETTE_SIZE; ++i) {
        uint32_t r = pal[i*4 + 2];
        uint32_t g = pal[i*4 + 1];
        uint32_t b = pal[i*4 + 0];
        cache[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
    const int n = CRONOPIO_FB_BYTES;
    for (int i = 0; i < n; ++i) {
        dst[i] = cache[fb[i]];   /* full 8-bit index */
    }
}

/* Advance one voice's ADSR envelope by one sample; returns the level in
 * Q8.8 (0..255<<8). A gate voice (no envelope) holds full level until it is
 * note-off'd, then falls fast to avoid a click. */
static int env_tick(cron_voice_t* v) {
    if (!v->has_env) {
        if (v->env_stage == CRON_ENV_RELEASE) {
            v->env_level -= 2048;            /* ~31 sample fade */
            if (v->env_level <= 0) { v->env_level = 0; v->env_stage = CRON_ENV_OFF; }
        } else {
            v->env_level = 255 << 8;
        }
        return v->env_level;
    }
    switch (v->env_stage) {
        case CRON_ENV_ATTACK:
            v->env_level += v->env_attack ? (255 << 8) / v->env_attack : (255 << 8);
            if (v->env_level >= (255 << 8)) { v->env_level = 255 << 8; v->env_stage = CRON_ENV_DECAY; }
            break;
        case CRON_ENV_DECAY: {
            int target = v->env_sustain << 8;
            int d = v->env_decay ? ((255 - v->env_sustain) << 8) / v->env_decay : (255 << 8);
            v->env_level -= d;
            if (v->env_level <= target) { v->env_level = target; v->env_stage = CRON_ENV_SUSTAIN; }
        } break;
        case CRON_ENV_SUSTAIN:
            v->env_level = v->env_sustain << 8;
            break;
        case CRON_ENV_RELEASE: {
            int r = v->env_release ? (255 << 8) / v->env_release : (255 << 8);
            v->env_level -= r;
            if (v->env_level <= 0) { v->env_level = 0; v->env_stage = CRON_ENV_OFF; }
        } break;
        default: v->env_level = 0; break;
    }
    return v->env_level;
}

void cronopio_console_mix(cronopio_console_t* c, int16_t* dst, int frames) {
    const int sr = CRONOPIO_AUDIO_HZ;

    /* Render the MIDI+SoundFont synth for the whole block up front (it also
     * drains its event ring here, on this audio thread). Sized generously vs
     * the host's audio buffer (~1024 frames); clamp defensively. */
    static int16_t synth_buf[CRONOPIO_STREAM_FRAMES * 2];
    int synth_frames = frames > CRONOPIO_STREAM_FRAMES ? CRONOPIO_STREAM_FRAMES : frames;
    if (c->synth) cron_synth_render(c->synth, synth_buf, synth_frames);

    for (int i = 0; i < frames; ++i) {
        /* Advance the MOD player at its tick rate. */
        if (c->mod.playing) {
            if (--c->mod.sample_counter <= 0) {
                cron_mod_tick(c);
                c->mod.sample_counter = c->mod.samples_per_tick;
            }
        }

        int32_t mix_l = 0, mix_r = 0;
        for (int vi = 0; vi < CRONOPIO_AUDIO_CHANS; ++vi) {
            cron_voice_t* voice = &c->voices[vi];
            if (!voice->active) continue;

            int32_t sample;   /* roughly -16384..16384 */
            if (voice->mode == 1) {
                /* PCM: 8-bit signed mono, resampled by pcm_step (Q16.16),
                 * with an optional sub-region sustain loop. */
                if (!c->heap) { voice->active = 0; continue; }
                uint32_t idx = voice->pcm_pos >> 16;
                uint32_t end = voice->pcm_looplen
                             ? (voice->pcm_loopstart + voice->pcm_looplen)
                             : voice->pcm_len;
                if (idx >= end) {
                    if (voice->pcm_looplen) {
                        voice->pcm_pos -= (uint32_t)voice->pcm_looplen << 16;
                        idx = voice->pcm_pos >> 16;
                    } else { voice->active = 0; continue; }
                }
                if (idx >= voice->pcm_len) { voice->active = 0; continue; }
                uint8_t b = c->heap[voice->pcm_off + idx];
                int32_t s8 = voice->pcm_unsigned ? ((int32_t)b - 128) : (int32_t)(int8_t)b;
                sample = s8 << 7;                 /* -128..127 -> ~-16384..16256 */
                voice->pcm_pos += voice->pcm_step;
            } else {
                uint32_t inc = (uint32_t)(((uint64_t)voice->freq_mhz << 32) /
                                          ((uint64_t)sr * 1000ull));
                voice->phase += inc;
                switch (voice->wave) {
                    case CRON_WAVE_SQR_:   sample = (voice->phase & 0x80000000u) ? 16384 : -16384; break;
                    case CRON_WAVE_TRI_: {
                        int32_t t = (int32_t)(voice->phase >> 16);
                        sample = ((t < 0 ? -t : t) - 16384) * 2;
                    } break;
                    case CRON_WAVE_NOISE_: sample = (int32_t)((voice->phase * 1664525u + 1013904223u) & 0xFFFF) - 32768; break;
                    case CRON_WAVE_PULSE_: sample = ((voice->phase >> 16) < 16384) ? 16384 : -16384; break; /* 25% duty-ish */
                    default: {  /* sine via parabola */
                        int32_t t = (int32_t)(voice->phase >> 16) - 16384;
                        int32_t t2 = (t * t) >> 14;
                        sample = ((t * (49152 - t2)) >> 14);
                    } break;
                }
            }

            /* envelope + voice volume */
            int env = env_tick(voice);
            if (voice->env_stage == CRON_ENV_OFF && (voice->has_env || voice->mode == 0)) {
                /* synth/enveloped voice finished its release */
                if (voice->mode == 0 || voice->has_env) { voice->active = 0; }
            }
            int32_t s = (((sample * voice->vol) >> 8) * (env >> 8)) >> 8;

            int32_t lg = 128 - voice->pan; if (lg < 0) lg = 0; if (lg > 255) lg = 255;
            int32_t rg = 128 + voice->pan; if (rg < 0) rg = 0; if (rg > 255) rg = 255;
            mix_l += (s * lg) >> 8;
            mix_r += (s * rg) >> 8;
        }
        /* Drain one streamed frame (pre-rendered music) into the mix. */
        if (c->stream_tail != c->stream_head) {
            mix_l += c->stream[c->stream_tail * 2];
            mix_r += c->stream[c->stream_tail * 2 + 1];
            c->stream_tail = (c->stream_tail + 1) % CRONOPIO_STREAM_FRAMES;
        }

        /* Add the MIDI+SoundFont synth (music) for this frame. */
        if (c->synth && i < synth_frames) {
            mix_l += synth_buf[i * 2];
            mix_r += synth_buf[i * 2 + 1];
        }

        mix_l = (mix_l * c->master_vol_q8) >> 8;
        mix_r = (mix_r * c->master_vol_q8) >> 8;
        if (mix_l >  32767) mix_l =  32767; else if (mix_l < -32768) mix_l = -32768;
        if (mix_r >  32767) mix_r =  32767; else if (mix_r < -32768) mix_r = -32768;
        dst[2*i]     = (int16_t)mix_l;
        dst[2*i + 1] = (int16_t)mix_r;
    }
}
