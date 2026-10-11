# Xtank Windows client shell (P2.9).
#
# The executable is SDL3 + ENet + the portable protocol codec.  It links no
# engine/simulation object, no bot program, no ucontext/threader, no X11 and
# no Unix loader; the source list below is the whole client.

add_executable(xtank-client
    main.c
    platform_sdl.c
    assets.c
    render_sdl.c
    timeline.c
    camera.c
    model.c
    world_render.c
    input.c
    "${CMAKE_SOURCE_DIR}/Protocol/encode.c"
    "${CMAKE_SOURCE_DIR}/Protocol/decode.c"
    "${CMAKE_SOURCE_DIR}/Protocol/content-manifest.c"
)

target_include_directories(xtank-client PRIVATE
    "${CMAKE_SOURCE_DIR}/Protocol"
    "${CMAKE_SOURCE_DIR}/Client"
)

# SDL3 is included only by the platform shell (platform_sdl.c) and the
# snapshot-driven renderer (render_sdl.c); assets.c and the protocol codec are
# pure C.
target_link_libraries(xtank-client PRIVATE SDL3::SDL3 xtk_enet)

if(UNIX)
    # lroundf/fabsf for the DPI mapping; MinGW folds these into the CRT.
    target_link_libraries(xtank-client PRIVATE m)
endif()

target_compile_options(xtank-client PRIVATE -Wall -Wextra)

if(WIN32)
    # No libgcc_s runtime DLL dependency (D2-D clean-machine audit).
    target_link_options(xtank-client PRIVATE -static-libgcc)
    # Ship SDL3.dll next to the executable so the archive is self-contained.
    if(NOT XTK_SDL3_DLL)
        get_filename_component(_sdl3_prefix "${SDL3_DIR}/../../.." ABSOLUTE)
        set(XTK_SDL3_DLL "${_sdl3_prefix}/bin/SDL3.dll")
    endif()
    if(EXISTS "${XTK_SDL3_DLL}")
        add_custom_command(TARGET xtank-client POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${XTK_SDL3_DLL}" "$<TARGET_FILE_DIR:xtank-client>/SDL3.dll"
            COMMENT "Staging SDL3.dll next to xtank-client.exe")
    endif()
endif()

install(TARGETS xtank-client RUNTIME DESTINATION bin)