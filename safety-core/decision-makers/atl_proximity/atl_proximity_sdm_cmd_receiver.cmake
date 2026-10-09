# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

find_package(Threads REQUIRED)

set(_safety_core_atl_proximity_dir "${CMAKE_CURRENT_LIST_DIR}")

# Separate receiver so an operator can watch the forklift-to-person command
# stream apart from ATL's. It decodes the same 64-byte packet and opcodes as the
# standalone proximity receiver, so that source is reused rather than copied.
if(SAFETY_CORE_BUILD_DECISION_MAKERS_ATL_PROXIMITY_CMD_RECEIVER)
  add_executable(atl_proximity_sdm_cmd_receiver
    "${SAFETY_CORE_DECISION_MAKERS_DIR}/proximity/udp_cmd_receiver/cmd_rx.cpp"
  )

  target_include_directories(atl_proximity_sdm_cmd_receiver PRIVATE
    "${SAFETY_CORE_DECISION_MAKERS_DIR}/proximity/include"
  )

  target_compile_definitions(atl_proximity_sdm_cmd_receiver PRIVATE
    $<$<CONFIG:Debug>:NVPSF_DBG>
  )

  target_compile_options(atl_proximity_sdm_cmd_receiver PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-error=redundant-decls>
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-error=missing-declarations>
  )

  target_link_libraries(atl_proximity_sdm_cmd_receiver PRIVATE
    Threads::Threads
  )
endif()
