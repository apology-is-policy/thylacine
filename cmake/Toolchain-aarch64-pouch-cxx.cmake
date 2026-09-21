# Cross-compile toolchain for C++ ports onto Thylacine's Pouch userland (the B-0 WebKit /
# JavaScriptCore probe; any CMake-based C++ port can use it).
#
# Sibling of Toolchain-aarch64-pouch.cmake (C only, the host clang). This one uses the
# llvm-thylacine FORK's clang++, whose Thylacine ToolChain links libc++ / libc++abi /
# libunwind and the CRT from the sysroot. CMAKE_SYSTEM_NAME is "Thylacine", served by
# cmake/Platform/Thylacine.cmake (static-only, UNIX-shaped) -- "Generic" will not do, because
# CMake's Generic platform leaves UNIX unset and WebKit's OS detection then refuses the build.
#
# Overrides: -DLLVMFORK=<llvm-thylacine build dir>  -DHOSTLLVM=<host llvm prefix>
#            -DTHYLACINE_SYSROOT=<pouch sysroot>    -DTHYLACINE_EXTRA_ROOTS=<;-list of staged prefixes>
set(CMAKE_SYSTEM_NAME      Thylacine)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")

if(NOT DEFINED LLVMFORK)
    set(LLVMFORK "$ENV{HOME}/projects/llvm-thylacine/build")
endif()
if(NOT DEFINED HOSTLLVM)
    set(HOSTLLVM /opt/homebrew/opt/llvm)
endif()
if(NOT DEFINED THYLACINE_SYSROOT)
    set(THYLACINE_SYSROOT "${CMAKE_CURRENT_LIST_DIR}/../build/sysroot")
endif()

set(CMAKE_C_COMPILER   ${LLVMFORK}/bin/clang)
set(CMAKE_CXX_COMPILER ${LLVMFORK}/bin/clang++)
set(CMAKE_ASM_COMPILER ${LLVMFORK}/bin/clang)
# The fork build ships no binutils; archive/strip tools come from the host LLVM.
set(CMAKE_AR      ${HOSTLLVM}/bin/llvm-ar)
set(CMAKE_RANLIB  ${HOSTLLVM}/bin/llvm-ranlib)
set(CMAKE_NM      ${HOSTLLVM}/bin/llvm-nm)
set(CMAKE_STRIP   ${HOSTLLVM}/bin/llvm-strip)
set(CMAKE_OBJCOPY ${HOSTLLVM}/bin/llvm-objcopy)
set(CMAKE_C_COMPILER_TARGET   aarch64-thylacine)
set(CMAKE_CXX_COMPILER_TARGET aarch64-thylacine)
set(CMAKE_ASM_COMPILER_TARGET aarch64-thylacine)
set(CMAKE_SYSROOT "${THYLACINE_SYSROOT}")

set(CMAKE_CROSSCOMPILING TRUE)
set(CMAKE_FIND_ROOT_PATH "${THYLACINE_SYSROOT}" ${THYLACINE_EXTRA_ROOTS})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# -march: the ARMv8.0 floor + outline atomics (PORTABILITY.md). -nostdlibinc keeps clang's own
# resource headers and drops every host system header; the sysroot supplies libc and libc++.
set(THYLACINE_COMMON "-march=armv8-a -moutline-atomics -fno-pie -nostdlibinc -D_GNU_SOURCE=1")
set(CMAKE_C_FLAGS_INIT   "${THYLACINE_COMMON} -isystem ${THYLACINE_SYSROOT}/include")
set(CMAKE_CXX_FLAGS_INIT "${THYLACINE_COMMON} -isystem ${THYLACINE_SYSROOT}/include/c++/v1 -isystem ${THYLACINE_SYSROOT}/include")
set(CMAKE_ASM_FLAGS_INIT "${THYLACINE_COMMON} -isystem ${THYLACINE_SYSROOT}/include")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
# Compiler probes cannot run target binaries; build a static library instead of linking an exe.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
