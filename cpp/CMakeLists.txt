# Built as a sibling of the RE-UE4SS checkout, per UE4SS's C++ mod guide:
#   MyMods/RE-UE4SS/
#   MyMods/CrabRandomizerUI/
# See .github/workflows/build-cpp.yml for the exact CI invocation.
cmake_minimum_required(VERSION 3.22)

set(TARGET CrabRandomizerUI)
add_library(${TARGET} SHARED dllmain.cpp)
target_include_directories(${TARGET} PRIVATE .)
target_link_libraries(${TARGET} PUBLIC UE4SS)
set_target_properties(${TARGET} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
