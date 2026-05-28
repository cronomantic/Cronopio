# Cronopio examples

Each subdirectory is a self-contained cartridge: one `.c` file plus a
`CMakeLists.txt` that calls `cronopio_add_cartridge`. All assets are built
procedurally at boot, so there are no external files to ship.

| Example | What it shows |
| --- | --- |
| [hello](hello/) | Minimal cartridge. The two drawing styles — direct framebuffer writes and helper syscalls (`cron_cls`/`cron_rect`/`cron_text`). Start here. |
| [sprites](sprites/) | The Pyxel-style 2D layer: image banks, tilemaps, `blt` with colour-key transparency and flipping, `bltm` scrolling, plus a shapes/text HUD. Move the sprite with the d-pad. |
| [cube](cube/) | The 3D pipeline end to end. Cart-side maths (`cronopio3d.h`: perspective/lookat/rotation, transform + near-clip + project) feeding the host triangle rasteriser (`cron_polys`, affine texturing, depth buffer). A spinning textured cube. |
| [lit](lit/) | Diffuse lighting through the 3D pipeline. Per-vertex normals dotted with a fixed light pick an index into a dark→bright palette ramp; `CRON_POLY_GOURAUD` interpolates it across each face. A shaded spinning cube. |
| [mouse](mouse/) | The mouse API end to end: absolute position, accumulated deltas, wheel, 5-button mask (L/R/M/X1/X2), `cron_cursor(0)` to hide the OS cursor + draw your own, and `cron_mouse_relative` for SDL mouselook mode. A pixel-art paint scratchpad — LEFT to paint, RIGHT to clear, wheel to cycle colour, MIDDLE to toggle relative mode. |

## Building

Examples are off by default. Configure with them enabled:

```sh
cmake -S . -B build -DCRONOPIO_BUILD_EXAMPLES=ON
cmake --build build
```

Each cartridge builds to `build/examples/<name>/<name>.bin`. The build uses
the `cronopio-cc` it just compiled in-tree (no install needed).

## Running

On the desktop host (opens a window):

```sh
build/host/desktop/cronopio build/examples/cube/cube.bin
```

Headless, for a quick render check without a window — prints a histogram of
framebuffer palette indices after N frames:

```sh
build/tools/headless/cronopio-headless build/examples/sprites/sprites.bin 3
```

## See also

- [docs/cartridge.md](../docs/cartridge.md) — cartridge anatomy and lifecycle
- [docs/syscalls.md](../docs/syscalls.md) — the full syscall reference
- [docs/3d.md](../docs/3d.md) — the 3D pipeline
- [docs/audio.md](../docs/audio.md) — sound and music
