# SPDX-License-Identifier: LicenseRef-CSSL-1.0

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
set(NF_TARGET     oai_nef)
set(MOUNTED_COMMON common-src)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../src/oai-nef/CMakeLists.txt)

ADD_SUBDIRECTORY(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/nef_app
    ${CMAKE_CURRENT_BINARY_DIR}/nef_app)

