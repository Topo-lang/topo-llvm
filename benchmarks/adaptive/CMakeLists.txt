cmake_minimum_required(VERSION 3.20)
project(adaptive_parallel LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)

add_executable(adaptive_parallel
    src/stages.cpp
    src/pipeline.cpp
    src/main.cpp
)
target_include_directories(adaptive_parallel PRIVATE include ../../runtime/include ../../../topo-lang-cpp/runtime/include)

# --- Threshold comparison variants ---
# Three builds with different min_trigger_ns values (1000 / 10000 / 100000)
# to evaluate the cost/benefit of aggressive vs conservative JIT triggering.

add_executable(adaptive_threshold_low
    src/stages.cpp
    src/pipeline.cpp
    src/main.cpp
)
target_include_directories(adaptive_threshold_low PRIVATE include ../../runtime/include ../../../topo-lang-cpp/runtime/include)

add_executable(adaptive_threshold_mid
    src/stages.cpp
    src/pipeline.cpp
    src/main.cpp
)
target_include_directories(adaptive_threshold_mid PRIVATE include ../../runtime/include ../../../topo-lang-cpp/runtime/include)

add_executable(adaptive_threshold_high
    src/stages.cpp
    src/pipeline.cpp
    src/main.cpp
)
target_include_directories(adaptive_threshold_high PRIVATE include ../../runtime/include ../../../topo-lang-cpp/runtime/include)
