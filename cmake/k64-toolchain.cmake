# kestrel64-sdk — CMake toolchain for the N64 (VR4300, n32 ABI, bare metal).
#
# Host-agnostic: needs only an LLVM toolchain (clang + ld.lld + llvm-objcopy) and
# Python 3 (+ Pillow/numpy for the asset baker). Same file on Windows, Linux, macOS.
#
#   cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/k64-toolchain.cmake
#
# K64_LLVM_DIR can point at the LLVM bin dir when it is not on PATH.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR mips64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

if(NOT K64_LLVM_DIR AND DEFINED ENV{K64_LLVM_DIR})
  set(K64_LLVM_DIR "$ENV{K64_LLVM_DIR}")
endif()
set(_hints "")
if(K64_LLVM_DIR)
  set(_hints HINTS "${K64_LLVM_DIR}" NO_DEFAULT_PATH)
endif()

find_program(K64_CLANG   NAMES clang ${_hints} REQUIRED)
find_program(K64_LLD     NAMES ld.lld ${_hints} REQUIRED)
find_program(K64_OBJCOPY NAMES llvm-objcopy ${_hints} REQUIRED)
find_program(K64_NM      NAMES llvm-nm ${_hints})
find_program(K64_OBJDUMP NAMES llvm-objdump ${_hints})

set(CMAKE_C_COMPILER   "${K64_CLANG}")
set(CMAKE_ASM_COMPILER "${K64_CLANG}")
set(CMAKE_C_COMPILER_TARGET   mips64-none-elf)
set(CMAKE_ASM_COMPILER_TARGET mips64-none-elf)
set(CMAKE_OBJCOPY "${K64_OBJCOPY}" CACHE FILEPATH "" FORCE)

# VR4300 = MIPS III. n32: 64-bit GPRs, 32-bit pointers, FR=1 FPU (32 x 64-bit FPRs).
# -G0: no $gp-relative small data (we own $gp-free codegen, simpler crt0).
# -mno-abicalls -fno-pic: absolute addressing, no PIC/GOT overhead.
set(K64_ARCH_FLAGS "-march=mips3 -mabi=n32 -mno-abicalls -fno-pic -G0 -mno-check-zero-division")
set(CMAKE_C_FLAGS_INIT   "${K64_ARCH_FLAGS} -ffreestanding -fno-stack-protector -fomit-frame-pointer -ffunction-sections -fdata-sections -fno-common")
set(CMAKE_ASM_FLAGS_INIT "${K64_ARCH_FLAGS}")
# Link with ld.lld directly: the clang driver for *-none-elf would try to run a gcc.
set(CMAKE_LINKER "${K64_LLD}" CACHE FILEPATH "" FORCE)
set(CMAKE_C_LINK_EXECUTABLE   "<CMAKE_LINKER> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
set(CMAKE_ASM_LINK_EXECUTABLE "<CMAKE_LINKER> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-m elf32btsmipn32 --gc-sections -nostdlib")

set(CMAKE_C_FLAGS_RELEASE "-O2" CACHE STRING "" FORCE)   # O2 not O3: I$ is 16 KB
set(CMAKE_C_FLAGS_DEBUG_INIT   "-O1 -g")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
