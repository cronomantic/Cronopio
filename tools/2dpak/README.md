# 2dpak

Convert an 8-bit indexed BMP into a Cronopio cart C header (palette +
pixel data + dimension macros). The cart `#include`s the generated
header and registers via `cron_image` + `cron_palette_set`.

## Why BMP-8 (not PNG)

PNG-8 would require ~700 lines of careful DEFLATE + framing code or a
~5 KLoC vendored decoder. BMP-8 is a 50-line parser that any paint tool
exports natively. The artist adds one ImageMagick step to their pipeline;
the toolchain stays tiny and dependency-free.

## Workflow

```
# 1. Author the tileset in your paint tool (GIMP / Krita / Aseprite),
#    save as PNG (or whatever). Use an indexed palette of <= 32 colours.

# 2. Convert to BMP-8 (BMP3 = simplest variant):
magick tileset.png BMP3:tileset.bmp

# 3. Pack into a C header:
2dpak tileset.bmp tileset.h
#    -> writes tileset.h with TILESET_pal[32], TILESET_pix[W*H],
#       TILESET_W, TILESET_H, TILESET_PAL_COUNT.

# 4. In the cart:
#include "tileset.h"

int main(void) {
    cron_resolve_video();
    /* Install the asset's palette. cron_palette_set takes 0x00RRGGBB. */
    for (int i = 0; i < TILESET_PAL_COUNT; ++i)
        cron_palette_set(i, TILESET_pal[i]);
    /* Register the pixel data as image bank 0. */
    cron_image(0, TILESET_pix, TILESET_W, TILESET_H);
    /* ... now cron_blt / cron_bltm reference bank 0. */
    cron_set_frame(frame);
    return 0;
}
```

## Options

```
2dpak <input.bmp> <output.h> [--prefix=NAME] [--max-pal=N]
```

- `--prefix=NAME` — sets the C symbol prefix. Default: derived from the
  output filename's basename (e.g. `tileset.h` → `TILESET`).
- `--max-pal=N` — clamps palette emission. Default 32 (Cronopio's
  palette size). Set higher if your cart manages a larger working set
  off-screen.

## Self-test

```
2dpak --selftest test.bmp     # writes a known 32x16 BMP-8 with 4 colours
2dpak test.bmp test.h         # converts it; inspect test.h to confirm
```

## Limits / what's NOT done yet

- **Input is BMP-8 only.** Reject everything else with a clear message.
  PNG support could be added by vendoring `stb_image.h` — deferred.
- **No tilemap import.** For small maps the cart hand-writes the u16
  array; for big maps a future `--tilemap=tiled.tmx` mode would parse
  Tiled XML. Add when first port hits a 50+ cell map.
- **No animation table import.** Same reasoning — hand-write small
  `cron_tile_anim_t` tables until the first port asks for it.
- **No quantization.** Input must be already-indexed with <= 32 colours.
  GIMP "Image → Mode → Indexed (palette: 32 colours)" does the right
  thing in one click.
- **Output is a header, not a ROM blob.** When asset size starts
  bloating the .c (multi-MB), add a `--rom` mode that writes a binary
  consumable by `--rom=FILE` at cart-build time. Until then the C
  literal is fine and integrates naturally with `cvm-cc`.
