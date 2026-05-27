cmake_minimum_required(VERSION 3.20)
project(stages LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)

add_executable(stages
    src/app.cpp
    src/main.cpp
)
target_include_directories(stages PRIVATE include)
