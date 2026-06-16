# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

find_package(Threads REQUIRED)

include("${CMAKE_CURRENT_LIST_DIR}/safety_core_msg_validation.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/nvpss_msg_que.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/nvpss_socket_comms.cmake")
