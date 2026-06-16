# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

function(safety_core_add_event_integrator_pss_daemon)
  if(NOT SAFETY_CORE_BUILD_EVENT_INTEGRATOR_PSS_DAEMON)
    return()
  endif()

  find_package(Threads REQUIRED)

  if(TARGET pss_daemon)
    return()
  endif()

  if(NOT TARGET nvpssd_interface)
    message(FATAL_ERROR "pss_daemon requires the nvpssd_interface target")
  endif()

  if(NOT TARGET nvpsd)
    message(FATAL_ERROR
      "pss_daemon requires nvpsd. Enable SAFETY_CORE_BUILD_PROTOCOLS_DECISION_MAKER_GATEWAY and SAFETY_CORE_BUILD_PROTOCOLS_DECISION_MAKER_GATEWAY_NVPSD."
    )
  endif()

  set(_safety_core_daemon_dir "${SAFETY_CORE_COMPONENTS_DIR}/event-integrator/daemon")
  set(_safety_core_safecomm_dir "${SAFETY_CORE_COMPONENTS_DIR}/safecomm")
  set(_safety_core_protocols_gateway_dir "${SAFETY_CORE_COMPONENTS_DIR}/protocols/decision-maker-gateway")

  set(_safety_core_sensor_config_dir "${SAFETY_CORE_SENSOR_CONFIG_DIR}")
  if(NOT EXISTS "${_safety_core_sensor_config_dir}/src/sensor_config_parser.cpp")
    message(FATAL_ERROR
      "sensor_config_parser.cpp not found. Set SAFETY_CORE_SENSOR_CONFIG_DIR to a directory "
      "with src/sensor_config_parser.cpp and inc/sensor_config_parser.h."
    )
  endif()

  add_executable(pss_daemon
    "${_safety_core_daemon_dir}/src/NvPSSDaemon.cpp"
    "${_safety_core_daemon_dir}/src/NvPSSDRPC.cpp"
    "${_safety_core_daemon_dir}/src/NvPSSConfigParser.cpp"
    "${_safety_core_daemon_dir}/src/NvPSSSafetyEventManager.cpp"
    "${_safety_core_daemon_dir}/src/NvPSSSafetyEventFusion.cpp"
    "${_safety_core_daemon_dir}/src/NvPSSSpatialCorrelation.cpp"
    "${_safety_core_daemon_dir}/src/NvPSSDToPSD.cpp"
    "${_safety_core_sensor_config_dir}/src/sensor_config_parser.cpp"
  )

  target_include_directories(pss_daemon PRIVATE
    "${_safety_core_daemon_dir}/include"
    "${_safety_core_safecomm_dir}/validation/include"
    "${_safety_core_safecomm_dir}/posix_msg_que/include"
    "${_safety_core_protocols_gateway_dir}/include"
    "${_safety_core_sensor_config_dir}/inc"
  )

  target_compile_definitions(pss_daemon PRIVATE
    $<$<CONFIG:Debug>:NVPSF_DBG>
  )

  target_link_libraries(pss_daemon PRIVATE
    nvpsd
    nvpssd_interface
    Threads::Threads
    rt
  )
endfunction()
