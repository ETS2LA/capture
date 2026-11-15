cmake_minimum_required(VERSION 3.20)
project(frame_reader_example LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV REQUIRED)

add_executable(frame_reader_example main.cpp)
target_include_directories(frame_reader_example PRIVATE ${OpenCV_INCLUDE_DIRS} ../../include)
target_link_libraries(frame_reader_example PRIVATE ${OpenCV_LIBS})

set(ETS2LA_TELEMETRY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../telemetry" CACHE PATH "Path to the ets2la_telemetry repository")
add_executable(synced_example synced_main.cpp)
target_include_directories(synced_example PRIVATE ${OpenCV_INCLUDE_DIRS} ../../include "${ETS2LA_TELEMETRY_DIR}/include")
target_link_libraries(synced_example PRIVATE ${OpenCV_LIBS})