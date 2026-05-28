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
2dpak <input.{png,bmp}> <output.h> [--prefix=NAME] [--max-pal=N] [--rom]
2dpak --tilemap <input.tmx> <output.h>           [--prefix=NAME] [--rom]
2dpak --selftest <output.bmp>
```

- `--prefix=NAME` — sets the C symbol prefix. Default: derived from the
  output filename's basename (e.g. `tileset.h` → `TILESET`). A leading
  digit gets a `_` prepended to keep the C identifier valid.
- `--max-pal=N` — clamps palette emission and is the target palette
  count for the median-cut quantizer (when input is truecolour). Default
  32 (Cronopio's palette size). Set higher if your cart manages a larger
  working set off-screen.
- `--rom` — write a binary blob (`2DPK` magic for images, `TMAP` magic
  for tilemaps) instead of a C header. Pair with `cvm-cc --rom=FILE`
  at cart-build time so `cron_rom()` returns the blob; parse via
  `sdk/include/cron_2dpak.h` (`cron_2dpak_parse_image` /
  `cron_2dpak_parse_tilemap`). Use this once your asset bloats the .c
  (typically past a couple hundred KB of pixels).
- `--tilemap` — input is a Tiled (.tmx) map exported with CSV layer
  format (Edit → Preferences → "Tile Layer Format: CSV"). GIDs are
  remapped to Cronopio's 14-bit indices (Tiled is 1-based, we're
  0-based; flip bits in GID bits 30/31 become the cell's bits 14/15).
  Anti-diagonal flip (bit 29) isn't representable and emits a warning.

## Self-test

```
2dpak --selftest test.bmp     # writes a known 32x16 BMP-8 with 4 colours
2dpak test.bmp test.h         # converts it; inspect test.h to confirm
```

## Limits / what's NOT done yet

- **No animation table import.** Hand-write small `cron_tile_anim_t`
  tables until the first port asks for it. Tiled has tile-animation
  metadata in `.tsx` files — could be parsed when needed.
- **Tiled CSV encoding only.** Base64 + zlib/gzip needs DEFLATE; only
  the CSV encoding is supported. Export your TMX with "Tile Layer
  Format: CSV" (Tiled's Preferences). Other encodings emit a clear
  "re-export as CSV" message.
- **Tiled anti-diagonal flip not supported.** GID bit 29 (rotated 90°)
  isn't representable in Cronopio's HFLIP/VFLIP-only cell layout.
  Emits a warning and renders without flip; in practice arcade-style
  maps rarely use 90° rotation.
- **Multi-asset ROM blobs.** Each `--rom` output holds ONE asset (one
  image OR one tilemap). For carts with many assets, write multiple
  blobs and concatenate them into a single `cron_rom`-loadable file
  (or wait for a future `--bundle` mode that adds a chunk index).
- **Quantization is per-image.** When you `2dpak` two RGB PNGs that
  should share a palette, the median-cut runs independently for each
  and you'll get different palette entries. Pre-quantize to a shared
  indexed PNG (GIMP "Image → Mode → Indexed → use existing palette"
  or `magick +remap`) and pipe both through the indexed path.
