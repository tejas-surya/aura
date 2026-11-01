cmake_minimum_required(VERSION 3.20)

enable_language(C)

add_library(app_hook SHARED
    HookMain.cpp
)

target_include_directories(app_hook PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/hook_dll
)

target_link_libraries(app_hook PRIVATE
    user32 kernel32
)

target_compile_definitions(app_hook PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    BUILDING_HOOK_DLL
)

if(MSVC)
    target_compile_options(app_hook PRIVATE /W4 /utf-8)
else()
    target_compile_options(app_hook PRIVATE -Wall -Wextra -shared)
endif()

set_target_properties(app_hook PROPERTIES
    OUTPUT_NAME "app_hook"
    PREFIX ""
)
