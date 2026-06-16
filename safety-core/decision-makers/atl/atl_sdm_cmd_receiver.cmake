# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

find_package(Threads REQUIRED)

set(_safety_core_atl_dir "${CMAKE_CURRENT_LIST_DIR}")

if(SAFETY_CORE_BUILD_DECISION_MAKERS_ATL_CMD_RECEIVER)
  add_executable(atl_sdm_cmd_receiver
    "${_safety_core_atl_dir}/udp_cmd_receiver/cmd_rx.cpp"
  )

  target_include_directories(atl_sdm_cmd_receiver PRIVATE
    "${_safety_core_atl_dir}/include"
  )

  target_compile_definitions(atl_sdm_cmd_receiver PRIVATE
    $<$<CONFIG:Debug>:NVPSF_DBG>
  )

  target_compile_options(atl_sdm_cmd_receiver PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-error=redundant-decls>
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-error=missing-declarations>
  )

  target_link_libraries(atl_sdm_cmd_receiver PRIVATE
    Threads::Threads
  )
endif()
