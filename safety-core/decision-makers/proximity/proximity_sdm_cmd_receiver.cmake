# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

find_package(Threads REQUIRED)

set(_safety_core_proximity_dir "${CMAKE_CURRENT_LIST_DIR}")

if(SAFETY_CORE_BUILD_DECISION_MAKERS_PROXIMITY_CMD_RECEIVER)
  add_executable(proximity_sdm_cmd_receiver
    "${_safety_core_proximity_dir}/udp_cmd_receiver/cmd_rx.cpp"
  )

  target_include_directories(proximity_sdm_cmd_receiver PRIVATE
    "${_safety_core_proximity_dir}/include"
  )

  target_compile_definitions(proximity_sdm_cmd_receiver PRIVATE
    $<$<CONFIG:Debug>:NVPSF_DBG>
  )

  target_compile_options(proximity_sdm_cmd_receiver PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-error=redundant-decls>
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-error=missing-declarations>
  )

  target_link_libraries(proximity_sdm_cmd_receiver PRIVATE
    Threads::Threads
  )
endif()
