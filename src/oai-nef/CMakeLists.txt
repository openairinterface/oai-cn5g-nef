# SPDX-License-Identifier: LicenseRef-CSSL-1.0

cmake_minimum_required (VERSION 3.0.2)

macro(add_option name val helpstr)
  if(DEFINED ${name})
    set(value ${${name}})
  else()
    set(value ${val})
  endif()
  set(${name} ${value} CACHE STRING "${helpstr}")
  add_definitions("-D${name}=${value}")
endmacro(add_option)

macro(add_boolean_option name val helpstr)
  if(DEFINED ${name})
    set(value ${${name}})
  else()
    set(value ${val})
  endif()
  set(${name} ${value} CACHE STRING "${helpstr}")
  set_property(CACHE ${name} PROPERTY TYPE BOOL)
  if(${value})
    add_definitions("-D${name}=1")
  else()
    add_definitions("-D${name}=0")
  endif()
endmacro(add_boolean_option)

macro(add_list_string_option name val helpstr)
  if(DEFINED ${name})
    set(value ${${name}})
  else()
    set(value ${val})
  endif()
  set(${name} ${value} CACHE STRING "${helpstr}")
  set_property(CACHE ${name} PROPERTY STRINGS ${ARGN})
  if(NOT "${value}" STREQUAL "False")
    add_definitions("-D${name}=\"${value}\"")
  endif()
endmacro(add_list_string_option)

# Build type
add_list_string_option(CMAKE_BUILD_TYPE "RelWithDebInfo"
    "Build type: Debug Release RelWithDebInfo MinSizeRel"
    Debug Release RelWithDebInfo MinSizeRel)
message("Build type is ${CMAKE_BUILD_TYPE}")
if(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
  add_boolean_option(LOG_OAI True "Thread safe logging API")
  SET(ASAN asan)
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_boolean_option(LOG_OAI True "Thread safe logging API")
  SET(ASAN asan)
endif()

# Processor flags
message("Architecture is ${CMAKE_SYSTEM_PROCESSOR}")
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "armv7l")
  set(C_FLAGS_PROCESSOR "-gdwarf-2 -mfloat-abi=hard -mfpu=neon -lgcc -lrt")
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
  set(C_FLAGS_PROCESSOR "-gdwarf-2 -lgcc -lrt")
else()
  set(C_FLAGS_PROCESSOR "-msse4.2")
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${C_FLAGS_PROCESSOR} -std=gnu99 -Wall -fno-strict-aliasing -rdynamic -fPIC")
set(CMAKE_CXX_FLAGS_DEBUG          "${CMAKE_CXX_FLAGS} -std=c++17 -g -O0 -fstack-protector-all -fsanitize=address -fno-omit-frame-pointer")
set(CMAKE_CXX_FLAGS_RELEASE        "${CMAKE_CXX_FLAGS} -std=c++17 -O2 -fno-omit-frame-pointer")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS} -std=c++17 -g -O2")
set(CMAKE_CXX_FLAGS_MINSIZEREL     "${CMAKE_CXX_FLAGS} -std=c++17 -Os")

add_definitions(-DCMAKER)
add_definitions(-DBSTRLIB_CAN_USE_STL=1 -DBSTRLIB_CAN_USE_IOSTREAM=1)

# Git version
set(GIT_BRANCH "UNKNOWN")
set(GIT_COMMIT_HASH "UNKNOWN")
set(GIT_COMMIT_DATE "UNKNOWN")
find_package(Git)
if(GIT_FOUND)
  execute_process(COMMAND git rev-parse --abbrev-ref HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE GIT_BRANCH OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(COMMAND git log -1 --format=%h
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE GIT_COMMIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(COMMAND git log -1 --format=%cd
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE GIT_COMMIT_DATE OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()
add_definitions("-DPACKAGE_VERSION=\"Branch: ${GIT_BRANCH} Hash: ${GIT_COMMIT_HASH} Date: ${GIT_COMMIT_DATE}\"")
add_definitions("-DPACKAGE_BUGREPORT=\"openaircn-user@lists.eurecom.fr\"")

set(CMAKE_MODULE_PATH "${OPENAIRCN_DIR}/build/common-build/cmake_modules" "${CMAKE_MODULE_PATH}")
include(FindPkgConfig)

find_package(cpr REQUIRED)

add_boolean_option(DISPLAY_LICENCE_INFO False "Show licence banner")
add_boolean_option(LOG_OAI             False "Thread safe logging utility")

include_directories(${CONFIG_INCLUDE_DIRS})
set(THREADS_PREFER_PTHREAD_FLAG ON)

ADD_SUBDIRECTORY(${CMAKE_CURRENT_SOURCE_DIR}/../../src/common       ${CMAKE_CURRENT_BINARY_DIR}/common)
ADD_SUBDIRECTORY(${CMAKE_CURRENT_SOURCE_DIR}/../../src/api-server   ${CMAKE_CURRENT_BINARY_DIR}/api-server)

add_definitions("-DPACKAGE_NAME=\"NEF\"")

include_directories(${SRC_TOP_DIR}/nef_app)
include_directories(${SRC_TOP_DIR}/oai-nef)
include_directories(${SRC_TOP_DIR}/common)
include_directories(${SRC_TOP_DIR}/api-server)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/http)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/logger)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/model)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/utils)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/utils/bstr)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/nas/common)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/nas/utils)
include_directories(${SRC_TOP_DIR}/${MOUNTED_COMMON}/nas/ies)

add_executable(nef
    ${SRC_TOP_DIR}/oai-nef/main.cpp
    ${SRC_TOP_DIR}/oai-nef/options.cpp
)

include(${SRC_TOP_DIR}/${MOUNTED_COMMON}/common/common.cmake)
include(${SRC_TOP_DIR}/${MOUNTED_COMMON}/config/config.cmake)
include(${SRC_TOP_DIR}/${MOUNTED_COMMON}/http/http.cmake)
include(${SRC_TOP_DIR}/${MOUNTED_COMMON}/logger/logger.cmake)
include(${SRC_TOP_DIR}/${MOUNTED_COMMON}/utils/utils.cmake)
include(${SRC_TOP_DIR}/${MOUNTED_COMMON}/model/nef_model.cmake)

target_link_libraries(nef ${ASAN}
    -Wl,--start-group
      NEF NEF_API
      -lnettle ${NETTLE_LIBRARIES} ${CRYPTO_LIBRARIES}
      -lnghttp2 -lboost_system -lboost_thread -lboost_date_time
      -lfmt -lspdlog -lssl -lcrypto dl
    -Wl,--end-group
    pthread m rt event event_pthreads boost_system curl -lyaml-cpp cpr::cpr)

add_boolean_option(USE_LTTNG False "Activate LTTng tracing" ON)
if(USE_LTTNG)
  message("Using the lttng build variant")
  add_definitions("-DLOGGER_CAN_USE_LTTNG")
  target_link_libraries(nef -ldl lttng-ust)
endif()
