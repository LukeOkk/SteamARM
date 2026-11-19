# CMake toolchain: build Linux aarch64 ELF programs on macOS with Homebrew
# LLVM, using the exact Steam Frame SteamOS root which will run the result.
# SteamOS identifies itself as Arch-based and ships the needed glibc, GCC
# support objects, libstdc++, Linux headers and GDB JIT-reader headers.
#
#   cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=scripts/toolchain-aarch64-linux-steamos.cmake ...
#
# Overrides (cache variable or environment variable of the same name):
#   STEAMOS_SYSROOT  default $STEAMARM_STATE/arm64root
#   LLVM_BIN         default /opt/homebrew/opt/llvm/bin
#   LLD              default ld.lld next to clang, else ld.lld on PATH

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

foreach(_v STEAMOS_SYSROOT LLVM_BIN LLD)
  if(NOT DEFINED ${_v} AND DEFINED ENV{${_v}})
    set(${_v} "$ENV{${_v}}")
  endif()
endforeach()
if(NOT DEFINED STEAMOS_SYSROOT)
  if(DEFINED ENV{STEAMARM_FRAME_SYSROOT})
    set(STEAMOS_SYSROOT "$ENV{STEAMARM_FRAME_SYSROOT}")
  elseif(DEFINED ENV{STEAMARM_STATE})
    set(STEAMOS_SYSROOT "$ENV{STEAMARM_STATE}/arm64root")
  else()
    set(STEAMOS_SYSROOT "$ENV{HOME}/Developer/SteamARM-roots/arm64root")
  endif()
endif()
if(NOT DEFINED LLVM_BIN)
  set(LLVM_BIN "/opt/homebrew/opt/llvm/bin")
endif()
if(NOT DEFINED LLD)
  if(EXISTS "${LLVM_BIN}/ld.lld")
    set(LLD "${LLVM_BIN}/ld.lld")
  else()
    find_program(LLD NAMES ld.lld PATHS /opt/homebrew/bin /opt/homebrew/opt/lld/bin NO_CMAKE_FIND_ROOT_PATH)
  endif()
endif()
# try_compile() projects re-read this file; hand them the same values.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES STEAMOS_SYSROOT LLVM_BIN LLD)

if(NOT EXISTS "${STEAMOS_SYSROOT}/.lxrt-frameroot" OR
   NOT EXISTS "${STEAMOS_SYSROOT}/etc/os-release" OR
   NOT EXISTS "${STEAMOS_SYSROOT}/usr/include/stdio.h")
  message(FATAL_ERROR "No Steam Frame SteamOS sysroot at ${STEAMOS_SYSROOT} (run the frameroot setup step first)")
endif()
if(NOT EXISTS "${LLD}")
  message(FATAL_ERROR "ld.lld not found (LLD=${LLD}); brew install lld")
endif()

set(STEAMOS_TRIPLE aarch64-unknown-linux-gnu)
file(GLOB _gccdirs LIST_DIRECTORIES true "${STEAMOS_SYSROOT}/usr/lib/gcc/aarch64-unknown-linux-gnu/*")
list(SORT _gccdirs COMPARE NATURAL ORDER DESCENDING)
if(NOT _gccdirs)
  message(FATAL_ERROR "No aarch64-unknown-linux-gnu GCC files under ${STEAMOS_SYSROOT}/usr/lib/gcc")
endif()
list(GET _gccdirs 0 STEAMOS_GCC_INSTALL_DIR)

set(CMAKE_SYSROOT "${STEAMOS_SYSROOT}")
set(CMAKE_C_COMPILER   "${LLVM_BIN}/clang")
set(CMAKE_CXX_COMPILER "${LLVM_BIN}/clang++")
set(CMAKE_ASM_COMPILER "${LLVM_BIN}/clang")
set(CMAKE_C_COMPILER_TARGET   ${STEAMOS_TRIPLE})
set(CMAKE_CXX_COMPILER_TARGET ${STEAMOS_TRIPLE})
set(CMAKE_ASM_COMPILER_TARGET ${STEAMOS_TRIPLE})

set(CMAKE_AR      "${LLVM_BIN}/llvm-ar"      CACHE FILEPATH "")
set(CMAKE_RANLIB  "${LLVM_BIN}/llvm-ranlib"  CACHE FILEPATH "")
set(CMAKE_NM      "${LLVM_BIN}/llvm-nm"      CACHE FILEPATH "")
set(CMAKE_OBJCOPY "${LLVM_BIN}/llvm-objcopy" CACHE FILEPATH "")
set(CMAKE_OBJDUMP "${LLVM_BIN}/llvm-objdump" CACHE FILEPATH "")
set(CMAKE_STRIP   "${LLVM_BIN}/llvm-strip"   CACHE FILEPATH "")
set(CMAKE_READELF "${LLVM_BIN}/llvm-readelf" CACHE FILEPATH "")
set(CMAKE_LINKER  "${LLD}"                   CACHE FILEPATH "")

set(CMAKE_C_FLAGS_INIT   "--gcc-install-dir=${STEAMOS_GCC_INSTALL_DIR}")
set(CMAKE_CXX_FLAGS_INIT "--gcc-install-dir=${STEAMOS_GCC_INSTALL_DIR}")
set(CMAKE_ASM_FLAGS_INIT "--gcc-install-dir=${STEAMOS_GCC_INSTALL_DIR}")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-fuse-ld=lld --ld-path=${LLD} -Wl,--build-id=sha1")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld --ld-path=${LLD} -Wl,--build-id=sha1")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld --ld-path=${LLD} -Wl,--build-id=sha1")

# Build tools remain on macOS; target headers and libraries come only from the
# Steam Frame root. Never resolve host Homebrew headers for Linux objects.
set(CMAKE_FIND_ROOT_PATH "${STEAMOS_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(ENV{PKG_CONFIG_LIBDIR} "${STEAMOS_SYSROOT}/usr/lib/pkgconfig:${STEAMOS_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${STEAMOS_SYSROOT}")
