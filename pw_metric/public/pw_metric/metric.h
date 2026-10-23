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
#include <array>
#include <atomic>
#include <cstddef>
#include <initializer_list>
#include <limits>

#include "pw_memory/no_destructor.h"
#include "pw_metric/config.h"
#include "pw_metric/list.h"
#include "pw_numeric/checked_arithmetic.h"
#include "pw_preprocessor/arguments.h"
#include "pw_tokenizer/tokenize.h"

/// Lightweight manual instrumentation library
namespace pw::metric {

namespace internal {

// Atomically increments value by amount with upper-bound saturation.
// Uses a compare-and-swap loop to ensure atomic saturating arithmetic.
template <typename T>
void SaturatedIncrement(std::atomic<T>& atomic_val, T amount) {
  constexpr T max_val = std::numeric_limits<T>::max();
  T value = atomic_val.load(std::memory_order_relaxed);
  T updated;
  do {
    if (value == max_val) {
      return;
    }
    if (!CheckedAdd(value, amount, updated)) {
      updated = max_val;
    }
  } while (!atomic_val.compare_exchange_weak(
      value, updated, std::memory_order_relaxed));
}

// Atomically decrements value by amount with lower-bound saturation.
// Uses a compare-and-swap loop to ensure atomic saturating arithmetic.
template <typename T>
void SaturatedDecrement(std::atomic<T>& atomic_val, T amount) {
  constexpr T min_val =
      std::is_signed_v<T> ? std::numeric_limits<T>::min() : static_cast<T>(0);
  T value = atomic_val.load(std::memory_order_relaxed);
  T updated;
  do {
    if (value == min_val) {
      return;
    }
    if (!CheckedSub(value, amount, updated)) {
      updated = min_val;
    }
  } while (!atomic_val.compare_exchange_weak(
      value, updated, std::memory_order_relaxed));
}

}  // namespace internal

/// @module{pw_metric}

/// Currently, this is for tokens, but later may be a `char*` when non-tokenized
/// metric names are supported.
using tokenizer::Token;

// The token mask is 28 bits (0x0fffffff) to allow 4 bits for type information
// in the 32-bit name_and_type_ field. While this increases the probability of
// token collisions compared to a 31-bit mask, 28 bits (268M states) is still
// extremely large and sufficient to avoid collisions in practice for typical
// metric sets.
// LINT.IfChange(token_mask_macro)
#define _PW_METRIC_TOKEN_MASK 0x0fffffff
// LINT.ThenChange(//pw_metric/py/pw_metric/constants.py:metric_masks)

/// Common base class for an individual metric stored in intrusive lists. Holds
/// the token and type discriminator.
///
/// More complicated compound metrics can be built on the primitive typed
/// metrics.
///
/// Size: 8 bytes / 64 bits - next, name_and_type.
class UntypedMetric : public MetricList::Item {
 public:
  Token name() const { return name_and_type_ & kTokenMask; }

  // Disallow copy and assign.
  UntypedMetric(UntypedMetric const&) = delete;
  void operator=(const UntypedMetric&) = delete;

  // LINT.IfChange(metric_types)
  enum Type : uint32_t {
    kTypeUint32 = 0x00000000,
    kTypeFloat = 0x10000000,
#if PW_METRIC_CONFIG_ENABLE_64BIT
    kTypeUint64 = 0x20000000,
    kTypeInt64 = 0x30000000,
    kTypeDouble = 0x40000000,
#endif  // PW_METRIC_CONFIG_ENABLE_64BIT
    kTypeInt32 = 0x50000000,
    kTypeBool = 0x60000000,
    kTypeToken = 0x70000000,
  };
  // LINT.ThenChange(//pw_metric/py/pw_metric/constants.py:metric_types)

  Type type() const { return static_cast<Type>(name_and_type_ & kTypeMask); }

  bool is_float() const { return type() == kTypeFloat; }
  bool is_uint32() const { return type() == kTypeUint32; }
#if PW_METRIC_CONFIG_ENABLE_64BIT
  bool is_uint64() const { return type() == kTypeUint64; }
  bool is_int64() const { return type() == kTypeInt64; }
  bool is_double() const { return type() == kTypeDouble; }
#endif  // PW_METRIC_CONFIG_ENABLE_64BIT
  bool is_bool() const { return type() == kTypeBool; }
  bool is_int32() const { return type() == kTypeInt32; }
  bool is_token() const { return type() == kTypeToken; }

  /// Backward compatibility alias.
  [[deprecated("Use is_uint32() instead")]]
  bool is_int() const {
    return is_uint32();
  }

  /// Dump a metric or metrics to logs. Level determines the indentation
  /// `indent_level` up to a maximum of 4. Example output:
  ///
  /// @code{.none}
  /// "$FCM4qQ==": 0
  /// @endcode
  ///
  /// Note the base64-encoded token name. Detokenization tools are necessary to
  /// convert this to human-readable form.
  void Dump(int indent_level, bool last) const;
  static void Dump(const MetricList& metrics, int indent_level = 0);

  /// Backward compatibility helpers.
  [[deprecated(
      "as_float() is deprecated. Use value() on TypedMetric<float> or "
      "as_double() instead.")]]
  float as_float() const;

  [[deprecated(
      "as_int() is deprecated. Use value() on TypedMetric<uint32_t> or "
      "as_int32() instead.")]]
  uint32_t as_int() const;

 protected:
  constexpr UntypedMetric(Token name, Type type)
      : name_and_type_((name & kTokenMask) | type) {}

  UntypedMetric(Token name, Type type, MetricList& metrics);

  ~UntypedMetric();

  Token name_and_type_;

  // LINT.IfChange(metric_masks)
  static constexpr uint32_t kTokenMask = _PW_METRIC_TOKEN_MASK;
  static constexpr uint32_t kTypeMask = 0xf0000000;
  static_assert((kTokenMask & kTypeMask) == 0,
                "Token mask and Type mask must not overlap.");
  // LINT.ThenChange(//pw_metric/py/pw_metric/constants.py:metric_masks)

  friend class ResumableMetricWalker;
  friend class MetricWalker;
};

/// Backward compatibility alias.
using Metric [[deprecated(
    "pw::metric::Metric is deprecated. Use pw::metric::TypedMetric<uint32_t> "
    "or pw::metric::TypedMetric<T> directly.")]] = UntypedMetric;

/// Type-safe wrapper for metrics. Only the operations valid for the type `T`
/// are exposed at compile-time.
template <typename T>
class TypedMetric;

/// A metric for `float` values.
///
/// @note `TypedMetric<float>` does not offer `Increment()` or `Decrement()`
/// operations, as floating-point addition is prone to precision loss when
/// accumulating small values.
template <>
class TypedMetric<float> : public UntypedMetric {
 public:
  constexpr TypedMetric(Token name, float value)
      : UntypedMetric(name, kTypeFloat), value_(value) {}
  TypedMetric(Token name, float value, MetricList& metrics)
      : UntypedMetric(name, kTypeFloat, metrics), value_(value) {}

  ~TypedMetric() = default;

  /// Atomically set the metric to the given value.
  void Set(float value) { value_.store(value, std::memory_order_relaxed); }

  /// Return the current value of the metric.
  float value() const { return value_.load(std::memory_order_relaxed); }

 private:
  std::atomic<float> value_;
};

/// A metric for `uint32_t` values. Offers `Set()`, `Increment()`, and
/// `Decrement()`.
template <>
class TypedMetric<uint32_t> : public UntypedMetric {
 public:
  constexpr TypedMetric(Token name, uint32_t value)
      : UntypedMetric(name, kTypeUint32), value_(value) {}
  TypedMetric(Token name, uint32_t value, MetricList& metrics)
      : UntypedMetric(name, kTypeUint32, metrics), value_(value) {}

  ~TypedMetric() = default;

  /// Atomically increment the metric by the given amount (saturating to max).
  void Increment(uint32_t amount = 1u);

  /// Atomically decrement the metric by the given amount (saturating to 0).
  void Decrement(uint32_t amount = 1u);

  /// Atomically set the metric to the given value.
  void Set(uint32_t value) { value_.store(value, std::memory_order_relaxed); }

  /// Return the current value of the metric.
  uint32_t value() const { return value_.load(std::memory_order_relaxed); }

 private:
  std::atomic<uint32_t> value_;
};

/// A wrapper type representing a 32-bit tokenized string value.
///
/// `TokenValue` is used to specialize `TypedMetric<TokenValue>`, allowing
/// a metric to store a tokenized string hash (such as a string tokenized
/// with `PW_TOKENIZE_STRING`) rather than a raw numerical `uint32_t` integer.
struct TokenValue {
  uint32_t value;

  constexpr explicit TokenValue(uint32_t val) : value(val) {}
  constexpr operator uint32_t() const { return value; }
};

/// A metric for `bool` values.
template <>
class TypedMetric<bool> : public UntypedMetric {
 public:
  constexpr TypedMetric(Token name, bool value)
      : UntypedMetric(name, kTypeBool), value_(value) {}
  TypedMetric(Token name, bool value, MetricList& metrics)
      : UntypedMetric(name, kTypeBool, metrics), value_(value) {}

  ~TypedMetric() = default;

  /// Atomically set the metric to the given value.
  void Set(bool value) { value_.store(value, std::memory_order_relaxed); }

  /// Return the current value of the metric.
  bool value() const { return value_.load(std::memory_order_relaxed); }

 private:
  std::atomic<bool> value_;
};

/// A metric for `int32_t` values.
template <>
class TypedMetric<int32_t> : public UntypedMetric {
 public:
  constexpr TypedMetric(Token name, int32_t value)
      : UntypedMetric(name, kTypeInt32), value_(value) {}
  TypedMetric(Token name, int32_t value, MetricList& metrics)
      : UntypedMetric(name, kTypeInt32, metrics), value_(value) {}

  ~TypedMetric() = default;

  /// Atomically increment the metric by the given amount (saturating to max).
  void Increment(int32_t amount = 1);

  /// Atomically decrement the metric by the given amount (saturating to min).
  void Decrement(int32_t amount = 1);

  /// Atomically set the metric to the given value.
  void Set(int32_t value) { value_.store(value, std::memory_order_relaxed); }

  /// Return the current value of the metric.
  int32_t value() const { return value_.load(std::memory_order_relaxed); }

 private:
  std::atomic<int32_t> value_;
};

/// A metric for `TokenValue` values.
template <>
class TypedMetric<TokenValue> : public UntypedMetric {
 public:
  constexpr TypedMetric(Token name, TokenValue value)
      : UntypedMetric(name, kTypeToken), value_(value.value) {}
  TypedMetric(Token name, TokenValue value, MetricList& metrics)
      : UntypedMetric(name, kTypeToken, metrics), value_(value.value) {}

  ~TypedMetric() = default;

  /// Atomically set the metric to the given token value.
  void Set(TokenValue value) {
    value_.store(value.value, std::memory_order_relaxed);
  }

  /// Return the current token value of the metric.
  TokenValue value() const {
    return TokenValue(value_.load(std::memory_order_relaxed));
  }

 private:
  std::atomic<uint32_t> value_;
};

/// A metric tree consisting of children groups and leaf metrics.
///
/// Size: 16 bytes / 128 bits on 32-bit platforms - next, name, metrics,
/// children.
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

  /// Recursively dump a metrics group or groups to `pw_log`. Level determines
  /// the indentation `indent_level` up to a maximum of 4. Produces output like:
  ///
  /// @code{.none}
  /// "$6doqFw==": {
  ///   "$05OCZw==": {
  ///     "$VpPfzg==": 1,
  ///     "$LGPMBQ==": 1.000000,
  ///     "$+iJvUg==": 5
  ///   },
  ///   "$9hPNxw==": 65,
  ///   "$oK7HmA==": 13,
  ///   "$FCM4qQ==": 0
  /// }
  /// @endcode
  ///
  /// Note the metric names are tokenized with base64. Decoding requires using
  /// the Pigweed detokenizer. With a detokenizing-enabled logger, you could get
  /// something like:
  ///
  /// @code{.none}
  /// "i2c_1": {
  ///   "gyro": {
  ///     "num_sampleses": 1,
  ///     "init_time_us": 1.000000,
  ///     "initialized": 5,
  ///   }
  ///   "bus_errors": 65,
  ///   "transactions": 13,
  ///   "bytes_sent": 0,
  /// }
  /// @endcode
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

/// Declare a metric, optionally adding it to a group.
///
/// Overloads:
/// - `PW_METRIC(variable_name, metric_name, value)`
/// - `PW_METRIC(group, variable_name, metric_name, value)`
///
/// - `variable_name` - An identifier name for the created variable or member.
///   For example: `i2c_transactions` might be used as a local or global metric;
///   inside a class, could be named according to members (`i2c_transactions_`
///   for Google's C++ style).
/// - `metric_name` - The string name for the metric. This will be tokenized.
///   There are no restrictions on the contents of the name; however, consider
///   restricting these to be valid C++ identifiers to ease integration with
///   other systems.
/// - `value` - The initial value for the metric. Must be either a floating
///   point value (e.g. `3.2f`) or unsigned int (e.g. `21u`).
/// - `group` - A `pw::metric::Group` instance. If provided, the metric is added
///   to the given group.
///
/// The macro declares a variable or member named `variable_name` with type
/// `pw::metric::TypedMetric<T>`, and works in three contexts: global, local,
/// and member.
///
/// 1. At global scope:
///    @code{.cpp}
///    PW_METRIC(foo, "foo", 15.5f);
///
///    void MyFunc() { foo.Increment(); }
///    @endcode
///
/// 2. At local function or member function scope:
///    @code{.cpp}
///    void MyFunc() {
///      PW_METRIC(foo, "foo", 15.5f);
///      foo.Increment();
///      // foo goes out of scope here; be careful!
///    }
///    @endcode
///
/// 3. At member level inside a class or struct:
///    @code{.cpp}
///    struct MyStructy {
///      void DoSomething() { somethings_.Increment(); }
///      // Every instance of MyStructy will have a separate somethings counter.
///      PW_METRIC(somethings_, "somethings", 0u);
///    };
///    @endcode
///
/// You can also put a metric into a group with the macro. Metrics can belong to
/// strictly one group, otherwise an assertion will fail. Example:
///
/// @code{.cpp}
/// PW_METRIC_GROUP(my_group, "my_group_name_here");
/// PW_METRIC(my_group, foo_, "foo", 0.2f);
/// PW_METRIC(my_group, bar_, "bar", 44000u);
/// PW_METRIC(my_group, zap_, "zap", 3.14f);
/// @endcode
///
/// @note If you want a globally registered metric, see `pw_metric/global.h`; in
/// that context, metrics are globally registered without the need to centrally
/// register in a single place.
#define PW_METRIC(...) PW_DELEGATE_BY_ARG_COUNT(_PW_METRIC_, , __VA_ARGS__)

/// Declares a `PW_METRIC` variable with `static` storage. These can be used in
/// function scopes, but not in classes.
///
/// Overloads:
/// - `PW_METRIC_STATIC(variable_name, metric_name, value)`
/// - `PW_METRIC_STATIC(group, variable_name, metric_name, value)`
#define PW_METRIC_STATIC(...) \
  PW_DELEGATE_BY_ARG_COUNT(_PW_METRIC_STATIC_, static, __VA_ARGS__)

#define _PW_METRIC_FLOAT_OR_UINT32(literal)                       \
  std::conditional_t<std::is_floating_point_v<decltype(literal)>, \
                     float,                                       \
                     uint32_t>

/// Declare a metric with an explicit type, optionally adding it to a group.
///
/// Overloads:
/// - `PW_METRIC_TYPED(variable_name, metric_name, type, value)`
/// - `PW_METRIC_TYPED(group, variable_name, metric_name, type, value)`
///
/// This macro works identically to `PW_METRIC`, but requires explicitly
/// specifying the metric type (e.g. `float`, `uint32_t`, or `uint64_t`).
/// This is useful for declaring 64-bit metrics or when you want to make the
/// metric's type explicit.
///
/// Example:
/// @code{.cpp}
/// PW_METRIC_GROUP(my_group, "my_group");
/// PW_METRIC_TYPED(my_group, my_64bit_metric, "my_64bit_metric", uint64_t,
/// 0ULL);
/// @endcode
#define PW_METRIC_TYPED(...) \
  PW_DELEGATE_BY_ARG_COUNT(_PW_METRIC_TYPED_, , __VA_ARGS__)

/// Declares a `PW_METRIC_TYPED` variable with `static` storage. These can be
/// used in function scopes, but not in classes.
///
/// Overloads:
/// - `PW_METRIC_TYPED_STATIC(variable_name, metric_name, type, value)`
/// - `PW_METRIC_TYPED_STATIC(group, variable_name, metric_name, type, value)`
#define PW_METRIC_TYPED_STATIC(...) \
  PW_DELEGATE_BY_ARG_COUNT(_PW_METRIC_TYPED_STATIC_, static, __VA_ARGS__)

/// Declare a `pw::metric::Token` (`pw::tokenizer::Token`) for a metric with
/// name `metric_name`. This token matches the `.name()` of a metric with the
/// same name.
///
/// This is a wrapper around `PW_TOKENIZE_STRING_MASK` and carries the same
/// semantics.
///
/// This is unlikely to be used by most `pw_metric` consumers.
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

/// Declares a `pw::metric::Group` with name `group_name`; the name is
/// tokenized. Works similar to `PW_METRIC` and can be used in the same contexts
/// (global, local, and member). Optionally, the group can be added to a parent
/// group.
///
/// Overloads:
/// - `PW_METRIC_GROUP(variable_name, group_name)`
/// - `PW_METRIC_GROUP(parent_group, variable_name, group_name)`
///
/// Example:
/// @code{.cpp}
/// class MySubsystem {
///  public:
///   void DoSomething() {
///     attempts_.Increment();
///     if (ActionSucceeds()) {
///       successes_.Increment();
///     }
///   }
///   const Group& metrics() const { return metrics_; }
///   Group& metrics() { return metrics_; }
///
///  private:
///   PW_METRIC_GROUP(metrics_, "my_subsystem");
///   PW_METRIC(metrics_, attempts_, "attempts", 0u);
///   PW_METRIC(metrics_, successes_, "successes", 0u);
/// };
/// @endcode
#define PW_METRIC_GROUP(...) \
  PW_DELEGATE_BY_ARG_COUNT(_PW_METRIC_GROUP_, , __VA_ARGS__)

/// Declares a `PW_METRIC_GROUP` variable with `static` storage. These can be
/// used in function scopes, but not in classes.
///
/// Overloads:
/// - `PW_METRIC_GROUP_STATIC(variable_name, group_name)`
/// - `PW_METRIC_GROUP_STATIC(parent_group, variable_name, group_name)`
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

/// Similar to `PW_TOKENIZE_STRING_EXPR`, converts a string literal to a
/// `uint32_t` token within an expression. Requires C++.
///
/// @code{.cpp}
/// // Tokenizes a string literal in the metric domain within an expression.
/// DoSomethingWithToken(PW_METRIC_TOKEN_EXPR("Succeed"));
/// @endcode
#define PW_METRIC_TOKEN_EXPR(metric_name) \
  PW_TOKENIZE_STRING_MASK_EXPR("metrics", _PW_METRIC_TOKEN_MASK, metric_name)

/// @endmodule

}  // namespace pw::metric

#if PW_METRIC_CONFIG_ENABLE_64BIT

namespace pw::metric {

/// @module{pw_metric}

/// A metric for `uint64_t` values.
template <>
class TypedMetric<uint64_t> : public UntypedMetric {
 public:
  constexpr TypedMetric(Token name, uint64_t value)
      : UntypedMetric(name, kTypeUint64), value_(value) {}
  TypedMetric(Token name, uint64_t value, MetricList& metrics)
      : UntypedMetric(name, kTypeUint64, metrics), value_(value) {}

  ~TypedMetric() = default;

  /// Atomically increment the metric by the given amount (saturating to max).
  void Increment(uint64_t amount = 1u);

  /// Atomically decrement the metric by the given amount (saturating to 0).
  void Decrement(uint64_t amount = 1u);

  /// Atomically set the metric to the given value.
  void Set(uint64_t value) { value_.store(value, std::memory_order_relaxed); }

  /// Return the current value of the metric.
  uint64_t value() const { return value_.load(std::memory_order_relaxed); }

 private:
  std::atomic<uint64_t> value_;
};

/// A metric for `int64_t` values.
template <>
class TypedMetric<int64_t> : public UntypedMetric {
 public:
  constexpr TypedMetric(Token name, int64_t value)
      : UntypedMetric(name, kTypeInt64), value_(value) {}
  TypedMetric(Token name, int64_t value, MetricList& metrics)
      : UntypedMetric(name, kTypeInt64, metrics), value_(value) {}

  ~TypedMetric() = default;

  /// Atomically increment the metric by the given amount (saturating to max).
  void Increment(int64_t amount = 1);

  /// Atomically decrement the metric by the given amount (saturating to min).
  void Decrement(int64_t amount = 1);

  /// Atomically set the metric to the given value.
  void Set(int64_t value) { value_.store(value, std::memory_order_relaxed); }

  /// Return the current value of the metric.
  int64_t value() const { return value_.load(std::memory_order_relaxed); }

 private:
  std::atomic<int64_t> value_;
};

/// A metric for `double` values.
///
/// @note `TypedMetric<double>` does not offer `Increment()` or `Decrement()`
/// operations for the same reason as `TypedMetric<float>`.
template <>
class TypedMetric<double> : public UntypedMetric {
 public:
  constexpr TypedMetric(Token name, double value)
      : UntypedMetric(name, kTypeDouble), value_(value) {}
  TypedMetric(Token name, double value, MetricList& metrics)
      : UntypedMetric(name, kTypeDouble, metrics), value_(value) {}

  ~TypedMetric() = default;

  /// Atomically set the metric to the given value.
  void Set(double value) { value_.store(value, std::memory_order_relaxed); }

  /// Return the current value of the metric.
  double value() const { return value_.load(std::memory_order_relaxed); }

 private:
  std::atomic<double> value_;
};

/// @endmodule

}  // namespace pw::metric

#endif  // PW_METRIC_CONFIG_ENABLE_64BIT
