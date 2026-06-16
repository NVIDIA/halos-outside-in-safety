# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

find_package(Threads REQUIRED)

set(_safety_core_proximity_dir "${CMAKE_CURRENT_LIST_DIR}")
set(_safety_core_proximity_sdm_dir "${_safety_core_proximity_dir}/sdm/ccplex")
set(_safety_core_proximity_include_dirs
  "${_safety_core_proximity_dir}/include"
  "${_safety_core_proximity_sdm_dir}"
  "${SAFETY_CORE_COMPONENTS_DIR}/event-integrator/daemon/include"
  "${SAFETY_CORE_COMPONENTS_DIR}/protocols/decision-maker-gateway/include"
  "${SAFETY_CORE_COMPONENTS_DIR}/safecomm/validation/include"
)

if(SAFETY_CORE_BUILD_DECISION_MAKERS_PROXIMITY_SDM)
  if(NOT TARGET safety_core_msg_validation)
    message(FATAL_ERROR "proximity_sdm requires the safety_core_msg_validation target from safecomm")
  endif()

  add_executable(proximity_sdm
    "${_safety_core_proximity_sdm_dir}/Proximity.cpp"
    "${_safety_core_proximity_sdm_dir}/ProximityControl.cpp"
  )

  target_include_directories(proximity_sdm PRIVATE
    ${_safety_core_proximity_include_dirs}
  )

  target_compile_definitions(proximity_sdm PRIVATE
    $<$<CONFIG:Debug>:NVPSF_DBG>
  )

  target_link_libraries(proximity_sdm PRIVATE
    safety_core_msg_validation
    Threads::Threads
    rt
  )
endif()
