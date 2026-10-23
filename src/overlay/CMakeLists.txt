# Performance overlay: FPS, frame time and whatever the other libraries add,
# drawn over the picture before it is shown. See overlay.h.
#   _WIN32 -> Direct3D 11 (overlay.c, overlay_d3d11.c)
#   POSIX  -> nothing; overlay.h has empty inline functions

if(WIN32)
    add_library(xbox_overlay STATIC
        overlay.c
        overlay_d3d11.c
    )
    target_link_libraries(xbox_overlay PUBLIC d3d11 dxgi d3dcompiler gdi32)
    target_include_directories(xbox_overlay PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

    if(MSVC)
        target_compile_options(xbox_overlay PRIVATE /W4 /O2)
        target_compile_definitions(xbox_overlay PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
else()
    add_library(xbox_overlay INTERFACE)
    target_include_directories(xbox_overlay INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
endif()
