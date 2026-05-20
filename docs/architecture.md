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
a streaming texture, calls into the common runtime per frame.

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
startup.

## Why this layout

- The host ABI is the only contract between cart and console.
  Changing the host implementation never breaks carts; changing the
  ABI is a versioned event.
- One portable runtime, three platform shells — embedded gets the
  same gameplay code that runs on desktop with zero changes.
- LLVM bitcode → CronoVM means cart authors keep writing plain C.
