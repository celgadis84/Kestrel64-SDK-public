# Starting point for a game outside the SDK tree (docs/GETTING-STARTED.md).
#   cmake -S . -B build -G Ninja -DK64_SDK=/path/to/kestrel64-sdk
#   ninja -C build
cmake_minimum_required(VERSION 3.20)
set(K64_SDK "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "kestrel64-sdk checkout")
# The toolchain has to be known before project().
set(CMAKE_TOOLCHAIN_FILE "${K64_SDK}/cmake/k64-toolchain.cmake")
project(mygame C ASM)

set(K64_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
add_subdirectory(${K64_SDK} k64)

k64_add_rom(NAME mygame SOURCES main.c TITLE "MY GAME")
