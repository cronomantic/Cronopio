# Web host (Emscripten). Build by configuring with the Emscripten toolchain:
#
#   emcmake cmake -B build-web -S . -DCRONOPIO_TARGET_DESKTOP=OFF -DCRONOPIO_TARGET_WEB=ON
#   cmake --build build-web
#
# Reuses the desktop SDL2 source — Emscripten ships an SDL2 port.

if(NOT EMSCRIPTEN)
    message(WARNING "Web target selected but CMake is not running under emcmake. Skipping.")
    return()
endif()

add_executable(cronopio_web ${CMAKE_CURRENT_SOURCE_DIR}/../desktop/main.c)
target_link_libraries(cronopio_web PRIVATE cronopio_common)

set_target_properties(cronopio_web PROPERTIES
    OUTPUT_NAME "cronopio"
    SUFFIX ".html")

target_compile_options(cronopio_web PRIVATE
    -sUSE_SDL=2)
target_link_options(cronopio_web PRIVATE
    -sUSE_SDL=2
    -sALLOW_MEMORY_GROWTH=1
    -sASYNCIFY=1
    --shell-file ${CMAKE_CURRENT_SOURCE_DIR}/shell.html)
