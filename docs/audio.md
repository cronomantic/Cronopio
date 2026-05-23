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
for synth "sounds" could be added later, but music is covered by the MIDI +
SoundFont engine below.)

## Layer 3 — music: MIDI + SoundFont (the music engine)

The native music model for a 90s-class console: a **MIDI sequence over a
sample bank** (the PSX VAB / AWE32 model). The host owns a **MIDI + SoundFont
synthesizer** ([TinySoundFont](https://github.com/schellingb/TinySoundFont),
vendored at `external/TinySoundFont`); the cart runs only the sequencer (e.g.
DOOM's MUS→MIDI) and pushes MIDI messages. Synthesis is host-native, so VM cost
is negligible — the same reason the rasteriser is offloaded to GPU primitives.

The console ships a **default General MIDI SoundFont** (handle 0, "BIOS"):
`GeneralUser GS`, **embedded straight into the binary** (C23 `#embed`, see
`host/common/bios_sf2.c`) so the executable is self-contained — no external
asset file. `console_init` loads it into slot 0. A cart may load its own `.sf2`
from RAM/ROM with `cron_sf2_load` (→ handle ≥1) and select it with
`cron_midi_soundfont`; ROM cost is then the programmer's choice.

| Name                 | Signature                                  | Notes |
|----------------------|--------------------------------------------|-------|
| `cron_midi_send`     | `void(i32 status, i32 d1, i32 d2)`         | One MIDI message: note on `0x9n`, note off `0x8n`, CC `0xBn`, program `0xCn`, pitch-bend `0xEn`. Channel 10 (`0x_9`) is GM percussion. |
| `cron_midi_volume`   | `void(i32 vol)`                            | Music master 0..255 (independent of `cron_snd_master`) |
| `cron_midi_reset`    | `void()`                                   | All notes off / panic |
| `cron_sf2_load`      | `i32(const void* sf2, i32 len)`            | Parse a SoundFont from cart memory → handle ≥1, or -1 |
| `cron_sf2_free`      | `void(i32 handle)`                         | Free a cart-loaded SoundFont slot |
| `cron_midi_soundfont`| `void(i32 handle)`                         | Select the active bank (0 = default BIOS bank) |

**Threading.** The cart thread is a single producer pushing MIDI/control events
into a lock-free SPSC ring; the audio thread (the mixer) is the single consumer
and the only thread that touches a live synth instance. `cron_sf2_load` parses a
brand-new instance off the audio thread (safe — not yet visible to the mixer);
it becomes active only via the ring `SELECT` the mixer processes.

**Limitations** (TinySoundFont v0.9): SF2 modulators and reverb/chorus are not
implemented, and the low-pass filter is basic. The base synthesis (samples,
ADSR envelopes, GM presets) is faithful enough for game music.

## Syscall range

`0x200–0x2FF` — extended audio. `0x200` block = Layer 1 (samples + voices)
and SFX triggers; `0x240` block = MIDI + SoundFont synth (Layer 3).

## DOOM audio

- **SFX** are DMX `DS*` lumps: 8-bit **unsigned** PCM, ~11 kHz mono. The port
  registers each lump straight from the WAD in cart ROM with `cron_sample_u8`
  (no copy/convert), then plays it with `cron_pcm(voice, sample, pitch, vol,
  pan, 0)` — vol/pan from DOOM's distance/angle, optional pitch wobble. It
  manages ~8 voices with DOOM's priority/cutoff logic (any free voices, since
  music is on the synth, not the voices).
- **Music** is MUS (a MIDI variant) — note events, no sound. The port converts
  MUS→MIDI in the cart (`mus2mid`/`midifile`, already in the tree) and feeds the
  events to the host MIDI + SoundFont synth (Layer 3) via `cron_midi_send`,
  played against the default GM bank (or a cart-supplied `.sf2`). This matches
  the 90s-console identity — sampled GM, not the "tinny" OPL/Adlib sound — and
  costs almost nothing on the VM. (OPL/Adlib was considered and dropped to avoid
  a redundant second music path.)

## Status

Implemented: Layer 1 voices (synth + PCM + ADSR, sample banks), SFX as direct
triggers, and the **MIDI + SoundFont synth (Layer 3)**
with a default GM "BIOS" bank and cart-loadable `.sf2`. The DOOM port uses all
of it: music via MUS→MIDI→`cron_midi_send` (host synth), SFX via DMX `DS*` →
`cron_sample_u8`+`cron_pcm` — both working. Possible future addition: a
`cron_pcm_params(voice, vol, pan)` syscall so a playing SFX can be repositioned
in 3D mid-sound (today it keeps its trigger-time volume/pan).
