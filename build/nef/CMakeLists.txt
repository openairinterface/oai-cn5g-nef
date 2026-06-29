################################################################################
# Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
# contributor license agreements.
################################################################################
cmake_minimum_required (VERSION 3.0.2)
project(oai-cn)

# Override options for NEF
set(PACKAGE_NAME  "NEF")
set(STATIC_LINKING False)

# Ensure C++17 is used regardless of build type
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
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

################################################################################
# Tests (GoogleTest) — opt-in via -DBUILD_TESTS=ON.
#
# Wires the tracked test/ sources into the build so a clean reconfigure
# re-discovers them via gtest_discover_tests. The GoogleTest and reflect-cpp
# dependencies are fetched by CMake FetchContent (same versions the dev/CI
# build already uses: googletest v1.14.0, reflect-cpp v0.19.0); a pre-populated
# ${FETCHCONTENT_BASE_DIR}/_deps cache is reused offline. reflect-cpp is fetched
# here (not only transitively) so the test targets that link it have the target
# available regardless of how the production build resolves the headers.
################################################################################
option(BUILD_TESTS "Build the NEF unit tests" OFF)
if(BUILD_TESTS)
  include(FetchContent)
  enable_testing()
  # googletest links Threads::Threads. Create the imported target (the main NEF
  # build links bare -lpthread and never triggers this find_package). On this
  # toolchain pthread is built into glibc, so find_package reports
  # "Found Threads: TRUE" without creating the imported target; guard-create it.
  set(THREADS_PREFER_PTHREAD_FLAG ON)
  find_package(Threads REQUIRED)
  if(NOT TARGET Threads::Threads)
    add_library(Threads::Threads INTERFACE IMPORTED)
    set_property(TARGET Threads::Threads PROPERTY
      INTERFACE_LINK_LIBRARIES "${CMAKE_THREAD_LIBS_INIT};-pthread")
  endif()

  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0)
  # Don't override the parent project's compiler/linker settings on Windows.
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  if(NOT TARGET reflectcpp)
    set(REFLECTCPP_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
      reflectcpp
      GIT_REPOSITORY https://github.com/getml/reflect-cpp.git
      GIT_TAG        v0.19.0)
    # reflect-cpp 0.19.0 requires C++20; the parent project pins
    # CMAKE_CXX_STANDARD 17, which the FetchContent subdir would otherwise
    # inherit and fail to compile. Bump for the reflectcpp scope only, restore
    # after.
    set(_nef_saved_cxx_standard ${CMAKE_CXX_STANDARD})
    set(CMAKE_CXX_STANDARD 20)
    FetchContent_MakeAvailable(reflectcpp)
    set(CMAKE_CXX_STANDARD ${_nef_saved_cxx_standard})
  endif()

  ADD_SUBDIRECTORY(
      ${CMAKE_CURRENT_SOURCE_DIR}/../../test
      ${CMAKE_CURRENT_BINARY_DIR}/test)
endif()
