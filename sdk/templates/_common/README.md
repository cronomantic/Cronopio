# @NAME@

A Cronopio cartridge.

## Build

One-line, no CMake:

```sh
cronopio-cc main.c -o @NAME@.bin
```

Or with CMake (finds the installed SDK):

```sh
cmake -B build -DCMAKE_PREFIX_PATH=<cronopio-install-prefix>
cmake --build build        # -> build/@NAME@.bin
```

## Run

```sh
cronopio @NAME@.bin
```

See the Cronopio docs for the syscall reference, sprites/tilemaps, 3D and audio.
