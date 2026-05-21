# CronopioCart.cmake
#
# Declare a cartridge target. A cartridge is a CronoVM .bin built from C
# sources, with the Cronopio memory map (fb/pal regions) and default reserves
# baked in. The build drives `cronopio-cc`, which owns those defaults — so
# this file just resolves the compiler and forwards overrides.
#
#   find_package(Cronopio REQUIRED)          # installed SDK
#   cronopio_add_cartridge(my_game SOURCES main.c)
#
# or in-tree (CronoVM + Cronopio built together), include this file directly.
# Produces ${CMAKE_CURRENT_BINARY_DIR}/<name>.bin.

# Resolve the cartridge compiler. Order:
#   1. CRONOPIO_CC already set (CronopioConfig points it at the installed exe)
#   2. the in-tree cronopio-cc target (fresh checkout builds with what it just
#      compiled)
#   3. cronopio-cc on PATH
if(NOT CRONOPIO_CC)
    if(TARGET cronopio-cc)
        set(CRONOPIO_CC "$<TARGET_FILE:cronopio-cc>")
    else()
        find_program(CRONOPIO_CC cronopio-cc DOC "Path to the cronopio-cc cartridge compiler")
    endif()
endif()

# Reserves the cart gets unless it overrides them. Kept in sync with the
# defaults cronopio-cc bakes in; passing them explicitly makes the build
# self-documenting and lets a project change them project-wide.
set(CRONOPIO_DEFAULT_HEAP_RESERVE  "32M"  CACHE STRING "Default cart heap reserve")
set(CRONOPIO_DEFAULT_STACK_RESERVE "256K" CACHE STRING "Default cart stack reserve")

function(cronopio_add_cartridge name)
    cmake_parse_arguments(ARG "LTO" "HEAP_RESERVE;STACK_RESERVE;ROM" "SOURCES" ${ARGN})
    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "cronopio_add_cartridge(${name}): SOURCES required")
    endif()
    if(NOT CRONOPIO_CC)
        message(WARNING
            "cronopio-cc not found — cartridge '${name}' will not build. "
            "Install the Cronopio SDK (and put its bin/ on PATH or pass "
            "-DCMAKE_PREFIX_PATH=<prefix>), or build Cronopio with "
            "-DCRONOPIO_BUILD_TOOLS=ON so the in-tree cronopio-cc target exists.")
        return()
    endif()

    if(NOT ARG_HEAP_RESERVE)
        set(ARG_HEAP_RESERVE  "${CRONOPIO_DEFAULT_HEAP_RESERVE}")
    endif()
    if(NOT ARG_STACK_RESERVE)
        set(ARG_STACK_RESERVE "${CRONOPIO_DEFAULT_STACK_RESERVE}")
    endif()

    set(rom_flag "")
    set(rom_dep  "")
    if(ARG_ROM)
        set(rom_flag "--rom=${ARG_ROM}")
        set(rom_dep  "${ARG_ROM}")
    endif()

    # LTO: link all SOURCES then run opt 'default<O2>' for cross-file inlining.
    # A win for multi-file ports (renderer/fixed-point helpers spread across
    # files); vectorisation stays off (the VM has no vector types).
    set(lto_flag "")
    if(ARG_LTO)
        set(lto_flag "--lto")
    endif()

    # Build-tree convenience: when the compiler is the in-tree target, depend
    # on it so it's built first. (No-op when CRONOPIO_CC is an installed path.)
    set(cc_dep "")
    if(TARGET cronopio-cc)
        set(cc_dep cronopio-cc)
    endif()

    set(out "${CMAKE_CURRENT_BINARY_DIR}/${name}.bin")
    add_custom_command(
        OUTPUT  "${out}"
        COMMAND "${CRONOPIO_CC}"
                "--heap-reserve=${ARG_HEAP_RESERVE}"
                "--stack-reserve=${ARG_STACK_RESERVE}"
                ${rom_flag}
                ${lto_flag}
                ${ARG_SOURCES}
                -o "${out}"
        DEPENDS ${ARG_SOURCES} ${rom_dep} ${cc_dep}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "cronopio-cc → ${name}.bin"
        VERBATIM)
    add_custom_target(${name} ALL DEPENDS "${out}")
endfunction()
