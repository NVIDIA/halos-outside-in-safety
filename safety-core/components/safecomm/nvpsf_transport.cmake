# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

if(NOT SAFETY_CORE_BUILD_SAFECOMM_TRANSPORT)
  return()
endif()

set(_safety_core_safecomm_dir "${CMAKE_CURRENT_LIST_DIR}")

add_library(nvpsf_transport SHARED
  "${_safety_core_safecomm_dir}/transport/src/nvpsf_transport.c"
)

target_include_directories(nvpsf_transport PUBLIC
  "${_safety_core_safecomm_dir}/transport/include"
)

target_compile_definitions(nvpsf_transport PRIVATE
  $<$<CONFIG:Debug>:NVPSF_DBG>
)

target_link_libraries(nvpsf_transport PUBLIC
  Threads::Threads
)

set_target_properties(nvpsf_transport PROPERTIES
  OUTPUT_NAME nvpsf_transport
)
