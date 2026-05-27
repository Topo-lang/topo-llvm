cmake_minimum_required(VERSION 3.20)
project(obfuscation LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)

add_executable(obfuscation
    src/helpers.cpp
    src/main.cpp
)
target_include_directories(obfuscation PRIVATE include)
