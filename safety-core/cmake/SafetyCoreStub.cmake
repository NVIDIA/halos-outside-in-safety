# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

function(safety_core_add_export_stub_library)
  set(options)
  set(oneValueArgs TARGET ALIAS EXPORT_FILE OUTPUT_NAME SOVERSION INCLUDE_DIR)
  set(multiValueArgs)
  cmake_parse_arguments(SAFETY_CORE_EXPORT_STUB "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(NOT SAFETY_CORE_EXPORT_STUB_TARGET)
    message(FATAL_ERROR "safety_core_add_export_stub_library requires TARGET")
  endif()

  if(NOT SAFETY_CORE_EXPORT_STUB_EXPORT_FILE)
    message(FATAL_ERROR "safety_core_add_export_stub_library requires EXPORT_FILE")
  endif()

  if(NOT SAFETY_CORE_EXPORT_STUB_OUTPUT_NAME)
    message(FATAL_ERROR "safety_core_add_export_stub_library requires OUTPUT_NAME")
  endif()

  if(NOT EXISTS "${SAFETY_CORE_EXPORT_STUB_EXPORT_FILE}")
    message(FATAL_ERROR "Stub export file not found: ${SAFETY_CORE_EXPORT_STUB_EXPORT_FILE}")
  endif()

  file(STRINGS "${SAFETY_CORE_EXPORT_STUB_EXPORT_FILE}" _stub_symbols REGEX "^[^# \t].*")
  set(_stub_source_body "/* Generated from ${SAFETY_CORE_EXPORT_STUB_EXPORT_FILE}. */\n")

  foreach(_stub_symbol IN LISTS _stub_symbols)
    string(STRIP "${_stub_symbol}" _stub_symbol)
    if(_stub_symbol STREQUAL "")
      continue()
    endif()

    string(APPEND _stub_source_body
      "__asm__(\".globl ${_stub_symbol}\\n\"\n"
      "        \".type ${_stub_symbol}, @function\\n\"\n"
      "        \"${_stub_symbol}:\\n\"\n"
      "        \"  ret\\n\");\n"
    )
  endforeach()

  set(_stub_source "${CMAKE_CURRENT_BINARY_DIR}/${SAFETY_CORE_EXPORT_STUB_TARGET}_exports.c")
  file(WRITE "${_stub_source}" "${_stub_source_body}")

  add_library("${SAFETY_CORE_EXPORT_STUB_TARGET}" SHARED
    "${_stub_source}"
  )

  set_target_properties("${SAFETY_CORE_EXPORT_STUB_TARGET}" PROPERTIES
    OUTPUT_NAME "${SAFETY_CORE_EXPORT_STUB_OUTPUT_NAME}"
  )

  if(SAFETY_CORE_EXPORT_STUB_SOVERSION)
    set_target_properties("${SAFETY_CORE_EXPORT_STUB_TARGET}" PROPERTIES
      SOVERSION "${SAFETY_CORE_EXPORT_STUB_SOVERSION}"
    )
  endif()

  if(SAFETY_CORE_EXPORT_STUB_INCLUDE_DIR)
    target_include_directories("${SAFETY_CORE_EXPORT_STUB_TARGET}" INTERFACE
      "${SAFETY_CORE_EXPORT_STUB_INCLUDE_DIR}"
    )
  endif()

  if(SAFETY_CORE_EXPORT_STUB_ALIAS)
    add_library("${SAFETY_CORE_EXPORT_STUB_ALIAS}" ALIAS "${SAFETY_CORE_EXPORT_STUB_TARGET}")
  endif()
endfunction()
