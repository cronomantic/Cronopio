# Cronopio Host ABI (v0 draft)

Cartridges call host functions through CronoVM's `SYSCALL` mechanism.
Every Cronopio syscall is exposed under a `cvm_sys_cron_*` name; the
CronoVM translator picks up any `cvm_sys_*` reference in the bitcode,
adds it to the IMPORTS section, and emits a `SYSCALL imm16` whose
index points at that import. There is no fixed numeric ABI — bindings
are by name. The host (`host/common/syscalls.c`) registers handlers
with `cvm_link(img, "cvm_sys_cron_log", …)` at cart-load time.

Cart code typically uses the short `cron_*` aliases in
`sdk/include/cronopio.h`; those are `static inline` wrappers over the
`cvm_sys_*` extern decls.

Pointer arguments are 32-bit byte offsets into the cart's heap.
The host validates every pointer/length via `cvm_heap_read` /
`cvm_heap_write`; out-of-range accesses make the syscall trap and
return zero to the cart.

The framebuffer and palette are CronoVM **host-shared regions** named
`"fb"` and `"pal"`. They are carved out of the cart heap by the loader
when the binary is built with `--region=fb:76800:rw
--region=pal:128:rw`. Cart code resolves their heap offsets at startup
with `cron_resolve_video()` and then plots pixels directly through the
returned pointers; the drawing syscalls below are convenience helpers
that touch the same memory.

## Core

| Name                      | Signature                          | Notes                                                 |
|---------------------------|------------------------------------|-------------------------------------------------------|
| `cvm_sys_cron_log`        | `void(const char* msg, i32 len)`   | Debug log to host stderr / browser console            |
| `cvm_sys_cron_trace_i32`  | `void(i32 tag, i32 value)`         | Numeric trace for debugging                           |
| `cvm_sys_cron_set_frame`  | `void(i32 fn_index)`               | Register cart's per-frame callback (FUNCS[fn_index])  |
| `cvm_sys_cron_exit`       | `void(i32 status)`                 | Terminate the cartridge                               |

The cart calls `cron_set_frame(frame)` passing a C function pointer;
CronoVM treats function pointers as FUNCS indices (CALLR reads the
index from a register), so the host stores the index and re-enters
the VM at that function each tick via `cvm_call`.

## System / time

| Name                         | Signature        | Notes                          |
|------------------------------|------------------|--------------------------------|
| `cvm_sys_cron_time_ms`       | `u32()`          | Milliseconds since boot        |
| `cvm_sys_cron_frame_count`   | `u32()`          | Frame counter since boot       |
| `cvm_sys_cron_random`        | `u32()`          | xorshift32 from host PRNG      |
| `cvm_sys_cron_seed`          | `void(u32 seed)` | Reseed the host PRNG           |

## Display

| Name                          | Signature                                               | Notes                                              |
|-------------------------------|---------------------------------------------------------|----------------------------------------------------|
| `cvm_sys_cron_palette_set`    | `void(i32 idx, u32 rgb)`                                | Set palette entry idx (0..31) to `0x00RRGGBB`      |
| `cvm_sys_cron_palette_get`    | `u32(i32 idx)`                                          | Read palette entry                                 |
| `cvm_sys_cron_cls`            | `void(i32 color)`                                       | Clear framebuffer to palette index                 |
| `cvm_sys_cron_pset`           | `void(i32 x, i32 y, i32 color)`                         | Plot a single pixel                                |
| `cvm_sys_cron_rect`           | `void(i32 x, i32 y, i32 w, i32 h, i32 color)`           | Filled rectangle                                   |
| `cvm_sys_cron_line`           | `void(i32 x0, i32 y0, i32 x1, i32 y1, i32 c)`           | Line                                               |
| `cvm_sys_cron_blit`           | `void(const u8* src, i32 sw, i32 sh, i32 dx, i32 dy)`   | Copy 8bpp bitmap from heap to framebuffer          |
| `cvm_sys_cron_text`           | `void(const char* s, i32 len, i32 x, i32 y, i32 color)` | Built-in 8×8 bitmap font (v0: stubbed glyphs)      |
| `cvm_sys_cron_present`        | `void()`                                                | No-op in callback model (kept for forward compat)  |

The platform shell already blits + vsyncs after the frame fn returns,
so `cron_present` is a stub today. It will become meaningful if a
future ABI lets carts flush mid-frame.

## Audio (0x030 + 0x200)

16 voices, 22 050 Hz, 16-bit stereo. Each voice plays a synth waveform or an
8-bit PCM sample, shaped by an ADSR envelope. Full model in
[`audio.md`](audio.md).

### Voices & SFX

| Name                       | Signature                                                       | Notes                                          |
|----------------------------|-----------------------------------------------------------------|------------------------------------------------|
| `cvm_sys_cron_snd_tone`    | `void(i32 v, i32 wave, i32 freq_mhz, i32 vol, i32 pan)`         | Synth on voice v. wave: 0 sine, 1 square, 2 triangle, 3 noise, 4 pulse |
| `cvm_sys_cron_snd_stop`    | `void(i32 v)`                                                   | Release/stop a voice                           |
| `cvm_sys_cron_snd_master`  | `void(i32 vol_q8)`                                              | Master volume, 0..256                          |
| `cvm_sys_cron_sample`      | `void(i32 slot, const void* ptr, i32 len, i32 rate, i32 fmt)`  | Register a PCM sample bank (fmt 0=signed8, 1=unsigned8/DMX) |
| `cvm_sys_cron_pcm`         | `void(i32 v, i32 slot, i32 pitch_q16, i32 vol, i32 pan, i32 loop)` | Play sample `slot` on voice v; pitch 0x10000 = native rate |
| `cvm_sys_cron_env`         | `void(i32 v, i32 attack_ms, i32 decay_ms, i32 sustain, i32 release_ms)` | ADSR for the next trigger on v          |
| `cvm_sys_cron_note_off`    | `void(i32 v)`                                                   | Enter the envelope release stage               |

### Streaming PCM

| Name                       | Signature                                       | Notes                                          |
|----------------------------|-------------------------------------------------|------------------------------------------------|
| `cvm_sys_cron_stream`      | `i32(const i16* frames, i32 nframes)`           | Queue 16-bit stereo frames into the playback ring; returns queued |
| `cvm_sys_cron_stream_free` | `i32()`                                         | Frames the stream ring can accept now          |

### Music (MIDI + SoundFont)

Host-native MIDI synth (TinySoundFont). Default GM bank is handle 0; carts may
load their own `.sf2`. See [audio.md](audio.md) Layer 4.

| Name                          | Signature                            | Notes                                          |
|-------------------------------|--------------------------------------|------------------------------------------------|
| `cvm_sys_cron_midi_send`      | `void(i32 status, i32 d1, i32 d2)`   | One MIDI message (note/CC/program/pitch-bend)  |
| `cvm_sys_cron_midi_volume`    | `void(i32 vol)`                      | Music master 0..255                            |
| `cvm_sys_cron_midi_reset`     | `void()`                             | All notes off / panic                          |
| `cvm_sys_cron_sf2_load`       | `i32(const void* sf2, i32 len)`      | Load a SoundFont from RAM/ROM → handle ≥1 / -1 |
| `cvm_sys_cron_sf2_free`       | `void(i32 handle)`                   | Free a cart-loaded SoundFont                   |
| `cvm_sys_cron_midi_soundfont` | `void(i32 handle)`                   | Select active bank (0 = default BIOS bank)     |

## Input

| Name                          | Signature                       | Notes                                    |
|-------------------------------|---------------------------------|------------------------------------------|
| `cvm_sys_cron_pad`            | `u32(i32 player)`               | Returns button bitmask (see below)       |
| `cvm_sys_cron_pad_pressed`    | `u32(i32 player)`               | Buttons pressed this frame               |
| `cvm_sys_cron_pad_released`   | `u32(i32 player)`               | Buttons released this frame              |
| `cvm_sys_cron_key`            | `i32(i32 scancode)`             | 1 if held, 0 otherwise (optional device). `scancode` is a USB HID Keyboard usage ID (see `CRON_KEY_*` in `cronopio.h`); the platform shell fills the key bitmap in that space. |
| `cvm_sys_cron_mouse`          | `u32(i32* out_x, i32* out_y)`   | Writes x,y; returns button bitmask       |

## Persistence

| Name                       | Signature                       | Notes                                     |
|----------------------------|---------------------------------|-------------------------------------------|
| `cvm_sys_cron_save_read`   | `i32(u8* dst, i32 len)`         | Read up to 1024 bytes; returns bytes read |
| `cvm_sys_cron_save_write`  | `i32(const u8* src, i32 len)`   | Write up to 1024 bytes; returns written   |

## Extended graphics (0x100) — sprites, tilemaps, shapes, draw state

A Pyxel-flavoured layer over the core display syscalls. Two design choices
shape it:

- **Banks are thin handles over cart memory.** `cron_image` / `cron_tilemap`
  bind a slot to a bitmap/grid that lives in the cart's own RAM or ROM —
  no upload, no host VRAM. A sprite sheet baked into the cartridge ROM is
  drawn straight from there.
- **Draw state is global.** `clip`, `camera` and `pal` affect *every*
  primitive (including the core `cls`/`pset`/`rect`/`line`/`text` and the
  blits below). Camera is subtracted from world coordinates; the clip rect
  is in screen space; `pal` remaps the colour at write time. `cls` is the
  one exception — it clears the whole framebuffer ignoring the state.

### Resources

| Name                  | Signature                                            | Notes                                                            |
|-----------------------|------------------------------------------------------|------------------------------------------------------------------|
| `cvm_sys_cron_image`  | `void(i32 slot, const u8* ptr, i32 w, i32 h)`        | Bind image bank `slot` (0..7) to a w×h 8bpp bitmap in cart memory |
| `cvm_sys_cron_tilemap`| `void(i32 slot, const u16* ptr, i32 w, i32 h, i32 img)` | Bind tilemap `slot` (0..7): w×h grid of u16 tile indices (0xFFFF = empty) drawn from 8×8 tiles of image bank `img` |

### Sprites & tiles

| Name                | Signature                                                         | Notes                                                          |
|---------------------|-------------------------------------------------------------------|----------------------------------------------------------------|
| `cvm_sys_cron_blt`  | `void(i32 img, i32 dx, i32 dy, i32 sx, i32 sy, i32 w, i32 h, i32 colkey)` | Blit (sx,sy,w,h) of image bank `img` to (dx,dy). `colkey` index transparent, −1 = opaque. **Negative w/h flip** (Pyxel convention). |
| `cvm_sys_cron_bltm` | `void(i32 tm, i32 dx, i32 dy, i32 sx, i32 sy, i32 w, i32 h, i32 colkey)` | Blit a pixel region (sx,sy,w,h) of tilemap `tm` to (dx,dy). `colkey` as `blt`. |

### Shapes (complete the core cls/pset/rect/line/text)

| Name                  | Signature                                              |
|-----------------------|--------------------------------------------------------|
| `cvm_sys_cron_rectb`  | `void(i32 x, i32 y, i32 w, i32 h, i32 color)` — outline |
| `cvm_sys_cron_circ`   | `void(i32 x, i32 y, i32 r, i32 color)` — filled         |
| `cvm_sys_cron_circb`  | `void(i32 x, i32 y, i32 r, i32 color)` — outline        |
| `cvm_sys_cron_elli`   | `void(i32 x, i32 y, i32 w, i32 h, i32 color)` — filled  |
| `cvm_sys_cron_ellib`  | `void(i32 x, i32 y, i32 w, i32 h, i32 color)` — outline  |
| `cvm_sys_cron_tri`    | `void(i32 x0,y0,x1,y1,x2,y2, i32 color)` — filled       |
| `cvm_sys_cron_trib`   | `void(i32 x0,y0,x1,y1,x2,y2, i32 color)` — outline      |
| `cvm_sys_cron_fill`   | `void(i32 x, i32 y, i32 color)` — flood fill            |

### Draw state

| Name                        | Signature                          | Notes                                  |
|-----------------------------|------------------------------------|----------------------------------------|
| `cvm_sys_cron_clip`         | `void(i32 x, i32 y, i32 w, i32 h)` | Clip rect (clamped to screen)          |
| `cvm_sys_cron_clip_reset`   | `void()`                           | Clip = full screen                     |
| `cvm_sys_cron_camera`       | `void(i32 x, i32 y)`               | Offset subtracted from world coords    |
| `cvm_sys_cron_camera_reset` | `void()`                           | Camera = (0,0)                         |
| `cvm_sys_cron_pal`          | `void(i32 c0, i32 c1)`             | Remap draw colour c0 → c1              |
| `cvm_sys_cron_pal_reset`    | `void()`                           | Identity remap                         |

### Rotozoom & software-3D accelerators

| Name                  | Signature (SDK)                                                              | Notes                                                                 |
|-----------------------|-----------------------------------------------------------------------------|-----------------------------------------------------------------------|
| `cron_blt_ex`         | `void(img, dx, dy, sx, sy, w, h, colkey, rotate, scale_q16)`                | Rotozoom blit: scale `scale_q16` (Q16.16, `CRON_SCALE_1X`=0x10000) and `rotate` degrees clockwise about the sprite centre (placed at dx+w/2, dy+h/2), nearest-neighbour. sx/sy and w/h are packed by the SDK to fit the syscall's 8 args. |
| `cron_cmap`           | `void(const u8* ptr)`                                                       | Set the active 256-byte light/colormap for tcol/tspan; NULL = identity |
| `cron_tcol`           | `void(x, y0, y1, const u8* src, mask, frac, step)`                         | Vertical textured column (DOOM R_DrawColumn): rows [y0,y1] at x; src is (mask+1) bytes, mask=texh-1 (pow2); frac/step Q16.16; writes cmap[src[(frac>>16)&mask]] |
| `cron_tcolm`          | `void(x, y0, y1, const u8* src, frac, step)`                               | Masked vertical column (DOOM masked posts: sprites/weapon): like `cron_tcol` but src is addressed LINEARLY (no pow2 wrap) — writes cmap[src[frac>>16]]. The host bounds src by the actual index span over [y0,y1], so no mask is needed. Caller keeps the index within the post |
| `cron_tspan`          | `void(y, x0, x1, const u8* src, u, v, du, dv)`                             | Horizontal textured span (DOOM R_DrawSpan) over a 64×64 src: cols [x0,x1] at y; (u,v) Q16.16 step (du,dv); writes cmap[src[((v>>16)&63)*64+((u>>16)&63)]] |

`tcol`/`tspan` are the perf escape hatch for software 3D: they run the hot
inner loop in native host C. They honour the clip rect (so a 3D viewport
clips correctly) but ignore camera and the draw palette — the active
`cmap` is the only remap, mirroring DOOM's light diminishing.

## 3D triangle submission (0x120) — PSX / 486-Pentium style

The cart transforms, projects and lights its geometry in fixed point
(Q16.16, via CronoVM `MUL`/`MULH`), fills a `cron_vert_t` array in its own
memory, and submits batches of screen-space triangles. The host rasterises
them in native C. This is the PSX model (the GTE transformed, the GPU
filled) and amortises the syscall cost across a whole batch.

```c
typedef struct {
    int32_t x, y;   /* screen pixels */
    int32_t z;      /* depth (ZTEST: nearer = smaller) */
    int32_t u, v;   /* texcoords, Q16.16 texels (TEX) */
    int32_t w;      /* perspective depth (PERSP) */
    int32_t c;      /* gouraud light/index (GOURAUD) */
} cron_vert_t;      /* CRON_POLY_* mode flags select which fields are used */
```

| Name                  | Signature (SDK)                                                  | Notes                                                                |
|-----------------------|-----------------------------------------------------------------|----------------------------------------------------------------------|
| `cron_polys`          | `void(mode, const cron_vert_t* verts, count, arg, colkey)`     | Draw `count/3` triangles. **mode** is a bitmask: `FLAT` (solid `arg`), `GOURAUD` (interpolate `.c` through the cmap), `TEX` (affine texture from image bank `arg`, `colkey` transparent), `PERSP` (perspective-correct, uses `.w`), `ZTEST` (depth test/write). |
| `cron_zbuf`           | `void(int32_t* zbuffer)`                                        | Bind a 320×240 i32 depth buffer in cart memory; NULL disables (painter's-only). |
| `cron_zclear`         | `void(int32_t far)`                                             | Fill the bound z-buffer with `far` (e.g. `0x7FFFFFFF`).              |

Like the rasteriser accelerators, triangles honour the clip rect (viewport)
but ignore camera and the draw palette; the active `cmap` shades Gouraud and
textured spans (DOOM-style per-triangle light). Texture coords wrap (repeat)
modulo the image-bank dimensions. Back-face culling is the cart's job (a 2D
screen-space cross product); the rasteriser fills either winding.

## Cartridge ROM

These are **CronoVM built-ins** (auto-bound by the loader, no host handler),
exposed through the SDK helpers `cron_rom()` / `cron_rom_size()`. They read
the read-only data blob baked into the `.bin` with `cvm-cc --rom=FILE`.

| Name                  | Signature   | Notes                                            |
|-----------------------|-------------|--------------------------------------------------|
| `cvm_sys_rom_base`    | `i32()`     | Heap offset of the cart ROM (treat as a pointer) |
| `cvm_sys_rom_size`    | `i32()`     | ROM length in bytes; 0 if the cart carries none  |

## Gamepad bitmask

```text
bit 0  D-Up      bit 4  A
bit 1  D-Down    bit 5  B
bit 2  D-Left    bit 6  X
bit 3  D-Right   bit 7  Y
```
