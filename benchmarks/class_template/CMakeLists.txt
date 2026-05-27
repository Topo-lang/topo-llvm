cmake_minimum_required(VERSION 3.20)
project(class_template LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)

add_executable(class_template
    src/class_template.cpp
    src/main.cpp
)
target_include_directories(class_template PRIVATE include)
