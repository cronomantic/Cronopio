# CronopioCart.cmake
#
# Declare a cartridge target. A cartridge is a CronoVM .bin built from C
# sources via cvm-cc, with the framebuffer/palette host regions baked in
# as the Cronopio runtime expects.
#
#   include(${CRONOPIO_DIR}/sdk/cmake/CronopioCart.cmake)
#   cronopio_add_cartridge(my_game SOURCES src/main.c src/world.c)
#
# Produces ${CMAKE_CURRENT_BINARY_DIR}/my_game.bin. The build invokes
# cvm-cc with --region=fb:76800:rw and --region=pal:128:rw so the
# host's cronopio_resolve_video_regions() finds them at load time.

if(NOT DEFINED CRONOPIO_SDK_DIR)
    set(CRONOPIO_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
endif()

find_program(CVM_CC cvm-cc DOC "Path to the cvm-cc compiler wrapper")

# Default reserves — most carts can override via cronopio_add_cartridge(
#   ... HEAP_RESERVE 4M STACK_RESERVE 32K). The 32 MiB default is the v0.2
# "pre-3D era" baseline (room for a DOOM zone heap alongside a WAD in ROM).
set(CRONOPIO_DEFAULT_HEAP_RESERVE  "32M"  CACHE STRING "Default cart heap reserve")
set(CRONOPIO_DEFAULT_STACK_RESERVE "256K" CACHE STRING "Default cart stack reserve")

function(cronopio_add_cartridge name)
    cmake_parse_arguments(ARG "" "HEAP_RESERVE;STACK_RESERVE;ROM" "SOURCES" ${ARGN})
    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "cronopio_add_cartridge(${name}): SOURCES required")
    endif()
    if(NOT CVM_CC)
        message(WARNING
            "cvm-cc not found in PATH — cartridge '${name}' will not build "
            "until the CronoVM toolchain is installed. Build CronoVM with "
            "LLVM available and either install it or add its build/tools/cvm-cc "
            "directory to PATH.")
        return()
    endif()

    if(NOT ARG_HEAP_RESERVE)  set(ARG_HEAP_RESERVE  "${CRONOPIO_DEFAULT_HEAP_RESERVE}") endif()
    if(NOT ARG_STACK_RESERVE) set(ARG_STACK_RESERVE "${CRONOPIO_DEFAULT_STACK_RESERVE}") endif()

    # Optional read-only cartridge ROM (e.g. a WAD), baked into the .bin.
    set(rom_flag "")
    set(rom_dep "")
    if(ARG_ROM)
        set(rom_flag "--rom=${ARG_ROM}")
        set(rom_dep "${ARG_ROM}")
    endif()

    set(out "${CMAKE_CURRENT_BINARY_DIR}/${name}.bin")
    add_custom_command(
        OUTPUT  "${out}"
        COMMAND "${CVM_CC}"
                -I "${CRONOPIO_SDK_DIR}/include"
                "--heap-reserve=${ARG_HEAP_RESERVE}"
                "--stack-reserve=${ARG_STACK_RESERVE}"
                "--region=fb:76800:rw"
                "--region=pal:1024:rw"
                ${rom_flag}
                ${ARG_SOURCES}
                -o "${out}"
        DEPENDS ${ARG_SOURCES} ${rom_dep}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "cvm-cc → ${name}.bin"
        VERBATIM)
    add_custom_target(${name} ALL DEPENDS "${out}")
endfunction()
