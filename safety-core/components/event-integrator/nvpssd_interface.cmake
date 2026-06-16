# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

find_package(Threads REQUIRED)

set(_safety_core_event_integrator_dir "${CMAKE_CURRENT_LIST_DIR}")
set(_safety_core_daemon_dir "${_safety_core_event_integrator_dir}/daemon")
set(_safety_core_safecomm_dir "${SAFETY_CORE_COMPONENTS_DIR}/safecomm")
set(_safety_core_protocols_gateway_dir "${SAFETY_CORE_COMPONENTS_DIR}/protocols/decision-maker-gateway")

set(_safety_core_sensor_config_dir "${SAFETY_CORE_SENSOR_CONFIG_DIR}")
if(NOT EXISTS "${_safety_core_sensor_config_dir}/src/sensor_config_parser.cpp")
  message(FATAL_ERROR
    "sensor_config_parser.cpp not found. Set SAFETY_CORE_SENSOR_CONFIG_DIR to a directory "
    "with src/sensor_config_parser.cpp and inc/sensor_config_parser.h."
  )
endif()

set(_safety_core_event_integrator_include_dirs
  "${_safety_core_daemon_dir}/include"
  "${_safety_core_safecomm_dir}/validation/include"
  "${_safety_core_safecomm_dir}/posix_msg_que/include"
  "${_safety_core_protocols_gateway_dir}/include"
  "${_safety_core_sensor_config_dir}/inc"
)

if(SAFETY_CORE_BUILD_EVENT_INTEGRATOR_INTERFACE)
  if(NOT TARGET nvpsb)
    message(FATAL_ERROR
      "nvpssd_interface requires nvpsb. Enable SAFETY_CORE_BUILD_BLACK_BOX and SAFETY_CORE_BUILD_BLACK_BOX_NVPSB."
    )
  endif()

  if(NOT TARGET safety_core_msg_validation)
    message(FATAL_ERROR "nvpssd_interface requires the safety_core_msg_validation target from safecomm")
  endif()

  add_library(nvpssd_interface SHARED
    "${_safety_core_daemon_dir}/src/NvPSSDaemon_interface.cpp"
  )

  target_include_directories(nvpssd_interface PUBLIC
    ${_safety_core_event_integrator_include_dirs}
  )

  target_compile_definitions(nvpssd_interface PRIVATE
    $<$<CONFIG:Debug>:NVPSF_DBG>
  )

  target_link_libraries(nvpssd_interface PUBLIC
    nvpsb
    safety_core_msg_validation
    Threads::Threads
  )

  set_target_properties(nvpssd_interface PROPERTIES
    OUTPUT_NAME nvpssd_interface
  )
endif()
