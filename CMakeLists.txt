# Xtank Windows client -- root CMake build (P2.9).
#
# CMake is used only for the new client/protocol/transport shell; the Phase 1
# server and oracle keep their GNUmakefile build unchanged.  The client links
# the pinned SDL3 (D2-A) and ENet (D2-B) and the portable Protocol/ codec.  It
# links no simulation, bots, ucontext, X11 or Unix loader.
#
# Native Linux (frontend build/test):
#   cmake -S . -B build -DCMAKE_PREFIX_PATH=<sdl3-prefix> \
#         -DXTK_ENET_DIR=<enet-1.3.18-src>
# Cross Windows x64 (D2-D):
#   cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake \
#         -DCMAKE_PREFIX_PATH=<sdl3-mingw-prefix> -DXTK_ENET_DIR=<enet-src>
#
# Authority: docs/PHASE2-PLAN.md P2.9; docs/PHASE2-DECISIONS.md D2-A/D2-B/D2-D.

cmake_minimum_required(VERSION 3.20)

project(xtank_client C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(XTK_BUILD_CLIENT "Build the SDL3 Windows/Linux client shell" ON)

# Pinned ENet 1.3.18 source tree (contains include/enet/enet.h and *.c).
set(XTK_ENET_DIR "" CACHE PATH "ENet 1.3.18 source directory")

# ---- Pinned SDL3 3.4.18 (D2-A) ---------------------------------------------
find_package(SDL3 CONFIG REQUIRED)

# ---- Pinned ENet 1.3.18 (D2-B), built from the pinned source ----------------
if(NOT XTK_ENET_DIR)
    message(FATAL_ERROR "XTK_ENET_DIR must point at the pinned ENet 1.3.18 source tree")
endif()
if(NOT EXISTS "${XTK_ENET_DIR}/include/enet/enet.h")
    message(FATAL_ERROR "XTK_ENET_DIR '${XTK_ENET_DIR}' has no include/enet/enet.h")
endif()

add_library(xtk_enet STATIC
    "${XTK_ENET_DIR}/callbacks.c"
    "${XTK_ENET_DIR}/compress.c"
    "${XTK_ENET_DIR}/host.c"
    "${XTK_ENET_DIR}/list.c"
    "${XTK_ENET_DIR}/packet.c"
    "${XTK_ENET_DIR}/peer.c"
    "${XTK_ENET_DIR}/protocol.c"
    "${XTK_ENET_DIR}/unix.c"
    "${XTK_ENET_DIR}/win32.c"
)
target_include_directories(xtk_enet PUBLIC "${XTK_ENET_DIR}/include")
# unix.c uses inet_aton(), which needs the POSIX/BSD feature macros.
target_compile_definitions(xtk_enet PRIVATE _DEFAULT_SOURCE)
if(WIN32)
    target_link_libraries(xtk_enet PUBLIC ws2_32 winmm)
endif()

if(XTK_BUILD_CLIENT)
    add_subdirectory(Client)
endif()