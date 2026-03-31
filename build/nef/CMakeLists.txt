# SPDX-License-Identifier: LicenseRef-CSSL-1.0

cmake_minimum_required (VERSION 3.0.2)
project(oai-cn)

# Override options for NEF
set (  PACKAGE_NAME                    "NEF" )
set (  STATIC_LINKING                   False )
#############################################
# Base directories, compatible with legacy OAI building
################################################
if (DEFINED ENV{OPENAIRCN_DIR})
  set (OPENAIRCN_DIR   $ENV{OPENAIRCN_DIR})
else()
  set (OPENAIRCN_DIR ${CMAKE_CURRENT_LIST_DIR}/../..)
endif()
set (BUILD_TOP_DIR   ${OPENAIRCN_DIR}/build)
set (SRC_TOP_DIR     ${OPENAIRCN_DIR}/src)

set (NF_TARGET           nef)
set (MOUNTED_COMMON      common-src)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../src/oai-nef/CMakeLists.txt)

ADD_SUBDIRECTORY(${CMAKE_CURRENT_SOURCE_DIR}/../../src/nef_app ${CMAKE_CURRENT_BINARY_DIR}/nef_app)
