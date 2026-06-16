# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

if(NOT SAFETY_CORE_BUILD_AI_MONITOR_SAFETY_MONITOR)
  return()
endif()

include(CheckLanguage)
find_package(Threads REQUIRED)

set(_safety_core_cuda_target_dir "${SAFETY_CORE_CUDA_TARGET_DIR}")
if(NOT _safety_core_cuda_target_dir)
  if(SAFETY_CORE_EFFECTIVE_TARGET_ARCH STREQUAL "aarch64")
    set(_safety_core_cuda_target_dir "${SAFETY_CORE_CUDA_TOOLKIT_ROOT}/targets/${SAFETY_CORE_ARM64_CUDA_TARGET_NAME}")
  elseif(SAFETY_CORE_EFFECTIVE_TARGET_ARCH STREQUAL "x86_64")
    set(_safety_core_cuda_target_dir "${SAFETY_CORE_CUDA_TOOLKIT_ROOT}/targets/x86_64-linux")
  endif()
endif()

if(_safety_core_cuda_target_dir AND NOT SAFETY_CORE_CUDA_TARGET_DIR)
  set(SAFETY_CORE_CUDA_TARGET_DIR "${_safety_core_cuda_target_dir}" CACHE PATH "CUDA target directory" FORCE)
endif()

if(NOT CMAKE_CUDA_COMPILER)
  find_program(SAFETY_CORE_NVCC
    NAMES nvcc
    HINTS
      "${SAFETY_CORE_CUDA_TOOLKIT_ROOT}/bin"
      "/usr/local/cuda/bin"
      "/usr/local/cuda-13.0/bin"
  )
  if(SAFETY_CORE_NVCC)
    set(CMAKE_CUDA_COMPILER "${SAFETY_CORE_NVCC}" CACHE FILEPATH "CUDA compiler" FORCE)
  endif()
endif()

if(NOT CUDAToolkit_ROOT AND EXISTS "${SAFETY_CORE_CUDA_TOOLKIT_ROOT}")
  set(CUDAToolkit_ROOT "${SAFETY_CORE_CUDA_TOOLKIT_ROOT}" CACHE PATH "CUDA Toolkit root")
endif()

if(SAFETY_CORE_CUDA_TARGET_DIR)
  set(CUDAToolkit_TARGET_DIR "${SAFETY_CORE_CUDA_TARGET_DIR}" CACHE PATH "CUDA Toolkit target directory")
endif()

if(NOT CMAKE_CUDA_ARCHITECTURES)
  set(CMAKE_CUDA_ARCHITECTURES "${SAFETY_CORE_AI_MONITOR_CUDA_ARCHITECTURES}" CACHE STRING "CUDA architectures" FORCE)
endif()

check_language(CUDA)
if(NOT CMAKE_CUDA_COMPILER)
  message(FATAL_ERROR
    "ai-monitor safety_monitor requires a CUDA compiler. "
    "Install/configure CUDA or turn SAFETY_CORE_BUILD_AI_MONITOR_SAFETY_MONITOR off."
  )
endif()

enable_language(CUDA)
find_package(CUDAToolkit REQUIRED)

include("${CMAKE_CURRENT_LIST_DIR}/safety_core_sensor_config.cmake")

set(_safety_core_ai_monitor_dir "${CMAKE_CURRENT_LIST_DIR}")
set(_safety_core_event_integrator_dir "${SAFETY_CORE_COMPONENTS_DIR}/event-integrator")

if(NOT TARGET nvpssd_interface)
  message(FATAL_ERROR
    "safety_monitor requires nvpssd_interface. "
    "Enable SAFETY_CORE_BUILD_EVENT_INTEGRATOR and SAFETY_CORE_BUILD_EVENT_INTEGRATOR_INTERFACE."
  )
endif()

if(NOT TARGET SafetyCore::Stub::nvcuvid)
  include("${SAFETY_CORE_SOURCE_DIR}/cmake/SafetyCoreStub.cmake")
  safety_core_add_export_stub_library(
    TARGET safety_core_stub_nvcuvid
    ALIAS SafetyCore::Stub::nvcuvid
    EXPORT_FILE "${SAFETY_CORE_STUB_ROOT}/nvcuvid/libnvcuvid_dummy.export"
    OUTPUT_NAME nvcuvid
    SOVERSION 1
    INCLUDE_DIR "${SAFETY_CORE_NVCUVID_INCLUDE_DIR}"
  )
endif()

if(SAFETY_CORE_CUDA_TARGET_DIR MATCHES "/targets/([^/]+)$")
  set(_safety_core_cuda_target_name "${CMAKE_MATCH_1}")
else()
  set(_safety_core_cuda_target_name "")
endif()

add_executable(safety_monitor
  "${_safety_core_ai_monitor_dir}/src/safety_monitor.cpp"
  "${_safety_core_ai_monitor_dir}/src/sai_common.cpp"
  "${_safety_core_ai_monitor_dir}/src/sai_config_parser.cpp"
  "${_safety_core_ai_monitor_dir}/src/rtsp_client.cpp"
  "${_safety_core_ai_monitor_dir}/src/nvdec_decoder.cpp"
  "${_safety_core_ai_monitor_dir}/src/frame_quality_analyzer.cpp"
  "${_safety_core_ai_monitor_dir}/src/saim_kernels.cu"
  $<TARGET_OBJECTS:safety_core_sensor_config>
)

target_include_directories(safety_monitor PRIVATE
  "${_safety_core_ai_monitor_dir}/include"
  "${SAFETY_CORE_NVCUVID_INCLUDE_DIR}"
  "${_safety_core_event_integrator_dir}/daemon/include"
  "${_safety_core_sensor_config_dir}/inc"
  $<$<BOOL:${SAFETY_CORE_CUDA_TARGET_DIR}>:${SAFETY_CORE_CUDA_TARGET_DIR}/include>
)

target_compile_definitions(safety_monitor PRIVATE
  $<$<CONFIG:Debug>:DEBUG>
  $<$<BOOL:${SAFETY_CORE_ENABLE_PROFILE}>:PROFILE>
)

target_compile_options(safety_monitor PRIVATE
  $<$<AND:$<COMPILE_LANGUAGE:CUDA>,$<CONFIG:Debug>>:-G>
)

if(_safety_core_cuda_target_name AND NOT _safety_core_cuda_target_name STREQUAL "x86_64-linux")
  target_compile_options(safety_monitor PRIVATE
    $<$<COMPILE_LANGUAGE:CUDA>:--target-directory=${_safety_core_cuda_target_name}>
  )
endif()

if(SAFETY_CORE_CUDA_TARGET_DIR)
  target_link_directories(safety_monitor PRIVATE
    "${SAFETY_CORE_CUDA_TARGET_DIR}/lib"
    "${SAFETY_CORE_CUDA_TARGET_DIR}/lib/stubs"
  )
endif()

target_link_libraries(safety_monitor PRIVATE
  nvpssd_interface
  SafetyCore::Stub::nvcuvid
  Threads::Threads
  rt
)

if(TARGET CUDA::cudart)
  target_link_libraries(safety_monitor PRIVATE CUDA::cudart)
endif()

if(TARGET CUDA::cuda_driver)
  target_link_libraries(safety_monitor PRIVATE CUDA::cuda_driver)
endif()
