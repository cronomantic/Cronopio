/* cron_module.h — host-side tracker-module music (MOD/S3M/XM/IT/…) via libxmp.
 *
 * Sibling of cron_ogg.h (streaming Ogg Vorbis): a cart hands over the bytes of
 * a module file (the cron_module syscall) and the host loads it with libxmp,
 * renders it straight at the output rate (with cubic-spline interpolation + the
 * lowpass DSP), and mixes it under the SFX, looping if asked. Rendering runs on
 * the audio thread; the cart thread only publishes commands (play/stop/volume)
 * — single-producer/single-consumer, like cron_ogg and the MIDI synth. (A
 * libxmp context is not thread-safe, so exactly one thread owns it at a time.) */
#ifndef CRON_MODULE_H
#define CRON_MODULE_H

#include <stdint.h>

void* cron_module_create(void);
void  cron_module_destroy(void* m);

/* Cart thread. Copies nothing — libxmp parses `mod` into its own context on
 * load — opens + starts a player, and begins playback at the next audio block
 * (replacing any current module). loop != 0 repeats forever. A malformed/empty
 * buffer silently plays nothing. */
void  cron_module_play(void* m, const uint8_t* mod, int len, int loop);

/* Cart thread. Stop and release the current module. */
void  cron_module_stop(void* m);

/* Cart thread. 0..256 (Q8) applied to the rendered module. */
void  cron_module_set_volume(void* m, int vol_q8);

/* Audio thread. Write `frames` stereo S16 frames of module music into `dst`
 * (overwrites; silence when nothing is playing). */
void  cron_module_render(void* m, int16_t* dst, int frames);

#endif
