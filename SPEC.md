# Cronopio — Console Specification (v0, draft)

Cronopio is a fantasy video game console. Cartridges are CronoVM
binaries (`.bin`) compiled from C via the CronoVM toolchain. The host
runtime supplies video, audio, input and persistence through a fixed
table of CronoVM syscalls (the **host ABI**).

This document is the source of truth for what a Cronopio cartridge can
assume about the machine it runs on. Everything here is intentionally
small; the goal is a coherent, well-defined target — not a maximalist
one.

## 1. Execution model

| Property               | Value                                                  |
|------------------------|--------------------------------------------------------|
| CPU                    | CronoVM (register-based, 32-bit, 256 registers)        |
| Cartridge format       | CronoVM `.bin` (CVM1, little-endian)                   |
| Code memory            | up to 1 MiB of instructions                            |
| RAM (heap + stack)     | 1 MiB total (configurable via `--heap-reserve`)        |
| Stack reserve          | 64 KiB by default (R255 = stack pointer)               |
| Frame rate             | 60 Hz fixed                                            |
| Frame entry            | host invokes cart-registered frame fn via `cvm_call`   |

The cartridge's CronoVM entry point runs once at boot. It is expected
to:

1. Call `cron_resolve_video()` once to discover the framebuffer and
   palette region addresses (populates the `CRON_FB` / `CRON_PAL`
   pointers).
2. Register a frame callback via `cron_set_frame(fn)`.
3. Return from `main`.

The host then drives the frame loop at 60 Hz, calling the registered
function via CronoVM's `cvm_call` API (see
[`external/CronoVM/include/cvm.h`](external/CronoVM/include/cvm.h)).
Each tick the host: polls input, calls the cart's frame fn, blits the
framebuffer to the window, vsyncs.

## 2. Display

| Property      | Value                                              |
|---------------|----------------------------------------------------|
| Resolution    | 320 × 240                                          |
| Pixel format  | 8 bpp indexed (1 byte per pixel)                   |
| Palette       | 32 entries, 24-bit RGB, fully programmable         |
| Refresh       | 60 Hz, vsync'd                                     |
| Framebuffer   | 76 800 bytes, region named `"fb"`                  |
| Palette RAM   | 128 bytes (32 × u32 little-endian, `0x00RRGGBB`)   |

Only the low 5 bits of each pixel byte select a palette entry; the
top 3 bits are reserved for future use (per-pixel flags, e.g. flip,
priority) and must be written as zero by current cartridges.

The framebuffer and palette **are CronoVM host-shared regions**, carved
out of the cart heap by the loader at cart-load time. The cart declares
them by passing `--region=fb:76800:rw --region=pal:128:rw` to `cvm-cc`
(or by using `cronopio_add_cartridge()` which adds the flags
automatically). At runtime the cart discovers the heap offsets via
`cron_resolve_video()`, which fills the `CRON_FB` and `CRON_PAL`
pointers; cart code then plots pixels with ordinary `*p = c;` writes
through those pointers. The drawing syscalls in §8 are convenience
helpers that operate on the same bytes.

> The cart must declare both regions; a cart without them runs but
> draws nothing (the host's drawing syscalls become no-ops and the
> blit produces a black frame).

## 3. Audio

| Property        | Value                                          |
|-----------------|------------------------------------------------|
| Channels        | 4                                              |
| Sample rate     | 22 050 Hz                                      |
| Output          | 16-bit signed stereo, mixed by host            |
| Per-channel ops | tone (sine/square/triangle/noise), volume, pan |

Music and SFX are produced by setting per-channel state through
syscalls (see [`docs/syscalls.md`](docs/syscalls.md)). Sample playback
(PCM buffers in heap) is reserved for v1.

## 4. Input

| Device       | Value                                                   |
|--------------|---------------------------------------------------------|
| Gamepads     | 2 virtual, 8 buttons each: D-pad U/D/L/R + A/B/X/Y      |
| Keyboard     | optional, exposed as a bitmap of scancodes              |
| Mouse        | optional, x/y + 2 buttons                               |

On embedded targets the host may report fewer devices; cartridges
must tolerate "device absent" (syscalls return zero).

## 5. Persistence

A 1 KiB save slot per cartridge, read/written via syscall. The host
decides where it lives (file on desktop, IndexedDB on web, NVS on
embedded).

## 6. Targets

| Target    | Backend                | Status      |
|-----------|------------------------|-------------|
| Desktop   | SDL2                   | in progress |
| Web       | SDL2 via Emscripten    | planned     |
| Embedded  | bare-metal HAL         | planned     |

All targets share `host/common/` (console state, syscall dispatch,
software mixer, etc.); the per-target code is just the platform
shell (window, audio device, file I/O, main loop).

## 7. Host ↔ VM contract

The integration relies on three CronoVM features and one Cronopio
addition to CronoVM:

| Feature                  | Where                              | Purpose                                      |
|--------------------------|------------------------------------|----------------------------------------------|
| `cvm_load` / `cvm_run`   | CronoVM public API                 | Load and bootstrap the cart                  |
| `cvm_link` (by name)     | CronoVM public API                 | Bind `cvm_sys_*` syscalls to host handlers   |
| `cvm_image_get_region`   | CronoVM public API                 | Host-side fb/pal offset lookup               |
| `cvm_call` (new in v0.2) | CronoVM (added for Cronopio)       | Re-enter the VM at a FUNCS index every frame |

Cart-facing syscall names follow CronoVM's `cvm_sys_*` prefix
convention; the `cron_*` names in `cronopio.h` are thin static-inline
aliases the cart programmer uses day-to-day.

## 8. Versioning

This document is **v0**. Anything here may change until v1.
v1 freezes the host ABI and the cart's view of memory.
