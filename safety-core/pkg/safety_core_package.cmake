# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

add_custom_target(safety_core_package
  DEPENDS safety_core_debian safety_core_docker
)
