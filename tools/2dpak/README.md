# 2dpak

Convert an 8-bit indexed image (PNG-8 or BMP-8) into a Cronopio cart C
header (palette + pixel data + dimension macros). The cart `#include`s
the generated header and registers via `cron_image` + `cron_palette_set`.

## Supported inputs

| Format | Path | Notes |
|---|---|---|
| **PNG-8 (indexed)** | stb_image (vendored, public domain) for IDAT decode + hand-rolled PLTE chunk parser for palette ordering | Preserves the artist's authored palette ordering (stb_image alone decodes to RGB and drops PLTE). Truecolour PNGs are rejected with a clear message. |
| **BMP-8** | Native ~50-line parser (uncompressed BI_RGB, palette in BITMAPINFOHEADER). Top-down or bottom-up rows both supported. | Used as fallback when PNG isn't convenient. |

Auto-detected by magic bytes (`89 50 4E 47` for PNG, `42 4D` for BMP).

## Workflow

```
# 1. Author the tileset in your paint tool (GIMP / Krita / Aseprite),
#    save as PNG (indexed mode) — use an indexed palette of <= 32 colours.
#    GIMP: Image > Mode > Indexed (palette: 32 colours).
#    Aseprite: native indexed mode.

# 2. Pack into a C header (PNG-8 or BMP-8 both work):
2dpak tileset.png tileset.h
#    -> writes tileset.h with TILESET_pal[32], TILESET_pix[W*H],
#       TILESET_W, TILESET_H, TILESET_PAL_COUNT.

# OR if you've already converted to BMP-8:
2dpak tileset.bmp tileset.h

# 3. In the cart:
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

- **No tilemap import.** For small maps the cart hand-writes the u16
  array; for big maps a future `--tilemap=tiled.tmx` mode would parse
  Tiled XML. Add when first port hits a 50+ cell map.
- **No animation table import.** Same reasoning — hand-write small
  `cron_tile_anim_t` tables until the first port asks for it.
- **No quantization.** Input must be already-indexed with <= 32 colours.
  GIMP "Image → Mode → Indexed (palette: 32 colours)" does the right
  thing in one click; same with Aseprite's native indexed mode. A
  truecolour PNG is rejected with a clear message.
- **Output is a header, not a ROM blob.** When asset size starts
  bloating the .c (multi-MB), add a `--rom` mode that writes a binary
  consumable by `--rom=FILE` at cart-build time. Until then the C
  literal is fine and integrates naturally with `cvm-cc`.
