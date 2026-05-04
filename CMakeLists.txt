cmake_minimum_required(VERSION 3.22)
project(rinha_2026_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

set(RINHA_CORE_SOURCES
  src/ann/ivf_scan.cc
  src/ann/flat_scan.cc
  src/http/server.cc
  src/json/request_parser.cc
  src/vectorize/fraud_vector.cc
  src/util/time_parse.cc
)

add_library(rinha_core
  ${RINHA_CORE_SOURCES}
)

target_include_directories(rinha_core PUBLIC src)
target_compile_options(rinha_core PRIVATE
  $<$<CONFIG:Release>:-O3>
  $<$<CONFIG:Release>:-DNDEBUG>
)
target_link_libraries(rinha_core PUBLIC Threads::Threads)

add_executable(build-index cmd/build-index/main.cc)
target_include_directories(build-index PRIVATE src)
target_link_libraries(build-index PRIVATE rinha_core ZLIB::ZLIB)
target_compile_options(build-index PRIVATE $<$<CONFIG:Release>:-O3>)

add_executable(fraud-server cmd/fraud-server/main.cc)
target_link_libraries(fraud-server PRIVATE rinha_core)
