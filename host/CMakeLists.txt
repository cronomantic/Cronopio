add_library(cronopio_common STATIC
    common/console.c
    common/syscalls.c
    common/gpu.c
    common/poly.c
    common/apu.c
    common/midisynth.c
    common/cron_ogg.c
    common/cron_module.c
    external/stb_vorbis.c
    common/bios_sf2.c
    common/input.c
)
target_include_directories(cronopio_common PUBLIC common)

# bios_sf2.c embeds the default SoundFont with C23 #embed — needs C23.
set_source_files_properties(common/bios_sf2.c PROPERTIES COMPILE_OPTIONS "-std=gnu23")

# stb_vorbis is vendored third-party code; don't fail the build on its warnings.
set_source_files_properties(external/stb_vorbis.c PROPERTIES COMPILE_OPTIONS "-w")

# TinySoundFont (vendored as a submodule under external/), the MIDI+SoundFont
# music synth used by common/midisynth.c.
target_include_directories(cronopio_common PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../external/TinySoundFont")

# libxmp (submodule under external/) — the tracker-module music player
# (MOD/S3M/XM/IT) used by common/cron_module.c. Build only the static lib.
# libxmp targets C90 but uses `static inline`, which the strict -std=c90 the
# project default (CMAKE_C_EXTENSIONS OFF) selects does NOT accept; enable GNU
# extensions for libxmp's scope so it builds as -std=gnu90 (its intended mode).
set(BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(BUILD_LITE   OFF CACHE BOOL "" FORCE)
set(LIBXMP_PIC   OFF CACHE BOOL "" FORCE)
set(CMAKE_C_EXTENSIONS ON)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../external/libxmp"
                 "${CMAKE_BINARY_DIR}/libxmp" EXCLUDE_FROM_ALL)
set(CMAKE_C_EXTENSIONS OFF)
target_include_directories(cronopio_common PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../external/libxmp/include")
target_link_libraries(cronopio_common PRIVATE xmp_static)

if(TARGET cvm)
    target_link_libraries(cronopio_common PUBLIC cvm)
endif()

# tsf.h needs the C math library (pow/exp/...). On Windows the UCRT/CRT provides
# it implicitly; on Unix-likes it is a separate -lm.
if(NOT WIN32)
    target_link_libraries(cronopio_common PUBLIC m)
endif()

if(CRONOPIO_TARGET_DESKTOP)
    add_subdirectory(desktop)
endif()
if(CRONOPIO_TARGET_WEB)
    add_subdirectory(web)
endif()
if(CRONOPIO_TARGET_EMBEDDED)
    add_subdirectory(embedded)
endif()
