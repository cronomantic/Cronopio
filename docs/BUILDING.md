# Building Cronopio

## The toolchain gotcha (Windows, dual LLVM installs)

`cvm-translate` (CronoVM's bitcode→bytecode translator) is C++ and links
against LLVM's dev libraries, which it finds via `llvm-config`. The
**compiler that builds it and the LLVM that `llvm-config` points at must
come from the same distribution.**

On this machine there are two LLVM toolchains:

| Distribution                              | Has `clang`/`clang++` | Has `llvm-config` | Default C++ stdlib |
|-------------------------------------------|-----------------------|-------------------|--------------------|
| scoop `mingw-mstorsjo-llvm-ucrt`          | yes (the default `cc`)| **no**            | libc++             |
| msys2 `ucrt64`                            | yes                   | **yes**           | libstdc++          |

If you let CMake pick the default compiler (scoop's, libc++) but the only
`llvm-config` on PATH is msys2's, the build adds
`-isystem C:/msys64/ucrt64/include` for the LLVM headers. That path shadows
libc++'s own header wrappers, and libc++'s `#include_next <wctype.h>`
(and `<wchar.h>`, `<cstdio>`, `<cerrno>`…) fails its self-check with:

```
<cwctype> tried including <wctype.h> but didn't find libc++'s <wctype.h> header.
```

The C-only parts (the `cvm` interpreter, the host runtime, the unit tests)
build fine regardless — only the C++ translator hits this.

### Fix: pair the compiler with its LLVM

Use msys2's clang, which pairs with msys2's `llvm-config` and uses
libstdc++ (no libc++ self-check, no shadowing). The repo ships a preset:

```sh
cmake --preset msys2-clang
cmake --build --preset msys2-clang
```

Or pass the compilers explicitly:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_C_COMPILER=C:/msys64/ucrt64/bin/clang.exe \
  -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/clang++.exe
```

The same applies when building CronoVM standalone under
`external/CronoVM/`.

## Building a cartridge end-to-end

The Cronopio build produces `cronopio-cc` (the cartridge compiler) at
`build/tools/cronopio-cc/cronopio-cc.exe`. It bakes in the memory map and SDK
include path, so compiling a cart is one line:

```sh
build/tools/cronopio-cc/cronopio-cc.exe examples/hello/hello.c -o hello.bin
```

In-tree, `cronopio_add_cartridge()` finds `cronopio-cc` automatically (it
prefers the build target), so `cmake --build build` builds every example with
no PATH setup.

## Installing the SDK (for standalone cart projects)

To author carts outside this repo, install the SDK to a prefix:

```sh
cmake --install build --prefix ~/cronopio
```

That lays down `bin/{cronopio,cronopio-cc,cvm-cc,cvm-translate}`,
`include/cronopio*.h`, the runtime headers under `share/`, and a CMake package
under `lib/cmake/Cronopio`. A standalone cart project then needs only:

```cmake
find_package(Cronopio REQUIRED)
cronopio_add_cartridge(mygame SOURCES main.c)
```

configured with `-DCMAKE_PREFIX_PATH=~/cronopio`. Put `~/cronopio/bin` on PATH
to call `cronopio-cc` / `cronopio` directly. `cronopio-cc new <name>`
scaffolds such a project.

## Running (desktop)

The desktop host needs SDL2. With SDL2 discoverable by CMake the build
produces `build/host/desktop/cronopio[.exe]`:

```sh
./build/host/desktop/cronopio hello.bin
```

Without SDL2 the desktop target is skipped (by design); the portable
runtime `cronopio_common` and CronoVM still build.

## Web (Emscripten)

The web host reuses `host/desktop/main.c` — under `__EMSCRIPTEN__` it drives
the frame loop with `emscripten_set_main_loop` (a requestAnimationFrame
callback) instead of the native blocking `while`. Emscripten ships an SDL2
port, so no separate SDL install is needed.

Requires the Emscripten SDK. On this machine it ships with msys2 under
`C:/msys64/ucrt64/lib/emscripten/`. **PATH order matters**: put
`C:/msys64/ucrt64/bin` *before* any scoop/Git mingw `bin` directories, or
emcc's bundled clang fails to start with a `0xC0000139`
(STATUS_ENTRYPOINT_NOT_FOUND) — it's a GCC-built binary that otherwise
loads incompatible `libstdc++-6.dll` / `zlib1.dll` / `libzstd.dll` from
winlibs or Git's mingw.

```sh
export PATH="/c/msys64/ucrt64/bin:/c/msys64/ucrt64/lib/emscripten:$PATH"
emcmake cmake -B build-web -S . -G Ninja -DCRONOPIO_TARGET_DESKTOP=OFF -DCRONOPIO_TARGET_WEB=ON
cmake --build build-web --target cronopio_web
```

This emits `build-web/host/web/cronopio.{html,js,wasm}`. The page fetches a
cartridge named `cart.bin` from the same directory at load time, so serve it
over HTTP (a `file://` open can't fetch):

```sh
cp hello.bin build-web/host/web/cart.bin
python -m http.server -d build-web/host/web 8000
# open http://localhost:8000/cronopio.html
```

Notes:
- Browsers gate audio behind a user gesture; sound stays silent until the
  first click/keypress on the page. That's browser policy, not a host bug.
- The build deliberately omits `-sASYNCIFY`: nothing in the host blocks
  (the cart entry returns promptly and the loop is a rAF callback), so the
  wasm stays small and frame cost predictable.
