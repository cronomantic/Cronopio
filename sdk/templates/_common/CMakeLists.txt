cmake_minimum_required(VERSION 3.16)
project(@NAME@ C)

# Needs an installed Cronopio SDK. Configure with:
#   cmake -B build -DCMAKE_PREFIX_PATH=<cronopio-install-prefix>
find_package(Cronopio REQUIRED)

cronopio_add_cartridge(@NAME@ SOURCES main.c)
