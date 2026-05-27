cmake_minimum_required(VERSION 3.20)
project(multi_return LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)

add_executable(multi_return
    src/color.cpp
    src/main.cpp
)
target_include_directories(multi_return PRIVATE include ../../runtime/include ../../../topo-lang-cpp/runtime/include)
