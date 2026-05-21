# Cronopio

A fantasy video game console.

Cartridges are CronoVM binaries compiled from C. The host runtime
provides video, audio, input and persistence through a fixed table
of CronoVM syscalls (the **host ABI**). The same runtime runs on
desktop (SDL2), web (Emscripten) and embedded hardware (HAL).

```text
              ┌─────────────────┐
   C source ──▶│     cvm-cc      │── cartridge.bin ──▶ Cronopio host
              └─────────────────┘                       (desktop / web / embedded)
```

## Specs in one paragraph

320 × 240 @ 60 Hz, 8 bpp indexed framebuffer with a 32-colour
programmable palette. 4-channel mixer at 22 050 Hz. Two virtual
8-button gamepads. 1 MiB cart RAM, 1 KiB save slot. Framebuffer
and palette are **CronoVM host-shared regions** carved into the
cart heap at load time, so plotting a pixel is just a byte store
through the pointers returned by `cron_resolve_video()` — no
syscall required. Full ABI in [`docs/syscalls.md`](docs/syscalls.md);
full machine in [`SPEC.md`](SPEC.md).

## Layout

```text
external/CronoVM/   the VM (git submodule, MIT) — host depends on libcvm
host/common/        portable runtime: framebuffer blit, mixer, dispatch
host/desktop/       SDL2 shell
host/web/           Emscripten shell, reuses host/desktop/main.c
host/embedded/      bare-metal HAL (stub for now)
sdk/include/        cronopio.h — what cart authors include
sdk/lib/            syscall stubs cart authors link against
sdk/cmake/          CronopioCart.cmake — CMake helper for carts
examples/hello/     minimal example cartridge
```

## Build

CronoVM is consumed as a submodule. After cloning:

```sh
git submodule add https://github.com/cronomantic/CronoVM external/CronoVM
git submodule update --init --recursive
```

### Desktop

```sh
cmake -B build -S .
cmake --build build
./build/host/desktop/cronopio path/to/cart.bin
```

Requires SDL2 (`libsdl2-dev` on Debian/Ubuntu, `brew install sdl2`
on macOS, `vcpkg install sdl2` on Windows).

### Web

```sh
emcmake cmake -B build-web -S . -DCRONOPIO_TARGET_DESKTOP=OFF -DCRONOPIO_TARGET_WEB=ON
cmake --build build-web
# open build-web/host/web/cronopio.html
```

### Embedded

Not wired up yet — see [`host/embedded/README.md`](host/embedded/README.md)
for the HAL contract.

## Building a cartridge

```sh
cvm-cc -I sdk/include \
       --heap-reserve=1M --stack-reserve=64K \
       --region=fb:76800:rw --region=pal:1024:rw \
       examples/hello/hello.c -o hello.bin
```

The `--region` flags carve the framebuffer and palette out of the
cart's heap; the host looks them up by name at load time. Skipping
them produces a runnable cart that just can't draw anything.

Or, from a CMake project:

```cmake
include(${CRONOPIO_DIR}/sdk/cmake/CronopioCart.cmake)
cronopio_add_cartridge(my_game SOURCES src/main.c src/world.c)
```

## Status

v0 draft. The host ABI is **not yet stable**. See
[`SPEC.md`](SPEC.md) for what's frozen vs. open, and
[`docs/architecture.md`](docs/architecture.md) for the design.

## License

MIT.
