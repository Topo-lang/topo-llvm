cmake_minimum_required(VERSION 3.20)
project(ir_embedding LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)

add_executable(ir_embedding
    src/stages.cpp
    src/pipeline.cpp
    src/main.cpp
)
target_include_directories(ir_embedding PRIVATE include ../../runtime/include ../../../topo-lang-cpp/runtime/include)
