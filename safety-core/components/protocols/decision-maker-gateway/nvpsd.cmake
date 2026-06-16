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

if(SAFETY_CORE_BUILD_PROTOCOLS_DECISION_MAKER_GATEWAY_NVPSD)
  if(NOT TARGET nvpsb)
    message(FATAL_ERROR
      "nvpsd requires nvpsb. Enable SAFETY_CORE_BUILD_BLACK_BOX and SAFETY_CORE_BUILD_BLACK_BOX_NVPSB."
    )
  endif()

  if(NOT TARGET nvpssd_interface)
    message(FATAL_ERROR
      "nvpsd requires nvpssd_interface. Enable SAFETY_CORE_BUILD_EVENT_INTEGRATOR_INTERFACE."
    )
  endif()

  if(NOT TARGET safety_core_msg_validation)
    message(FATAL_ERROR "nvpsd requires the safety_core_msg_validation target from safecomm")
  endif()

  if(NOT TARGET nvpss_msg_que)
    message(FATAL_ERROR
      "nvpsd requires nvpss_msg_que. Enable SAFETY_CORE_BUILD_SAFECOMM_POSIX_MSG_QUE."
    )
  endif()

  add_library(nvpsd SHARED
    "${_safety_core_gateway_dir}/src/NvPSD_interface.cpp"
    "${_safety_core_gateway_dir}/src/NvPSD.cpp"
  )

  target_include_directories(nvpsd PUBLIC
    ${_safety_core_gateway_include_dirs}
  )

  target_compile_definitions(nvpsd PRIVATE
    $<$<CONFIG:Debug>:NVPSF_DBG>
  )

  target_link_libraries(nvpsd PUBLIC
    nvpsb
    nvpssd_interface
    safety_core_msg_validation
    nvpss_msg_que
    Threads::Threads
    rt
  )

  set_target_properties(nvpsd PROPERTIES
    OUTPUT_NAME nvpsd
  )
endif()
