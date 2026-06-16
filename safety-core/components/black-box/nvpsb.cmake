# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

if(NOT SAFETY_CORE_BUILD_BLACK_BOX_NVPSB)
  return()
endif()

set(_safety_core_black_box_dir "${CMAKE_CURRENT_LIST_DIR}")

add_library(nvpsb SHARED
  "${_safety_core_black_box_dir}/src/NvPSB.cpp"
)

target_include_directories(nvpsb PUBLIC
  "${_safety_core_black_box_dir}/include"
)

target_compile_definitions(nvpsb PRIVATE
  $<$<CONFIG:Debug>:NVPSF_DBG>
)

set_target_properties(nvpsb PROPERTIES
  OUTPUT_NAME nvpsb
)
