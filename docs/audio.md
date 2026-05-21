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
| `cron_sample`        | `void(i32 slot, const i8* ptr, i32 len, i32 rate)`             | Register an 8-bit **signed** mono PCM sample |
| `cron_sample_u8`     | `void(i32 slot, const u8* ptr, i32 len, i32 rate)`            | Register an 8-bit **unsigned** mono sample (DOOM DMX `DS*`), played straight from ROM — no copy/convert |
| `cron_snd_tone`      | `void(i32 v, i32 wave, i32 freq_mhz, i32 vol, i32 pan)`        | Synth on voice v (existing) |
| `cron_pcm`           | `void(i32 v, i32 sample, i32 pitch_q16, i32 vol, i32 pan, i32 loop)` | Play a sample on voice v; pitch 0x10000 = the sample's native rate |
| `cron_env`           | `void(i32 v, i32 attack_ms, i32 decay_ms, i32 sustain, i32 release_ms)` | Envelope applied to the next trigger on v |
| `cron_note_off`      | `void(i32 v)`                                                  | Enter release |
| `cron_snd_stop`      | `void(i32 v)`                                                  | Hard stop |
| `cron_snd_master`    | `void(i32 vol_q8)`                                             | Master volume 0..256 |
| `cron_stream`        | `i32(const i16* frames, i32 nframes)`                         | Queue 16-bit signed *stereo* frames into the host playback ring; returns frames queued |
| `cron_stream_free`   | `i32()`                                                       | Frames the stream ring can accept right now |

The stream is the escape hatch for music a cart renders itself (any synth or
codec): fill it each frame from `cron_stream_free()` worth of samples and the
mixer plays it alongside the voices.

## Layer 2 — sound effects

SFX are triggered directly on a free voice: `cron_pcm` for a one-shot PCM
sample (explosions, voice, percussion — what DOOM's SFX are) and
`cron_snd_tone` + `cron_env` for synth blips. No separate sound-data format
is needed; the voice layer is the SFX layer. (A Pyxel-style step sequencer
for synth "sounds" could be added later, but music is the bigger need and is
covered by MOD below.)

## Layer 3 — music: MOD playback

Music is a **ProTracker `.mod`** file, played by a host-side player that
parses the blob (in cart RAM or ROM) and drives voices 0..n_channels-1.
MOD samples are 8-bit signed mono — exactly the voice PCM format — so the
player just points voices at the sample bytes inside the blob and steps by
the Amiga period. SFX use the voices above the module's channel count.

| Name              | Signature                                        | Notes |
|-------------------|--------------------------------------------------|-------|
| `cron_mod_play`   | `i32(const void* mod, i32 len, i32 loop)`        | Parse + start a .mod; returns 0 / -1 (not a MOD). Uses voices 0..n_channels-1. |
| `cron_mod_stop`   | `void()`                                         | Stop and fade the module's voices. |

Supported: 4/6/8-channel modules, sample sustain loops, and the common
effects `Cxx` (set volume), `Fxx` (speed/tempo), `Bxx` (position jump),
`Dxx` (pattern break), `9xx` (sample offset), `Axy` (volume slide). Other
effects are ignored for now (the note still plays). MIDI is **deferred** —
it needs an instrument synth (FM/OPL or a GM sample bank); the path there is
a future FM voice mode, or converting MIDI to MOD/patterns offline.

## Syscall range

`0x200–0x2FF` — extended audio. `0x200` block = Layer 1 (samples + voices)
and SFX triggers; `0x220` block = MOD music.

## DOOM audio

- **SFX** are DMX `DS*` lumps: 8-bit **unsigned** PCM, ~11 kHz mono. The port
  registers each lump straight from the WAD in cart ROM with `cron_sample_u8`
  (no copy/convert), then plays it with `cron_pcm(voice, sample, pitch, vol,
  pan, 0)` — vol/pan from DOOM's distance/angle, optional pitch wobble. It
  manages ~8 voices with DOOM's priority/cutoff logic; SFX use voices above
  the MOD channel count (or any free voices if no MOD is playing).
- **Music** is MUS (a MIDI variant) — note events, no sound. The port renders
  it with its own bundled **OPL2 emulator** (the authentic DOS sound) into
  16-bit stereo and feeds the host via `cron_stream` each frame (sized by
  `cron_stream_free`). The console stays synth-agnostic; the same path serves
  any streamed music. (A host-side OPL+MUS player remains a possible future
  alternative; not needed for a port.)

## Status

Implemented: Layer 1 voices (synth + PCM + ADSR, sample banks), SFX as direct
triggers, and the MOD player (Layer 3). MIDI/FM is the remaining audio item,
deferred until a cart needs it.
