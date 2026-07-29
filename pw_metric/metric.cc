// Copyright 2020 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

#include "pw_metric/metric.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "pw_assert/check.h"
#include "pw_log/log.h"
#include "pw_metric/config.h"
#include "pw_metric/list.h"
#include "pw_span/span.h"
#include "pw_tokenizer/nested_tokenization.h"

namespace pw::metric {
namespace {

const char* Indent(int level) {
  static const char* kWhitespace10 = "          ";
  level = std::min(level, 4);
  return kWhitespace10 + 8 - 2 * level;
}

}  // namespace

UntypedMetric::UntypedMetric(Token name, Type type, MetricList& metrics)
    : UntypedMetric(name, type) {
  metrics.list().push_front(*this);
}

UntypedMetric::~UntypedMetric() {
  if (!unlisted()) {
    unlist();
  }
}

void UntypedMetric::Dump(int level, bool last) const {
  const char* indent = Indent(level);
  const char* comma = last ? "" : ",";
  switch (type()) {
    case kTypeFloat: {
      const auto& m = static_cast<const TypedMetric<float>&>(*this);
      // Variadic macros promote float to double. Explicitly cast here to
      // acknowledge this and allow projects to use -Wdouble-promotion.
      PW_LOG_INFO("%s \"" PW_TOKEN_FMT() "\": %f%s",
                  indent,
                  name(),
                  static_cast<double>(m.value()),
                  comma);
      break;
    }
    case kTypeUint32: {
      const auto& m = static_cast<const TypedMetric<uint32_t>&>(*this);
      PW_LOG_INFO("%s \"" PW_TOKEN_FMT() "\": %u%s",
                  indent,
                  name(),
                  static_cast<unsigned int>(m.value()),
                  comma);
      break;
    }
#if PW_METRIC_CONFIG_ENABLE_64BIT
    case kTypeUint64: {
      const auto& m = static_cast<const TypedMetric<uint64_t>&>(*this);
      PW_LOG_INFO("%s \"" PW_TOKEN_FMT() "\": %llu%s",
                  indent,
                  name(),
                  static_cast<unsigned long long>(m.value()),
                  comma);
      break;
    }
    case kTypeInt64: {
      const auto& m = static_cast<const TypedMetric<int64_t>&>(*this);
      PW_LOG_INFO("%s \"" PW_TOKEN_FMT() "\": %lld%s",
                  indent,
                  name(),
                  static_cast<long long>(m.value()),
                  comma);
      break;
    }
#endif  // PW_METRIC_CONFIG_ENABLE_64BIT
    case kTypeBool: {
      const auto& m = static_cast<const TypedMetric<bool>&>(*this);
      PW_LOG_INFO("%s \"" PW_TOKEN_FMT() "\": %s%s",
                  indent,
                  name(),
                  m.value() ? "true" : "false",
                  comma);
      break;
    }
    case kTypeInt32: {
      const auto& m = static_cast<const TypedMetric<int32_t>&>(*this);
      PW_LOG_INFO("%s \"" PW_TOKEN_FMT() "\": %d%s",
                  indent,
                  name(),
                  static_cast<int>(m.value()),
                  comma);
      break;
    }
#if PW_METRIC_CONFIG_ENABLE_64BIT
    case kTypeDouble: {
      const auto& m = static_cast<const TypedMetric<double>&>(*this);
      PW_LOG_INFO(
          "%s \"" PW_TOKEN_FMT() "\": %f%s", indent, name(), m.value(), comma);
      break;
    }
#endif  // PW_METRIC_CONFIG_ENABLE_64BIT
    case kTypeToken: {
      const auto& m = static_cast<const TypedMetric<TokenValue>&>(*this);
      PW_LOG_INFO("%s \"" PW_TOKEN_FMT() "\": \"" PW_TOKEN_FMT() "\"%s",
                  indent,
                  name(),
                  m.value().value,
                  comma);
      break;
    }
  }
}

float UntypedMetric::as_float() const {
  PW_DCHECK(is_float());
  if (is_float()) {
    return static_cast<const TypedMetric<float>*>(this)->value();
  }
  return 0.0f;
}

uint32_t UntypedMetric::as_int() const {
  PW_DCHECK(is_uint32());
  if (is_uint32()) {
    return static_cast<const TypedMetric<uint32_t>*>(this)->value();
  }
  return 0;
}

void UntypedMetric::Dump(const MetricList& metrics, int level) {
  const auto& list = metrics.list();
  auto iter = list.begin();
  while (iter != list.end()) {
    const UntypedMetric& m = *iter++;
    m.Dump(level, iter == list.end());
  }
}

void TypedMetric<uint32_t>::Increment(uint32_t amount) {
  PW_DCHECK(is_uint32());
  internal::SaturatedIncrement(value_, amount);
}

void TypedMetric<uint32_t>::Decrement(uint32_t amount) {
  PW_DCHECK(is_uint32());
  internal::SaturatedDecrement(value_, amount);
}

void TypedMetric<int32_t>::Increment(int32_t amount) {
  PW_DCHECK(is_int32());
  internal::SaturatedIncrement(value_, amount);
}

void TypedMetric<int32_t>::Decrement(int32_t amount) {
  PW_DCHECK(is_int32());
  internal::SaturatedDecrement(value_, amount);
}

Group::Group(Token name, GroupList& groups) : name_(name) {
  groups.list().push_front(*this);
}

Group::Group(Token name) : name_(name) {}

Group::~Group() {
  if (!unlisted()) {
    unlist();
  }
}

void Group::Dump() const {
  PW_LOG_INFO("{");
  Dump(0, true);
  PW_LOG_INFO("}");
}

void Group::Dump(int level, bool last) const {
  const char* indent = Indent(level);
  const char* comma = last ? "" : ",";
  PW_LOG_INFO("%s\"" PW_TOKEN_FMT() "\": {", indent, name());
  Group::Dump(children(), level + 1);
  UntypedMetric::Dump(metrics(), level + 1);
  PW_LOG_INFO("%s}%s", indent, comma);
}

void Group::Dump(const GroupList& groups, int level) {
  const auto& list = groups.list();
  auto iter = list.begin();
  while (iter != list.end()) {
    const Group& g = *iter++;
    g.Dump(level, iter == list.end());
  }
}

}  // namespace pw::metric
