# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

find_package(Threads REQUIRED)

set(_safety_core_cjs_dir "${CMAKE_CURRENT_LIST_DIR}")
set(_safety_core_cjs_sdm_dir "${_safety_core_cjs_dir}/sdm/ccplex")
set(_safety_core_cjs_include_dirs
  "${_safety_core_cjs_dir}/include"
  "${_safety_core_cjs_sdm_dir}"
  "${SAFETY_CORE_DECISION_MAKERS_DIR}/common"
  "${SAFETY_CORE_DECISION_MAKERS_DIR}/common/include"
  "${SAFETY_CORE_COMPONENTS_DIR}/event-integrator/daemon/include"
  "${SAFETY_CORE_COMPONENTS_DIR}/protocols/decision-maker-gateway/include"
  "${SAFETY_CORE_COMPONENTS_DIR}/safecomm/validation/include"
  "${SAFETY_CORE_COMPONENTS_DIR}/safecomm/transport/include"
)

if(SAFETY_CORE_BUILD_DECISION_MAKERS_CROSSING_JUNCTION_SDM)
  if(NOT TARGET safety_core_msg_validation)
    message(FATAL_ERROR "crossing_junction_safety_monitor_sdm requires the safety_core_msg_validation target from safecomm")
  endif()

  add_executable(crossing_junction_safety_monitor_sdm
    "${_safety_core_cjs_sdm_dir}/CrossingJunctionSafety.cpp"
    "${_safety_core_cjs_sdm_dir}/CrossingJunctionSafetyControl.cpp"
  )

  target_include_directories(crossing_junction_safety_monitor_sdm PRIVATE
    ${_safety_core_cjs_include_dirs}
  )

  target_compile_definitions(crossing_junction_safety_monitor_sdm PRIVATE
    $<$<CONFIG:Debug>:NVPSF_DBG>
  )

  target_link_libraries(crossing_junction_safety_monitor_sdm PRIVATE
    safety_core_msg_validation
    nvpsf_transport
    Threads::Threads
    rt
  )
endif()
