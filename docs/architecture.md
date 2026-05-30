# Cronopio — Architecture

```
            ┌───────────────────────────────────────────────────┐
            │                   cartridge.bin                   │
            │   (C source → cvm-cc → CronoVM .bin via LLVM)     │
            └───────────────────────────────────────────────────┘
                                    │
                                    ▼
       ┌───────────────────────────────────────────────────────────┐
       │                       Cronopio host                       │
       │                                                           │
       │   ┌────────────────┐   ┌─────────────────────────────┐    │
       │   │   CronoVM      │◀─▶│   host/common (portable)    │    │
       │   │   (libcvm)     │   │  • console state            │    │
       │   │   cvm_run()    │   │  • syscall dispatch table   │    │
       │   │   cvm_link()   │   │  • framebuffer 320×240×8    │    │
       │   │                │   │  • software audio mixer     │    │
       │   └────────────────┘   │  • input state              │    │
       │                        └─────────────────────────────┘    │
       │                                    │                      │
       │              ┌─────────────────────┼─────────────────┐    │
       │              ▼                     ▼                 ▼    │
       │   ┌──────────────────┐ ┌────────────────────┐ ┌─────────┐ │
       │   │ host/desktop     │ │ host/web           │ │embedded │ │
       │   │ SDL2 window,     │ │ Emscripten + SDL2  │ │ HAL     │ │
       │   │ audio, files     │ │ → canvas/WebAudio  │ │(stub)   │ │
       │   └──────────────────┘ └────────────────────┘ └─────────┘ │
       └───────────────────────────────────────────────────────────┘
```

## Component responsibilities

**CronoVM** (`external/CronoVM`, MIT) — the execution engine.
Embedded as a CMake subdirectory. We only consume its public API
(`cvm_load`, `cvm_run`, `cvm_link`, heap accessors). We do not patch it.

**`host/common/`** — portable runtime. Owns the console state
(framebuffer, palette, audio voices, input snapshot), the syscall
dispatch table, and the per-frame driver. Knows nothing about
windows or audio devices.

**`host/desktop/`** — SDL2 platform shell. Creates the window,
opens the audio device, polls events, copies the host framebuffer to
a streaming texture, calls into the common runtime per frame. Video is
configurable (persisted in `cronopio.cfg`, set on the CLI, or in the F1
menu): integer window **scale** 1–6, **fullscreen** (the 320×240 image is
letterboxed 4:3), **vsync** (F1 → Video), and the **save folder** for
`<cart>.sav` files (F1 main menu toggles beside-cart ↔ a host `saves/` dir).
CLI: `cronopio[.exe] [cart] [--scale=N] [--fullscreen|--windowed]
[--vsync|--no-vsync] [--saves=DIR]` (DIR relative = under the host dir, absolute = verbatim).

**`host/web/`** — same shell built through Emscripten. Reuses
`host/common/` and most of the SDL2 code; differs only in main loop
(`emscripten_set_main_loop`) and file I/O (IndexedDB).

**`host/embedded/`** — TBD. Will replace SDL2 with a thin HAL
(framebuffer flush, I²S/PWM audio, GPIO buttons).

**`sdk/`** — what cart authors include.
`cronopio.h` declares the host ABI as `cvm_sys_cron_*` extern decls
(CronoVM's translator convention); the cart never sees a separate
stub file. The user-friendly `cron_*` names are `static inline`
aliases in the same header. The framebuffer and palette become
ordinary C pointers after a one-call `cron_resolve_video()` at
startup. Bundled assets (a DOOM WAD, etc.) are baked in with
`cvm-cc --rom=FILE` and read through `cron_rom()` / `cron_rom_size()`,
which front the CronoVM `cvm_sys_rom_*` built-ins.
The C library is **picolibc** (`runtime/lib/build_picolibc.sh` →
`picolibc.bc`, a CronoVM-side artifact): it owns the standard string /
mem / ctype / stdlib / numeric surface. `sdk/lib/cron_sys.c` is the
Cronopio **machine port + platform layer** that a cart links alongside
picolibc — `errno`, process control (`exit`/`abort`/`assert`), the
stdio + FS layer routed to cron syscalls and the cart ROM, and the few
classifiers/strings picolibc's curated build omits. (picolibc's `mem*`
still lower to CronoVM's single `MEMSET`/`MEMCPY`/`MEMMOVE` opcodes, so
the memory-heavy-cart win is unchanged.)

**Allocator (per-cart choice).** By default the canonical
`malloc`/`free`/`calloc`/`realloc` are **picolibc's**, backed by a
`sbrk` over the cron heap in `cron_sys.c`. picolibc's nano-malloc has an
**O(n) free** (its free-list insert walks the list), which is slow for
workloads that keep many small blocks live — e.g. UQM's 10559-entry ZIP
content mount (~9 s). Building a cart with **`-DCRON_LIBC_TUNED_MALLOC`**
(and `picolibc.bc` via `build_picolibc.sh --no-malloc`) instead makes
the Cronopio **tuned O(1)-free allocator** (`cvm_alloc.h`) the canonical
`malloc` — recovering UQM's mount to ~1 s. The two allocators both draw
from the same cron heap, so a cart uses exactly one: UQM selects the
tuned one (`build_uqm.sh`), DOOM/Quake keep picolibc's. The unused
family is dead-code-eliminated.

`sdk/include/coro.h` exposes the cart-facing API for cooperative
coroutines on top of CronoVM's `CVM_OP_CORO_SWAP` opcode:
`cron_coro_init(coro)` sets up a fresh context on a user-owned stack;
`cron_coro_swap(from, to)` atomically saves the calling context into
`from` and resumes `to`; `cron_coro_yield(self)` is a convenience that
swaps back to whoever last resumed `self`. The default trampoline
`__cron_coro_trampoline` lives in `sdk/lib/cron_sys.c` and runs
`self->fn(self->arg)` on the new stack, transitioning the coroutine's
status FRESH → RUNNING → DEAD before swapping back to the resumer. This
is what the cronopio-uqm port uses to retrofit Ur-Quan Masters'
preemptive `libs/threads/` abstraction into Cronopio's single
`cron_frame()`-per-frame model; carts can also use the primitive
directly to implement Lua-style generators, async/await desugarings,
or any other cooperative-concurrency scheme.

### The per-scanline table pattern

The 2D capability set (`cron_bltm_raster`, `cron_bltm_affine`) and the 3D
accelerated renderer (`cron_polys`, `cron_xform_polys`) share an
architectural shape: the cart owns a buffer of per-element parameters
in its heap, the host reads that buffer in a tight inner loop, and the
whole thing is a single syscall per primitive. No per-scanline VM
round-trips, no host-side state to keep in sync between calls.

For 2D, this lets a tilemap blit carry SNES PPU-style HDMA effects
(linescroll, palette gradients) and Mode-7 perspective floors in one
host call — the cart fills a 240-entry table once per frame (typically
via a sin/cos lookup or a perspective formula) and forgets. The format
is small and aligned for direct indexing (`cron_raster_t` is 8 bytes,
`cron_affine_t` is 16 bytes; the entry index is the destination y).

For 3D, `cron_polys` reads N vertices from the same kind of buffer; the
cart submits one batch, the host walks it. The pattern generalises: any
"the cart submits a list, the host rasters" primitive looks the same.
This is the cleanest way to add expressive features without bloating
the syscall count or imposing host-side state. It also means cart code
that mixes 2D and 3D needs no special integration — both flow through
the cart's `frame()` callback in whatever order the cart picks.

**`tools/headless/`** — windowless cart runners that link only
`host/common/` (no SDL). `cronopio-headless` drives N frames and prints
a framebuffer histogram (and an optional PPM), for CI / "does this cart
render" checks. It can also **script gamepad input** for headless testing
of interactive carts (menus, gameplay): `--pad=script.txt`, one directive
per line `<frame> <TOKEN>...` (TOKENs: UP DOWN LEFT RIGHT A B X Y L R START
SELECT, or NONE), where the pad is set to the OR of those buttons at that
frame and held until the next directive. The injection happens after
`begin_frame` so `cron_pad_pressed` edge-detection works. (Usage:
`cronopio-headless cart.bin [frames] [out.ppm] [--pad=script]`.)
`cronopio-headless-prof` is the same harness built
against a `-DCVM_PROFILE` CronoVM (see CronoVM `CHANGELOG`): it ranks
functions by interpreter self-time, resolving names from the `CVM_SYMS`
`<cart>.bin.sym` sidecar, with a `warmup` arg to drop startup cost and a
`CVM_PROF_WATCH=<fid>` caller histogram. It is built standalone (not via
CMake) — one `clang -DCVM_PROFILE` over `cvm.c` + `host/common/*.c` +
`headless_prof.c`; the recipe is in the file header.

## Why this layout

- The host ABI is the only contract between cart and console.
  Changing the host implementation never breaks carts; changing the
  ABI is a versioned event.
- One portable runtime, three platform shells — embedded gets the
  same gameplay code that runs on desktop with zero changes.
- LLVM bitcode → CronoVM means cart authors keep writing plain C.
