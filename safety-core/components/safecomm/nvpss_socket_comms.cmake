# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

if(NOT SAFETY_CORE_BUILD_SAFECOMM_POSIX_SOCKETS)
  return()
endif()

set(_safety_core_safecomm_dir "${CMAKE_CURRENT_LIST_DIR}")

add_library(nvpss_socket_comms SHARED
  "${_safety_core_safecomm_dir}/posix_sockets/src/posix_socket_comms.c"
)

target_include_directories(nvpss_socket_comms PUBLIC
  "${_safety_core_safecomm_dir}/posix_sockets/include"
)

target_compile_definitions(nvpss_socket_comms PRIVATE
  $<$<CONFIG:Debug>:NVPSF_DBG>
)

target_link_libraries(nvpss_socket_comms PUBLIC
  rt
)

set_target_properties(nvpss_socket_comms PROPERTIES
  OUTPUT_NAME nvpss_socket_comms
)
