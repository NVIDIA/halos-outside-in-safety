# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.22)

project(safety-core
  VERSION 1.2.1
  LANGUAGES C CXX
)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SAFETY_CORE_BUILD_ADAPTERS "Build safety-core adapters" ON)
option(SAFETY_CORE_BUILD_ADAPTERS_VSS "Build VSS adapters" ON)
option(SAFETY_CORE_BUILD_ADAPTERS_VSS_MDX_MSG_BUS "Build VSS MDX libnvpsf_msgbus.so" ON)
option(SAFETY_CORE_BUILD_ADAPTERS_VSS_MDX_MSG_CODEC "Build VSS MDX libnvpsf_msgcodec.so" ON)
option(SAFETY_CORE_BUILD_ADAPTERS_VSS_MDX_CLIENT "Build VSS mdx_client adapter" ON)
option(SAFETY_CORE_BUILD_AI_MONITOR "Build the ai-monitor component" ON)
option(SAFETY_CORE_BUILD_AI_MONITOR_SAFETY_MONITOR "Build ai-monitor safety_monitor" ON)
option(SAFETY_CORE_BUILD_BLACK_BOX "Build the black-box component" ON)
option(SAFETY_CORE_BUILD_BLACK_BOX_NVPSB "Build black-box libnvpsb.so" ON)
option(SAFETY_CORE_BUILD_COMMON "Build common safety-core libraries" ON)
option(SAFETY_CORE_BUILD_DECISION_MAKERS "Build safety-core decision-makers" ON)
option(SAFETY_CORE_BUILD_DECISION_MAKERS_ATL "Build ATL decision-maker" ON)
option(SAFETY_CORE_BUILD_DECISION_MAKERS_ATL_SDM "Build ATL atl_sdm executable" ON)
option(SAFETY_CORE_BUILD_DECISION_MAKERS_ATL_CMD_RECEIVER "Build ATL atl_sdm_cmd_receiver executable" ON)
option(SAFETY_CORE_BUILD_DECISION_MAKERS_PROXIMITY "Build proximity decision-maker" ON)
option(SAFETY_CORE_BUILD_DECISION_MAKERS_PROXIMITY_SDM "Build proximity proximity_sdm executable" ON)
option(SAFETY_CORE_BUILD_DECISION_MAKERS_PROXIMITY_CMD_RECEIVER "Build proximity proximity_sdm_cmd_receiver executable" ON)
option(SAFETY_CORE_BUILD_EVENT_INTEGRATOR "Build the event-integrator component" ON)
option(SAFETY_CORE_BUILD_EVENT_INTEGRATOR_PSS_DAEMON "Build event-integrator pss_daemon" ON)
option(SAFETY_CORE_BUILD_EVENT_INTEGRATOR_INTERFACE "Build event-integrator nvpssd_interface library" ON)
option(SAFETY_CORE_BUILD_PROTOCOLS "Build the protocols component" ON)
option(SAFETY_CORE_BUILD_PROTOCOLS_DECISION_MAKER_GATEWAY "Build the protocols/decision-maker-gateway component" ON)
option(SAFETY_CORE_BUILD_PROTOCOLS_DECISION_MAKER_GATEWAY_NVPSD "Build protocols/decision-maker-gateway libnvpsd.so" ON)
option(SAFETY_CORE_BUILD_PROTOCOLS_DECISION_MAKER_GATEWAY_PROCESS "Build protocols/decision-maker-gateway nvpsd_gateway process" ON)
option(SAFETY_CORE_BUILD_SAFECOMM "Build the safecomm component" ON)
option(SAFETY_CORE_BUILD_SAFECOMM_POSIX_MSG_QUE "Build safecomm libnvpss_msg_que.so" ON)
option(SAFETY_CORE_BUILD_SAFECOMM_POSIX_SOCKETS "Build safecomm libnvpss_socket_comms.so" ON)
option(SAFETY_CORE_ENABLE_PROFILE "Enable ai-monitor PROFILE compile definition" OFF)

set(SAFETY_CORE_TARGET_ARCH "native" CACHE STRING "Target architecture: native, x86_64, arm64, or aarch64")
set_property(CACHE SAFETY_CORE_TARGET_ARCH PROPERTY STRINGS native x86_64 arm64 aarch64)

set(_safety_core_effective_target_arch "${SAFETY_CORE_TARGET_ARCH}")
string(TOLOWER "${_safety_core_effective_target_arch}" _safety_core_effective_target_arch)
if(_safety_core_effective_target_arch STREQUAL "native")
  set(_safety_core_effective_target_arch "${CMAKE_SYSTEM_PROCESSOR}")
  string(TOLOWER "${_safety_core_effective_target_arch}" _safety_core_effective_target_arch)
endif()

if(_safety_core_effective_target_arch STREQUAL "amd64")
  set(_safety_core_effective_target_arch "x86_64")
elseif(_safety_core_effective_target_arch STREQUAL "arm64")
  set(_safety_core_effective_target_arch "aarch64")
endif()

if(NOT _safety_core_effective_target_arch MATCHES "^(x86_64|aarch64)$")
  message(FATAL_ERROR
    "Unsupported SAFETY_CORE_TARGET_ARCH '${SAFETY_CORE_TARGET_ARCH}'. "
    "Use native, x86_64, arm64, or aarch64."
  )
endif()

set(SAFETY_CORE_EFFECTIVE_TARGET_ARCH "${_safety_core_effective_target_arch}" CACHE INTERNAL
  "Normalized safety-core target architecture")

if(SAFETY_CORE_EFFECTIVE_TARGET_ARCH STREQUAL "aarch64")
  set(_safety_core_system_processor "${CMAKE_SYSTEM_PROCESSOR}")
  string(TOLOWER "${_safety_core_system_processor}" _safety_core_system_processor)
  if(_safety_core_system_processor STREQUAL "arm64")
    set(_safety_core_system_processor "aarch64")
  endif()

  if(NOT _safety_core_system_processor STREQUAL "aarch64")
    message(FATAL_ERROR
      "SAFETY_CORE_TARGET_ARCH=${SAFETY_CORE_TARGET_ARCH} requires an aarch64 native build "
      "or a cross toolchain. Configure with "
      "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchains/aarch64-linux-gnu.cmake."
    )
  endif()
endif()

set(SAFETY_CORE_CUDA_TOOLKIT_ROOT "/usr/local/cuda-13.0" CACHE PATH "CUDA Toolkit root")
set(SAFETY_CORE_CUDA_TARGET_DIR "" CACHE PATH "CUDA target directory, for example /usr/local/cuda-13.0/targets/x86_64-linux")
set(SAFETY_CORE_ARM64_CUDA_TARGET_NAME "sbsa-linux" CACHE STRING
  "CUDA target directory name for arm64 builds, for example sbsa-linux or aarch64-linux")
set_property(CACHE SAFETY_CORE_ARM64_CUDA_TARGET_NAME PROPERTY STRINGS sbsa-linux aarch64-linux)
set(SAFETY_CORE_AI_MONITOR_CUDA_ARCHITECTURES "75" CACHE STRING "CUDA architectures for ai-monitor safety_monitor")

set(SAFETY_CORE_SENSOR_CONFIG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/components/common/sensor_config" CACHE PATH
  "Path to the sensor_config helper source")

set(SAFETY_CORE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(SAFETY_CORE_COMPONENTS_DIR "${SAFETY_CORE_SOURCE_DIR}/components")
set(SAFETY_CORE_ADAPTERS_DIR "${SAFETY_CORE_SOURCE_DIR}/adapters")
set(SAFETY_CORE_DECISION_MAKERS_DIR "${SAFETY_CORE_SOURCE_DIR}/decision-makers")
set(SAFETY_CORE_THIRD_PARTY_HEADERS_DIR "${SAFETY_CORE_SOURCE_DIR}/cmake/third-party-headers")
set(SAFETY_CORE_STUB_ROOT "${SAFETY_CORE_SOURCE_DIR}/cmake/stubs" CACHE PATH
  "Path to safety-core dependency stub export files")
set(SAFETY_CORE_NVCUVID_INCLUDE_DIR "${SAFETY_CORE_THIRD_PARTY_HEADERS_DIR}/nvcuvid/include" CACHE PATH
  "Path to nvcuvid/cuviddec headers")

add_subdirectory(components)

if(SAFETY_CORE_BUILD_DECISION_MAKERS)
  add_subdirectory(decision-makers)
endif()

if(SAFETY_CORE_BUILD_ADAPTERS)
  add_subdirectory(adapters)
endif()

add_subdirectory(pkg)
