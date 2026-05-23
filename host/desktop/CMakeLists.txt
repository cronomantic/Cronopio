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

add_executable(cronopio main.c menu.c hostcfg.c)

# Self-contained executable: link SDL2 statically so no SDL2.dll is needed.
# (This LLVM-mingw/UCRT toolchain already links libgcc/libc++/winpthread
# statically by default, and the api-ms-win-crt-* imports are the system UCRT,
# present on Windows — so static SDL2 is the only piece left to drop the DLL.)
# SDL2::SDL2-static carries its transitive Win32 system libs. Fall back to the
# shared target where a static SDL2 isn't installed.
if(TARGET SDL2::SDL2-static)
    set(CRONOPIO_SDL2_LIB SDL2::SDL2-static)
else()
    set(CRONOPIO_SDL2_LIB SDL2::SDL2)
    message(WARNING "SDL2::SDL2-static not found — linking SDL2 dynamically "
                    "(cronopio.exe will need SDL2.dll beside it).")
endif()
# SDL2main must precede SDL2 on the link line: on Windows it provides WinMain
# (which calls our main) and references SDL_* symbols the SDL2 library resolves.
target_link_libraries(cronopio PRIVATE cronopio_common SDL2::SDL2main ${CRONOPIO_SDL2_LIB})

if(CRONOPIO_INSTALL)
    include(GNUInstallDirs)
    install(TARGETS cronopio RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
