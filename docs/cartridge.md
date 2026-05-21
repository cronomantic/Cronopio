# Writing a Cronopio cartridge

A cartridge is a CronoVM `.bin` compiled from C with `cvm-cc`. You include
`<cronopio.h>` (and, for 3D, `<cronopio3d.h>`), write a frame callback, and
the host drives it at 60 Hz. This page is the front door; the full syscall
list is in [`syscalls.md`](syscalls.md).

## Anatomy

```c
#include <cronopio.h>

/* The framebuffer/palette pointers the SDK fills in. Define them once. */
volatile uint8_t  *CRON_FB  = 0;
volatile uint32_t *CRON_PAL = 0;

static void frame(void) {          /* called every 1/60 s */
    cron_cls(0);                   /* clear to palette index 0 */
    /* ... read input, update state, draw ... */
}

int main(void) {                   /* runs once at boot */
    cron_resolve_video();          /* resolve CRON_FB / CRON_PAL */
    /* ... load resources: cron_image / cron_sample / cron_mod_play ... */
    cron_set_frame(frame);         /* register the per-frame callback */
    return 0;
}
```

The boot `main` runs once: resolve video, register resources, set the frame
function, return. The host then calls `frame` every tick (via CronoVM's
`cvm_call`), polling input and presenting the framebuffer around it.

## Drawing

The framebuffer is 320×240, 8 bpp into a **256-colour** palette. Two ways to
draw, freely mixed:

- **Direct**: `CRON_FB[y*CRON_SCREEN_W + x] = color;` — the fast path.
- **Helpers**: `cron_cls/pset/rect/rectb/line/circ/tri/fill/text`, sprite
  blits `cron_blt`/`cron_bltm`, and the draw state `cron_clip`/`cron_camera`/
  `cron_pal` (these affect every helper). See the 0x020 + 0x100 syscalls.

Palette entries are `cron_palette_set(i, 0x00RRGGBB)`.

## Sprites & tilemaps

Register a sheet/grid that lives in your own memory (RAM or cart ROM), then
blit from it:

```c
cron_image(0, sheet, 64, 64);                 /* image bank 0 = 64x64 8bpp */
cron_blt(0, x, y, sx, sy, w, h, /*colkey*/0); /* colkey -1 = opaque; -w/-h flips */
cron_tilemap(0, cells, 32, 32, /*img*/0);     /* 32x32 grid of u16 tile indices */
cron_bltm(0, x, y, sx, sy, w, h, 0);          /* draw a pixel window of the map */
```

## Input

`cron_pad(0)` / `cron_pad_pressed(0)` / `cron_pad_released(0)` return button
bitmasks (`CRON_BTN_UP…CRON_BTN_Y`). `cron_key(scancode)` and
`cron_mouse(&x,&y)` are also available.

## Audio

- SFX: `cron_snd_tone(v, wave, freq_mhz, vol, pan)` for synth, or
  `cron_sample`/`cron_sample_u8` + `cron_pcm(v, slot, pitch, vol, pan, loop)`
  for PCM. `cron_env` adds an ADSR envelope.
- Music: a ProTracker module — `cron_mod_play(mod, len, loop)`. Or render
  your own and push it with `cron_stream`. See [`audio.md`](audio.md).

## 3D

Transform/project/light in `cronopio3d.h`, submit triangles with
`cron_polys` (flat / Gouraud / textured, optional z-buffer). See
[`3d.md`](3d.md) and `examples/cube`, `examples/lit`.

## Bundling assets (cart ROM)

Big read-only data (sprite sheets, a WAD, a `.mod`) goes in the cart ROM:

```sh
cvm-cc ... --rom=assets.bin game.c -o game.bin
```

Read it with `cron_rom()` / `cron_rom_size()`. Small assets can just be
`static const` arrays compiled into the cart.

## Building & running

```sh
cvm-cc -I sdk/include \
       --region=fb:76800:rw --region=pal:1024:rw \
       game.c -o game.bin
./build/host/desktop/cronopio game.bin
```

Or use the CMake helper: `cronopio_add_cartridge(game SOURCES game.c)` (it
adds the region flags and reserves; see `sdk/cmake/CronopioCart.cmake`).
Toolchain setup and the Windows/msys2 gotcha are in [`BUILDING.md`](BUILDING.md).
The same `.bin` runs on the desktop and web hosts.
