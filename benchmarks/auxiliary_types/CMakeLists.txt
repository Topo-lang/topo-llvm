cmake_minimum_required(VERSION 3.20)
project(auxiliary_types LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)

add_executable(auxiliary_types
    src/auxiliary_types.cpp
    src/main.cpp
)
target_include_directories(auxiliary_types PRIVATE include)
