cmake_minimum_required(VERSION 3.20)
project(pipeline LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)

add_executable(pipeline
    src/stages.cpp
    src/pipeline.cpp
    src/main.cpp
)
target_include_directories(pipeline PRIVATE include ../../runtime/include ../../../topo-lang-cpp/runtime/include)
