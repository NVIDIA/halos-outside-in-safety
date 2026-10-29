# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

find_package(Threads REQUIRED)

set(_safety_core_perception_monitor_dir "${CMAKE_CURRENT_LIST_DIR}")

# nvpsf_msgbus and nvpsf_msgcodec are defined later, under adapters/, so they
# are checked by option here and resolved by name at generate time.
if(NOT (SAFETY_CORE_BUILD_ADAPTERS AND SAFETY_CORE_BUILD_ADAPTERS_VSS
        AND SAFETY_CORE_BUILD_ADAPTERS_VSS_MDX_MSG_BUS
        AND SAFETY_CORE_BUILD_ADAPTERS_VSS_MDX_MSG_CODEC))
  message(FATAL_ERROR
    "nv_perception_monitor requires the VSS MDX message bus and codec adapters. "
    "Enable them or set SAFETY_CORE_BUILD_PERCEPTION_MONITOR_NV_PERCEPTION_MONITOR=OFF.")
endif()

if(NOT TARGET nvpssd_interface)
  message(FATAL_ERROR "nv_perception_monitor requires target nvpssd_interface")
endif()

add_executable(nv_perception_monitor
  "${_safety_core_perception_monitor_dir}/src/perception_monitor.cpp"
  "${_safety_core_perception_monitor_dir}/src/container_health_checker.cpp"
  "${_safety_core_perception_monitor_dir}/src/data_validation_monitor.cpp"
  "${_safety_core_perception_monitor_dir}/src/decision_contract.cpp"
  "${_safety_core_perception_monitor_dir}/src/kafka_frame_source.cpp"
  "${_safety_core_perception_monitor_dir}/src/pcm_fault_aggregator.cpp"
  "${_safety_core_perception_monitor_dir}/src/pcm_event_reporter.cpp"
  "${_safety_core_perception_monitor_dir}/src/docker_client.cpp"
  "${_safety_core_perception_monitor_dir}/src/json_extract.cpp"
  "${_safety_core_perception_monitor_dir}/src/pcm_log.cpp"
)

target_include_directories(nv_perception_monitor PRIVATE
  "${_safety_core_perception_monitor_dir}/include"
  "${SAFETY_CORE_COMPONENTS_DIR}/event-integrator/daemon/include"
)

target_link_libraries(nv_perception_monitor PRIVATE
  nvpssd_interface
  nvpsf_msgbus
  nvpsf_msgcodec
  Threads::Threads
  rt
)
