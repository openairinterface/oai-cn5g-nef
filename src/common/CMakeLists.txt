################################################################################
# Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
# contributor license agreements.
################################################################################

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${SRC_TOP_DIR}/common)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/logger)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/common)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/config)

# NEF common is header-only; provide an empty CN_UTILS target so the
# link line shared with other NFs resolves without error.
add_library(CN_UTILS INTERFACE)
