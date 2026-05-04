cmake_minimum_required(VERSION 3.22)
project(rinha_2026_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

set(RINHA_CXX_SOURCES
  ${RINHA_CORE_SOURCES}
  cmd/build-index/main.cc
  cmd/fraud-server/main.cc
  cmd/validate-index/main.cc
)

set(RINHA_FORMAT_FILES
  ${RINHA_CXX_SOURCES}
  src/ann/index.h
  src/ann/topk.h
  src/http/server.h
  src/util/time_parse.h
  src/vectorize/fraud_vector.h
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

add_executable(validate-index cmd/validate-index/main.cc)
target_link_libraries(validate-index PRIVATE rinha_core)

find_program(CLANG_FORMAT_BIN
  NAMES clang-format clang-format-19 clang-format-18 clang-format-17 clang-format-16
  HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin
)

if(CLANG_FORMAT_BIN)
  add_custom_target(format
    COMMAND ${CLANG_FORMAT_BIN} -i ${RINHA_FORMAT_FILES}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Formatting C++ sources with clang-format"
  )

  add_custom_target(format-check
    COMMAND ${CLANG_FORMAT_BIN} --dry-run --Werror ${RINHA_FORMAT_FILES}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Checking C++ formatting with clang-format"
  )
else()
  add_custom_target(format
    COMMAND ${CMAKE_COMMAND} -E echo "clang-format not found. Install LLVM or set CLANG_FORMAT_BIN."
    COMMAND ${CMAKE_COMMAND} -E false
  )

  add_custom_target(format-check
    COMMAND ${CMAKE_COMMAND} -E echo "clang-format not found. Install LLVM or set CLANG_FORMAT_BIN."
    COMMAND ${CMAKE_COMMAND} -E false
  )
endif()

find_program(CLANG_TIDY_BIN
  NAMES clang-tidy clang-tidy-19 clang-tidy-18 clang-tidy-17 clang-tidy-16
  HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin
)

if(CLANG_TIDY_BIN)
  add_custom_target(lint
    COMMAND ${CLANG_TIDY_BIN} --quiet -p ${CMAKE_BINARY_DIR} ${RINHA_CXX_SOURCES}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Linting C++ sources with clang-tidy"
  )
else()
  add_custom_target(lint
    COMMAND ${CMAKE_COMMAND} -E echo "clang-tidy not found. Install LLVM or set CLANG_TIDY_BIN."
    COMMAND ${CMAKE_COMMAND} -E false
  )
endif()

add_custom_target(check
  DEPENDS format-check lint
)
