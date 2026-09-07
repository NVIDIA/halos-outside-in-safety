# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

if(SAFETY_CORE_BUILD_DECISION_MAKERS_ATL)
  add_subdirectory(atl)
endif()

if(SAFETY_CORE_BUILD_DECISION_MAKERS_PROXIMITY)
  add_subdirectory(proximity)
endif()

# Added after proximity: atl_proximity reads proximity_cmd_pkt.h and reuses the
# proximity command receiver source.
if(SAFETY_CORE_BUILD_DECISION_MAKERS_ATL_PROXIMITY)
  add_subdirectory(atl_proximity)
endif()
