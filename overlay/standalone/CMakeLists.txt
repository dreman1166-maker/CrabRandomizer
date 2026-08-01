# Plan B: standalone transparent overlay EXE. Injects nothing, hooks nothing.
cmake_minimum_required(VERSION 3.20)
project(CrabOverlayApp LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.90.4 GIT_SHALLOW TRUE)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR})

# WIN32 => a windowed app with no console popping up behind the game.
add_executable(CrabOverlayApp WIN32 main.cpp)
target_link_libraries(CrabOverlayApp PRIVATE imgui d3d11 dxgi dxguid)

if(MSVC)
    target_compile_options(CrabOverlayApp PRIVATE /W3 /permissive-)
    target_compile_definitions(CrabOverlayApp PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()
