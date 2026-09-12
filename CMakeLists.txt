cmake_minimum_required(VERSION 3.15)
project(ets2la_capture LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        v1.3.4
)
FetchContent_MakeAvailable(minhook)

add_library(capture SHARED
    dllmain.cpp
    present_hook.cpp
)
target_link_libraries(capture PRIVATE minhook d3d11 dxgi)
set_target_properties(capture PROPERTIES OUTPUT_NAME "ets2la_capture")