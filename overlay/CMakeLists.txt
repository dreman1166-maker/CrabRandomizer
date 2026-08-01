# Standalone D3D11/ImGui overlay for CrabRandomizer.
#
# Deliberately depends on NOTHING from UE4SS: its UEPseudo submodule 404s, so its SDK
# cannot be assembled from a fresh clone. We only need UE4SS to LoadLibrary us, which the
# exported start_mod/uninstall_mod stubs satisfy.
#
# ImGui and MinHook are fetched at configure time, so a clean CI runner needs no vendoring.

cmake_minimum_required(VERSION 3.20)
project(CrabRandomizerOverlay CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.90.4
    GIT_SHALLOW    TRUE
)
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        v1.3.3
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(minhook)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()

# ImGui ships no CMakeLists, so list its sources explicitly - core plus the two backends
# we actually use.
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR})

add_library(CrabRandomizerOverlay SHARED dllmain.cpp)

target_link_libraries(CrabRandomizerOverlay PRIVATE
    imgui
    minhook
    d3d11
    dxgi
    dxguid
)

# UE4SS expects Mods/<Name>/dlls/main.dll
set_target_properties(CrabRandomizerOverlay PROPERTIES
    OUTPUT_NAME "main"
    PREFIX ""
)

if(MSVC)
    target_compile_options(CrabRandomizerOverlay PRIVATE /W3 /permissive-)
    target_compile_definitions(CrabRandomizerOverlay PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()
