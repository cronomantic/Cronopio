/* Audio voice setters. The mixing (synth + PCM + ADSR) lives in console.c
 * (cronopio_console_mix); this file configures voices and sample banks from
 * the syscall layer. */

#include "console.h"

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* Start the envelope for a freshly triggered voice: ADSR from zero if an
 * envelope was configured (cron_apu_env), otherwise a full-level gate. */
static void trigger_env(cron_voice_t* v) {
    if (v->has_env) { v->env_stage = CRON_ENV_ATTACK;  v->env_level = 0; }
    else            { v->env_stage = CRON_ENV_SUSTAIN; v->env_level = 255 << 8; }
}

void cron_apu_tone(cronopio_console_t* c, int ch, int wave, uint32_t freq_mhz, int vol, int pan) {
    if ((unsigned)ch >= CRONOPIO_AUDIO_CHANS) return;
    cron_voice_t* v = &c->voices[ch];
    v->mode     = 0;
    v->wave     = wave & 7;
    v->freq_mhz = freq_mhz;
    v->vol      = clampi(vol, 0, 255);
    v->pan      = clampi(pan, -128, 127);
    v->phase    = 0;
    v->active   = 1;
    trigger_env(v);
}

void cron_apu_pcm(cronopio_console_t* c, int ch, int slot,
                  uint32_t pitch_q16, int vol, int pan, int loop) {
    if ((unsigned)ch >= CRONOPIO_AUDIO_CHANS) return;
    if ((unsigned)slot >= CRONOPIO_SAMPLE_SLOTS || !c->samples[slot].used) return;
    cron_voice_t* v = &c->voices[ch];
    /* native advance per output sample, then scaled by pitch (Q16.16). */
    uint32_t native = (uint32_t)(((uint64_t)c->samples[slot].rate << 16) / CRONOPIO_AUDIO_HZ);
    v->mode     = 1;
    v->sample   = slot;
    v->pcm_pos  = 0;
    v->pcm_step = (uint32_t)(((uint64_t)native * (uint64_t)(uint32_t)pitch_q16) >> 16);
    v->loop     = loop ? 1 : 0;
    v->vol      = clampi(vol, 0, 255);
    v->pan      = clampi(pan, -128, 127);
    v->active   = 1;
    trigger_env(v);
}

void cron_apu_env(cronopio_console_t* c, int ch, int attack_ms, int decay_ms,
                  int sustain, int release_ms) {
    if ((unsigned)ch >= CRONOPIO_AUDIO_CHANS) return;
    cron_voice_t* v = &c->voices[ch];
    v->env_attack  = attack_ms  * CRONOPIO_AUDIO_HZ / 1000;
    v->env_decay   = decay_ms   * CRONOPIO_AUDIO_HZ / 1000;
    v->env_release = release_ms * CRONOPIO_AUDIO_HZ / 1000;
    v->env_sustain = clampi(sustain, 0, 255);
    v->has_env     = 1;
}

void cron_apu_note_off(cronopio_console_t* c, int ch) {
    if ((unsigned)ch >= CRONOPIO_AUDIO_CHANS) return;
    c->voices[ch].env_stage = CRON_ENV_RELEASE;
}

void cron_apu_stop(cronopio_console_t* c, int ch) {
    if ((unsigned)ch >= CRONOPIO_AUDIO_CHANS) return;
    /* Graceful: enter release so the mixer fades out without a click. */
    c->voices[ch].env_stage = CRON_ENV_RELEASE;
}

void cron_apu_master(cronopio_console_t* c, int vol_q8) {
    c->master_vol_q8 = clampi(vol_q8, 0, 256);
}

void cron_apu_sample(cronopio_console_t* c, int slot, uint32_t offset,
                     uint32_t len, uint32_t rate, uint32_t mem_size) {
    if ((unsigned)slot >= CRONOPIO_SAMPLE_SLOTS) return;
    if (len == 0 || rate == 0) return;
    if ((uint64_t)offset + len > mem_size) return;   /* out of cart memory */
    c->samples[slot].offset = offset;
    c->samples[slot].len    = len;
    c->samples[slot].rate   = rate;
    c->samples[slot].used   = 1;
}
