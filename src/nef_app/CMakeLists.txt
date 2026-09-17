# SPDX-License-Identifier: LicenseRef-CSSL-1.0

include_directories(${SRC_TOP_DIR}/nef_app/helper/)
include_directories(${SRC_TOP_DIR}/common/)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/3gpp)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/logger)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/model)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/sba)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/utils)

include(${SRC_TOP_DIR}/${MOUNTED_COMMON}/config/config.cmake)
include(${SRC_TOP_DIR}/${MOUNTED_COMMON}/model/common_model.cmake)

add_library(NEF STATIC
  nef_app_core.cpp
  nef_app_analytics.cpp
  nef_app_bdt.cpp
  nef_app_event_exposure.cpp
  nef_app_monitoring.cpp
  nef_app_pfd.cpp
  nef_app_qos.cpp
  nef_app_traffic_influence.cpp
  helper/nef_af_profile.cpp
  helper/nef_jwt.cpp
  helper/nef_notification_mapper.cpp
  helper/nef_config_types.cpp
  helper/nef_subscription.cpp
  helper/nef_client.cpp
  helper/nef_event.cpp
  helper/nef_app_adapter.cpp
)
