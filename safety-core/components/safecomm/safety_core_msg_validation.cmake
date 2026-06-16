# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

set(_safety_core_safecomm_dir "${CMAKE_CURRENT_LIST_DIR}")
set(_safety_core_event_integrator_include_dir "${SAFETY_CORE_COMPONENTS_DIR}/event-integrator/daemon/include")

add_library(safety_core_msg_validation OBJECT
  "${_safety_core_safecomm_dir}/validation/src/pss_message_validate.c"
)

target_include_directories(safety_core_msg_validation PUBLIC
  "${_safety_core_safecomm_dir}/validation/include"
  "${_safety_core_event_integrator_include_dir}"
)

target_compile_definitions(safety_core_msg_validation PRIVATE
  $<$<CONFIG:Debug>:NVPSF_DBG>
)
