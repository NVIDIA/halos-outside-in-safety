# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

find_package(Threads REQUIRED)

set(_safety_core_atl_dir "${CMAKE_CURRENT_LIST_DIR}")
set(_safety_core_atl_include_dirs
  "${_safety_core_atl_dir}/include"
  "${_safety_core_atl_dir}/sdm"
  "${SAFETY_CORE_COMPONENTS_DIR}/event-integrator/daemon/include"
  "${SAFETY_CORE_COMPONENTS_DIR}/protocols/decision-maker-gateway/include"
  "${SAFETY_CORE_COMPONENTS_DIR}/safecomm/validation/include"
)

if(SAFETY_CORE_BUILD_DECISION_MAKERS_ATL_SDM)
  if(NOT TARGET safety_core_msg_validation)
    message(FATAL_ERROR "atl_sdm requires the safety_core_msg_validation target from safecomm")
  endif()

  add_executable(atl_sdm
    "${_safety_core_atl_dir}/sdm/ATL.cpp"
    "${_safety_core_atl_dir}/sdm/ATLControl.cpp"
  )

  target_include_directories(atl_sdm PRIVATE
    ${_safety_core_atl_include_dirs}
  )

  target_compile_definitions(atl_sdm PRIVATE
    $<$<CONFIG:Debug>:NVPSF_DBG>
  )

  target_link_libraries(atl_sdm PRIVATE
    safety_core_msg_validation
    Threads::Threads
    rt
  )
endif()
