#include "console.h"

#include <string.h>

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

void cronopio_console_mix(cronopio_console_t* c, int16_t* dst, int frames) {
    const int sr = CRONOPIO_AUDIO_HZ;
    for (int i = 0; i < frames; ++i) {
        int32_t mix_l = 0, mix_r = 0;
        for (int vi = 0; vi < CRONOPIO_AUDIO_CHANS; ++vi) {
            cron_voice_t* voice = &c->voices[vi];
            if (voice->vol == 0 || voice->freq_mhz == 0) continue;
            uint32_t inc = (uint32_t)(((uint64_t)voice->freq_mhz << 32) /
                                      ((uint64_t)sr * 1000ull));
            voice->phase += inc;
            int32_t sample;
            switch (voice->wave) {
                case 1: sample = (voice->phase & 0x80000000u) ? 16384 : -16384; break;
                case 2: {
                    int32_t t = (int32_t)(voice->phase >> 16);
                    sample = (t < 0 ? -t : t) - 16384;
                    sample *= 2;
                } break;
                case 3: sample = (int32_t)((voice->phase * 1664525u + 1013904223u) & 0xFFFF) - 32768; break;
                default: {
                    int32_t t = (int32_t)(voice->phase >> 16) - 16384;
                    int32_t t2 = (t * t) >> 14;
                    sample = ((t * (49152 - t2)) >> 14);
                } break;
            }
            int32_t s = (sample * voice->vol) >> 8;
            /* Cheap equal-power pan: pan in [-128..127]. */
            int32_t lg = 128 - voice->pan; if (lg < 0) lg = 0; if (lg > 255) lg = 255;
            int32_t rg = 128 + voice->pan; if (rg < 0) rg = 0; if (rg > 255) rg = 255;
            mix_l += (s * lg) >> 8;
            mix_r += (s * rg) >> 8;
        }
        mix_l = (mix_l * c->master_vol_q8) >> 8;
        mix_r = (mix_r * c->master_vol_q8) >> 8;
        if (mix_l >  32767) mix_l =  32767; else if (mix_l < -32768) mix_l = -32768;
        if (mix_r >  32767) mix_r =  32767; else if (mix_r < -32768) mix_r = -32768;
        dst[2*i]     = (int16_t)mix_l;
        dst[2*i + 1] = (int16_t)mix_r;
    }
}
