# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

if(SAFETY_CORE_EFFECTIVE_TARGET_ARCH STREQUAL "aarch64")
  set(_safety_core_docker_platform "linux/arm64")
  set(_safety_core_docker_image_tag "psf-tegra:latest")
else()
  set(_safety_core_docker_platform "linux/amd64")
  set(_safety_core_docker_image_tag "psf-desktop:latest")
endif()

set(SAFETY_CORE_DOCKER_IMAGE_TAG "${_safety_core_docker_image_tag}" CACHE STRING
  "Docker image tag used by the safety_core_docker target")
set(SAFETY_CORE_DOCKER_PLATFORM "${_safety_core_docker_platform}" CACHE STRING
  "Docker target platform used by the safety_core_docker target")
set_property(CACHE SAFETY_CORE_DOCKER_PLATFORM PROPERTY STRINGS linux/amd64 linux/arm64)

add_custom_target(safety_core_docker
  COMMAND bash "${CMAKE_CURRENT_LIST_DIR}/create_docker_image.sh"
    --package-dir "${SAFETY_CORE_PACKAGE_OUTPUT_DIR}"
    --output-dir "${SAFETY_CORE_PACKAGE_OUTPUT_DIR}"
    --dockerfile "${CMAKE_CURRENT_LIST_DIR}/Dockerfile"
    --image-tag "${SAFETY_CORE_DOCKER_IMAGE_TAG}"
    --platform "${SAFETY_CORE_DOCKER_PLATFORM}"
    --version "${PROJECT_VERSION}"
    --arch "${SAFETY_CORE_EFFECTIVE_TARGET_ARCH}"
  DEPENDS safety_core_debian
  USES_TERMINAL
  COMMENT "Building local safety-core Docker image archive"
)
