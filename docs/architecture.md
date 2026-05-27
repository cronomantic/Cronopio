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
`sdk/lib/cvm_libc.c` is the bundled freestanding libc; its block
primitives (`memset`/`memcpy`/`memmove`) are written with
`__builtin_mem*` so the translator lowers them to CronoVM's single
`MEMSET`/`MEMCPY`/`MEMMOVE` opcodes (one host call each) rather than a
per-byte VM loop — a large win for memory-heavy carts.

**`tools/headless/`** — windowless cart runners that link only
`host/common/` (no SDL). `cronopio-headless` drives N frames and prints
a framebuffer histogram (and an optional PPM), for CI / "does this cart
render" checks. `cronopio-headless-prof` is the same harness built
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
