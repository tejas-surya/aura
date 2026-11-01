cmake_minimum_required(VERSION 3.20)

enable_language(C)

add_library(dxgi_assist SHARED
    HookMain.cpp
)

target_include_directories(dxgi_assist PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/hook_dll
)

target_link_libraries(dxgi_assist PRIVATE
    user32 kernel32
)

target_compile_definitions(dxgi_assist PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    BUILDING_HOOK_DLL
)

if(MSVC)
    target_compile_options(dxgi_assist PRIVATE /W4 /utf-8)
else()
    target_compile_options(dxgi_assist PRIVATE -Wall -Wextra -shared)
endif()

set_target_properties(dxgi_assist PROPERTIES
    OUTPUT_NAME "dxgi_assist"
    PREFIX ""
)
