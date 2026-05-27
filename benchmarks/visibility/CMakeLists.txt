cmake_minimum_required(VERSION 3.20)
project(hello_visibility LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)

add_executable(hello_visibility
    src/calculator.cpp
    src/main.cpp
)
target_include_directories(hello_visibility PRIVATE include)
