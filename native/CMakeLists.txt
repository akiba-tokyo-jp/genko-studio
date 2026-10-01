cmake_minimum_required(VERSION 3.24)

# Genko Studio native (C++20 + Qt 6). The product being built by the C++ migration
# (docs/cpp-migration/). Lives beside the Python baseline in src/genko until M7.
project(GenkoNative VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(GENKO_BUILD_APP "Build the Qt Widgets desktop app (off: headless genko only)" ON)
option(GENKO_BUILD_TESTS "Build the CTest suites" ON)
option(GENKO_WERROR "Treat warnings in Genko's own code as errors (CI)" OFF)
set(GENKO_SANITIZE "" CACHE STRING "Sanitizers for Debug builds: empty, address,undefined, or thread")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GenkoFlags)
include(GenkoDeps)

set(GENKO_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(GENKO_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_subdirectory(src)

if(GENKO_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
