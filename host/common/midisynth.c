/* MIDI + SoundFont music synthesizer for the Cronopio host.
 *
 * Cronopio's native music engine (the "90s console" model: sequenced MIDI over
 * a sample bank, like a PSX VAB / an AWE32). The cart runs only the sequencer
 * (e.g. DOOM's MUS->MIDI) and pushes MIDI messages; this module owns the synth
 * (TinySoundFont, vendored in third_party/tsf.h) and renders it natively, so
 * there is zero VM cost. See cron_stream for cart-rendered PCM instead.
 *
 * THREADING: the cart thread (syscall layer) is the single producer; it pushes
 * MIDI/control events into an SPSC ring. The audio thread (cronopio_console_mix)
 * is the single consumer and the ONLY thread that touches a live tsf instance.
 * cron_synth_load_mem allocates a brand-new tsf on the cart thread (safe: not
 * yet visible to the mixer); it is published into a font slot and only made
 * active via a ring SELECT command the audio thread processes. Slots are freed
 * by the audio thread (FREE command). Same benign-SPSC discipline as the stream
 * ring in console.c: data is written before the ring head advances. */

#include "console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TSF_IMPLEMENTATION
#include "tsf.h"   /* external/TinySoundFont (submodule); include dir set in CMake */

#define CRON_SF_SLOTS    4      /* slot 0 = default (BIOS) font, 1.. = cart-loaded */
#define CRON_MIDI_RING   2048   /* SPSC event ring depth (power of two) */

/* Control events share the ring with MIDI messages. Real MIDI status bytes are
 * 0x80..0xEF; we tag control commands with the otherwise-unused 0xF0..0xFF
 * system range so one ring carries both. d1/d2 carry the command arguments. */
#define CRON_CMD_SELECT  0xF0   /* d1 = slot                       */
#define CRON_CMD_FREE    0xF1   /* d1 = slot                       */
#define CRON_CMD_RESET   0xF2   /* all notes off (panic)           */
#define CRON_CMD_VOLUME  0xF3   /* d1 = music master 0..255        */

typedef struct cron_synth {
    tsf*     fonts[CRON_SF_SLOTS];   /* font[0] = default; cart fonts in 1..     */
    int      active;                 /* slot the mixer is currently rendering    */
    float    music_gain;             /* 0..1, applied via tsf_set_volume         */

    /* SPSC event ring: packed (status<<16)|(d1<<8)|d2. */
    uint32_t ring[CRON_MIDI_RING];
    volatile int head;               /* written by producer (cart thread)        */
    volatile int tail;               /* written by consumer (audio thread)       */
} cron_synth;

/* --- helpers ------------------------------------------------------------ */

static void synth_config_font(cron_synth* s, tsf* f) {
    if (!f) return;
    tsf_set_output(f, TSF_STEREO_INTERLEAVED, CRONOPIO_AUDIO_HZ, 0.0f);
    tsf_set_volume(f, s->music_gain);
    tsf_set_max_voices(f, 48);
    /* GM: channel 10 (0-based 9) is the percussion kit. */
    tsf_channel_set_presetnumber(f, 9, 0, 1);
}

static void ring_push(cron_synth* s, int status, int d1, int d2) {
    int head = s->head;
    int next = (head + 1) & (CRON_MIDI_RING - 1);
    if (next == s->tail) return;     /* ring full: drop (music event, tolerable) */
    s->ring[head] = ((uint32_t)(status & 0xFF) << 16)
                  | ((uint32_t)(d1 & 0xFF) << 8)
                  |  (uint32_t)(d2 & 0xFF);
    s->head = next;                  /* publish after the slot is written        */
}

/* Apply one decoded MIDI message to the active font (audio thread only). */
static void synth_apply_midi(tsf* f, int status, int d1, int d2) {
    int ch   = status & 0x0F;
    switch (status & 0xF0) {
        case 0x90: /* note on (vel 0 == note off) */
            if (d2) tsf_channel_note_on(f, ch, d1, (float)d2 / 127.0f);
            else    tsf_channel_note_off(f, ch, d1);
            break;
        case 0x80: tsf_channel_note_off(f, ch, d1); break;
        case 0xB0: tsf_channel_midi_control(f, ch, d1, d2); break;
        case 0xC0: tsf_channel_set_presetnumber(f, ch, d1, (ch == 9)); break;
        case 0xE0: tsf_channel_set_pitchwheel(f, ch, (d2 << 7) | d1); break;
        /* 0xA0 poly aftertouch, 0xD0 channel pressure: ignored. */
        default: break;
    }
}

/* Drain the event ring into the synth (audio thread). */
static void synth_drain(cron_synth* s) {
    int head = s->head;              /* snapshot once                            */
    while (s->tail != head) {
        uint32_t e  = s->ring[s->tail];
        int status  = (e >> 16) & 0xFF;
        int d1      = (e >> 8) & 0xFF;
        int d2      =  e & 0xFF;
        s->tail = (s->tail + 1) & (CRON_MIDI_RING - 1);

        if (status < 0xF0) {
            tsf* f = s->fonts[s->active];
            if (f) synth_apply_midi(f, status, d1, d2);
            continue;
        }
        switch (status) {
            case CRON_CMD_SELECT:
                if ((unsigned)d1 < CRON_SF_SLOTS && s->fonts[d1]) {
                    if (s->fonts[s->active]) tsf_note_off_all(s->fonts[s->active]);
                    s->active = d1;
                }
                break;
            case CRON_CMD_FREE:
                if ((unsigned)d1 < CRON_SF_SLOTS && d1 != 0 && s->fonts[d1]) {
                    if (s->active == d1) s->active = 0;
                    tsf_close(s->fonts[d1]);
                    s->fonts[d1] = NULL;
                }
                break;
            case CRON_CMD_RESET: {
                tsf* f = s->fonts[s->active];
                if (f) tsf_reset(f);
                break;
            }
            case CRON_CMD_VOLUME: {
                s->music_gain = (float)d1 / 255.0f;
                for (int i = 0; i < CRON_SF_SLOTS; ++i)
                    if (s->fonts[i]) tsf_set_volume(s->fonts[i], s->music_gain);
                break;
            }
            default: break;
        }
    }
}

/* --- public API (declared in console.h) --------------------------------- */

void* cron_synth_create(void) {
    cron_synth* s = (cron_synth*)calloc(1, sizeof(cron_synth));
    if (!s) return NULL;
    s->active     = 0;
    s->music_gain = 1.0f;
    return s;
}

void cron_synth_destroy(void* sp) {
    cron_synth* s = (cron_synth*)sp;
    if (!s) return;
    for (int i = 0; i < CRON_SF_SLOTS; ++i)
        if (s->fonts[i]) tsf_close(s->fonts[i]);
    free(s);
}

/* Load the default (BIOS) SoundFont into slot 0 from an in-memory blob (the
 * font is embedded in the binary — see bios_sf2.c). Called at console init,
 * before the audio thread runs, so it can touch tsf directly. Returns 0/-1. */
int cron_synth_load_default_mem(void* sp, const void* sf2, int len) {
    cron_synth* s = (cron_synth*)sp;
    if (!s || !sf2 || len <= 0) return -1;
    tsf* f = tsf_load_memory(sf2, len);
    if (!f) return -1;
    if (s->fonts[0]) tsf_close(s->fonts[0]);
    s->fonts[0] = f;
    synth_config_font(s, f);
    return 0;
}

/* Cart thread: parse an SF2 blob into a free slot. Allocates a brand-new tsf
 * (not yet visible to the mixer), so this is safe off the audio thread.
 * Returns the slot handle (>=1) or -1. Activate it with cron_synth_select. */
int cron_synth_load_mem(void* sp, const void* sf2, int len) {
    cron_synth* s = (cron_synth*)sp;
    if (!s || !sf2 || len <= 0) return -1;
    int slot = -1;
    for (int i = 1; i < CRON_SF_SLOTS; ++i) if (!s->fonts[i]) { slot = i; break; }
    if (slot < 0) return -1;         /* no free slot */
    tsf* f = tsf_load_memory(sf2, len);
    if (!f) return -1;
    synth_config_font(s, f);
    s->fonts[slot] = f;              /* publish before the cart issues SELECT     */
    return slot;
}

void cron_synth_free_slot(void* sp, int slot) {
    cron_synth* s = (cron_synth*)sp;
    if (s) ring_push(s, CRON_CMD_FREE, slot, 0);
}

void cron_synth_select(void* sp, int slot) {
    cron_synth* s = (cron_synth*)sp;
    if (s) ring_push(s, CRON_CMD_SELECT, slot, 0);
}

void cron_synth_send(void* sp, int status, int d1, int d2) {
    cron_synth* s = (cron_synth*)sp;
    if (s) ring_push(s, status, d1, d2);
}

void cron_synth_reset(void* sp) {
    cron_synth* s = (cron_synth*)sp;
    if (s) ring_push(s, CRON_CMD_RESET, 0, 0);
}

void cron_synth_volume(void* sp, int vol) {
    cron_synth* s = (cron_synth*)sp;
    if (s) ring_push(s, CRON_CMD_VOLUME, vol, 0);
}

/* Audio thread: drain pending events, then render `frames` interleaved stereo
 * samples into `out` (replacing its contents). Silent if no font is active. */
void cron_synth_render(void* sp, int16_t* out, int frames) {
    cron_synth* s = (cron_synth*)sp;
    if (!s) { memset(out, 0, (size_t)frames * 2 * sizeof(int16_t)); return; }
    synth_drain(s);
    tsf* f = s->fonts[s->active];
    if (!f) { memset(out, 0, (size_t)frames * 2 * sizeof(int16_t)); return; }
    tsf_render_short(f, out, frames, 0);   /* replace, not mix */
}
