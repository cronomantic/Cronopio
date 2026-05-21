# cronopio-cc — cartridge compiler wrapper around cvm-cc.
add_executable(cronopio-cc cronopio-cc.c)

# Build-tree defaults baked in, so cronopio-cc works straight from build/
# without anything on PATH. The install layout (sibling cvm-cc, ../include)
# is probed at runtime and takes precedence when present.
set(_sdk_inc "${CMAKE_SOURCE_DIR}/sdk/include")
target_compile_definitions(cronopio-cc PRIVATE
    "CRONOPIO_SDK_INCLUDE=\"${_sdk_inc}\"")

if(TARGET cvm-cc)
    # Generator expression can't go in a -D string portably; stamp it via a
    # generated header instead.
    set(_gen "${CMAKE_CURRENT_BINARY_DIR}/cronopio_cc_paths.h")
    file(GENERATE OUTPUT "${_gen}" CONTENT
         "#define CRONOPIO_CVM_CC_DEFAULT \"$<TARGET_FILE:cvm-cc>\"\n")
    target_compile_definitions(cronopio-cc PRIVATE CRONOPIO_CC_HAVE_PATHS_H)
    target_include_directories(cronopio-cc PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    add_dependencies(cronopio-cc cvm-cc)
endif()

if(CRONOPIO_INSTALL)
    include(GNUInstallDirs)
    install(TARGETS cronopio-cc RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
