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

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_cronopio_install_default ON)
else()
    set(_cronopio_install_default OFF)
endif()
option(CRONOPIO_INSTALL "Generate install rules for the Cronopio SDK"
       ${_cronopio_install_default})

# CronoVM is added EXCLUDE_FROM_ALL (we don't want its tests/fixtures in our
# default build), which also keeps its own install rules out of ours. So we
# install its toolchain into the Cronopio prefix ourselves, below.

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

if(CRONOPIO_BUILD_TOOLS)
    if(TARGET cronopio_common)
        add_subdirectory(tools/headless)
    endif()
    add_subdirectory(tools/cronopio-cc)
endif()

if(CRONOPIO_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ---- install / find_package(Cronopio) ------------------------------------
if(CRONOPIO_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    # CronoVM toolchain into our prefix, so cronopio-cc finds cvm-cc as a
    # sibling and cvm-cc finds the runtime headers under share/. (CronoVM was
    # added EXCLUDE_FROM_ALL, so its own install rules don't run here.)
    set(_cvm_tools cvm-cc)
    if(TARGET cvm-translate)
        list(APPEND _cvm_tools cvm-translate)
    endif()
    install(TARGETS ${_cvm_tools} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(DIRECTORY ${CRONOVM_DIR}/runtime/lib/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/cronovm/runtime/lib
        FILES_MATCHING PATTERN "*.h")

    # SDK headers (cronopio.h, cronopio3d.h).
    install(DIRECTORY sdk/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.h")

    # Project templates for `cronopio-cc new`. Probed at runtime relative to
    # the exe as <bin>/../share/cronopio/templates.
    install(DIRECTORY sdk/templates/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/cronopio/templates)

    # The cart rule, installed next to the package config so the generated
    # CronopioConfig.cmake can include() it by relative path.
    install(FILES sdk/cmake/CronopioCart.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Cronopio)

    configure_package_config_file(
        sdk/cmake/CronopioConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/CronopioConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Cronopio
        PATH_VARS CMAKE_INSTALL_INCLUDEDIR CMAKE_INSTALL_BINDIR)

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/CronopioConfigVersion.cmake
        VERSION       ${PROJECT_VERSION}
        COMPATIBILITY SameMinorVersion)

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/CronopioConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/CronopioConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Cronopio)
endif()
