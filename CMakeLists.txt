cmake_minimum_required(VERSION 3.20)
if(NOT CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/k64-toolchain.cmake")
endif()
project(kestrel64_sdk C ASM)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(K64_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

add_compile_options(-Wall -Wextra -Wno-unused-parameter -std=gnu11)

# ---- libk64 ------------------------------------------------------------------
file(GLOB K64_LIB_SOURCES CONFIGURE_DEPENDS
  src/hal/*.c src/rt/*.c src/gfx/*.c src/audio/*.c src/video/*.c)
# Flashcart SD (k64/sd.h): libcart cart drivers + FatFs, vendored as is (third_party/*/LICENSE*)
set(K64_SD_SOURCES third_party/libcart/cart.c third_party/fatfs/ff.c third_party/fatfs/ffunicode.c)
set_source_files_properties(${K64_SD_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties(third_party/fatfs/ff.c PROPERTIES
  INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fatfs/k64)
add_library(k64 STATIC ${K64_LIB_SOURCES} ${K64_SD_SOURCES})
target_include_directories(k64 PUBLIC include)

# crt0 is linked as an object (not from the archive) so _start is always present.
add_library(k64_crt0 OBJECT src/boot/crt0.S)

# Boot logo, linked into every ROM by k64_add_rom (per ROM: NO_LOGO). Its data is
# tools/k64logo.py output, committed (regenerate: python tools/k64logo.py c src/logo/logo_data.h).
option(K64_LOGO "Play the Kestrel64 boot logo before main() in every ROM" ON)
add_library(k64_logo OBJECT src/logo/logo.c)
target_link_libraries(k64_logo PRIVATE k64)

include(cmake/k64.cmake)

# Standard microcode, resident in libk64.
k64_add_ucode(k64 kgfx ${CMAKE_CURRENT_SOURCE_DIR}/src/rsp/kgfx.S)
k64_add_ucode(k64 kvid ${CMAKE_CURRENT_SOURCE_DIR}/src/rsp/kvid.S)

# ---- demos ---------------------------------------------------------------------
option(K64_BUILD_DEMOS "Build the demo ROMs" ON)
if(K64_BUILD_DEMOS)
  file(GLOB _demo_dirs LIST_DIRECTORIES true ${CMAKE_CURRENT_SOURCE_DIR}/demos/*)
  foreach(d ${_demo_dirs})
    if(EXISTS ${d}/CMakeLists.txt)
      add_subdirectory(${d})
    endif()
  endforeach()
endif()

