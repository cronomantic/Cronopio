# Cronopio — Console Specification (v0.2, draft)

Cronopio is a fantasy video game console. Cartridges are CronoVM
binaries (`.bin`) compiled from C via the CronoVM toolchain. The host
runtime supplies video, audio, input and persistence through a fixed
table of CronoVM syscalls (the **host ABI**).

This document is the source of truth for what a Cronopio cartridge can
assume about the machine it runs on.

> **v0.2 — the "pre-3D era" baseline.** Cronopio targets the class of
> game that defined the mid-90s software-rendered 3D era — the explicit
> north star is **running DOOM**. That goal sets the machine's shape: a
> full 256-colour palette, megabytes of RAM, and read-only cartridge ROM
> for bundling multi-megabyte asset files (WADs). This is a deliberate
> step up from a PICO-8-tier micro-console; CronoVM was designed with
> DOOM-style software rendering as an intended target.

## 1. Execution model

| Property               | Value                                                  |
|------------------------|--------------------------------------------------------|
| CPU                    | CronoVM (register-based, 32-bit, 256 registers)        |
| Cartridge format       | CronoVM `.bin` (CVM1, little-endian)                   |
| Code memory            | up to 4 MiB of instructions                            |
| RAM (heap + stack)     | 32 MiB default (set per-cart via `--heap-reserve`)     |
| Cartridge ROM          | read-only data bundled in the `.bin` (e.g. a game WAD) |
| Stack reserve          | 256 KiB by default (R255 = stack pointer)              |
| Fixed-point            | Q16.16 via CronoVM `MUL`/`MULH` (no FPU needed)        |
| Frame rate             | 60 Hz fixed (carts may run their own logic tic)        |
| Frame entry            | host invokes cart-registered frame fn via `cvm_call`   |

The cartridge's CronoVM entry point runs once at boot. It is expected to:

1. Call `cron_resolve_video()` once to discover the framebuffer and
   palette region addresses (populates `CRON_FB` / `CRON_PAL`).
2. If it ships bundled data, call `cron_rom()` to get a read-only pointer
   to its cartridge ROM (and `cron_rom_size()` for the length).
3. Register a frame callback via `cron_set_frame(fn)`.
4. Return from `main`.

The host then drives the frame loop at 60 Hz, calling the registered
function via CronoVM's `cvm_call`. Each tick the host polls input, calls
the cart's frame fn, blits the framebuffer, vsyncs.

## 2. Display

| Property      | Value                                              |
|---------------|----------------------------------------------------|
| Resolution    | 320 × 240                                          |
| Pixel format  | 8 bpp indexed (1 byte per pixel)                   |
| Palette       | **256 entries**, 24-bit RGB, fully programmable     |
| Refresh       | 60 Hz, vsync'd                                     |
| Framebuffer   | 76 800 bytes, region named `"fb"`                  |
| Palette RAM   | 1024 bytes (256 × u32 little-endian, `0x00RRGGBB`) |

Every one of the 8 bits of a pixel byte selects a palette entry, so all
256 colours are addressable. (DOOM's renderer writes 8-bit indices into
the framebuffer and swaps the palette for damage/pickup flashes and light
diminishing — this maps directly onto the memory-mapped framebuffer.)

The framebuffer and palette are CronoVM **host-shared regions** named
`"fb"` and `"pal"`, carved out of the cart heap by the loader when the
binary is built with `--region=fb:76800:rw --region=pal:1024:rw`. The
cart resolves their heap offsets at startup via `cron_resolve_video()`
and plots pixels directly through the returned pointers; the drawing
syscalls in §9 are convenience helpers over the same bytes.

> 320 × 240 fully contains DOOM's 320 × 200 view plus a 40-px status bar.

## 3. Cartridge ROM (bundled assets)

A cartridge can carry a read-only data blob — its **ROM** — baked into
the `.bin` at build time. This is how a cart ships multi-megabyte assets
(a DOOM WAD, level packs, sample banks) without compiling them into giant
C arrays.

| Property        | Value                                                  |
|-----------------|--------------------------------------------------------|
| Contents        | arbitrary bytes supplied at build time (`--rom=FILE`)  |
| Visibility      | read-only; mapped into the cart's address space        |
| Access (cart)   | `cron_rom()` → pointer, `cron_rom_size()` → byte count  |
| Access (host)   | `cvm_image` ROM offset/size (host can pre-fill it too) |

The ROM lives inside the cart's addressable memory, so the cart reads it
with ordinary pointer loads (CronoVM `LDB`/`LDW`, bounds-checked). It is
read-only by convention — the VM enforces only heap bounds. A DOOM port
points its WAD reader at `cron_rom()` and never touches a filesystem.

Built with:

```sh
cvm-cc ... --rom=doom1.wad game.c -o doomopio.bin
```

## 4. Audio

| Property        | Value                                          |
|-----------------|------------------------------------------------|
| Channels        | 4 tone + (planned) PCM mixing                  |
| Sample rate     | 22 050 Hz                                      |
| Output          | 16-bit signed stereo, mixed by host            |
| Per-channel ops | tone (sine/square/triangle/noise), volume, pan |

v0.2 ships the 4-channel tone synth. **PCM sample playback** (needed for
DOOM SFX) is the next audio milestone — see §8 reserved range `0x200`.
Music (MUS/MIDI) is a cart-side concern once PCM lands.

## 5. Input

| Device       | Value                                                   |
|--------------|---------------------------------------------------------|
| Gamepads     | 2 virtual, 8 buttons each: D-pad U/D/L/R + A/B/X/Y      |
| Keyboard     | exposed as a bitmap of scancodes (DOOM needs this)      |
| Mouse        | x/y + 2 buttons                                         |

## 6. Persistence

A 1 KiB save slot per cartridge, read/written via syscall. The host
decides where it lives (file on desktop, IndexedDB on web, NVS on
embedded).

## 7. Targets

| Target    | Backend                | Status      |
|-----------|------------------------|-------------|
| Desktop   | SDL2                   | working     |
| Web       | SDL2 via Emscripten    | working     |
| Embedded  | bare-metal HAL         | planned     |

All targets share `host/common/`; per-target code is the platform shell
(window, audio device, file I/O, main loop). Note the 32 MiB RAM baseline
makes the embedded target a stretch goal — small carts still run there;
DOOM-class carts assume a desktop/web-class host.

## 8. Host ↔ VM contract

| Feature                  | Where                        | Purpose                                      |
|--------------------------|------------------------------|----------------------------------------------|
| `cvm_load` / `cvm_run`   | CronoVM public API           | Load and bootstrap the cart                  |
| `cvm_link` (by name)     | CronoVM public API           | Bind `cvm_sys_*` syscalls to host handlers   |
| `cvm_image_get_region`   | CronoVM public API           | Host-side fb/pal offset lookup               |
| `cvm_call`               | CronoVM (added for Cronopio) | Re-enter the VM at a FUNCS index every frame |
| `cvm_sys_rom_base/size`  | CronoVM (added for Cronopio) | Cart-side cartridge-ROM discovery            |

Cart-facing syscall names follow CronoVM's `cvm_sys_*` prefix convention;
the `cron_*` names in `cronopio.h` are thin static-inline aliases.

## 9. Performance note

CronoVM is a bytecode interpreter (no JIT). The v0.2 plan is to port DOOM
with **all rendering in bytecode** and measure real framerate before
optimising. If the software rasteriser's hot loops (column/span drawing)
prove too slow, the escape hatch is to expose them as host syscalls
(native C `R_DrawColumn`/`R_DrawSpan` over the framebuffer region) — the
"rasteriser as host primitives" model. Deferred until measured.

## 10. Versioning

This document is **v0.2**, a draft. The host ABI is not yet frozen.
v1 will freeze the host ABI and the cart's view of memory. The jump from
v0 (32 colours / 1 MiB) to v0.2 (256 colours / 32 MiB / cart ROM) is the
"make it run DOOM" revision.
