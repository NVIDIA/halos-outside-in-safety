# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

find_package(Threads REQUIRED)

set(_safety_core_atl_proximity_dir "${CMAKE_CURRENT_LIST_DIR}")
set(_safety_core_atl_proximity_sdm_dir "${_safety_core_atl_proximity_dir}/sdm/ccplex")
set(_safety_core_atl_proximity_include_dirs
  "${_safety_core_atl_proximity_dir}/include"
  "${_safety_core_atl_proximity_sdm_dir}"
  # proximity_cmd_pkt.h only: the 64-byte CmdPacket layout, opcodes and CRC are
  # the PLC wire contract, shared read-only with the standalone proximity
  # decision-maker. Both SDMs drive the same PLC, so a private copy here could
  # drift from the format the PLC implements. No source file is shared.
  "${SAFETY_CORE_DECISION_MAKERS_DIR}/proximity/include"
  "${SAFETY_CORE_DECISION_MAKERS_DIR}/common"
  "${SAFETY_CORE_DECISION_MAKERS_DIR}/common/include"
  "${SAFETY_CORE_COMPONENTS_DIR}/event-integrator/daemon/include"
  "${SAFETY_CORE_COMPONENTS_DIR}/protocols/decision-maker-gateway/include"
  "${SAFETY_CORE_COMPONENTS_DIR}/safecomm/validation/include"
)

if(SAFETY_CORE_BUILD_DECISION_MAKERS_ATL_PROXIMITY_SDM)
  if(NOT TARGET safety_core_msg_validation)
    message(FATAL_ERROR "atl_proximity_sdm requires the safety_core_msg_validation target from safecomm")
  endif()

  add_executable(atl_proximity_sdm
    "${_safety_core_atl_proximity_sdm_dir}/AtlProximity.cpp"
    "${_safety_core_atl_proximity_sdm_dir}/AtlProximityControl.cpp"
  )

  target_include_directories(atl_proximity_sdm PRIVATE
    ${_safety_core_atl_proximity_include_dirs}
  )

  target_compile_definitions(atl_proximity_sdm PRIVATE
    $<$<CONFIG:Debug>:NVPSF_DBG>
  )

  target_link_libraries(atl_proximity_sdm PRIVATE
    safety_core_msg_validation
    Threads::Threads
    rt
  )
endif()
