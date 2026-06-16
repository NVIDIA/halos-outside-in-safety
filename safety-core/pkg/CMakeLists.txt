# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

set(SAFETY_CORE_PACKAGE_OUTPUT_DIR "${CMAKE_BINARY_DIR}/packages" CACHE PATH
  "Directory where safety-core package artifacts are written")

add_subdirectory(deb)
add_subdirectory(docker)

include("${CMAKE_CURRENT_LIST_DIR}/safety_core_package.cmake")
