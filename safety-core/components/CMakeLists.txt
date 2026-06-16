# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

if(SAFETY_CORE_BUILD_COMMON)
  add_subdirectory(common)
endif()

if(SAFETY_CORE_BUILD_BLACK_BOX)
  add_subdirectory(black-box)
endif()

if(SAFETY_CORE_BUILD_SAFECOMM)
  add_subdirectory(safecomm)
endif()

if(SAFETY_CORE_BUILD_EVENT_INTEGRATOR)
  add_subdirectory(event-integrator)
endif()

if(SAFETY_CORE_BUILD_PROTOCOLS)
  add_subdirectory(protocols)
endif()

if(SAFETY_CORE_BUILD_EVENT_INTEGRATOR AND COMMAND safety_core_add_event_integrator_pss_daemon)
  safety_core_add_event_integrator_pss_daemon()
endif()

if(SAFETY_CORE_BUILD_AI_MONITOR)
  add_subdirectory(ai-monitor)
endif()
