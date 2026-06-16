# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

set(_safety_core_sensor_config_dir "${SAFETY_CORE_SENSOR_CONFIG_DIR}")
if(NOT EXISTS "${_safety_core_sensor_config_dir}/src/sensor_config_parser.cpp")
  message(FATAL_ERROR
    "sensor_config_parser.cpp not found. Set SAFETY_CORE_SENSOR_CONFIG_DIR to a directory "
    "with src/sensor_config_parser.cpp and inc/sensor_config_parser.h."
  )
endif()

if(NOT TARGET safety_core_sensor_config)
  add_library(safety_core_sensor_config OBJECT
    "${_safety_core_sensor_config_dir}/src/sensor_config_parser.cpp"
  )

  target_include_directories(safety_core_sensor_config PUBLIC
    "${_safety_core_sensor_config_dir}/inc"
  )
endif()
