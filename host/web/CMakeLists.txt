# Web host (Emscripten). Build by configuring with the Emscripten toolchain:
#
#   emcmake cmake -B build-web -S . -DCRONOPIO_TARGET_DESKTOP=OFF -DCRONOPIO_TARGET_WEB=ON
#   cmake --build build-web
#
# Then serve build-web/host/web/ over HTTP (a file:// open won't fetch the
# cart). The page loads a cartridge named "cart.bin" from the same directory:
#
#   cp your_game.bin build-web/host/web/cart.bin
#   python -m http.server -d build-web/host/web 8000
#   # browse to http://localhost:8000/cronopio.html
#
# Reuses the desktop SDL2 source — Emscripten ships an SDL2 port, and
# host/desktop/main.c drives its frame loop through emscripten_set_main_loop
# under __EMSCRIPTEN__ (no ASYNCIFY needed: the cart entry is non-blocking
# and the per-frame step is a requestAnimationFrame callback).

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

# -sASYNCIFY is deliberately NOT used: the cart's entry returns promptly
# after cron_set_frame, and the frame loop is driven by the browser's
# animation frame, so nothing in the host blocks. Dropping it keeps the
# wasm smaller and the frame cost predictable.
target_link_options(cronopio_web PRIVATE
    -sUSE_SDL=2
    -sALLOW_MEMORY_GROWTH=1
    -sEXIT_RUNTIME=0
    --shell-file ${CMAKE_CURRENT_SOURCE_DIR}/shell.html)
