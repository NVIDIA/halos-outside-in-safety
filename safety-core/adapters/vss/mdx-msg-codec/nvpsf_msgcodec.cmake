# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

if(NOT SAFETY_CORE_BUILD_ADAPTERS_VSS_MDX_MSG_CODEC)
  return()
endif()

set(_safety_core_msg_codec_dir "${CMAKE_CURRENT_LIST_DIR}")

find_path(SAFETY_CORE_PROTOBUF_INCLUDE_DIR
  NAMES google/protobuf/message.h
  DOC "Directory containing protobuf headers from the system Protobuf development installation"
)

if(NOT SAFETY_CORE_PROTOBUF_INCLUDE_DIR)
  message(FATAL_ERROR
    "nvpsf_msgcodec requires Protobuf headers from the system development files. "
    "Install libprotobuf-dev or set SAFETY_CORE_PROTOBUF_INCLUDE_DIR."
  )
endif()

if(NOT TARGET SafetyCore::Stub::protobuf)
  include("${SAFETY_CORE_SOURCE_DIR}/cmake/SafetyCoreStub.cmake")
  safety_core_add_export_stub_library(
    TARGET safety_core_stub_protobuf
    ALIAS SafetyCore::Stub::protobuf
    EXPORT_FILE "${SAFETY_CORE_STUB_ROOT}/protobuf-3.21.12.0/libprotobuf_dummy.export"
    OUTPUT_NAME protobuf
    SOVERSION 32
    INCLUDE_DIR "${SAFETY_CORE_PROTOBUF_INCLUDE_DIR}"
  )
endif()

add_library(nvpsf_msgcodec SHARED
  "${_safety_core_msg_codec_dir}/src/protobuf_util.cpp"
  "${_safety_core_msg_codec_dir}/src/NvPSFMsgCodec.cpp"
  "${_safety_core_msg_codec_dir}/proto/gen/mdx-messages/schema.pb.cc"
  "${_safety_core_msg_codec_dir}/proto/gen/mdx-messages/ext.pb.cc"
  "${_safety_core_msg_codec_dir}/proto/gen/event-mapping/event_mapping.pb.cc"
)

target_include_directories(nvpsf_msgcodec PUBLIC
  "${_safety_core_msg_codec_dir}/include"
  "${_safety_core_msg_codec_dir}/internal"
  "${_safety_core_msg_codec_dir}/proto/gen/mdx-messages"
  "${_safety_core_msg_codec_dir}/proto/gen/event-mapping"
  "${SAFETY_CORE_PROTOBUF_INCLUDE_DIR}"
)

target_compile_definitions(nvpsf_msgcodec PRIVATE
  $<$<CONFIG:Debug>:NVPSF_DBG>
)

target_link_libraries(nvpsf_msgcodec PUBLIC
  SafetyCore::Stub::protobuf
)

set_target_properties(nvpsf_msgcodec PROPERTIES
  OUTPUT_NAME nvpsf_msgcodec
)
