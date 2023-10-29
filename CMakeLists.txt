cmake_minimum_required(VERSION 3.26)

set( TargetName test_bbx15 )
project(${TargetName})

set(CMAKE_CXX_STANDARD 20)

find_package(Threads REQUIRED)

add_executable(${TargetName} main.cpp include/fswatch.hpp )
target_include_directories(${TargetName}  PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(${TargetName} PUBLIC Threads::Threads)
