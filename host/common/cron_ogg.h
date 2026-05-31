/* cron_ogg.h — host-side streaming OGG music for Cronopio.
 *
 * A general console capability (not Quake-specific): a cart hands over the
 * bytes of an Ogg Vorbis file (cron_ogg syscall) and the host decodes +
 * resamples it to the output rate and mixes it under the SFX, looping if
 * asked. Decoding runs on the audio thread; the cart thread only publishes
 * commands (play/stop/volume) — single-producer/single-consumer, like the
 * MIDI synth and the PCM stream ring. */
#ifndef CRON_OGG_H
#define CRON_OGG_H

#include <stdint.h>

void* cron_ogg_create(void);
void  cron_ogg_destroy(void* m);

/* Cart thread. Copies `len` bytes from `ogg`, opens a decoder, and starts
 * playback at the next audio block (replacing any current track). loop != 0
 * restarts at the end. A malformed/empty buffer silently plays nothing. */
void  cron_ogg_play(void* m, const uint8_t* ogg, int len, int loop);

/* Cart thread. Stop and release the current track. */
void  cron_ogg_stop(void* m);

/* Cart thread. 0..256 (Q8) applied to the decoded music. */
void  cron_ogg_set_volume(void* m, int vol_q8);

/* Audio thread. Write `frames` stereo S16 frames of music into `dst`
 * (overwrites; silence when nothing is playing). */
void  cron_ogg_render(void* m, int16_t* dst, int frames);

#endif
