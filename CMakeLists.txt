cmake_minimum_required(VERSION 3.20)
project(Cronopio
    VERSION 0.1.0
    DESCRIPTION "Cronopio fantasy console"
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(CRONOPIO_TARGET_DESKTOP  "Build desktop host (SDL2)"        ON)
option(CRONOPIO_TARGET_WEB      "Build web host (Emscripten)"      OFF)
option(CRONOPIO_TARGET_EMBEDDED "Build embedded host (HAL stub)"   OFF)
option(CRONOPIO_BUILD_EXAMPLES  "Build example cartridges"         OFF)
option(CRONOPIO_BUILD_TOOLS     "Build host-side tools (headless)" ON)

# The embedded VM. Expected as a git submodule under external/CronoVM, but
# we also accept external/external/CronoVM (a common accident when running
# `git submodule add` from inside external/).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/CronoVM/CMakeLists.txt")
    set(CRONOVM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/CronoVM")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/external/CronoVM/CMakeLists.txt")
    set(CRONOVM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/external/CronoVM")
endif()
if(CRONOVM_DIR)
    add_subdirectory(${CRONOVM_DIR} ${CMAKE_BINARY_DIR}/_cvm EXCLUDE_FROM_ALL)
else()
    message(WARNING
        "CronoVM not found — host will not link. Run:\n"
        "  git submodule add https://github.com/cronomantic/CronoVM external/CronoVM\n"
        "  git submodule update --init --recursive")
endif()

add_subdirectory(host)

if(CRONOPIO_BUILD_TOOLS AND TARGET cronopio_common)
    add_subdirectory(tools/headless)
endif()

if(CRONOPIO_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
