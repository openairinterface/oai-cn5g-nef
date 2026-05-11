################################################################################
# Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
# contributor license agreements.
################################################################################
cmake_minimum_required (VERSION 3.14)
project(oai-cn)

find_package(Threads REQUIRED)

include(FetchContent)
FetchContent_Declare(
    reflectcpp
    GIT_REPOSITORY https://github.com/getml/reflect-cpp.git
    GIT_TAG        v0.19.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(reflectcpp)

# Override options for NEF
set(PACKAGE_NAME  "NEF")
set(STATIC_LINKING False)

# Ensure C++20 is used regardless of build type
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default build type if none specified
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# Base directories
if(DEFINED ENV{OPENAIRCN_DIR})
    set(OPENAIRCN_DIR $ENV{OPENAIRCN_DIR})
else()
    set(OPENAIRCN_DIR ${CMAKE_CURRENT_LIST_DIR}/../..)
endif()
set(BUILD_TOP_DIR ${OPENAIRCN_DIR}/build)
set(SRC_TOP_DIR   ${OPENAIRCN_DIR}/src)

# For common-src usage
set(NF_TARGET     nef)
set(MOUNTED_COMMON common-src)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../src/oai-nef/CMakeLists.txt)

ADD_SUBDIRECTORY(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/nef_app
    ${CMAKE_CURRENT_BINARY_DIR}/nef_app)

# ---- Unit tests (optional) ---------------------------------------------------
option(BUILD_TESTS "Build unit tests" OFF)
if(BUILD_TESTS)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
    GIT_SHALLOW    TRUE
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  enable_testing()
  ADD_SUBDIRECTORY(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../test
    ${CMAKE_CURRENT_BINARY_DIR}/test)
endif()

# ---- Benchmarks (optional) ---------------------------------------------------
option(BUILD_BENCHMARKS "Build JSON performance benchmarks" OFF)
if(BUILD_BENCHMARKS)
  set(BENCHMARK_ENABLE_TESTING  OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL  OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.8.3
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(googlebenchmark)
  find_package(nlohmann_json REQUIRED)
  ADD_SUBDIRECTORY(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../benchmark
    ${CMAKE_CURRENT_BINARY_DIR}/benchmark)
endif()
