# Some SDL2 CMake configs (notably msys2's) call set_package_properties()
# without pulling in the module that defines it; include it up front so the
# find_package below doesn't error out on an otherwise-fine SDL2 install.
include(FeatureSummary)

find_package(SDL2 QUIET)
if(NOT SDL2_FOUND)
    message(WARNING "SDL2 not found — desktop host will not build. "
                    "Install libsdl2-dev or set SDL2_DIR.")
    return()
endif()

add_executable(cronopio main.c)
# SDL2main must precede SDL2 on the link line: on Windows it provides
# WinMain (which calls our main) and references SDL_* symbols that the
# SDL2 library after it resolves. Listing SDL2main last leaves those
# references undefined.
target_link_libraries(cronopio PRIVATE cronopio_common SDL2::SDL2main SDL2::SDL2)

# Dev fallback: point at the in-tree default SoundFont so the host finds it
# when run straight from the build tree (the distribution ships it beside the
# exe instead). CMAKE_SOURCE_DIR is the Cronopio repo root.
target_compile_definitions(cronopio PRIVATE
    CRONOPIO_SOURCE_SF2="${CMAKE_SOURCE_DIR}/assets/bios/GeneralUser-GS.sf2")

if(CRONOPIO_INSTALL)
    include(GNUInstallDirs)
    install(TARGETS cronopio RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
