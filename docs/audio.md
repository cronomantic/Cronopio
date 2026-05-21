# Cronopio Audio (v0.2 design)

Two layers, like the graphics stack: the host provides the **voices** (a
software mixer) and a data-driven **sequencer**; the cart supplies the data
(samples, sounds, music) and triggers playback. 16 voices, 22 050 Hz,
16-bit signed stereo out, mixed by the host.

> North star is the same as the rest of the machine: it must be able to run
> DOOM, whose SFX are 8-bit PCM (~11 kHz mono) mixed across ~8 channels.

## Layer 1 — voices (the mixer)

16 independent voices share one pool (music and SFX both draw from it). Each
voice plays **either** a synth waveform **or** a PCM sample, shaped by an
ADSR envelope, with pitch, volume (0..255) and pan (-128..127).

| Source | Detail |
|--------|--------|
| Synth  | sine / square / triangle / pulse / noise; pitch as milli-Hz |
| PCM    | 8-bit **signed** mono sample from a sample bank, resampled to 22 050 Hz by pitch; optional loop |
| Envelope | ADSR — attack/decay/release in ms, sustain level 0..255. No envelope set ⇒ gate on at full level, short release on stop (anti-click). |

**Sample banks** are thin handles over the cart's own memory (RAM or cart
ROM — zero copy, like image banks): `cron_sample(slot, ptr, len, rate)`.
DMX/8-bit-unsigned source data is converted to signed by the cart (`^0x80`).

Low-level voice syscalls (the cart can drive these directly per frame for
full control):

| Name                 | Signature                                                       | Notes |
|----------------------|-----------------------------------------------------------------|-------|
| `cron_sample`        | `void(i32 slot, const i8* ptr, i32 len, i32 rate)`             | Register an 8-bit signed mono PCM sample |
| `cron_snd_tone`      | `void(i32 v, i32 wave, i32 freq_mhz, i32 vol, i32 pan)`        | Synth on voice v (existing) |
| `cron_pcm`           | `void(i32 v, i32 sample, i32 pitch_q16, i32 vol, i32 pan, i32 loop)` | Play a sample on voice v; pitch 0x10000 = the sample's native rate |
| `cron_env`           | `void(i32 v, i32 attack_ms, i32 decay_ms, i32 sustain, i32 release_ms)` | Envelope applied to the next trigger on v |
| `cron_note_off`      | `void(i32 v)`                                                  | Enter release |
| `cron_snd_stop`      | `void(i32 v)`                                                  | Hard stop |
| `cron_snd_master`    | `void(i32 vol_q8)`                                             | Master volume 0..256 |

## Layer 2 — SFX (data-driven sounds)  *(planned, builds on Layer 1)*

A **sound** is a compact step array in cart memory, à la Pyxel: each step is
`{note, wave, volume, effect}`; the sound has a `speed` (ticks per step).
The host sequencer advances it and writes the voice each step.

| Name            | Signature                                          |
|-----------------|----------------------------------------------------|
| `cron_sfx`      | `void(i32 slot, const u8* steps, i32 n, i32 speed)`|
| `cron_sfx_play` | `void(i32 slot, i32 voice)`                        |

Step bytes: `note` (0 = rest, 1..96 = semitone, 255 = note-off), `wave`
(0..4), `volume` (0..15), `effect` (0 none, 1 fade, 2 slide↑, 3 slide↓,
4 vibrato). PCM one-shots are triggered with `cron_pcm` directly (or a
sample-backed sound).

## Layer 3 — music (patterns)  *(planned, builds on Layer 2)*

A **music** is a set of tracks; each track is a voice plus a sequence of
sound IDs played in order. The host advances all tracks together and loops.

| Name              | Signature                          |
|-------------------|------------------------------------|
| `cron_music`      | `void(i32 slot, const u8* blob)`   |
| `cron_music_play` | `void(i32 slot, i32 loop)`         |
| `cron_music_stop` | `void()`                           |

## Syscall range

`0x200–0x2FF` — extended audio. `0x200` block = Layer 1 (samples + voices),
`0x210` block = SFX, `0x220` block = music.

## Status

Layer 1 (voices: synth + PCM + ADSR, sample banks, direct triggers) is
implemented and is what DOOM-class PCM SFX need. Layers 2–3 (the data-driven
SFX/music sequencer) are specified here and land next.
