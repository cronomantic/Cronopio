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

## Audio

| Name                       | Signature                                                | Notes                                          |
|----------------------------|----------------------------------------------------------|------------------------------------------------|
| `cvm_sys_cron_snd_tone`    | `void(i32 ch, i32 wave, i32 freq_mhz, i32 vol, i32 pan)` | wave: 0 sine, 1 square, 2 triangle, 3 noise    |
| `cvm_sys_cron_snd_stop`    | `void(i32 ch)`                                           | Silence channel                                |
| `cvm_sys_cron_snd_master`  | `void(i32 vol_q8)`                                       | Master volume, 0..256                          |

## Input

| Name                          | Signature                       | Notes                                    |
|-------------------------------|---------------------------------|------------------------------------------|
| `cvm_sys_cron_pad`            | `u32(i32 player)`               | Returns button bitmask (see below)       |
| `cvm_sys_cron_pad_pressed`    | `u32(i32 player)`               | Buttons pressed this frame               |
| `cvm_sys_cron_pad_released`   | `u32(i32 player)`               | Buttons released this frame              |
| `cvm_sys_cron_key`            | `i32(i32 scancode)`             | 1 if held, 0 otherwise (optional device) |
| `cvm_sys_cron_mouse`          | `u32(i32* out_x, i32* out_y)`   | Writes x,y; returns button bitmask       |

## Persistence

| Name                       | Signature                       | Notes                                     |
|----------------------------|---------------------------------|-------------------------------------------|
| `cvm_sys_cron_save_read`   | `i32(u8* dst, i32 len)`         | Read up to 1024 bytes; returns bytes read |
| `cvm_sys_cron_save_write`  | `i32(const u8* src, i32 len)`   | Write up to 1024 bytes; returns written   |

## Gamepad bitmask

```text
bit 0  D-Up      bit 4  A
bit 1  D-Down    bit 5  B
bit 2  D-Left    bit 6  X
bit 3  D-Right   bit 7  Y
```
