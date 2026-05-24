# Writing a Cronopio cartridge

A cartridge is a CronoVM binary compiled from C, by convention with the
`.crom` extension (Cronopio ROM; `.bin` is still accepted). You include
`<cronopio.h>` (and, for 3D, `<cronopio3d.h>`), write a frame callback, and the
host drives it at 60 Hz. This page is the front door; the full syscall list is
in [`syscalls.md`](syscalls.md).

## Quickstart

```sh
cronopio-cc new mygame      # scaffold mygame/{main.c,CMakeLists.txt,README.md,.gitignore}
cd mygame
cronopio-cc main.c -o mygame.crom
cronopio mygame.crom        # run it
```

## Metadata (title / author / controls)

Tag a cart so the launcher can show what it is and how to play it — pass at
build time:

```sh
cronopio-cc main.c -o mygame.crom \
  --title="My Game" --author="Me" --controls="D-pad move | A jump | B shoot"
```

These are baked into a host-only `CVM_SEC_META` section (not visible to the
running program). The desktop launcher reads it without loading the cart: the
file browser lists the title and shows the author + controls of the focused
cart.

`new` picks a starting point — `--template=basic` (default), `sprites`, or
`3d`; run `cronopio-cc new --list` to see them, or omit `--template` on a
terminal to be prompted.

`cronopio-cc` is the cartridge compiler: it bakes in the Cronopio memory map
and SDK include path so you don't pass `--region`/`-I` flags by hand. (Under
the hood it drives `cvm-cc`, the generic CronoVM compiler.)

## Anatomy

```c
#include <cronopio.h>

static void setup(void) {          /* runs once at boot */
    /* ... load resources: cron_image / cron_sample / cron_sf2_load ... */
}

static void frame(void) {          /* called every 1/60 s */
    cron_cls(0);                   /* clear to palette index 0 */
    /* ... read input, update state, draw ... */
}

CRONOPIO_CART_INIT(setup, frame)   /* generates main() + CRON_FB/CRON_PAL */
```

`CRONOPIO_CART_INIT(setup, frame)` writes the boilerplate every cart needs:
storage for the `CRON_FB`/`CRON_PAL` pointers, a `main()` that resolves the
video regions, runs `setup` once, and registers `frame`. Use `CRONOPIO_CART(frame)`
if you have no setup pass. The host then calls `frame` every tick (via
CronoVM's `cvm_call`), polling input and presenting the framebuffer around it.

If you need full control of `main` (no frame callback, custom error handling),
define `CRON_FB`/`CRON_PAL` and `main()` by hand instead — see
[`examples/hello`](../examples/hello/hello.c).

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
bitmasks for the 12-button pad (`CRON_BTN_UP…CRON_BTN_Y`, plus `CRON_BTN_L`,
`CRON_BTN_R`, `CRON_BTN_START`, `CRON_BTN_SELECT`). `cron_mouse(&x,&y)` is also
available. The console exposes only the abstract pad — a host may drive it from
a keyboard/controller, but carts read input solely through the pad.

## Audio

- SFX: `cron_snd_tone(v, wave, freq_mhz, vol, pan)` for synth, or
  `cron_sample`/`cron_sample_u8` + `cron_pcm(v, slot, pitch, vol, pan, loop)`
  for PCM. `cron_env` adds an ADSR envelope.
- Music: MIDI through the host SoundFont synth — `cron_midi_send(...)` (and
  optionally `cron_sf2_load` for a custom bank). Or render your own PCM and push
  it with `cron_stream`. See [`audio.md`](audio.md).

## 3D

Transform/project/light in `cronopio3d.h`, submit triangles with
`cron_polys` (flat / Gouraud / textured, optional z-buffer). See
[`3d.md`](3d.md) and `examples/cube`, `examples/lit`.

## Bundling assets (cart ROM)

Big read-only data (sprite sheets, a WAD, a `.mod`) goes in the cart ROM:

```sh
cronopio-cc --rom=assets.bin game.c -o game.bin
```

Read it with `cron_rom()` / `cron_rom_size()`. Small assets can just be
`static const` arrays compiled into the cart.

## Building & running

The one-line path:

```sh
cronopio-cc game.c -o game.bin
cronopio game.bin
```

For a project with its own build, scaffold one and build it with CMake — it
finds the installed SDK via `find_package(Cronopio)`:

```sh
cronopio-cc new game
cd game
cmake -B build -DCMAKE_PREFIX_PATH=<cronopio-install-prefix>
cmake --build build        # -> build/game.bin
```

`cronopio_add_cartridge(game SOURCES game.c)` is the CMake helper (it picks up
the reserves and routes through `cronopio-cc`; see
`sdk/cmake/CronopioCart.cmake`). Installing the SDK (`cmake --install`) is
covered in [`BUILDING.md`](BUILDING.md), along with the Windows/msys2 gotcha.
The same `.bin` runs on the desktop and web hosts.
