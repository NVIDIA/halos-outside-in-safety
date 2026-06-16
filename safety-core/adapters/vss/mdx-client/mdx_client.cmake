# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

find_package(Threads REQUIRED)

set(_safety_core_mdx_client_dir "${CMAKE_CURRENT_LIST_DIR}")
set(_safety_core_sensor_config_dir "${SAFETY_CORE_SENSOR_CONFIG_DIR}")

if(NOT EXISTS "${_safety_core_sensor_config_dir}/src/sensor_config_parser.cpp")
  message(FATAL_ERROR
    "sensor_config_parser.cpp not found. Set SAFETY_CORE_SENSOR_CONFIG_DIR to a directory "
    "with src/sensor_config_parser.cpp and inc/sensor_config_parser.h."
  )
endif()

foreach(_safety_core_required_target IN ITEMS nvpsb nvpssd_interface nvpsf_msgbus nvpsf_msgcodec)
  if(NOT TARGET "${_safety_core_required_target}")
    message(FATAL_ERROR "mdx_client requires target ${_safety_core_required_target}")
  endif()
endforeach()

add_executable(mdx_client
  "${_safety_core_mdx_client_dir}/src/MDXClient.cpp"
  "${_safety_core_mdx_client_dir}/src/EventsParser.cpp"
  "${_safety_core_mdx_client_dir}/src/FramesParser.cpp"
  "${_safety_core_mdx_client_dir}/src/SafetyEventReporter.cpp"
  "${_safety_core_mdx_client_dir}/src/main.cpp"
  "${_safety_core_sensor_config_dir}/src/sensor_config_parser.cpp"
)

target_include_directories(mdx_client PRIVATE
  "${_safety_core_mdx_client_dir}/include"
  "${SAFETY_CORE_COMPONENTS_DIR}/black-box/include"
  "${SAFETY_CORE_COMPONENTS_DIR}/event-integrator/daemon/include"
  "${_safety_core_sensor_config_dir}/inc"
)

target_compile_definitions(mdx_client PRIVATE
  $<$<CONFIG:Debug>:NVPSF_DBG>
)

target_compile_options(mdx_client PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-Wno-error=redundant-decls>
  $<$<COMPILE_LANGUAGE:CXX>:-Wno-error=missing-declarations>
)

target_link_libraries(mdx_client PRIVATE
  nvpsf_msgbus
  nvpsf_msgcodec
  nvpssd_interface
  nvpsb
  Threads::Threads
  rt
)
