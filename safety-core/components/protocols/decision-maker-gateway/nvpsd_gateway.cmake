# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

find_package(Threads REQUIRED)

set(_safety_core_gateway_dir "${CMAKE_CURRENT_LIST_DIR}")
set(_safety_core_event_integrator_dir "${SAFETY_CORE_COMPONENTS_DIR}/event-integrator")
set(_safety_core_safecomm_dir "${SAFETY_CORE_COMPONENTS_DIR}/safecomm")

set(_safety_core_gateway_include_dirs
  "${_safety_core_gateway_dir}/include"
  "${_safety_core_event_integrator_dir}/daemon/include"
  "${_safety_core_safecomm_dir}/validation/include"
  "${_safety_core_safecomm_dir}/posix_msg_que/include"
)

if(SAFETY_CORE_BUILD_PROTOCOLS_DECISION_MAKER_GATEWAY_PROCESS)
  if(NOT TARGET nvpsd)
    message(FATAL_ERROR "nvpsd_gateway requires the nvpsd target")
  endif()

  add_executable(nvpsd_gateway
    "${_safety_core_gateway_dir}/src/NvPSDGateway.cpp"
  )

  target_include_directories(nvpsd_gateway PRIVATE
    ${_safety_core_gateway_include_dirs}
  )

  target_compile_definitions(nvpsd_gateway PRIVATE
    $<$<CONFIG:Debug>:NVPSF_DBG>
  )

  target_link_libraries(nvpsd_gateway PRIVATE
    nvpsd
    nvpssd_interface
    nvpsb
    safety_core_msg_validation
    Threads::Threads
    rt
  )
endif()
