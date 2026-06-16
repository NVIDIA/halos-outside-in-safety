/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "event_mapping.pb.h"

#include <algorithm>

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/extension_set.h>
#include <google/protobuf/wire_format_lite.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/generated_message_reflection.h>
#include <google/protobuf/reflection_ops.h>
#include <google/protobuf/wire_format.h>
// @@protoc_insertion_point(includes)
#include <google/protobuf/port_def.inc>

PROTOBUF_PRAGMA_INIT_SEG

namespace _pb = ::PROTOBUF_NAMESPACE_ID;
namespace _pbi = _pb::internal;

namespace mdx {
namespace client {
namespace config {
PROTOBUF_CONSTEXPR EventMappingRule::EventMappingRule(
    ::_pbi::ConstantInitialized): _impl_{
    /*decltype(_impl_.name_)*/{&::_pbi::fixed_address_empty_string, ::_pbi::ConstantInitialized{}}
  , /*decltype(_impl_.message_source_)*/{&::_pbi::fixed_address_empty_string, ::_pbi::ConstantInitialized{}}
  , /*decltype(_impl_.alert_type_)*/{&::_pbi::fixed_address_empty_string, ::_pbi::ConstantInitialized{}}
  , /*decltype(_impl_.event_type_)*/{&::_pbi::fixed_address_empty_string, ::_pbi::ConstantInitialized{}}
  , /*decltype(_impl_.object_type_)*/{&::_pbi::fixed_address_empty_string, ::_pbi::ConstantInitialized{}}
  , /*decltype(_impl_.rule_id_)*/{&::_pbi::fixed_address_empty_string, ::_pbi::ConstantInitialized{}}
  , /*decltype(_impl_.restricted_area_violation_)*/{&::_pbi::fixed_address_empty_string, ::_pbi::ConstantInitialized{}}
  , /*decltype(_impl_.confined_area_violation_)*/{&::_pbi::fixed_address_empty_string, ::_pbi::ConstantInitialized{}}
  , /*decltype(_impl_.social_distancing_violation_)*/{&::_pbi::fixed_address_empty_string, ::_pbi::ConstantInitialized{}}
  , /*decltype(_impl_.output_event_)*/{&::_pbi::fixed_address_empty_string, ::_pbi::ConstantInitialized{}}
  , /*decltype(_impl_.severity_)*/{&::_pbi::fixed_address_empty_string, ::_pbi::ConstantInitialized{}}
  , /*decltype(_impl_.object_type_secondary_)*/{&::_pbi::fixed_address_empty_string, ::_pbi::ConstantInitialized{}}
  , /*decltype(_impl_.object_type_primary_)*/{&::_pbi::fixed_address_empty_string, ::_pbi::ConstantInitialized{}}
  , /*decltype(_impl_.rule_id_prefix_match_)*/false
  , /*decltype(_impl_.scale_factor_)*/0
  , /*decltype(_impl_.distance_threshold_meters_)*/0
  , /*decltype(_impl_._cached_size_)*/{}} {}
struct EventMappingRuleDefaultTypeInternal {
  PROTOBUF_CONSTEXPR EventMappingRuleDefaultTypeInternal()
      : _instance(::_pbi::ConstantInitialized{}) {}
  ~EventMappingRuleDefaultTypeInternal() {}
  union {
    EventMappingRule _instance;
  };
};
PROTOBUF_ATTRIBUTE_NO_DESTROY PROTOBUF_CONSTINIT PROTOBUF_ATTRIBUTE_INIT_PRIORITY1 EventMappingRuleDefaultTypeInternal _EventMappingRule_default_instance_;
PROTOBUF_CONSTEXPR EventMappingConfig::EventMappingConfig(
    ::_pbi::ConstantInitialized): _impl_{
    /*decltype(_impl_.rules_)*/{}
  , /*decltype(_impl_._cached_size_)*/{}} {}
struct EventMappingConfigDefaultTypeInternal {
  PROTOBUF_CONSTEXPR EventMappingConfigDefaultTypeInternal()
      : _instance(::_pbi::ConstantInitialized{}) {}
  ~EventMappingConfigDefaultTypeInternal() {}
  union {
    EventMappingConfig _instance;
  };
};
PROTOBUF_ATTRIBUTE_NO_DESTROY PROTOBUF_CONSTINIT PROTOBUF_ATTRIBUTE_INIT_PRIORITY1 EventMappingConfigDefaultTypeInternal _EventMappingConfig_default_instance_;
}  // namespace config
}  // namespace client
}  // namespace mdx
static ::_pb::Metadata file_level_metadata_event_5fmapping_2eproto[2];
static constexpr ::_pb::EnumDescriptor const** file_level_enum_descriptors_event_5fmapping_2eproto = nullptr;
static constexpr ::_pb::ServiceDescriptor const** file_level_service_descriptors_event_5fmapping_2eproto = nullptr;

const uint32_t TableStruct_event_5fmapping_2eproto::offsets[] PROTOBUF_SECTION_VARIABLE(protodesc_cold) = {
  ~0u,  // no _has_bits_
  PROTOBUF_FIELD_OFFSET(::mdx::client::config::EventMappingRule, _internal_metadata_),
  ~0u,  // no _extensions_
  ~0u,  // no _oneof_case_
  ~0u,  // no _weak_field_map_
  ~0u,  // no _inlined_string_donated_
  PROTOBUF_FIELD_OFFSET(::mdx::client::config::EventMappingRule, _impl_.name_),
  PROTOBUF_FIELD_OFFSET(::mdx::client::config::EventMappingRule, _impl_.message_source_),
  PROTOBUF_FIELD_OFFSET(::mdx::client::config::EventMappingRule, _impl_.alert_type_),
  PROTOBUF_FIELD_OFFSET(::mdx::client::config::EventMappingRule, _impl_.event_type_),
  PROTOBUF_FIELD_OFFSET(::mdx::client::config::EventMappingRule, _impl_.object_type_),
  PROTOBUF_FIELD_OFFSET(::mdx::client::config::EventMappingRule, _impl_.rule_id_),
  PROTOBUF_FIELD_OFFSET(::mdx::client::config::EventMappingRule, _impl_.rule_id_prefix_match_),
  PROTOBUF_FIELD_OFFSET(::mdx::client::config::EventMappingRule, _impl_.restricted_area_violation_),
  PROTOBUF_FIELD_OFFSET(::mdx::client::config::EventMappingRule, _impl_.confined_area_violation_),
  PROTOBUF_FIELD_OFFSET(::mdx::client::config::EventMappingRule, _impl_.social_distancing_violation_),
  PROTOBUF_FIELD_OFFSET(::mdx::client::config::EventMappingRule, _impl_.output_event_),
  PROTOBUF_FIELD_OFFSET(::mdx::client::config::EventMappingRule, _impl_.severity_),
  PROTOBUF_FIELD_OFFSET(::mdx::client::config::EventMappingRule, _impl_.distance_threshold_meters_),
  PROTOBUF_FIELD_OFFSET(::mdx::client::config::EventMappingRule, _impl_.object_type_secondary_),
  PROTOBUF_FIELD_OFFSET(::mdx::client::config::EventMappingRule, _impl_.object_type_primary_),
  PROTOBUF_FIELD_OFFSET(::mdx::client::config::EventMappingRule, _impl_.scale_factor_),
  ~0u,  // no _has_bits_
  PROTOBUF_FIELD_OFFSET(::mdx::client::config::EventMappingConfig, _internal_metadata_),
  ~0u,  // no _extensions_
  ~0u,  // no _oneof_case_
  ~0u,  // no _weak_field_map_
  ~0u,  // no _inlined_string_donated_
  PROTOBUF_FIELD_OFFSET(::mdx::client::config::EventMappingConfig, _impl_.rules_),
};
static const ::_pbi::MigrationSchema schemas[] PROTOBUF_SECTION_VARIABLE(protodesc_cold) = {
  { 0, -1, -1, sizeof(::mdx::client::config::EventMappingRule)},
  { 22, -1, -1, sizeof(::mdx::client::config::EventMappingConfig)},
};

static const ::_pb::Message* const file_default_instances[] = {
  &::mdx::client::config::_EventMappingRule_default_instance_._instance,
  &::mdx::client::config::_EventMappingConfig_default_instance_._instance,
};

const char descriptor_table_protodef_event_5fmapping_2eproto[] PROTOBUF_SECTION_VARIABLE(protodesc_cold) =
  "\n\023event_mapping.proto\022\021mdx.client.config"
  "\"\252\003\n\020EventMappingRule\022\014\n\004name\030\001 \001(\t\022\026\n\016m"
  "essage_source\030\002 \001(\t\022\022\n\nalert_type\030\003 \001(\t\022"
  "\022\n\nevent_type\030\004 \001(\t\022\023\n\013object_type\030\005 \001(\t"
  "\022\017\n\007rule_id\030\006 \001(\t\022\034\n\024rule_id_prefix_matc"
  "h\030\007 \001(\010\022!\n\031restricted_area_violation\030\010 \001"
  "(\t\022\037\n\027confined_area_violation\030\t \001(\t\022#\n\033s"
  "ocial_distancing_violation\030\n \001(\t\022\024\n\014outp"
  "ut_event\030\013 \001(\t\022\020\n\010severity\030\014 \001(\t\022!\n\031dist"
  "ance_threshold_meters\030\r \001(\001\022\035\n\025object_ty"
  "pe_secondary\030\016 \001(\t\022\033\n\023object_type_primar"
  "y\030\017 \001(\t\022\024\n\014scale_factor\030\020 \001(\005\"H\n\022EventMa"
  "ppingConfig\0222\n\005rules\030\001 \003(\0132#.mdx.client."
  "config.EventMappingRuleB\003\200\001\000b\006proto3"
  ;
static ::_pbi::once_flag descriptor_table_event_5fmapping_2eproto_once;
const ::_pbi::DescriptorTable descriptor_table_event_5fmapping_2eproto = {
    false, false, 556, descriptor_table_protodef_event_5fmapping_2eproto,
    "event_mapping.proto",
    &descriptor_table_event_5fmapping_2eproto_once, nullptr, 0, 2,
    schemas, file_default_instances, TableStruct_event_5fmapping_2eproto::offsets,
    file_level_metadata_event_5fmapping_2eproto, file_level_enum_descriptors_event_5fmapping_2eproto,
    file_level_service_descriptors_event_5fmapping_2eproto,
};
PROTOBUF_ATTRIBUTE_WEAK const ::_pbi::DescriptorTable* descriptor_table_event_5fmapping_2eproto_getter() {
  return &descriptor_table_event_5fmapping_2eproto;
}

// Force running AddDescriptors() at dynamic initialization time.
PROTOBUF_ATTRIBUTE_INIT_PRIORITY2 static ::_pbi::AddDescriptorsRunner dynamic_init_dummy_event_5fmapping_2eproto(&descriptor_table_event_5fmapping_2eproto);
namespace mdx {
namespace client {
namespace config {

// ===================================================================

class EventMappingRule::_Internal {
 public:
};

EventMappingRule::EventMappingRule(::PROTOBUF_NAMESPACE_ID::Arena* arena,
                         bool is_message_owned)
  : ::PROTOBUF_NAMESPACE_ID::Message(arena, is_message_owned) {
  SharedCtor(arena, is_message_owned);
  // @@protoc_insertion_point(arena_constructor:mdx.client.config.EventMappingRule)
}
EventMappingRule::EventMappingRule(const EventMappingRule& from)
  : ::PROTOBUF_NAMESPACE_ID::Message() {
  EventMappingRule* const _this = this; (void)_this;
  new (&_impl_) Impl_{
      decltype(_impl_.name_){}
    , decltype(_impl_.message_source_){}
    , decltype(_impl_.alert_type_){}
    , decltype(_impl_.event_type_){}
    , decltype(_impl_.object_type_){}
    , decltype(_impl_.rule_id_){}
    , decltype(_impl_.restricted_area_violation_){}
    , decltype(_impl_.confined_area_violation_){}
    , decltype(_impl_.social_distancing_violation_){}
    , decltype(_impl_.output_event_){}
    , decltype(_impl_.severity_){}
    , decltype(_impl_.object_type_secondary_){}
    , decltype(_impl_.object_type_primary_){}
    , decltype(_impl_.rule_id_prefix_match_){}
    , decltype(_impl_.scale_factor_){}
    , decltype(_impl_.distance_threshold_meters_){}
    , /*decltype(_impl_._cached_size_)*/{}};

  _internal_metadata_.MergeFrom<::PROTOBUF_NAMESPACE_ID::UnknownFieldSet>(from._internal_metadata_);
  _impl_.name_.InitDefault();
  #ifdef PROTOBUF_FORCE_COPY_DEFAULT_STRING
    _impl_.name_.Set("", GetArenaForAllocation());
  #endif // PROTOBUF_FORCE_COPY_DEFAULT_STRING
  if (!from._internal_name().empty()) {
    _this->_impl_.name_.Set(from._internal_name(),
      _this->GetArenaForAllocation());
  }
  _impl_.message_source_.InitDefault();
  #ifdef PROTOBUF_FORCE_COPY_DEFAULT_STRING
    _impl_.message_source_.Set("", GetArenaForAllocation());
  #endif // PROTOBUF_FORCE_COPY_DEFAULT_STRING
  if (!from._internal_message_source().empty()) {
    _this->_impl_.message_source_.Set(from._internal_message_source(),
      _this->GetArenaForAllocation());
  }
  _impl_.alert_type_.InitDefault();
  #ifdef PROTOBUF_FORCE_COPY_DEFAULT_STRING
    _impl_.alert_type_.Set("", GetArenaForAllocation());
  #endif // PROTOBUF_FORCE_COPY_DEFAULT_STRING
  if (!from._internal_alert_type().empty()) {
    _this->_impl_.alert_type_.Set(from._internal_alert_type(),
      _this->GetArenaForAllocation());
  }
  _impl_.event_type_.InitDefault();
  #ifdef PROTOBUF_FORCE_COPY_DEFAULT_STRING
    _impl_.event_type_.Set("", GetArenaForAllocation());
  #endif // PROTOBUF_FORCE_COPY_DEFAULT_STRING
  if (!from._internal_event_type().empty()) {
    _this->_impl_.event_type_.Set(from._internal_event_type(),
      _this->GetArenaForAllocation());
  }
  _impl_.object_type_.InitDefault();
  #ifdef PROTOBUF_FORCE_COPY_DEFAULT_STRING
    _impl_.object_type_.Set("", GetArenaForAllocation());
  #endif // PROTOBUF_FORCE_COPY_DEFAULT_STRING
  if (!from._internal_object_type().empty()) {
    _this->_impl_.object_type_.Set(from._internal_object_type(),
      _this->GetArenaForAllocation());
  }
  _impl_.rule_id_.InitDefault();
  #ifdef PROTOBUF_FORCE_COPY_DEFAULT_STRING
    _impl_.rule_id_.Set("", GetArenaForAllocation());
  #endif // PROTOBUF_FORCE_COPY_DEFAULT_STRING
  if (!from._internal_rule_id().empty()) {
    _this->_impl_.rule_id_.Set(from._internal_rule_id(),
      _this->GetArenaForAllocation());
  }
  _impl_.restricted_area_violation_.InitDefault();
  #ifdef PROTOBUF_FORCE_COPY_DEFAULT_STRING
    _impl_.restricted_area_violation_.Set("", GetArenaForAllocation());
  #endif // PROTOBUF_FORCE_COPY_DEFAULT_STRING
  if (!from._internal_restricted_area_violation().empty()) {
    _this->_impl_.restricted_area_violation_.Set(from._internal_restricted_area_violation(),
      _this->GetArenaForAllocation());
  }
  _impl_.confined_area_violation_.InitDefault();
  #ifdef PROTOBUF_FORCE_COPY_DEFAULT_STRING
    _impl_.confined_area_violation_.Set("", GetArenaForAllocation());
  #endif // PROTOBUF_FORCE_COPY_DEFAULT_STRING
  if (!from._internal_confined_area_violation().empty()) {
    _this->_impl_.confined_area_violation_.Set(from._internal_confined_area_violation(),
      _this->GetArenaForAllocation());
  }
  _impl_.social_distancing_violation_.InitDefault();
  #ifdef PROTOBUF_FORCE_COPY_DEFAULT_STRING
    _impl_.social_distancing_violation_.Set("", GetArenaForAllocation());
  #endif // PROTOBUF_FORCE_COPY_DEFAULT_STRING
  if (!from._internal_social_distancing_violation().empty()) {
    _this->_impl_.social_distancing_violation_.Set(from._internal_social_distancing_violation(),
      _this->GetArenaForAllocation());
  }
  _impl_.output_event_.InitDefault();
  #ifdef PROTOBUF_FORCE_COPY_DEFAULT_STRING
    _impl_.output_event_.Set("", GetArenaForAllocation());
  #endif // PROTOBUF_FORCE_COPY_DEFAULT_STRING
  if (!from._internal_output_event().empty()) {
    _this->_impl_.output_event_.Set(from._internal_output_event(),
      _this->GetArenaForAllocation());
  }
  _impl_.severity_.InitDefault();
  #ifdef PROTOBUF_FORCE_COPY_DEFAULT_STRING
    _impl_.severity_.Set("", GetArenaForAllocation());
  #endif // PROTOBUF_FORCE_COPY_DEFAULT_STRING
  if (!from._internal_severity().empty()) {
    _this->_impl_.severity_.Set(from._internal_severity(),
      _this->GetArenaForAllocation());
  }
  _impl_.object_type_secondary_.InitDefault();
  #ifdef PROTOBUF_FORCE_COPY_DEFAULT_STRING
    _impl_.object_type_secondary_.Set("", GetArenaForAllocation());
  #endif // PROTOBUF_FORCE_COPY_DEFAULT_STRING
  if (!from._internal_object_type_secondary().empty()) {
    _this->_impl_.object_type_secondary_.Set(from._internal_object_type_secondary(),
      _this->GetArenaForAllocation());
  }
  _impl_.object_type_primary_.InitDefault();
  #ifdef PROTOBUF_FORCE_COPY_DEFAULT_STRING
    _impl_.object_type_primary_.Set("", GetArenaForAllocation());
  #endif // PROTOBUF_FORCE_COPY_DEFAULT_STRING
  if (!from._internal_object_type_primary().empty()) {
    _this->_impl_.object_type_primary_.Set(from._internal_object_type_primary(),
      _this->GetArenaForAllocation());
  }
  ::memcpy(&_impl_.rule_id_prefix_match_, &from._impl_.rule_id_prefix_match_,
    static_cast<size_t>(reinterpret_cast<char*>(&_impl_.distance_threshold_meters_) -
    reinterpret_cast<char*>(&_impl_.rule_id_prefix_match_)) + sizeof(_impl_.distance_threshold_meters_));
  // @@protoc_insertion_point(copy_constructor:mdx.client.config.EventMappingRule)
}

inline void EventMappingRule::SharedCtor(
    ::_pb::Arena* arena, bool is_message_owned) {
  (void)arena;
  (void)is_message_owned;
  new (&_impl_) Impl_{
      decltype(_impl_.name_){}
    , decltype(_impl_.message_source_){}
    , decltype(_impl_.alert_type_){}
    , decltype(_impl_.event_type_){}
    , decltype(_impl_.object_type_){}
    , decltype(_impl_.rule_id_){}
    , decltype(_impl_.restricted_area_violation_){}
    , decltype(_impl_.confined_area_violation_){}
    , decltype(_impl_.social_distancing_violation_){}
    , decltype(_impl_.output_event_){}
    , decltype(_impl_.severity_){}
    , decltype(_impl_.object_type_secondary_){}
    , decltype(_impl_.object_type_primary_){}
    , decltype(_impl_.rule_id_prefix_match_){false}
    , decltype(_impl_.scale_factor_){0}
    , decltype(_impl_.distance_threshold_meters_){0}
    , /*decltype(_impl_._cached_size_)*/{}
  };
  _impl_.name_.InitDefault();
  #ifdef PROTOBUF_FORCE_COPY_DEFAULT_STRING
    _impl_.name_.Set("", GetArenaForAllocation());
  #endif // PROTOBUF_FORCE_COPY_DEFAULT_STRING
  _impl_.message_source_.InitDefault();
  #ifdef PROTOBUF_FORCE_COPY_DEFAULT_STRING
    _impl_.message_source_.Set("", GetArenaForAllocation());
  #endif // PROTOBUF_FORCE_COPY_DEFAULT_STRING
  _impl_.alert_type_.InitDefault();
  #ifdef PROTOBUF_FORCE_COPY_DEFAULT_STRING
    _impl_.alert_type_.Set("", GetArenaForAllocation());
  #endif // PROTOBUF_FORCE_COPY_DEFAULT_STRING
  _impl_.event_type_.InitDefault();
  #ifdef PROTOBUF_FORCE_COPY_DEFAULT_STRING
    _impl_.event_type_.Set("", GetArenaForAllocation());
  #endif // PROTOBUF_FORCE_COPY_DEFAULT_STRING
  _impl_.object_type_.InitDefault();
  #ifdef PROTOBUF_FORCE_COPY_DEFAULT_STRING
    _impl_.object_type_.Set("", GetArenaForAllocation());
  #endif // PROTOBUF_FORCE_COPY_DEFAULT_STRING
  _impl_.rule_id_.InitDefault();
  #ifdef PROTOBUF_FORCE_COPY_DEFAULT_STRING
    _impl_.rule_id_.Set("", GetArenaForAllocation());
  #endif // PROTOBUF_FORCE_COPY_DEFAULT_STRING
  _impl_.restricted_area_violation_.InitDefault();
  #ifdef PROTOBUF_FORCE_COPY_DEFAULT_STRING
    _impl_.restricted_area_violation_.Set("", GetArenaForAllocation());
  #endif // PROTOBUF_FORCE_COPY_DEFAULT_STRING
  _impl_.confined_area_violation_.InitDefault();
  #ifdef PROTOBUF_FORCE_COPY_DEFAULT_STRING
    _impl_.confined_area_violation_.Set("", GetArenaForAllocation());
  #endif // PROTOBUF_FORCE_COPY_DEFAULT_STRING
  _impl_.social_distancing_violation_.InitDefault();
  #ifdef PROTOBUF_FORCE_COPY_DEFAULT_STRING
    _impl_.social_distancing_violation_.Set("", GetArenaForAllocation());
  #endif // PROTOBUF_FORCE_COPY_DEFAULT_STRING
  _impl_.output_event_.InitDefault();
  #ifdef PROTOBUF_FORCE_COPY_DEFAULT_STRING
    _impl_.output_event_.Set("", GetArenaForAllocation());
  #endif // PROTOBUF_FORCE_COPY_DEFAULT_STRING
  _impl_.severity_.InitDefault();
  #ifdef PROTOBUF_FORCE_COPY_DEFAULT_STRING
    _impl_.severity_.Set("", GetArenaForAllocation());
  #endif // PROTOBUF_FORCE_COPY_DEFAULT_STRING
  _impl_.object_type_secondary_.InitDefault();
  #ifdef PROTOBUF_FORCE_COPY_DEFAULT_STRING
    _impl_.object_type_secondary_.Set("", GetArenaForAllocation());
  #endif // PROTOBUF_FORCE_COPY_DEFAULT_STRING
  _impl_.object_type_primary_.InitDefault();
  #ifdef PROTOBUF_FORCE_COPY_DEFAULT_STRING
    _impl_.object_type_primary_.Set("", GetArenaForAllocation());
  #endif // PROTOBUF_FORCE_COPY_DEFAULT_STRING
}

EventMappingRule::~EventMappingRule() {
  // @@protoc_insertion_point(destructor:mdx.client.config.EventMappingRule)
  if (auto *arena = _internal_metadata_.DeleteReturnArena<::PROTOBUF_NAMESPACE_ID::UnknownFieldSet>()) {
  (void)arena;
    return;
  }
  SharedDtor();
}

inline void EventMappingRule::SharedDtor() {
  GOOGLE_DCHECK(GetArenaForAllocation() == nullptr);
  _impl_.name_.Destroy();
  _impl_.message_source_.Destroy();
  _impl_.alert_type_.Destroy();
  _impl_.event_type_.Destroy();
  _impl_.object_type_.Destroy();
  _impl_.rule_id_.Destroy();
  _impl_.restricted_area_violation_.Destroy();
  _impl_.confined_area_violation_.Destroy();
  _impl_.social_distancing_violation_.Destroy();
  _impl_.output_event_.Destroy();
  _impl_.severity_.Destroy();
  _impl_.object_type_secondary_.Destroy();
  _impl_.object_type_primary_.Destroy();
}

void EventMappingRule::SetCachedSize(int size) const {
  _impl_._cached_size_.Set(size);
}

void EventMappingRule::Clear() {
// @@protoc_insertion_point(message_clear_start:mdx.client.config.EventMappingRule)
  uint32_t cached_has_bits = 0;
  // Prevent compiler warnings about cached_has_bits being unused
  (void) cached_has_bits;

  _impl_.name_.ClearToEmpty();
  _impl_.message_source_.ClearToEmpty();
  _impl_.alert_type_.ClearToEmpty();
  _impl_.event_type_.ClearToEmpty();
  _impl_.object_type_.ClearToEmpty();
  _impl_.rule_id_.ClearToEmpty();
  _impl_.restricted_area_violation_.ClearToEmpty();
  _impl_.confined_area_violation_.ClearToEmpty();
  _impl_.social_distancing_violation_.ClearToEmpty();
  _impl_.output_event_.ClearToEmpty();
  _impl_.severity_.ClearToEmpty();
  _impl_.object_type_secondary_.ClearToEmpty();
  _impl_.object_type_primary_.ClearToEmpty();
  ::memset(&_impl_.rule_id_prefix_match_, 0, static_cast<size_t>(
      reinterpret_cast<char*>(&_impl_.distance_threshold_meters_) -
      reinterpret_cast<char*>(&_impl_.rule_id_prefix_match_)) + sizeof(_impl_.distance_threshold_meters_));
  _internal_metadata_.Clear<::PROTOBUF_NAMESPACE_ID::UnknownFieldSet>();
}

const char* EventMappingRule::_InternalParse(const char* ptr, ::_pbi::ParseContext* ctx) {
#define CHK_(x) if (PROTOBUF_PREDICT_FALSE(!(x))) goto failure
  while (!ctx->Done(&ptr)) {
    uint32_t tag;
    ptr = ::_pbi::ReadTag(ptr, &tag);
    switch (tag >> 3) {
      // string name = 1;
      case 1:
        if (PROTOBUF_PREDICT_TRUE(static_cast<uint8_t>(tag) == 10)) {
          auto str = _internal_mutable_name();
          ptr = ::_pbi::InlineGreedyStringParser(str, ptr, ctx);
          CHK_(ptr);
          CHK_(::_pbi::VerifyUTF8(str, "mdx.client.config.EventMappingRule.name"));
        } else
          goto handle_unusual;
        continue;
      // string message_source = 2;
      case 2:
        if (PROTOBUF_PREDICT_TRUE(static_cast<uint8_t>(tag) == 18)) {
          auto str = _internal_mutable_message_source();
          ptr = ::_pbi::InlineGreedyStringParser(str, ptr, ctx);
          CHK_(ptr);
          CHK_(::_pbi::VerifyUTF8(str, "mdx.client.config.EventMappingRule.message_source"));
        } else
          goto handle_unusual;
        continue;
      // string alert_type = 3;
      case 3:
        if (PROTOBUF_PREDICT_TRUE(static_cast<uint8_t>(tag) == 26)) {
          auto str = _internal_mutable_alert_type();
          ptr = ::_pbi::InlineGreedyStringParser(str, ptr, ctx);
          CHK_(ptr);
          CHK_(::_pbi::VerifyUTF8(str, "mdx.client.config.EventMappingRule.alert_type"));
        } else
          goto handle_unusual;
        continue;
      // string event_type = 4;
      case 4:
        if (PROTOBUF_PREDICT_TRUE(static_cast<uint8_t>(tag) == 34)) {
          auto str = _internal_mutable_event_type();
          ptr = ::_pbi::InlineGreedyStringParser(str, ptr, ctx);
          CHK_(ptr);
          CHK_(::_pbi::VerifyUTF8(str, "mdx.client.config.EventMappingRule.event_type"));
        } else
          goto handle_unusual;
        continue;
      // string object_type = 5;
      case 5:
        if (PROTOBUF_PREDICT_TRUE(static_cast<uint8_t>(tag) == 42)) {
          auto str = _internal_mutable_object_type();
          ptr = ::_pbi::InlineGreedyStringParser(str, ptr, ctx);
          CHK_(ptr);
          CHK_(::_pbi::VerifyUTF8(str, "mdx.client.config.EventMappingRule.object_type"));
        } else
          goto handle_unusual;
        continue;
      // string rule_id = 6;
      case 6:
        if (PROTOBUF_PREDICT_TRUE(static_cast<uint8_t>(tag) == 50)) {
          auto str = _internal_mutable_rule_id();
          ptr = ::_pbi::InlineGreedyStringParser(str, ptr, ctx);
          CHK_(ptr);
          CHK_(::_pbi::VerifyUTF8(str, "mdx.client.config.EventMappingRule.rule_id"));
        } else
          goto handle_unusual;
        continue;
      // bool rule_id_prefix_match = 7;
      case 7:
        if (PROTOBUF_PREDICT_TRUE(static_cast<uint8_t>(tag) == 56)) {
          _impl_.rule_id_prefix_match_ = ::PROTOBUF_NAMESPACE_ID::internal::ReadVarint64(&ptr);
          CHK_(ptr);
        } else
          goto handle_unusual;
        continue;
      // string restricted_area_violation = 8;
      case 8:
        if (PROTOBUF_PREDICT_TRUE(static_cast<uint8_t>(tag) == 66)) {
          auto str = _internal_mutable_restricted_area_violation();
          ptr = ::_pbi::InlineGreedyStringParser(str, ptr, ctx);
          CHK_(ptr);
          CHK_(::_pbi::VerifyUTF8(str, "mdx.client.config.EventMappingRule.restricted_area_violation"));
        } else
          goto handle_unusual;
        continue;
      // string confined_area_violation = 9;
      case 9:
        if (PROTOBUF_PREDICT_TRUE(static_cast<uint8_t>(tag) == 74)) {
          auto str = _internal_mutable_confined_area_violation();
          ptr = ::_pbi::InlineGreedyStringParser(str, ptr, ctx);
          CHK_(ptr);
          CHK_(::_pbi::VerifyUTF8(str, "mdx.client.config.EventMappingRule.confined_area_violation"));
        } else
          goto handle_unusual;
        continue;
      // string social_distancing_violation = 10;
      case 10:
        if (PROTOBUF_PREDICT_TRUE(static_cast<uint8_t>(tag) == 82)) {
          auto str = _internal_mutable_social_distancing_violation();
          ptr = ::_pbi::InlineGreedyStringParser(str, ptr, ctx);
          CHK_(ptr);
          CHK_(::_pbi::VerifyUTF8(str, "mdx.client.config.EventMappingRule.social_distancing_violation"));
        } else
          goto handle_unusual;
        continue;
      // string output_event = 11;
      case 11:
        if (PROTOBUF_PREDICT_TRUE(static_cast<uint8_t>(tag) == 90)) {
          auto str = _internal_mutable_output_event();
          ptr = ::_pbi::InlineGreedyStringParser(str, ptr, ctx);
          CHK_(ptr);
          CHK_(::_pbi::VerifyUTF8(str, "mdx.client.config.EventMappingRule.output_event"));
        } else
          goto handle_unusual;
        continue;
      // string severity = 12;
      case 12:
        if (PROTOBUF_PREDICT_TRUE(static_cast<uint8_t>(tag) == 98)) {
          auto str = _internal_mutable_severity();
          ptr = ::_pbi::InlineGreedyStringParser(str, ptr, ctx);
          CHK_(ptr);
          CHK_(::_pbi::VerifyUTF8(str, "mdx.client.config.EventMappingRule.severity"));
        } else
          goto handle_unusual;
        continue;
      // double distance_threshold_meters = 13;
      case 13:
        if (PROTOBUF_PREDICT_TRUE(static_cast<uint8_t>(tag) == 105)) {
          _impl_.distance_threshold_meters_ = ::PROTOBUF_NAMESPACE_ID::internal::UnalignedLoad<double>(ptr);
          ptr += sizeof(double);
        } else
          goto handle_unusual;
        continue;
      // string object_type_secondary = 14;
      case 14:
        if (PROTOBUF_PREDICT_TRUE(static_cast<uint8_t>(tag) == 114)) {
          auto str = _internal_mutable_object_type_secondary();
          ptr = ::_pbi::InlineGreedyStringParser(str, ptr, ctx);
          CHK_(ptr);
          CHK_(::_pbi::VerifyUTF8(str, "mdx.client.config.EventMappingRule.object_type_secondary"));
        } else
          goto handle_unusual;
        continue;
      // string object_type_primary = 15;
      case 15:
        if (PROTOBUF_PREDICT_TRUE(static_cast<uint8_t>(tag) == 122)) {
          auto str = _internal_mutable_object_type_primary();
          ptr = ::_pbi::InlineGreedyStringParser(str, ptr, ctx);
          CHK_(ptr);
          CHK_(::_pbi::VerifyUTF8(str, "mdx.client.config.EventMappingRule.object_type_primary"));
        } else
          goto handle_unusual;
        continue;
      // int32 scale_factor = 16;
      case 16:
        if (PROTOBUF_PREDICT_TRUE(static_cast<uint8_t>(tag) == 128)) {
          _impl_.scale_factor_ = ::PROTOBUF_NAMESPACE_ID::internal::ReadVarint32(&ptr);
          CHK_(ptr);
        } else
          goto handle_unusual;
        continue;
      default:
        goto handle_unusual;
    }  // switch
  handle_unusual:
    if ((tag == 0) || ((tag & 7) == 4)) {
      CHK_(ptr);
      ctx->SetLastTag(tag);
      goto message_done;
    }
    ptr = UnknownFieldParse(
        tag,
        _internal_metadata_.mutable_unknown_fields<::PROTOBUF_NAMESPACE_ID::UnknownFieldSet>(),
        ptr, ctx);
    CHK_(ptr != nullptr);
  }  // while
message_done:
  return ptr;
failure:
  ptr = nullptr;
  goto message_done;
#undef CHK_
}

uint8_t* EventMappingRule::_InternalSerialize(
    uint8_t* target, ::PROTOBUF_NAMESPACE_ID::io::EpsCopyOutputStream* stream) const {
  // @@protoc_insertion_point(serialize_to_array_start:mdx.client.config.EventMappingRule)
  uint32_t cached_has_bits = 0;
  (void) cached_has_bits;

  // string name = 1;
  if (!this->_internal_name().empty()) {
    ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::VerifyUtf8String(
      this->_internal_name().data(), static_cast<int>(this->_internal_name().length()),
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::SERIALIZE,
      "mdx.client.config.EventMappingRule.name");
    target = stream->WriteStringMaybeAliased(
        1, this->_internal_name(), target);
  }

  // string message_source = 2;
  if (!this->_internal_message_source().empty()) {
    ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::VerifyUtf8String(
      this->_internal_message_source().data(), static_cast<int>(this->_internal_message_source().length()),
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::SERIALIZE,
      "mdx.client.config.EventMappingRule.message_source");
    target = stream->WriteStringMaybeAliased(
        2, this->_internal_message_source(), target);
  }

  // string alert_type = 3;
  if (!this->_internal_alert_type().empty()) {
    ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::VerifyUtf8String(
      this->_internal_alert_type().data(), static_cast<int>(this->_internal_alert_type().length()),
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::SERIALIZE,
      "mdx.client.config.EventMappingRule.alert_type");
    target = stream->WriteStringMaybeAliased(
        3, this->_internal_alert_type(), target);
  }

  // string event_type = 4;
  if (!this->_internal_event_type().empty()) {
    ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::VerifyUtf8String(
      this->_internal_event_type().data(), static_cast<int>(this->_internal_event_type().length()),
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::SERIALIZE,
      "mdx.client.config.EventMappingRule.event_type");
    target = stream->WriteStringMaybeAliased(
        4, this->_internal_event_type(), target);
  }

  // string object_type = 5;
  if (!this->_internal_object_type().empty()) {
    ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::VerifyUtf8String(
      this->_internal_object_type().data(), static_cast<int>(this->_internal_object_type().length()),
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::SERIALIZE,
      "mdx.client.config.EventMappingRule.object_type");
    target = stream->WriteStringMaybeAliased(
        5, this->_internal_object_type(), target);
  }

  // string rule_id = 6;
  if (!this->_internal_rule_id().empty()) {
    ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::VerifyUtf8String(
      this->_internal_rule_id().data(), static_cast<int>(this->_internal_rule_id().length()),
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::SERIALIZE,
      "mdx.client.config.EventMappingRule.rule_id");
    target = stream->WriteStringMaybeAliased(
        6, this->_internal_rule_id(), target);
  }

  // bool rule_id_prefix_match = 7;
  if (this->_internal_rule_id_prefix_match() != 0) {
    target = stream->EnsureSpace(target);
    target = ::_pbi::WireFormatLite::WriteBoolToArray(7, this->_internal_rule_id_prefix_match(), target);
  }

  // string restricted_area_violation = 8;
  if (!this->_internal_restricted_area_violation().empty()) {
    ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::VerifyUtf8String(
      this->_internal_restricted_area_violation().data(), static_cast<int>(this->_internal_restricted_area_violation().length()),
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::SERIALIZE,
      "mdx.client.config.EventMappingRule.restricted_area_violation");
    target = stream->WriteStringMaybeAliased(
        8, this->_internal_restricted_area_violation(), target);
  }

  // string confined_area_violation = 9;
  if (!this->_internal_confined_area_violation().empty()) {
    ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::VerifyUtf8String(
      this->_internal_confined_area_violation().data(), static_cast<int>(this->_internal_confined_area_violation().length()),
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::SERIALIZE,
      "mdx.client.config.EventMappingRule.confined_area_violation");
    target = stream->WriteStringMaybeAliased(
        9, this->_internal_confined_area_violation(), target);
  }

  // string social_distancing_violation = 10;
  if (!this->_internal_social_distancing_violation().empty()) {
    ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::VerifyUtf8String(
      this->_internal_social_distancing_violation().data(), static_cast<int>(this->_internal_social_distancing_violation().length()),
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::SERIALIZE,
      "mdx.client.config.EventMappingRule.social_distancing_violation");
    target = stream->WriteStringMaybeAliased(
        10, this->_internal_social_distancing_violation(), target);
  }

  // string output_event = 11;
  if (!this->_internal_output_event().empty()) {
    ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::VerifyUtf8String(
      this->_internal_output_event().data(), static_cast<int>(this->_internal_output_event().length()),
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::SERIALIZE,
      "mdx.client.config.EventMappingRule.output_event");
    target = stream->WriteStringMaybeAliased(
        11, this->_internal_output_event(), target);
  }

  // string severity = 12;
  if (!this->_internal_severity().empty()) {
    ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::VerifyUtf8String(
      this->_internal_severity().data(), static_cast<int>(this->_internal_severity().length()),
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::SERIALIZE,
      "mdx.client.config.EventMappingRule.severity");
    target = stream->WriteStringMaybeAliased(
        12, this->_internal_severity(), target);
  }

  // double distance_threshold_meters = 13;
  static_assert(sizeof(uint64_t) == sizeof(double), "Code assumes uint64_t and double are the same size.");
  double tmp_distance_threshold_meters = this->_internal_distance_threshold_meters();
  uint64_t raw_distance_threshold_meters;
  memcpy(&raw_distance_threshold_meters, &tmp_distance_threshold_meters, sizeof(tmp_distance_threshold_meters));
  if (raw_distance_threshold_meters != 0) {
    target = stream->EnsureSpace(target);
    target = ::_pbi::WireFormatLite::WriteDoubleToArray(13, this->_internal_distance_threshold_meters(), target);
  }

  // string object_type_secondary = 14;
  if (!this->_internal_object_type_secondary().empty()) {
    ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::VerifyUtf8String(
      this->_internal_object_type_secondary().data(), static_cast<int>(this->_internal_object_type_secondary().length()),
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::SERIALIZE,
      "mdx.client.config.EventMappingRule.object_type_secondary");
    target = stream->WriteStringMaybeAliased(
        14, this->_internal_object_type_secondary(), target);
  }

  // string object_type_primary = 15;
  if (!this->_internal_object_type_primary().empty()) {
    ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::VerifyUtf8String(
      this->_internal_object_type_primary().data(), static_cast<int>(this->_internal_object_type_primary().length()),
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::SERIALIZE,
      "mdx.client.config.EventMappingRule.object_type_primary");
    target = stream->WriteStringMaybeAliased(
        15, this->_internal_object_type_primary(), target);
  }

  // int32 scale_factor = 16;
  if (this->_internal_scale_factor() != 0) {
    target = stream->EnsureSpace(target);
    target = ::_pbi::WireFormatLite::WriteInt32ToArray(16, this->_internal_scale_factor(), target);
  }

  if (PROTOBUF_PREDICT_FALSE(_internal_metadata_.have_unknown_fields())) {
    target = ::_pbi::WireFormat::InternalSerializeUnknownFieldsToArray(
        _internal_metadata_.unknown_fields<::PROTOBUF_NAMESPACE_ID::UnknownFieldSet>(::PROTOBUF_NAMESPACE_ID::UnknownFieldSet::default_instance), target, stream);
  }
  // @@protoc_insertion_point(serialize_to_array_end:mdx.client.config.EventMappingRule)
  return target;
}

size_t EventMappingRule::ByteSizeLong() const {
// @@protoc_insertion_point(message_byte_size_start:mdx.client.config.EventMappingRule)
  size_t total_size = 0;

  uint32_t cached_has_bits = 0;
  // Prevent compiler warnings about cached_has_bits being unused
  (void) cached_has_bits;

  // string name = 1;
  if (!this->_internal_name().empty()) {
    total_size += 1 +
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::StringSize(
        this->_internal_name());
  }

  // string message_source = 2;
  if (!this->_internal_message_source().empty()) {
    total_size += 1 +
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::StringSize(
        this->_internal_message_source());
  }

  // string alert_type = 3;
  if (!this->_internal_alert_type().empty()) {
    total_size += 1 +
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::StringSize(
        this->_internal_alert_type());
  }

  // string event_type = 4;
  if (!this->_internal_event_type().empty()) {
    total_size += 1 +
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::StringSize(
        this->_internal_event_type());
  }

  // string object_type = 5;
  if (!this->_internal_object_type().empty()) {
    total_size += 1 +
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::StringSize(
        this->_internal_object_type());
  }

  // string rule_id = 6;
  if (!this->_internal_rule_id().empty()) {
    total_size += 1 +
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::StringSize(
        this->_internal_rule_id());
  }

  // string restricted_area_violation = 8;
  if (!this->_internal_restricted_area_violation().empty()) {
    total_size += 1 +
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::StringSize(
        this->_internal_restricted_area_violation());
  }

  // string confined_area_violation = 9;
  if (!this->_internal_confined_area_violation().empty()) {
    total_size += 1 +
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::StringSize(
        this->_internal_confined_area_violation());
  }

  // string social_distancing_violation = 10;
  if (!this->_internal_social_distancing_violation().empty()) {
    total_size += 1 +
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::StringSize(
        this->_internal_social_distancing_violation());
  }

  // string output_event = 11;
  if (!this->_internal_output_event().empty()) {
    total_size += 1 +
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::StringSize(
        this->_internal_output_event());
  }

  // string severity = 12;
  if (!this->_internal_severity().empty()) {
    total_size += 1 +
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::StringSize(
        this->_internal_severity());
  }

  // string object_type_secondary = 14;
  if (!this->_internal_object_type_secondary().empty()) {
    total_size += 1 +
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::StringSize(
        this->_internal_object_type_secondary());
  }

  // string object_type_primary = 15;
  if (!this->_internal_object_type_primary().empty()) {
    total_size += 1 +
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::StringSize(
        this->_internal_object_type_primary());
  }

  // bool rule_id_prefix_match = 7;
  if (this->_internal_rule_id_prefix_match() != 0) {
    total_size += 1 + 1;
  }

  // int32 scale_factor = 16;
  if (this->_internal_scale_factor() != 0) {
    total_size += 2 +
      ::_pbi::WireFormatLite::Int32Size(
        this->_internal_scale_factor());
  }

  // double distance_threshold_meters = 13;
  static_assert(sizeof(uint64_t) == sizeof(double), "Code assumes uint64_t and double are the same size.");
  double tmp_distance_threshold_meters = this->_internal_distance_threshold_meters();
  uint64_t raw_distance_threshold_meters;
  memcpy(&raw_distance_threshold_meters, &tmp_distance_threshold_meters, sizeof(tmp_distance_threshold_meters));
  if (raw_distance_threshold_meters != 0) {
    total_size += 1 + 8;
  }

  return MaybeComputeUnknownFieldsSize(total_size, &_impl_._cached_size_);
}

const ::PROTOBUF_NAMESPACE_ID::Message::ClassData EventMappingRule::_class_data_ = {
    ::PROTOBUF_NAMESPACE_ID::Message::CopyWithSourceCheck,
    EventMappingRule::MergeImpl
};
const ::PROTOBUF_NAMESPACE_ID::Message::ClassData*EventMappingRule::GetClassData() const { return &_class_data_; }


void EventMappingRule::MergeImpl(::PROTOBUF_NAMESPACE_ID::Message& to_msg, const ::PROTOBUF_NAMESPACE_ID::Message& from_msg) {
  auto* const _this = static_cast<EventMappingRule*>(&to_msg);
  auto& from = static_cast<const EventMappingRule&>(from_msg);
  // @@protoc_insertion_point(class_specific_merge_from_start:mdx.client.config.EventMappingRule)
  GOOGLE_DCHECK_NE(&from, _this);
  uint32_t cached_has_bits = 0;
  (void) cached_has_bits;

  if (!from._internal_name().empty()) {
    _this->_internal_set_name(from._internal_name());
  }
  if (!from._internal_message_source().empty()) {
    _this->_internal_set_message_source(from._internal_message_source());
  }
  if (!from._internal_alert_type().empty()) {
    _this->_internal_set_alert_type(from._internal_alert_type());
  }
  if (!from._internal_event_type().empty()) {
    _this->_internal_set_event_type(from._internal_event_type());
  }
  if (!from._internal_object_type().empty()) {
    _this->_internal_set_object_type(from._internal_object_type());
  }
  if (!from._internal_rule_id().empty()) {
    _this->_internal_set_rule_id(from._internal_rule_id());
  }
  if (!from._internal_restricted_area_violation().empty()) {
    _this->_internal_set_restricted_area_violation(from._internal_restricted_area_violation());
  }
  if (!from._internal_confined_area_violation().empty()) {
    _this->_internal_set_confined_area_violation(from._internal_confined_area_violation());
  }
  if (!from._internal_social_distancing_violation().empty()) {
    _this->_internal_set_social_distancing_violation(from._internal_social_distancing_violation());
  }
  if (!from._internal_output_event().empty()) {
    _this->_internal_set_output_event(from._internal_output_event());
  }
  if (!from._internal_severity().empty()) {
    _this->_internal_set_severity(from._internal_severity());
  }
  if (!from._internal_object_type_secondary().empty()) {
    _this->_internal_set_object_type_secondary(from._internal_object_type_secondary());
  }
  if (!from._internal_object_type_primary().empty()) {
    _this->_internal_set_object_type_primary(from._internal_object_type_primary());
  }
  if (from._internal_rule_id_prefix_match() != 0) {
    _this->_internal_set_rule_id_prefix_match(from._internal_rule_id_prefix_match());
  }
  if (from._internal_scale_factor() != 0) {
    _this->_internal_set_scale_factor(from._internal_scale_factor());
  }
  static_assert(sizeof(uint64_t) == sizeof(double), "Code assumes uint64_t and double are the same size.");
  double tmp_distance_threshold_meters = from._internal_distance_threshold_meters();
  uint64_t raw_distance_threshold_meters;
  memcpy(&raw_distance_threshold_meters, &tmp_distance_threshold_meters, sizeof(tmp_distance_threshold_meters));
  if (raw_distance_threshold_meters != 0) {
    _this->_internal_set_distance_threshold_meters(from._internal_distance_threshold_meters());
  }
  _this->_internal_metadata_.MergeFrom<::PROTOBUF_NAMESPACE_ID::UnknownFieldSet>(from._internal_metadata_);
}

void EventMappingRule::CopyFrom(const EventMappingRule& from) {
// @@protoc_insertion_point(class_specific_copy_from_start:mdx.client.config.EventMappingRule)
  if (&from == this) return;
  Clear();
  MergeFrom(from);
}

bool EventMappingRule::IsInitialized() const {
  return true;
}

void EventMappingRule::InternalSwap(EventMappingRule* other) {
  using std::swap;
  auto* lhs_arena = GetArenaForAllocation();
  auto* rhs_arena = other->GetArenaForAllocation();
  _internal_metadata_.InternalSwap(&other->_internal_metadata_);
  ::PROTOBUF_NAMESPACE_ID::internal::ArenaStringPtr::InternalSwap(
      &_impl_.name_, lhs_arena,
      &other->_impl_.name_, rhs_arena
  );
  ::PROTOBUF_NAMESPACE_ID::internal::ArenaStringPtr::InternalSwap(
      &_impl_.message_source_, lhs_arena,
      &other->_impl_.message_source_, rhs_arena
  );
  ::PROTOBUF_NAMESPACE_ID::internal::ArenaStringPtr::InternalSwap(
      &_impl_.alert_type_, lhs_arena,
      &other->_impl_.alert_type_, rhs_arena
  );
  ::PROTOBUF_NAMESPACE_ID::internal::ArenaStringPtr::InternalSwap(
      &_impl_.event_type_, lhs_arena,
      &other->_impl_.event_type_, rhs_arena
  );
  ::PROTOBUF_NAMESPACE_ID::internal::ArenaStringPtr::InternalSwap(
      &_impl_.object_type_, lhs_arena,
      &other->_impl_.object_type_, rhs_arena
  );
  ::PROTOBUF_NAMESPACE_ID::internal::ArenaStringPtr::InternalSwap(
      &_impl_.rule_id_, lhs_arena,
      &other->_impl_.rule_id_, rhs_arena
  );
  ::PROTOBUF_NAMESPACE_ID::internal::ArenaStringPtr::InternalSwap(
      &_impl_.restricted_area_violation_, lhs_arena,
      &other->_impl_.restricted_area_violation_, rhs_arena
  );
  ::PROTOBUF_NAMESPACE_ID::internal::ArenaStringPtr::InternalSwap(
      &_impl_.confined_area_violation_, lhs_arena,
      &other->_impl_.confined_area_violation_, rhs_arena
  );
  ::PROTOBUF_NAMESPACE_ID::internal::ArenaStringPtr::InternalSwap(
      &_impl_.social_distancing_violation_, lhs_arena,
      &other->_impl_.social_distancing_violation_, rhs_arena
  );
  ::PROTOBUF_NAMESPACE_ID::internal::ArenaStringPtr::InternalSwap(
      &_impl_.output_event_, lhs_arena,
      &other->_impl_.output_event_, rhs_arena
  );
  ::PROTOBUF_NAMESPACE_ID::internal::ArenaStringPtr::InternalSwap(
      &_impl_.severity_, lhs_arena,
      &other->_impl_.severity_, rhs_arena
  );
  ::PROTOBUF_NAMESPACE_ID::internal::ArenaStringPtr::InternalSwap(
      &_impl_.object_type_secondary_, lhs_arena,
      &other->_impl_.object_type_secondary_, rhs_arena
  );
  ::PROTOBUF_NAMESPACE_ID::internal::ArenaStringPtr::InternalSwap(
      &_impl_.object_type_primary_, lhs_arena,
      &other->_impl_.object_type_primary_, rhs_arena
  );
  ::PROTOBUF_NAMESPACE_ID::internal::memswap<
      PROTOBUF_FIELD_OFFSET(EventMappingRule, _impl_.distance_threshold_meters_)
      + sizeof(EventMappingRule::_impl_.distance_threshold_meters_)
      - PROTOBUF_FIELD_OFFSET(EventMappingRule, _impl_.rule_id_prefix_match_)>(
          reinterpret_cast<char*>(&_impl_.rule_id_prefix_match_),
          reinterpret_cast<char*>(&other->_impl_.rule_id_prefix_match_));
}

::PROTOBUF_NAMESPACE_ID::Metadata EventMappingRule::GetMetadata() const {
  return ::_pbi::AssignDescriptors(
      &descriptor_table_event_5fmapping_2eproto_getter, &descriptor_table_event_5fmapping_2eproto_once,
      file_level_metadata_event_5fmapping_2eproto[0]);
}

// ===================================================================

class EventMappingConfig::_Internal {
 public:
};

EventMappingConfig::EventMappingConfig(::PROTOBUF_NAMESPACE_ID::Arena* arena,
                         bool is_message_owned)
  : ::PROTOBUF_NAMESPACE_ID::Message(arena, is_message_owned) {
  SharedCtor(arena, is_message_owned);
  // @@protoc_insertion_point(arena_constructor:mdx.client.config.EventMappingConfig)
}
EventMappingConfig::EventMappingConfig(const EventMappingConfig& from)
  : ::PROTOBUF_NAMESPACE_ID::Message() {
  EventMappingConfig* const _this = this; (void)_this;
  new (&_impl_) Impl_{
      decltype(_impl_.rules_){from._impl_.rules_}
    , /*decltype(_impl_._cached_size_)*/{}};

  _internal_metadata_.MergeFrom<::PROTOBUF_NAMESPACE_ID::UnknownFieldSet>(from._internal_metadata_);
  // @@protoc_insertion_point(copy_constructor:mdx.client.config.EventMappingConfig)
}

inline void EventMappingConfig::SharedCtor(
    ::_pb::Arena* arena, bool is_message_owned) {
  (void)arena;
  (void)is_message_owned;
  new (&_impl_) Impl_{
      decltype(_impl_.rules_){arena}
    , /*decltype(_impl_._cached_size_)*/{}
  };
}

EventMappingConfig::~EventMappingConfig() {
  // @@protoc_insertion_point(destructor:mdx.client.config.EventMappingConfig)
  if (auto *arena = _internal_metadata_.DeleteReturnArena<::PROTOBUF_NAMESPACE_ID::UnknownFieldSet>()) {
  (void)arena;
    return;
  }
  SharedDtor();
}

inline void EventMappingConfig::SharedDtor() {
  GOOGLE_DCHECK(GetArenaForAllocation() == nullptr);
  _impl_.rules_.~RepeatedPtrField();
}

void EventMappingConfig::SetCachedSize(int size) const {
  _impl_._cached_size_.Set(size);
}

void EventMappingConfig::Clear() {
// @@protoc_insertion_point(message_clear_start:mdx.client.config.EventMappingConfig)
  uint32_t cached_has_bits = 0;
  // Prevent compiler warnings about cached_has_bits being unused
  (void) cached_has_bits;

  _impl_.rules_.Clear();
  _internal_metadata_.Clear<::PROTOBUF_NAMESPACE_ID::UnknownFieldSet>();
}

const char* EventMappingConfig::_InternalParse(const char* ptr, ::_pbi::ParseContext* ctx) {
#define CHK_(x) if (PROTOBUF_PREDICT_FALSE(!(x))) goto failure
  while (!ctx->Done(&ptr)) {
    uint32_t tag;
    ptr = ::_pbi::ReadTag(ptr, &tag);
    switch (tag >> 3) {
      // repeated .mdx.client.config.EventMappingRule rules = 1;
      case 1:
        if (PROTOBUF_PREDICT_TRUE(static_cast<uint8_t>(tag) == 10)) {
          ptr -= 1;
          do {
            ptr += 1;
            ptr = ctx->ParseMessage(_internal_add_rules(), ptr);
            CHK_(ptr);
            if (!ctx->DataAvailable(ptr)) break;
          } while (::PROTOBUF_NAMESPACE_ID::internal::ExpectTag<10>(ptr));
        } else
          goto handle_unusual;
        continue;
      default:
        goto handle_unusual;
    }  // switch
  handle_unusual:
    if ((tag == 0) || ((tag & 7) == 4)) {
      CHK_(ptr);
      ctx->SetLastTag(tag);
      goto message_done;
    }
    ptr = UnknownFieldParse(
        tag,
        _internal_metadata_.mutable_unknown_fields<::PROTOBUF_NAMESPACE_ID::UnknownFieldSet>(),
        ptr, ctx);
    CHK_(ptr != nullptr);
  }  // while
message_done:
  return ptr;
failure:
  ptr = nullptr;
  goto message_done;
#undef CHK_
}

uint8_t* EventMappingConfig::_InternalSerialize(
    uint8_t* target, ::PROTOBUF_NAMESPACE_ID::io::EpsCopyOutputStream* stream) const {
  // @@protoc_insertion_point(serialize_to_array_start:mdx.client.config.EventMappingConfig)
  uint32_t cached_has_bits = 0;
  (void) cached_has_bits;

  // repeated .mdx.client.config.EventMappingRule rules = 1;
  for (unsigned i = 0,
      n = static_cast<unsigned>(this->_internal_rules_size()); i < n; i++) {
    const auto& repfield = this->_internal_rules(i);
    target = ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::
        InternalWriteMessage(1, repfield, repfield.GetCachedSize(), target, stream);
  }

  if (PROTOBUF_PREDICT_FALSE(_internal_metadata_.have_unknown_fields())) {
    target = ::_pbi::WireFormat::InternalSerializeUnknownFieldsToArray(
        _internal_metadata_.unknown_fields<::PROTOBUF_NAMESPACE_ID::UnknownFieldSet>(::PROTOBUF_NAMESPACE_ID::UnknownFieldSet::default_instance), target, stream);
  }
  // @@protoc_insertion_point(serialize_to_array_end:mdx.client.config.EventMappingConfig)
  return target;
}

size_t EventMappingConfig::ByteSizeLong() const {
// @@protoc_insertion_point(message_byte_size_start:mdx.client.config.EventMappingConfig)
  size_t total_size = 0;

  uint32_t cached_has_bits = 0;
  // Prevent compiler warnings about cached_has_bits being unused
  (void) cached_has_bits;

  // repeated .mdx.client.config.EventMappingRule rules = 1;
  total_size += 1UL * this->_internal_rules_size();
  for (const auto& msg : this->_impl_.rules_) {
    total_size +=
      ::PROTOBUF_NAMESPACE_ID::internal::WireFormatLite::MessageSize(msg);
  }

  return MaybeComputeUnknownFieldsSize(total_size, &_impl_._cached_size_);
}

const ::PROTOBUF_NAMESPACE_ID::Message::ClassData EventMappingConfig::_class_data_ = {
    ::PROTOBUF_NAMESPACE_ID::Message::CopyWithSourceCheck,
    EventMappingConfig::MergeImpl
};
const ::PROTOBUF_NAMESPACE_ID::Message::ClassData*EventMappingConfig::GetClassData() const { return &_class_data_; }


void EventMappingConfig::MergeImpl(::PROTOBUF_NAMESPACE_ID::Message& to_msg, const ::PROTOBUF_NAMESPACE_ID::Message& from_msg) {
  auto* const _this = static_cast<EventMappingConfig*>(&to_msg);
  auto& from = static_cast<const EventMappingConfig&>(from_msg);
  // @@protoc_insertion_point(class_specific_merge_from_start:mdx.client.config.EventMappingConfig)
  GOOGLE_DCHECK_NE(&from, _this);
  uint32_t cached_has_bits = 0;
  (void) cached_has_bits;

  _this->_impl_.rules_.MergeFrom(from._impl_.rules_);
  _this->_internal_metadata_.MergeFrom<::PROTOBUF_NAMESPACE_ID::UnknownFieldSet>(from._internal_metadata_);
}

void EventMappingConfig::CopyFrom(const EventMappingConfig& from) {
// @@protoc_insertion_point(class_specific_copy_from_start:mdx.client.config.EventMappingConfig)
  if (&from == this) return;
  Clear();
  MergeFrom(from);
}

bool EventMappingConfig::IsInitialized() const {
  return true;
}

void EventMappingConfig::InternalSwap(EventMappingConfig* other) {
  using std::swap;
  _internal_metadata_.InternalSwap(&other->_internal_metadata_);
  _impl_.rules_.InternalSwap(&other->_impl_.rules_);
}

::PROTOBUF_NAMESPACE_ID::Metadata EventMappingConfig::GetMetadata() const {
  return ::_pbi::AssignDescriptors(
      &descriptor_table_event_5fmapping_2eproto_getter, &descriptor_table_event_5fmapping_2eproto_once,
      file_level_metadata_event_5fmapping_2eproto[1]);
}

// @@protoc_insertion_point(namespace_scope)
}  // namespace config
}  // namespace client
}  // namespace mdx
PROTOBUF_NAMESPACE_OPEN
template<> PROTOBUF_NOINLINE ::mdx::client::config::EventMappingRule*
Arena::CreateMaybeMessage< ::mdx::client::config::EventMappingRule >(Arena* arena) {
  return Arena::CreateMessageInternal< ::mdx::client::config::EventMappingRule >(arena);
}
template<> PROTOBUF_NOINLINE ::mdx::client::config::EventMappingConfig*
Arena::CreateMaybeMessage< ::mdx::client::config::EventMappingConfig >(Arena* arena) {
  return Arena::CreateMessageInternal< ::mdx::client::config::EventMappingConfig >(arena);
}
PROTOBUF_NAMESPACE_CLOSE

// @@protoc_insertion_point(global_scope)
#include <google/protobuf/port_undef.inc>
