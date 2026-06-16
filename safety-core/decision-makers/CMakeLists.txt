# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

if(SAFETY_CORE_BUILD_DECISION_MAKERS_ATL)
  add_subdirectory(atl)
endif()

if(SAFETY_CORE_BUILD_DECISION_MAKERS_PROXIMITY)
  add_subdirectory(proximity)
endif()
