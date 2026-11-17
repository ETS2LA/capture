cmake_minimum_required(VERSION 3.15)
project(ets2la_capture VERSION 1.1.1 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ETS2LA_TELEMETRY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../telemetry" CACHE PATH "Path to the ets2la_telemetry repository")
if(NOT EXISTS "${ETS2LA_TELEMETRY_DIR}/include/ets2la_telemetry/telemetry_reader.h")
    message(FATAL_ERROR "ETS2LA_TELEMETRY_DIR does not contain include/ets2la_telemetry (got '${ETS2LA_TELEMETRY_DIR}'). Pass -DETS2LA_TELEMETRY_DIR=<path to telemetry repo>.")
endif()

include(FetchContent)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        v1.3.4
)
FetchContent_MakeAvailable(minhook)

add_library(ets2la_capture SHARED
    src/dll/dllmain.cpp
    src/dll/present_hook.cpp
)

target_compile_definitions(ets2la_capture PRIVATE "ETS2LA_CAPTURE_VERSION=\"${PROJECT_VERSION}\"")
target_include_directories(ets2la_capture PRIVATE include "${ETS2LA_TELEMETRY_DIR}/include")
target_link_libraries(ets2la_capture PRIVATE minhook d3d11 dxgi)
set_target_properties(ets2la_capture PROPERTIES OUTPUT_NAME "ets2la_capture")