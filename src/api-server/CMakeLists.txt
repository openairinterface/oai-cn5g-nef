# SPDX-License-Identifier: LicenseRef-CSSL-1.0

set(NEF_API_SERVER_DIR "${SRC_TOP_DIR}/api-server")

include_directories(${NEF_API_SERVER_DIR}/)
include_directories(${SRC_TOP_DIR}/nef_app)
include_directories(${SRC_TOP_DIR}/common)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/3gpp)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/common)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/logger)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/model)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/sba)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/utils)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/utils/bstr)

include(${SRC_TOP_DIR}/${MOUNTED_COMMON}/config/config.cmake)
include(${SRC_TOP_DIR}/${MOUNTED_COMMON}/model/common_model.cmake)

file(GLOB NEF_API_SERVER_src_files
    ${NEF_API_SERVER_DIR}/nef-http2-server.cpp
)

add_library(NEF_API STATIC
    ${NEF_API_SERVER_src_files})
