cmake_minimum_required(VERSION 3.20)

project(synchronized_value LANGUAGES CXX)

# Global C++ settings
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(synchronized_value main.cpp)