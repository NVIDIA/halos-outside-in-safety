# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

if(NOT SAFETY_CORE_BUILD_ADAPTERS_VSS_MDX_MSG_BUS)
  return()
endif()

find_package(Threads REQUIRED)

set(_safety_core_mdx_msg_bus_dir "${CMAKE_CURRENT_LIST_DIR}")

find_path(SAFETY_CORE_RDKAFKA_INCLUDE_DIR
  NAMES rdkafka.h
  PATH_SUFFIXES librdkafka
  DOC "Directory containing rdkafka.h from the system librdkafka installation"
)

if(NOT SAFETY_CORE_RDKAFKA_INCLUDE_DIR)
  message(FATAL_ERROR
    "nvpsf_msgbus requires rdkafka.h from the system librdkafka development files. "
    "Install librdkafka-dev or set SAFETY_CORE_RDKAFKA_INCLUDE_DIR."
  )
endif()

if(NOT TARGET SafetyCore::Stub::rdkafka)
  include("${SAFETY_CORE_SOURCE_DIR}/cmake/SafetyCoreStub.cmake")
  safety_core_add_export_stub_library(
    TARGET safety_core_stub_rdkafka
    ALIAS SafetyCore::Stub::rdkafka
    EXPORT_FILE "${SAFETY_CORE_STUB_ROOT}/rdkafka/librdkafka_dummy.export"
    OUTPUT_NAME rdkafka
    SOVERSION 1
    INCLUDE_DIR "${SAFETY_CORE_RDKAFKA_INCLUDE_DIR}"
  )
endif()

add_library(nvpsf_msgbus SHARED
  "${_safety_core_mdx_msg_bus_dir}/src/NvPSFMsgBus.c"
)

target_include_directories(nvpsf_msgbus PUBLIC
  "${_safety_core_mdx_msg_bus_dir}/include"
  "${SAFETY_CORE_RDKAFKA_INCLUDE_DIR}"
)

target_compile_definitions(nvpsf_msgbus PRIVATE
  _POSIX_C_SOURCE=200809L
  $<$<CONFIG:Debug>:NVPSF_DBG>
)

target_link_libraries(nvpsf_msgbus PUBLIC
  SafetyCore::Stub::rdkafka
  Threads::Threads
  rt
)

set_target_properties(nvpsf_msgbus PROPERTIES
  OUTPUT_NAME nvpsf_msgbus
)
