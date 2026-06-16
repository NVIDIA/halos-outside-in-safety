# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

if(NOT SAFETY_CORE_BUILD_SAFECOMM_POSIX_MSG_QUE)
  return()
endif()

set(_safety_core_safecomm_dir "${CMAKE_CURRENT_LIST_DIR}")

add_library(nvpss_msg_que SHARED
  "${_safety_core_safecomm_dir}/posix_msg_que/src/posix_msg_que.c"
)

target_include_directories(nvpss_msg_que PUBLIC
  "${_safety_core_safecomm_dir}/posix_msg_que/include"
)

target_compile_definitions(nvpss_msg_que PRIVATE
  $<$<CONFIG:Debug>:NVPSF_DBG>
)

target_link_libraries(nvpss_msg_que PUBLIC
  rt
)

set_target_properties(nvpss_msg_que PROPERTIES
  OUTPUT_NAME nvpss_msg_que
)
