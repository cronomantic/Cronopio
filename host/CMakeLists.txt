add_library(cronopio_common STATIC
    common/console.c
    common/syscalls.c
    common/gpu.c
    common/poly.c
    common/apu.c
    common/mod.c
    common/midisynth.c
    common/input.c
)
target_include_directories(cronopio_common PUBLIC common)

# TinySoundFont (vendored as a submodule under external/), the MIDI+SoundFont
# music synth used by common/midisynth.c.
target_include_directories(cronopio_common PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../external/TinySoundFont")

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
