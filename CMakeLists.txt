cmake_minimum_required(VERSION 3.22)
project(rinha_2026_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

add_library(rinha_core INTERFACE)
target_include_directories(rinha_core INTERFACE src)
target_link_libraries(rinha_core INTERFACE Threads::Threads)
