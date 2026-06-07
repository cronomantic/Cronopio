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
| `cvm_sys_cron_pcm_params`  | `void(i32 v, i32 vol, i32 pan)`                                 | Update vol/pan of a playing voice (no-op if idle) |

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

### Music (streaming OGG)

Host-native Ogg Vorbis playback (vendored stb_vorbis). A cart hands over the
bytes of an `.ogg` (e.g. a track read from its ROM/pak); the host copies them,
decodes + resamples to the output rate, and mixes the music under the SFX. One
track plays at a time. See [audio.md](audio.md) Layer 4.

| Name                       | Signature                              | Notes                                          |
|----------------------------|----------------------------------------|------------------------------------------------|
| `cvm_sys_cron_ogg`       | `void(const void* ogg, i32 len, i32 loop)` | Decode + play `len` bytes of ogg; loop!=0 restarts; replaces any current track |
| `cvm_sys_cron_ogg_stop`  | `void()`                               | Stop and release the current track             |
| `cvm_sys_cron_ogg_volume`| `void(i32 vol)`                        | Music volume 0..256 (Q8)                       |
| `cvm_sys_cron_module`       | `void(const void* mod, i32 len, i32 loop)` | Load + play `len` bytes of a tracker module (MOD/S3M/XM/IT) via libxmp; loop!=0 repeats; replaces any current module |
| `cvm_sys_cron_module_stop`  | `void()`                               | Stop and release the current module            |
| `cvm_sys_cron_module_volume`| `void(i32 vol)`                        | Module music volume 0..256 (Q8)                |
| `cvm_sys_cron_module_set`   | `void(i32 param, i32 value)`           | libxmp player effect: interp/DSP/amp/stereo-mix/flags (see `CRON_MOD_*`) |

## Input

| Name                          | Signature                       | Notes                                    |
|-------------------------------|---------------------------------|------------------------------------------|
| `cvm_sys_cron_pad`            | `u32(i32 player)`               | Returns button bitmask (see below)       |
| `cvm_sys_cron_pad_pressed`    | `u32(i32 player)`               | Buttons pressed this frame               |
| `cvm_sys_cron_pad_released`   | `u32(i32 player)`               | Buttons released this frame              |
| `cvm_sys_cron_mouse`          | `u32(i32* out_x, i32* out_y)`   | Writes absolute position in cart 320×240 coords (NULL ok); returns button bitmask (1=L, 2=R, 4=M, 8=X1, 16=X2). In relative mode the position is meaningless — read deltas instead. |
| `cvm_sys_cron_mouse_delta`    | `void(i32* out_dx, i32* out_dy)`| Accumulated relative motion since the previous call (in cart coords); the host zeroes the accumulator on read. Works in both absolute and relative-mouse mode. |
| `cvm_sys_cron_mouse_wheel`    | `i32()`                         | Accumulated vertical wheel ticks since the previous call (+ = wheel up / away from user); cleared on read |
| `cvm_sys_cron_cursor`         | `void(i32 show)`                | Show (`1`) / hide (`0`) the OS cursor — for carts that draw their own pixelated cursor. Default: visible. While the F1 menu is open the host forces it visible. |
| `cvm_sys_cron_mouse_relative` | `void(i32 enable)`              | Toggle SDL relative-mouse mode (mouselook): hides + locks the cursor, deltas keep flowing via `cron_mouse_delta`. Default: off. Forced off while the F1 menu is open. |

## Persistence

A per-cart save blob (a "memory card"). The host persists `save_write`'s bytes
to `<cart>.sav` (atomically) and reloads them before the cart runs. The SDK libc
layers a small **RAM filesystem** on top — `fopen`/`fread`/`fwrite`/`rename`/… —
so a ported engine's file-based saves (e.g. DOOM's savegames) persist with no
engine changes. The region is host-allocated and **grows on demand** (writes
auto-grow it; a cart can pre-reserve), up to a 64 MB cap.

| Name                       | Signature                       | Notes                                          |
|----------------------------|---------------------------------|------------------------------------------------|
| `cvm_sys_cron_save_read`   | `i32(u8* dst, i32 len)`         | Read up to `len` live bytes; returns bytes read |
| `cvm_sys_cron_save_write`  | `i32(const u8* src, i32 len)`   | Replace the save blob with `len` bytes (auto-grows); persisted |
| `cvm_sys_cron_save_size`   | `i32(void)`                     | Current capacity of the save blob in bytes      |
| `cvm_sys_cron_save_used`   | `i32(void)`                     | Live bytes currently stored (what read returns) |
| `cvm_sys_cron_save_reserve`| `i32(i32 bytes)`                | Ensure capacity ≥ `bytes`; returns new capacity |

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
| `cvm_sys_cron_blt_buf` | `void(u8* dst, i32 dst_dim, i32 dst_pitch, const u8* src, i32 src_pitch, i32 dst_pos, i32 blt_dim, i32 colkey)` | **Buffer→buffer** colour-key blit (NOT to the framebuffer): composite a flat 8bpp sprite from one cart buffer into another, in native host code, so the VM composes a sprite without running the per-pixel loop as bytecode (the win on weak targets where the VM is ~100× slower than native). `dst`/`src` are cart pointers; `dst_dim` packs dst `w`<<16\|`h`, `blt_dim` packs the blit `w`<<16\|`h`, `dst_pos` packs `dx`<<16\|`dy` (each signed i16). `src` rows are `src_pitch` apart (pass a src already advanced to its sub-origin). The blit lands at (dx,dy) in dst, clamped to dst bounds; source pixels == `colkey` are skipped (−1 = opaque). Both buffers' touched byte ranges are bounds-checked against the cart image size. Wrapper `cron_blt_buf` does the packing. |
| `cvm_sys_cron_blt_buf_blend` | `void(u8* dst, i32 dst_dim, i32 dst_pitch, const u8* src, i32 src_pitch, i32 dst_pos, i32 blt_dim, i32 colkey_blend)` | Like `cvm_sys_cron_blt_buf`, but each written pixel is composited through a 256×256 blend LUT instead of copied: `out = table[src*256 + dst]` (same format as `cron_blend_table`). The last arg packs `blend_slot`<<16 \| (`colkey` & 0xFFFF); `blend_slot` 1..7 selects a bound LUT, 0 falls back to a plain opaque copy. Lets a cart offload **translucent** buffer→buffer compositing (e.g. paletted alpha tables) to the host. Wrapper `cron_blt_buf_blend` does the packing. |
| `cvm_sys_cron_bltm` | `void(i32 tm, i32 dx, i32 dy, i32 sx, i32 sy, i32 w, i32 h, i32 colkey)` | Blit a pixel region (sx,sy,w,h) of tilemap `tm` to (dx,dy). `colkey` as `blt`. Tile cells use a u16 layout: `0xFFFF` = empty, bit 15 = HFLIP, bit 14 = VFLIP, bits 13..0 = tile index (16384 max). Old tilemaps with values below `0x4000` are unchanged. |
| `cvm_sys_cron_bltm_raster` | `void(i32 tm, i32 dx, i32 dy, i32 srcpack, i32 dimpack, i32 colkey, ptr table)` | Tilemap blit with **per-scanline parameter overrides** (HDMA-style raster effects). `srcpack` carries `sx` as i16 low and `sy` as i16 high; `dimpack` packs `w`/`h` the same way. `table` points at a `cron_raster_t[]` indexed by destination y — each entry adds its `scroll_x`/`scroll_y` to the line's source coord, and `pal_offset` is added to the sampled colour index. One syscall walks the table, zero per-line VM round-trips. Use cases: linescroll, palette-cycling gradients. Cart-side wrapper `cron_bltm_raster` does the packing. |
| `cvm_sys_cron_bltm_affine` | `void(i32 tm, i32 dx, i32 dy, i32 dimpack, i32 colkey, ptr table)` | "Mode-7" tilemap blit. `dimpack` packs w/h as above. `table` points at a `cron_affine_t[]` indexed by destination y — each entry holds `u, v, du, dv` (all Q16.16). Texel at screen pixel `(i, dy+j)` sampled at `(u + i*du, v + i*dv)` with the tuple read from `table[dy+j]`. Texture coords wrap modulo the tilemap size (infinite floor / sky). Wrapper `cron_bltm_affine`. |
| `cvm_sys_cron_blt_flip` | `void(i32 img, i32 dx, i32 dy, i32 srcpack, i32 dimpack, i32 colkey, i32 flags)` | Image-bank blit with **horizontal/vertical flip**. `flags` bits: `CRON_BLT_HFLIP` (bit 0), `CRON_BLT_VFLIP` (bit 1). Fast path for character sprites that face left/right or flip upside-down; `cron_blt_ex` covers the rotation+scale case. Wrapper `cron_blt_flip`. |
| `cvm_sys_cron_blt_scale` | `void(i32 img, i32 dx, i32 dy, i32 srcpack, i32 dimpack, i32 colkey, i32 scale_q16, i32 flags)` | Image-bank blit with **variable scale + flip** (no rotation — for rotation use `blt_ex`). `scale_q16` is Q16.16 (`0x10000` = 1.0). The sprite is anchored at top-left `(dx, dy)` so position math is predictable; for centre-anchored scaling the cart shifts `(dx, dy)` by half the scaled size. `flags` as `blt_flip`. Cheaper than `blt_ex` when only scale is needed (no trig, single pass per dest pixel). Wrapper `cron_blt_scale`. |

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
| `cron_blt_ex`         | `void(img, dx, dy, sx, sy, w, h, colkey, rotate, scale_q16)`                | Rotozoom blit: scale `scale_q16` (Q16.16, `CRON_SCALE_1X`=0x10000) and `rotate` degrees clockwise about the sprite centre (placed at dx+w/2, dy+h/2), nearest-neighbour. sx/sy and w/h are packed by the SDK to fit the syscall's 8 args. `rotate` is stored as the low 16 bits of the syscall arg; the high 16 carry flip flags (see `cron_blt_ex_flip` below) — `cron_blt_ex` masks them to 0. |
| `cron_blt_ex_flip`    | `void(img, dx, dy, sx, sy, w, h, colkey, rotate, scale_q16, flags)`         | Same syscall as `cron_blt_ex` but the SDK wrapper packs HFLIP/VFLIP (`CRON_BLT_HFLIP` / `CRON_BLT_VFLIP`) into the high 16 bits of the rotate arg. The host honours the flip in the SOURCE rect (mirror within the sprite's own bounds) BEFORE rotation. Use this for "facing left + rotating" sprites; the plain `cron_blt_ex` is equivalent to `flags = 0`. |
| `cron_palette_bank`   | `void(i32 slot, const u8* table)`                                            | Register a 256-byte palette-remap table as bank `slot` (1..31; bank 0 is the identity sentinel). The table is `input_idx → output_idx`. Referenced per scanline by `cron_raster_t.pal_bank` in `cron_bltm_raster` — combined with `pal_offset` the cart can do SNES-style sunset gradients, water reflections, palette-cycling effects without a per-line callback. The host reads the table every frame via the registered pointer, so the cart can mutate it in place to animate. |
| `cron_tile_anim`      | `void(i32 img_slot, const cron_tile_anim_t* table, i32 count)`               | Register an array of tile-animation rules for image bank `img_slot`. Each `cron_tile_anim_t` entry says "when a tilemap cell's tile index matches `src_tile`, substitute it through `frames[(host_frame_count / period_frames) MOD num_frames]`". Applied by `cron_bltm` / `cron_bltm_raster` / `cron_bltm_affine` per sampled cell. `count=0` (or `table=NULL`) clears the bank's anim. The cart can mutate `period_frames` or the frames arrays in place between frames — host re-reads via the registered pointers. The substituted index is masked to 14 bits, so embedded flip flags in the cell are PRESERVED across the substitution. |
| `cron_blend_table`    | `void(i32 slot, const u8* table)`                                            | Register a 64 KB blend lookup table as slot 1..7. Format: `out = table[src*256 + dst]`. Slot 0 is the opaque sentinel ("no blend", default). The SDK ships `cron_blend_build(out, pal, mode)` to bake one of the standard modes (ADD / AVG / SUB / MUL) from the current palette in ~5–10 ms; carts that ship pre-baked tables in ROM can define `CRON_NO_BLEND_BUILD` to omit it. Host stores only the heap offset, so the cart can mutate the table in place to retint dynamically. |
| `cron_blend_set`      | `void(i32 slot)`                                                             | Set the active blend slot for subsequent draws. 0 disables blending (opaque writes). Applies to every primitive that goes through `put_px` — sprite blits (`cron_blt` / `_flip` / `_scale` / `_ex` / `_ex_flip`), tilemap blits (`cron_bltm` / `_raster` / `_affine`), shapes, text. Stateful: persists until changed (same pattern as `cron_camera` / `cron_clip` / `cron_cmap`). |
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
    int32_t lu, lv; /* lightmap texcoords, Q16.16 (LIGHTMAP) */
} cron_vert_t;      /* CRON_POLY_* mode flags select which fields are used */
```

| Name                  | Signature (SDK)                                                  | Notes                                                                |
|-----------------------|-----------------------------------------------------------------|----------------------------------------------------------------------|
| `cron_polys`          | `void(mode, const cron_vert_t* verts, count, arg, colkey)`     | Draw `count/3` triangles. **mode** is a bitmask: `FLAT` (solid `arg`), `GOURAUD` (interpolate `.c` through the cmap), `TEX` (affine texture from image bank `arg`, `colkey` transparent), `PERSP` (perspective-correct, uses `.w`), `ZTEST` (depth test/write), `LIGHTMAP` (per-texel light, below), `CLAMP` (clamp texcoords to the edge instead of wrapping — single-sheet skins), `TURB` (per-pixel turbulence, below). |
| `cron_mvp`            | `void(const float* mat16)`                                      | Bind the 4×4 row-major MVP matrix used by `cron_xform_polys`. `NULL` unbinds (xform_polys then no-ops). |
| `cron_xform_polys`    | `void(mode, const cron_wvert_t* verts, count, arg, colkey)`    | Host-side T&L draw: verts are world-space (see `cron_wvert_t` below). The host transforms each vert by the bound `cron_mvp`, near-clips per source triangle, perspective-divides + maps to the bound clip rect (viewport), then rasterises through the same inner loop as `cron_polys`. Same `CRONOPIO_POLY_*` mode flags / image / lightmap / colormap / turb bindings. |
| `cron_zbuf`           | `void(int32_t* zbuffer)`                                        | Bind a 320×240 i32 depth buffer in cart memory; NULL disables (painter's-only). |
| `cron_zclear`         | `void(int32_t far)`                                             | Fill the bound z-buffer with `far` (e.g. `0x7FFFFFFF`).              |
| `cron_lightmap`       | `void(const u8* ptr, i32 w, i32 h)`                            | Bind a per-surface light grid (8bpp, each byte a colormap *row*) for `LIGHTMAP` draws; NULL/0 disables |
| `cron_colormap`       | `void(const u8* ptr, i32 levels)`                              | Bind a `levels*256` colormap indexed `[light*256 + texel]` (e.g. Quake's 64×256); NULL/0 disables |
| `cron_turb`           | `void(i32 phase, i32 amp)`                                     | Bind per-pixel texcoord turbulence for `TURB` draws (water/lava ripple); `phase` advances the sine (period 128), `amp` is the texel amplitude; `amp<=0` disables |

`cron_xform_polys`'s vertex format (`cron_wvert_t`, 8 little-endian floats):

```c
typedef struct {
    float x, y, z;     /* world space */
    float u, v;        /* texels (pixels)    — TEX */
    float lu, lv;      /* lumels             — LIGHTMAP */
    float light;       /* Gouraud light row  — GOURAUD */
} cron_wvert_t;
```

Use it instead of `cron_polys` when you have world-space geometry + an MVP
matrix: the cart skips the per-vertex matrix multiply, near-clip and viewport
map (those run native-C in the host instead of the VM). Bind the matrix once
with `cron_mvp`, rebind only when the entity transform changes (the world,
each brush ent, each alias model). The result is bit-identical to doing the
maths in the cart with `cron_mat_point` + `cron_clip_near` + `cron_to_screen`
+ `cron_polys`, just much cheaper on the cart side.

Like the rasteriser accelerators, triangles honour the clip rect (viewport)
but ignore camera and the draw palette; the active `cmap` shades Gouraud and
textured spans (DOOM-style per-triangle light). Texture coords wrap (repeat)
modulo the image-bank dimensions. Back-face culling is the cart's job (a 2D
screen-space cross product); the rasteriser fills either winding.

**`LIGHTMAP`** (with `TEX`) is Quake-style per-texel lighting: each pixel
samples the texture (index `t`) *and* the bound light grid via `.lu/.lv`
(perspective-correct when `PERSP` is set, like `.u/.v`) for a light row `l`,
then writes `colormap[l*256 + t]`. Bind the grid with `cron_lightmap` (rebind
per surface) and the table once with `cron_colormap`. This keeps a full
per-texel lightmap (not the per-triangle `cmap` shade) on the indexed
framebuffer — the offload path for Quake's lightmapped world.

**`TEX | GOURAUD`** with a `cron_colormap` table bound (and no `cron_lightmap`
grid) is **per-vertex lit texture**: the vertex `.c` is interpolated as the
colormap *row* (0…levels-1) and the texel remapped through it —
`colormap[row*256 + texel]`. This is Gouraud light on a textured model (Quake
alias monsters/items: each vertex `ambient + shade*dot(normal,lightdir)`).
`GOURAUD` without `TEX` instead reads `.c` as a direct `cron_cmap` index.

**`TURB`** (with `TEX`) warps texcoords per pixel for a Quake-style water/lava
ripple: each texel `(u,v)` is displaced by a sine of the *other* coordinate,
`u' = u + turbsin[(v+phase) & 127]`, `v' = v + turbsin[(u+phase) & 127]`, where
`turbsin` spans `[0, 2*amp]` texels. Bind `phase`/`amp` with `cron_turb`
(advance `phase` per frame to animate). The result still wraps modulo the image
dimensions, so water textures keep tiling.

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
