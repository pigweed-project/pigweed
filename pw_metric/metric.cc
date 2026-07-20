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

#include <array>
#include <atomic>
#include <limits>

#include "pw_assert/check.h"
#include "pw_log/log.h"
#include "pw_numeric/checked_arithmetic.h"
#include "pw_span/span.h"
#include "pw_tokenizer/base64.h"

namespace pw::metric {
namespace {

template <typename T>
span<const std::byte> AsSpan(const T& t) {
  return span<const std::byte>(reinterpret_cast<const std::byte*>(&t),
                               sizeof(t));
}

// A convenience class to encode a token as base64 while managing the storage.
// TODO(keir): Consider putting this into upstream pw_tokenizer.
struct Base64EncodedToken {
  Base64EncodedToken(Token token) {
    size_t encoded_size = tokenizer::PrefixedBase64Encode(AsSpan(token), data);
    data[encoded_size] = 0;
  }

  const char* value() { return data.data(); }
  std::array<char, 16> data;
};

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
  Base64EncodedToken encoded_name(name());
  const char* indent = Indent(level);
  const char* comma = last ? "" : ",";
  switch (type()) {
    case kTypeFloat: {
      const auto& m = static_cast<const TypedMetric<float>&>(*this);
      // Variadic macros promote float to double. Explicitly cast here to
      // acknowledge this and allow projects to use -Wdouble-promotion.
      PW_LOG_INFO("%s \"%s\": %f%s",
                  indent,
                  encoded_name.value(),
                  static_cast<double>(m.value()),
                  comma);
      break;
    }
    case kTypeUint32: {
      const auto& m = static_cast<const TypedMetric<uint32_t>&>(*this);
      PW_LOG_INFO("%s \"%s\": %u%s",
                  indent,
                  encoded_name.value(),
                  static_cast<unsigned int>(m.value()),
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

  uint32_t value = value_.load();
  uint32_t updated;

  do {
    if (value == std::numeric_limits<uint32_t>::max()) {
      return;
    }
    if (!CheckedAdd(value, amount, updated)) {
      updated = std::numeric_limits<uint32_t>::max();
    }
  } while (!value_.compare_exchange_weak(value, updated));
}

void TypedMetric<uint32_t>::Decrement(uint32_t amount) {
  PW_DCHECK(is_uint32());

  uint32_t value = value_.load();
  uint32_t updated;

  do {
    if (value == 0) {
      return;
    }

    if (!CheckedSub(value, amount, updated)) {
      updated = 0;
    }
  } while (!value_.compare_exchange_weak(value, updated));
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
  Base64EncodedToken encoded_name(name());
  const char* indent = Indent(level);
  const char* comma = last ? "" : ",";
  PW_LOG_INFO("%s\"%s\": {", indent, encoded_name.value());
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
