# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

set(_safety_core_package_dependencies)
foreach(_safety_core_target IN ITEMS
    safety_monitor
    nvpsb
    nvpssd_interface
    pss_daemon
    nvpsd
    nvpsd_gateway
    nvpss_msg_que
    nvpss_socket_comms
    nvpsf_msgbus
    nvpsf_msgcodec
    mdx_client
    atl_sdm
    atl_sdm_cmd_receiver
    proximity_sdm
    proximity_sdm_cmd_receiver)
  if(TARGET "${_safety_core_target}")
    list(APPEND _safety_core_package_dependencies "${_safety_core_target}")
  endif()
endforeach()

add_custom_target(safety_core_debian
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${SAFETY_CORE_PACKAGE_OUTPUT_DIR}"
  COMMAND bash "${CMAKE_CURRENT_LIST_DIR}/create_debian_package.sh"
    --source-dir "${SAFETY_CORE_SOURCE_DIR}"
    --build-dir "${CMAKE_BINARY_DIR}"
    --output-dir "${SAFETY_CORE_PACKAGE_OUTPUT_DIR}"
    --version "${PROJECT_VERSION}"
    --arch "${SAFETY_CORE_EFFECTIVE_TARGET_ARCH}"
  DEPENDS ${_safety_core_package_dependencies}
  USES_TERMINAL
  COMMENT "Building safety-core Debian packages"
)
