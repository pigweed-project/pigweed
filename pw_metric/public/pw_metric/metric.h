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
#pragma once

#include <algorithm>
#include <atomic>
#include <initializer_list>
#include <limits>

#include "pw_memory/no_destructor.h"
#include "pw_metric/list.h"
#include "pw_preprocessor/arguments.h"
#include "pw_tokenizer/tokenize.h"

/// Lightweight manual instrumentation library
namespace pw::metric {

// Currently, this is for tokens, but later may be a char* when non-tokenized
// metric names are supported.
using tokenizer::Token;

// The token mask is 28 bits (0x0fffffff) to allow 4 bits for type information
// in the 32-bit name_and_type_ field. While this increases the probability of
// token collisions compared to a 31-bit mask, 28 bits (268M states) is still
// extremely large and sufficient to avoid collisions in practice for typical
// metric sets.
#define _PW_METRIC_TOKEN_MASK 0x0fffffff

// An individual metric. There are only two supported types: uint32_t and
// float. More complicated compound metrics can be built on these primitives.
// See the documentation for a discussion for this design was selected.
//
// Size: 12 bytes / 96 bits - next, name, value.
//
// TODO(keir): Implement Set() and Increment() using atomics.
// TODO(keir): Consider an alternative structure where metrics have pointers to
// parent groups, which would enable (1) safe destruction and (2) safe static
// initialization, but at the cost of an additional 4 bytes per metric and 4
// bytes per group..
class UntypedMetric : public MetricList::Item {
 public:
  Token name() const { return name_and_type_ & kTokenMask; }

  // Disallow copy and assign.
  UntypedMetric(UntypedMetric const&) = delete;
  void operator=(const UntypedMetric&) = delete;

  enum Type : uint32_t {
    kTypeUint32 = 0x00000000,
    kTypeFloat = 0x10000000,
  };

  Type type() const { return static_cast<Type>(name_and_type_ & kTypeMask); }

  bool is_float() const { return type() == kTypeFloat; }
  bool is_uint32() const { return type() == kTypeUint32; }

  // Backward compatibility alias.
  [[deprecated("Use is_uint32() instead")]]
  bool is_int() const {
    return is_uint32();
  }

  // Dump a metric or metrics to logs. Level determines the indentation
  // indent_level up to a maximum of 4. Example output:
  //
  //   "$FCM4qQ==": 0
  //
  // Note the base64-encoded token name. Detokenization tools are necessary to
  // convert this to human-readable form.
  void Dump(int indent_level, bool last) const;
  static void Dump(const MetricList& metrics, int indent_level = 0);

  // Backward compatibility helpers.
  // TODO: b/527616876 - Remove these deprecated helper methods in a later CL.
  float as_float() const;
  uint32_t as_int() const;

 protected:
  constexpr UntypedMetric(Token name, Type type)
      : name_and_type_((name & kTokenMask) | type) {}

  UntypedMetric(Token name, Type type, MetricList& metrics);

  ~UntypedMetric();

  Token name_and_type_;

  static constexpr uint32_t kTokenMask = _PW_METRIC_TOKEN_MASK;
  static constexpr uint32_t kTypeMask = 0xf0000000;
  static_assert((kTokenMask & kTypeMask) == 0,
                "Token mask and Type mask must not overlap.");

  friend class ResumableMetricWalker;
  friend class MetricWalker;
};

// TODO: b/527616876 - Remove this backward compatibility alias in a later CL.
using Metric = UntypedMetric;

template <typename T>
class TypedMetric;

// A metric for floats. Does not offer an Increment() function, since it is too
// easy to do unsafe operations like accumulating small values in floats.
template <>
class TypedMetric<float> : public UntypedMetric {
 public:
  constexpr TypedMetric(Token name, float value)
      : UntypedMetric(name, kTypeFloat), value_(value) {}
  TypedMetric(Token name, float value, MetricList& metrics)
      : UntypedMetric(name, kTypeFloat, metrics), value_(value) {}

  ~TypedMetric() = default;

  void Set(float value) { value_.store(value, std::memory_order_relaxed); }
  float value() const { return value_.load(std::memory_order_relaxed); }

 private:
  std::atomic<float> value_;
};

// A metric for uint32_ts. Offers both Set() and Increment().
template <>
class TypedMetric<uint32_t> : public UntypedMetric {
 public:
  constexpr TypedMetric(Token name, uint32_t value)
      : UntypedMetric(name, kTypeUint32), value_(value) {}
  TypedMetric(Token name, uint32_t value, MetricList& metrics)
      : UntypedMetric(name, kTypeUint32, metrics), value_(value) {}

  ~TypedMetric() = default;

  void Increment(uint32_t amount = 1u);
  void Decrement(uint32_t amount = 1u);
  void Set(uint32_t value) { value_.store(value, std::memory_order_relaxed); }
  uint32_t value() const { return value_.load(std::memory_order_relaxed); }

 private:
  std::atomic<uint32_t> value_;
};

// A metric tree; consisting of children groups and leaf metrics.
//
// Size: 16 bytes/128 bits - next, name, metrics, children.
class Group : public GroupList::Item {
 public:
  Group(Token name);
  Group(Token name, GroupList& groups);

  Token name() const { return name_; }

  // Disallow copy and assign.
  Group(Group const&) = delete;
  void operator=(const Group&) = delete;

  ~Group();

  void Add(UntypedMetric& metric) { metrics_.list().push_front(metric); }
  void Add(Group& group) { children_.list().push_front(group); }

  MetricList& metrics() { return metrics_; }
  GroupList& children() { return children_; }

  const MetricList& metrics() const { return metrics_; }
  const GroupList& children() const { return children_; }

  // Dump a metric group or groups to logs. Level determines the indentation
  // indent_level up to a maximum of 4. Example output:
  //
  //   "$6doqFw==": {
  //     "$05OCZw==": {
  //       "$VpPfzg==": 1,
  //       "$LGPMBQ==": 1.000000,
  //       "$+iJvUg==": 5
  //     },
  //     "$9hPNxw==": 65,
  //     "$oK7HmA==": 13,
  //     "$FCM4qQ==": 0
  //   }
  //
  // Note the base64-encoded token name. Detokenization tools are necessary to
  // convert this to human-readable form.
  void Dump() const;
  static void Dump(const GroupList& groups, int indent_level = 0);

 private:
  friend GroupList;

  void Dump(int indent_level, bool last) const;

  // The name of this group as a token; from PW_TOKENIZE_STRING("my_group").
  Token name_;

  MetricList metrics_;
  GroupList children_;
};

// Declare a metric, optionally adding it to a group. Use:
//
//   PW_METRIC(variable_name, metric_name, value)
//   PW_METRIC(group, variable_name, metric_name, value)
//
// - variable_name is an identifier
// - metric_name is a string name for the metric (will be tokenized)
// - value must be either a floating point value (3.2f) or unsigned int (21u).
// - group is a Group instance.
//
// The macro declares a variable or member named "name" with type Metric, and
// works in three contexts: global, local, and member.
//
// 1. At global scope
//
//    PW_METRIC(foo, 15.5f);
//
//    void MyFunc() {
//      foo.Increment();
//    }
//
// 2. At local function or member function scope:
//
//    void MyFunc() {
//      PW_METRIC(foo, "foo", 15.5f);
//      foo.Increment();
//      // foo goes out of scope here; be careful!
//    }
//
// 3. At member level inside a class or struct:
//
//    struct MyStructy {
//      void DoSomething() {
//        somethings_.Increment();
//      }
//      // Every instance of MyStructy will have a separate somethings counter.
//      PW_METRIC(somethings_, "somethings", 0u);
//    }
//
// You can also put a metric into a group with the macro. Metrics can belong to
// strictly one group, otherwise a assertion will fail. Example:
//
//   PW_METRIC_GROUP(my_group, "my_group_name_here");
//   PW_METRIC(my_group, foo_, "foo", 0.2f);
//   PW_METRIC(my_group, bar_, "bar", 44000u);
//   PW_METRIC(my_group, zap_, "zap", 3.14f);
//
// NOTE: If you want a globally registered metric, see pw_metric/global.h; in
// that contexts, metrics are globally registered without the need to centrally
// register in a single place.
#define PW_METRIC(...) PW_DELEGATE_BY_ARG_COUNT(_PW_METRIC_, , __VA_ARGS__)
#define PW_METRIC_STATIC(...) \
  PW_DELEGATE_BY_ARG_COUNT(_PW_METRIC_STATIC_, static, __VA_ARGS__)

#define _PW_METRIC_FLOAT_OR_UINT32(literal)                       \
  std::conditional_t<std::is_floating_point_v<decltype(literal)>, \
                     float,                                       \
                     uint32_t>

#define PW_METRIC_TYPED(...) \
  PW_DELEGATE_BY_ARG_COUNT(_PW_METRIC_TYPED_, , __VA_ARGS__)

#define PW_METRIC_TYPED_STATIC(...) \
  PW_DELEGATE_BY_ARG_COUNT(_PW_METRIC_TYPED_STATIC_, static, __VA_ARGS__)

/// Get the token for a given metric name.
///
/// This is a wrapper around `PW_TOKENIZE_STRING_MASK` and carries the same
/// semantics.
#define PW_METRIC_TOKEN(metric_name) \
  PW_TOKENIZE_STRING_MASK("metrics", _PW_METRIC_TOKEN_MASK, metric_name)

// Case: PW_METRIC(name, initial_value)
#define _PW_METRIC_4(static_def, variable_name, metric_name, init)      \
  static constexpr uint32_t variable_name##_token =                     \
      PW_METRIC_TOKEN(metric_name);                                     \
  static_def::pw::metric::TypedMetric<_PW_METRIC_FLOAT_OR_UINT32(init)> \
      variable_name = {variable_name##_token, init}

// Case: PW_METRIC(group, name, initial_value)
#define _PW_METRIC_5(static_def, group, variable_name, metric_name, init) \
  static constexpr uint32_t variable_name##_token =                       \
      PW_METRIC_TOKEN(metric_name);                                       \
  static_def::pw::metric::TypedMetric<_PW_METRIC_FLOAT_OR_UINT32(init)>   \
      variable_name = {variable_name##_token, init, group.metrics()}

// Case: PW_METRIC_STATIC(name, initial_value)
#define _PW_METRIC_STATIC_4(static_def, variable_name, metric_name, init) \
  static constexpr uint32_t variable_name##_token =                       \
      PW_METRIC_TOKEN(metric_name);                                       \
  static_def::pw::NoDestructor<                                           \
      ::pw::metric::TypedMetric<_PW_METRIC_FLOAT_OR_UINT32(init)>>        \
      variable_name##_storage(variable_name##_token, init);               \
  static_def::pw::metric::TypedMetric<_PW_METRIC_FLOAT_OR_UINT32(init)>&  \
      variable_name [[maybe_unused]] = *variable_name##_storage

// Case: PW_METRIC_STATIC(group, name, initial_value)
#define _PW_METRIC_STATIC_5(                                                 \
    static_def, group, variable_name, metric_name, init)                     \
  static constexpr uint32_t variable_name##_token =                          \
      PW_METRIC_TOKEN(metric_name);                                          \
  static_def::pw::NoDestructor<                                              \
      ::pw::metric::TypedMetric<_PW_METRIC_FLOAT_OR_UINT32(init)>>           \
      variable_name##_storage(variable_name##_token, init, group.metrics()); \
  static_def::pw::metric::TypedMetric<_PW_METRIC_FLOAT_OR_UINT32(init)>&     \
      variable_name [[maybe_unused]] = *variable_name##_storage

// Case: PW_METRIC_TYPED(name, type, initial_value)
#define _PW_METRIC_TYPED_5(static_def, variable_name, metric_name, type, init) \
  static constexpr uint32_t variable_name##_token =                            \
      PW_METRIC_TOKEN(metric_name);                                            \
  static_def::pw::metric::TypedMetric<type> variable_name = {                  \
      variable_name##_token, init}

// Case: PW_METRIC_TYPED(group, name, type, initial_value)
#define _PW_METRIC_TYPED_6(                                    \
    static_def, group, variable_name, metric_name, type, init) \
  static constexpr uint32_t variable_name##_token =            \
      PW_METRIC_TOKEN(metric_name);                            \
  static_def::pw::metric::TypedMetric<type> variable_name = {  \
      variable_name##_token, init, group.metrics()}

// Case: PW_METRIC_TYPED_STATIC(name, type, initial_value)
#define _PW_METRIC_TYPED_STATIC_5(                                            \
    static_def, variable_name, metric_name, type, init)                       \
  static constexpr uint32_t variable_name##_token =                           \
      PW_METRIC_TOKEN(metric_name);                                           \
  static_def::pw::NoDestructor<::pw::metric::TypedMetric<type>>               \
      variable_name##_storage(variable_name##_token, init);                   \
  static_def::pw::metric::TypedMetric<type>& variable_name [[maybe_unused]] = \
      *variable_name##_storage

// Case: PW_METRIC_TYPED_STATIC(group, name, type, initial_value)
#define _PW_METRIC_TYPED_STATIC_6(                                            \
    static_def, group, variable_name, metric_name, type, init)                \
  static constexpr uint32_t variable_name##_token =                           \
      PW_METRIC_TOKEN(metric_name);                                           \
  static_def::pw::NoDestructor<::pw::metric::TypedMetric<type>>               \
      variable_name##_storage(variable_name##_token, init, group.metrics());  \
  static_def::pw::metric::TypedMetric<type>& variable_name [[maybe_unused]] = \
      *variable_name##_storage

// Define a metric group. Works like PW_METRIC, and works in the same contexts.
//
// Example:
//
//   class MySubsystem {
//    public:
//     void DoSomething() {
//       attempts.Increment();
//       if (ActionSucceeds()) {
//         successes.Increment();
//       }
//     }
//     const Group& metrics() const { return metrics_; }
//     Group& metrics() { return metrics_; }
//
//    private:
//     PW_METRIC_GROUP(metrics_, "my_subsystem");
//     PW_METRIC(metrics_, attempts_, "attempts", 0u);
//     PW_METRIC(metrics_, successes_, "successes", 0u);
//   };
//
#define PW_METRIC_GROUP(...) \
  PW_DELEGATE_BY_ARG_COUNT(_PW_METRIC_GROUP_, , __VA_ARGS__)
#define PW_METRIC_GROUP_STATIC(...) \
  PW_DELEGATE_BY_ARG_COUNT(_PW_METRIC_GROUP_STATIC_, static, __VA_ARGS__)

#define _PW_METRIC_GROUP_3(static_def, variable_name, group_name) \
  static constexpr uint32_t variable_name##_token =               \
      PW_TOKENIZE_STRING_DOMAIN("metrics", group_name);           \
  static_def::pw::metric::Group variable_name = {variable_name##_token}

#define _PW_METRIC_GROUP_4(static_def, parent, variable_name, group_name) \
  static constexpr uint32_t variable_name##_token =                       \
      PW_TOKENIZE_STRING_DOMAIN("metrics", group_name);                   \
  static_def::pw::metric::Group variable_name = {variable_name##_token,   \
                                                 parent.children()}

#define _PW_METRIC_GROUP_STATIC_3(static_def, variable_name, group_name)     \
  static constexpr uint32_t variable_name##_token =                          \
      PW_TOKENIZE_STRING_DOMAIN("metrics", group_name);                      \
  static_def::pw::NoDestructor<::pw::metric::Group> variable_name##_storage( \
      variable_name##_token);                                                \
  static_def::pw::metric::Group& variable_name [[maybe_unused]] =            \
      *variable_name##_storage

#define _PW_METRIC_GROUP_STATIC_4(                                           \
    static_def, parent, variable_name, group_name)                           \
  static constexpr uint32_t variable_name##_token =                          \
      PW_TOKENIZE_STRING_DOMAIN("metrics", group_name);                      \
  static_def::pw::NoDestructor<::pw::metric::Group> variable_name##_storage( \
      variable_name##_token, parent.children());                             \
  static_def::pw::metric::Group& variable_name [[maybe_unused]] =            \
      *variable_name##_storage

// Similar to PW_TOKENIZE_STRING_EXPR, converts a string literal to a
// ``uint32_t`` token within an expression.
// Requires C++.
//
//   // Tokenizes a string literal in the metric domain within an expression.
//   DoSomethingWithToken(PW_METRIC_TOKEN_EXPR("Succeed"));
//
// @endcode
#define PW_METRIC_TOKEN_EXPR(metric_name) \
  PW_TOKENIZE_STRING_MASK_EXPR("metrics", _PW_METRIC_TOKEN_MASK, metric_name)

}  // namespace pw::metric
