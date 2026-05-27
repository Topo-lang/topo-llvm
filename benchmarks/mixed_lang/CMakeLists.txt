cmake_minimum_required(VERSION 3.20)
project(mixed_lang LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
# Note: standalone CMake build requires pre-built Rust library
add_executable(mixed_lang src/engine.cpp src/main.cpp)
target_include_directories(mixed_lang PRIVATE include)
