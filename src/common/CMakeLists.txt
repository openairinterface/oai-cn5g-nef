# SPDX-License-Identifier: LicenseRef-CSSL-1.0

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${SRC_TOP_DIR}/common)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/logger)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/common)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/config)

add_library(CN_UTILS INTERFACE)
