# SPDX-License-Identifier: LicenseRef-CSSL-1.0

include_directories(${SRC_TOP_DIR}/api-server/)
include_directories(${SRC_TOP_DIR}/common/)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/logger)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/model)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/utils)

include(${SRC_TOP_DIR}/${MOUNTED_COMMON}/config/config.cmake)
include(${SRC_TOP_DIR}/${MOUNTED_COMMON}/model/common_model.cmake)

add_library(NEF STATIC
  nef_app.cpp
  nef_af_profile.cpp
  nef_jwt.cpp
  nef_notification_mapper.cpp
  nef_config_types.cpp
  nef_subscription.cpp
  nef_client.cpp
  task_manager.cpp
  nef_event.cpp
)
