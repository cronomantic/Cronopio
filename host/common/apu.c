/* Audio helpers. The actual mixing lives in console.c (cronopio_console_mix);
 * this file just exposes the per-voice configuration entry points used by
 * the syscall layer. */

#include "console.h"

void cron_apu_tone(cronopio_console_t* c, int ch, int wave, uint32_t freq_mhz, int vol, int pan) {
    if ((unsigned)ch >= CRONOPIO_AUDIO_CHANS) return;
    cron_voice_t* v = &c->voices[ch];
    v->wave     = wave & 3;
    v->freq_mhz = freq_mhz;
    v->vol      = vol < 0 ? 0 : (vol > 255 ? 255 : vol);
    v->pan      = pan < -128 ? -128 : (pan > 127 ? 127 : pan);
}

void cron_apu_stop(cronopio_console_t* c, int ch) {
    if ((unsigned)ch >= CRONOPIO_AUDIO_CHANS) return;
    c->voices[ch].vol = 0;
    c->voices[ch].freq_mhz = 0;
}

void cron_apu_master(cronopio_console_t* c, int vol_q8) {
    if (vol_q8 < 0) vol_q8 = 0;
    if (vol_q8 > 256) vol_q8 = 256;
    c->master_vol_q8 = vol_q8;
}
