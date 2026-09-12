cmake_minimum_required(VERSION 3.20)
project(frame_reader_example LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV REQUIRED)

add_executable(frame_reader_example main.cpp)
target_include_directories(frame_reader_example PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(frame_reader_example PRIVATE ${OpenCV_LIBS})