# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

if(SAFETY_CORE_BUILD_ADAPTERS_VSS_MDX_MSG_BUS)
  add_subdirectory(mdx-msg-bus)
endif()

if(SAFETY_CORE_BUILD_ADAPTERS_VSS_MDX_MSG_CODEC)
  add_subdirectory(mdx-msg-codec)
endif()

if(SAFETY_CORE_BUILD_ADAPTERS_VSS_MDX_CLIENT)
  add_subdirectory(mdx-client)
endif()
