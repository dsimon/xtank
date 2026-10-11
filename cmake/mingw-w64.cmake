# cmake/mingw-w64.cmake -- MinGW-w64 x86-64 cross toolchain (D2-D, P2.9).
#
# Cross-builds the SDL3/ENet client for Windows 11 x64 from Linux using the
# Ubuntu mingw-w64 packages pinned in P2.1-REPORT.md (mingw-w64 11.0.1,
# gcc-mingw-w64-x86-64 13.2.0, resolved target x86_64-w64-mingw32 GCC 13-win32).
#
# The default `x86_64-w64-mingw32-gcc` here is the win32 thread model variant,
# so the client carries no libwinpthread-1.dll dependency.  libgcc is linked
# statically by Client/CMakeLists.txt.
#
# Usage:
#   cmake -S . -B build-win \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake \
#         -DCMAKE_PREFIX_PATH=<sdl3-mingw-prefix> \
#         -DXTK_ENET_DIR=<enet-1.3.18-src>

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)

set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_RC_COMPILER  ${TOOLCHAIN_PREFIX}-windres)

set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX})

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)