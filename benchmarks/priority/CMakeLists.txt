cmake_minimum_required(VERSION 3.20)
project(priority LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)

add_executable(priority
    src/compute.cpp
    src/main.cpp
)
target_include_directories(priority PRIVATE include)
