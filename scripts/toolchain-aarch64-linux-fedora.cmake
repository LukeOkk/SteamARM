# CMake toolchain: build Linux aarch64 ELF programs (Fedora 43 userland) on the
# macOS host with Homebrew LLVM -- no VM, no GCC cross toolchain.
#
# clang + lld are already cross compilers; all they lack is a target userland
# to compile and link against. That is the sysroot scripts/build-fex-host.sh
# unpacks from Fedora 43 aarch64 RPMs (glibc, glibc-devel, kernel-headers,
# libgcc, libstdc++, libstdc++-devel, gcc for crtbegin*.o / libgcc.a, and gdb
# for <gdb/jit-reader.h>).
#
#   cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=scripts/toolchain-aarch64-linux-fedora.cmake ...
#
# Overrides (cache variable or environment variable of the same name):
#   FEDORA_SYSROOT  default $HOME/SteamARM-build/sysroot-f43
#   LLVM_BIN        default /opt/homebrew/opt/llvm/bin
#   LLD             default ld.lld next to clang, else ld.lld on PATH
#
# The triple is Fedora's own (aarch64-redhat-linux-gnu), so clang finds the GCC
# install at <sysroot>/usr/lib/gcc/aarch64-redhat-linux/<ver> and the libstdc++
# headers at <sysroot>/usr/include/c++/<ver> exactly as Fedora's clang does.
# The dynamic linker clang records is /lib/ld-linux-aarch64.so.1, as on Fedora.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

foreach(_v FEDORA_SYSROOT LLVM_BIN LLD)
  if(NOT DEFINED ${_v} AND DEFINED ENV{${_v}})
    set(${_v} "$ENV{${_v}}")
  endif()
endforeach()
if(NOT DEFINED FEDORA_SYSROOT)
  if(DEFINED ENV{STEAMARM_BUILD})
    set(FEDORA_SYSROOT "$ENV{STEAMARM_BUILD}/sysroot-f43")
  else()
    set(FEDORA_SYSROOT "$ENV{HOME}/SteamARM-build/sysroot-f43")
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
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES FEDORA_SYSROOT LLVM_BIN LLD)

if(NOT EXISTS "${FEDORA_SYSROOT}/usr/include/stdio.h")
  message(FATAL_ERROR "No Fedora sysroot at ${FEDORA_SYSROOT} (run scripts/build-fex-host.sh sysroot)")
endif()
if(NOT EXISTS "${LLD}")
  message(FATAL_ERROR "ld.lld not found (LLD=${LLD}); brew install lld")
endif()

set(FEDORA_TRIPLE aarch64-redhat-linux-gnu)
file(GLOB _gccdirs LIST_DIRECTORIES true "${FEDORA_SYSROOT}/usr/lib/gcc/aarch64-redhat-linux/*")
list(SORT _gccdirs COMPARE NATURAL ORDER DESCENDING)
list(GET _gccdirs 0 FEDORA_GCC_INSTALL_DIR)

set(CMAKE_SYSROOT "${FEDORA_SYSROOT}")
set(CMAKE_C_COMPILER   "${LLVM_BIN}/clang")
set(CMAKE_CXX_COMPILER "${LLVM_BIN}/clang++")
set(CMAKE_ASM_COMPILER "${LLVM_BIN}/clang")
set(CMAKE_C_COMPILER_TARGET   ${FEDORA_TRIPLE})
set(CMAKE_CXX_COMPILER_TARGET ${FEDORA_TRIPLE})
set(CMAKE_ASM_COMPILER_TARGET ${FEDORA_TRIPLE})

set(CMAKE_AR      "${LLVM_BIN}/llvm-ar"      CACHE FILEPATH "")
set(CMAKE_RANLIB  "${LLVM_BIN}/llvm-ranlib"  CACHE FILEPATH "")
set(CMAKE_NM      "${LLVM_BIN}/llvm-nm"      CACHE FILEPATH "")
set(CMAKE_OBJCOPY "${LLVM_BIN}/llvm-objcopy" CACHE FILEPATH "")
set(CMAKE_OBJDUMP "${LLVM_BIN}/llvm-objdump" CACHE FILEPATH "")
set(CMAKE_STRIP   "${LLVM_BIN}/llvm-strip"   CACHE FILEPATH "")
set(CMAKE_READELF "${LLVM_BIN}/llvm-readelf" CACHE FILEPATH "")
set(CMAKE_LINKER  "${LLD}"                   CACHE FILEPATH "")

# Pin the GCC install explicitly (clang would also find it through --sysroot).
# These only seed CMAKE_<LANG>_FLAGS on the first configure; CFLAGS/CXXFLAGS
# from the environment are added to them, a -DCMAKE_CXX_FLAGS=... replaces them.
set(CMAKE_C_FLAGS_INIT   "--gcc-install-dir=${FEDORA_GCC_INSTALL_DIR}")
set(CMAKE_CXX_FLAGS_INIT "--gcc-install-dir=${FEDORA_GCC_INSTALL_DIR}")
set(CMAKE_ASM_FLAGS_INIT "--gcc-install-dir=${FEDORA_GCC_INSTALL_DIR}")
# --build-id: Fedora's GNU ld adds one by default, lld does not.
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-fuse-ld=lld --ld-path=${LLD} -Wl,--build-id=sha1")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld --ld-path=${LLD} -Wl,--build-id=sha1")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld --ld-path=${LLD} -Wl,--build-id=sha1")

# Programs (python3, git) come from the host; headers, libraries and CMake
# packages only from the sysroot -- never Homebrew's Mach-O fmt, xxhash, ...
set(CMAKE_FIND_ROOT_PATH "${FEDORA_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(ENV{PKG_CONFIG_LIBDIR} "${FEDORA_SYSROOT}/usr/lib64/pkgconfig:${FEDORA_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${FEDORA_SYSROOT}")
