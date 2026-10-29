# Cross build for 64-bit Windows with mingw-w64 (Debian / Ubuntu: gcc-mingw-w64-x86-64), e.g. from Linux:
#
#   cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
#         -DCMAKE_BUILD_TYPE=Release -DSDL3_DIR=<SDL3-devel-x.y.z-mingw>/x86_64-w64-mingw32/lib/cmake/SDL3 \
#         [-DwxWidgets_ROOT_DIR=<wxMSW gcc dev + headers>]   (for the launcher)
#
# libgcc, libstdc++ and winpthread are linked statically, so the programs only need SDL3.dll (and the
# launcher the wxWidgets DLLs), which the build copies beside them.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)
set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
set(CMAKE_RC_COMPILER  ${TOOLCHAIN_PREFIX}-windres)
find_program(CMAKE_OBJDUMP ${TOOLCHAIN_PREFIX}-objdump)

set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# SDL3_DIR / wxWidgets folders are given as absolute paths outside the sysroot
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++ -Wl,-Bstatic,--whole-archive -lwinpthread -Wl,--no-whole-archive,-Bdynamic")
