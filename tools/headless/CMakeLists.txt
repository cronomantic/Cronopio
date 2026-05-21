# Headless cart runner — loads a .bin, runs N frames, prints a framebuffer
# histogram. No SDL, so it builds anywhere cronopio_common does and is handy
# for CI / quick "does this cart render" checks.
add_executable(cronopio-headless headless.c)
target_link_libraries(cronopio-headless PRIVATE cronopio_common)
