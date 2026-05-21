add_library(cronopio_common STATIC
    common/console.c
    common/syscalls.c
    common/gpu.c
    common/poly.c
    common/apu.c
    common/mod.c
    common/input.c
)
target_include_directories(cronopio_common PUBLIC common)

if(TARGET cvm)
    target_link_libraries(cronopio_common PUBLIC cvm)
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
