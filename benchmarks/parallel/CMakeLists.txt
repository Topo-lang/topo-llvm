cmake_minimum_required(VERSION 3.20)
project(parallel_runtime LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)

add_executable(parallel_runtime
    src/main.cpp
)
target_include_directories(parallel_runtime PRIVATE include ../../runtime/include ../../../topo-lang-cpp/runtime/include)
target_link_libraries(parallel_runtime PRIVATE topo-parallel)
