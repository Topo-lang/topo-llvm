cmake_minimum_required(VERSION 3.20)
project(data_layout_perf LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)

add_executable(data_layout_perf
    src/simulate.cpp
    src/pipeline.cpp
    src/main.cpp
)
target_include_directories(data_layout_perf PRIVATE
    include
    ../../../topo-lang-cpp/runtime/include
    ../../runtime/include
)
