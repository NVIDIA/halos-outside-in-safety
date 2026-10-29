# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

find_package(Threads REQUIRED)

set(_safety_core_cjs_dir "${CMAKE_CURRENT_LIST_DIR}")

if(SAFETY_CORE_BUILD_DECISION_MAKERS_CROSSING_JUNCTION_CMD_RECEIVER)
  add_executable(crossing_junction_safety_monitor_sdm_cmd_receiver
    "${_safety_core_cjs_dir}/udp_cmd_receiver/cmd_rx.cpp"
  )

  target_include_directories(crossing_junction_safety_monitor_sdm_cmd_receiver PRIVATE
    "${_safety_core_cjs_dir}/include"
  )

  target_compile_definitions(crossing_junction_safety_monitor_sdm_cmd_receiver PRIVATE
    $<$<CONFIG:Debug>:NVPSF_DBG>
  )

  target_compile_options(crossing_junction_safety_monitor_sdm_cmd_receiver PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-error=redundant-decls>
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-error=missing-declarations>
  )

  target_link_libraries(crossing_junction_safety_monitor_sdm_cmd_receiver PRIVATE
    Threads::Threads
  )
endif()
