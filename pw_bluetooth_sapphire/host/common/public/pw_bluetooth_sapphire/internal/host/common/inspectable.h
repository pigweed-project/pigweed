// Copyright 2023 The Pigweed Authors
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
#include <lib/fit/function.h>
#include <pw_assert/check.h>

#include <iterator>
#include <string>
#include <type_traits>

#include "pw_bluetooth_sapphire/internal/host/common/inspect.h"
#include "pw_bluetooth_sapphire/internal/host/common/macros.h"

namespace bt {

namespace internal {

template <typename T, typename = void>
struct is_equality_comparable : std::false_type {};

template <typename T>
struct is_equality_comparable<
    T,
    std::void_t<decltype(std::declval<const T&>() == std::declval<const T&>())>>
    : std::is_convertible<decltype(std::declval<const T&>() ==
                                   std::declval<const T&>()),
                          bool> {};

template <typename T>
inline constexpr bool is_equality_comparable_v =
    is_equality_comparable<T>::value;

}  // namespace internal

// InspectableGuard is returned by |Inspectable::Mutable()|.
// It will update the corresponding Inspect property when it is destroyed.
// Therefore, the lifetime of InspectableGuard should usually be that of a
// temporary, or be scoped for a single update.
//
// InspectableGuard's primary use cases are calling non-const methods on objects
// and assigning member variable values in structs.
//
// Example:
//   StringInspectable<hci_spec::LMPFeatureSet> lmp_features;
//   lmp_features.Mutable()->SetPage(page, features);
template <typename ValueT>
class InspectableGuard {
 public:
  InspectableGuard(ValueT& value, fit::closure update_cb)
      : value_(value), update_cb_(std::move(update_cb)) {}
  ~InspectableGuard() { update_cb_(); }

  ValueT& operator*() { return value_; }

  ValueT* operator->() { return &value_; }

 private:
  ValueT& value_;
  fit::closure update_cb_;

  BT_DISALLOW_COPY_ASSIGN_AND_MOVE(InspectableGuard);
};

// Inspectable is a utility class for keeping inspected values in sync with
// their corresponding Inspect property.
//
// PropertyT must be an Inspect property type such that
// PropertyT::Set(PropertyInnerT) is valid. PropertyInnerT corresponds to the
// type contained in PropertyT (e.g. string is contained by
// inspect::StringProperty).
//
// Example:
//   inspect::Inspector inspector;
//   auto& root = inspector.GetRoot();
//   Inspectable inspectable(std::string("A"),
//   root.CreateString("property_name", "foo"));
//
//   // Hierarchy: { root: property_name: "A" }
//
//   inspectable.Set("B");
//
//   // Hierarchy: { root: property_name: "B" }
template <typename ValueT, typename PropertyT, typename PropertyInnerT = ValueT>
class Inspectable {
 public:
  // When the desired property type DOES NOT match ValueT, a conversion function
  // |convert| is necessary. This is often the case when using optionals or when
  // converting a number/enum into a string is desired. Example:
  //   auto convert = [](std::optional<hci_spec::HCIVersion> version) ->
  //   std::string {
  //     return version ? HCIVersionToString(*version) : "null";
  //   };
  //   Inspectable<std::optional<HCIVersion>> hci_version(
  //     HCIVersion::k5_0,
  //     inspect_node.CreateString("hci", ""),
  //     convert);
  using ConvertFunction = fit::function<PropertyInnerT(const ValueT&)>;
  Inspectable(ValueT initial_value,
              PropertyT property,
              ConvertFunction convert = Inspectable::DefaultConvert)
      : value_(std::move(initial_value)),
        property_(std::move(property)),
        convert_(std::move(convert)) {
    // Update property immediately to ensure consistency between property and
    // initial value.
    UpdateProperty();
  }

  // Construct with null property (updates will be no-ops).
  explicit Inspectable(ValueT initial_value,
                       ConvertFunction convert = &Inspectable::DefaultConvert)
      : value_(std::move(initial_value)), convert_(std::move(convert)) {}

  // Construct with null property and with default value for ValueT.
  explicit Inspectable(ConvertFunction convert = &Inspectable::DefaultConvert)
      : Inspectable(ValueT(), std::move(convert)) {
    static_assert(std::is_default_constructible<ValueT>(),
                  "ValueT is not default constructable");
  }

  Inspectable(Inspectable&&) = default;
  Inspectable& operator=(Inspectable&&) = default;
  virtual ~Inspectable() = default;

  const ValueT& value() const { return value_; }

  const ValueT& operator*() const { return value_; }

  const ValueT* operator->() const { return &value_; }

  // Update value and property, making a copy of `value`.
  const ValueT& Set(const ValueT& value) {
    if constexpr (internal::is_equality_comparable_v<ValueT>) {
      if (value_ == value) {
        return value_;
      }
    }
    value_ = value;
    UpdateProperty();
    return value_;
  }

  // Update value and property, moving `value`.
  const ValueT& Set(ValueT&& value) {
    if constexpr (internal::is_equality_comparable_v<ValueT>) {
      if (value_ == value) {
        return value_;
      }
    }
    value_ = std::move(value);
    UpdateProperty();
    return value_;
  }

  void SetProperty(PropertyT property) {
    property_ = std::move(property);
    UpdateProperty();
  }

  virtual void AttachInspect(inspect::Node& /*node*/, std::string /*name*/) {
    PW_CHECK(false, "AttachInspect not implemented for PropertyT");
  }

  // Returns a InspectableGuard wrapper around the contained value that allows
  // for non-const methods to be called. The returned value should only be used
  // as a temporary.
  InspectableGuard<ValueT> Mutable() {
    return InspectableGuard<ValueT>(
        value_, fit::bind_member<&Inspectable::UpdateProperty>(this));
  }

  static PropertyInnerT DefaultConvert(const ValueT& value) { return value; }

 private:
  void UpdateProperty() { property_.Set(convert_(value_)); }

  ValueT value_;
  PropertyT property_;
  ConvertFunction convert_;

  static_assert(!std::is_pointer_v<ValueT>, "Pointer passed to Inspectable");
  static_assert(!std::is_reference_v<ValueT>,
                "Reference passed to Inspectable");

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(Inspectable);
};

// Convenience Inspectable types:

#define CREATE_INSPECTABLE_TYPE(property_t, inner_t)                         \
  template <typename ValueT>                                                 \
  class property_t##Inspectable                                              \
      : public Inspectable<ValueT, inspect::property_t##Property, inner_t> { \
   public:                                                                   \
    using Inspectable<ValueT, inspect::property_t##Property, inner_t>::      \
        Inspectable;                                                         \
    void AttachInspect(inspect::Node& node, std::string name) override {     \
      this->SetProperty(node.Create##property_t(name, inner_t()));           \
    }                                                                        \
  };                                                                         \
  template <typename ValueT>                                                 \
  property_t##Inspectable(ValueT, ...)->property_t##Inspectable<ValueT>

CREATE_INSPECTABLE_TYPE(Int, int64_t);
CREATE_INSPECTABLE_TYPE(Uint, uint64_t);
CREATE_INSPECTABLE_TYPE(Bool, bool);
CREATE_INSPECTABLE_TYPE(String, std::string);

// A common practice in the Bluetooth stack is to define ToString() for classes.
// MakeToStringInspectConvertFunction allows these classes to be used with
// StringInspectable. Example: class Foo {
//   public:
//     std::string ToString() { ... }
// };
//
// StringInspectable foo(Foo(), inspect_node.CreateString("foo", ""),
//                       MakeToStringInspectConvertFunction());
inline auto MakeToStringInspectConvertFunction() {
  return [](auto value) { return value.ToString(); };
}

// Similar to ToStringInspectable, but for containers of types that implement
// ToString(). The resulting string property will be formatted using the
// ContainerOfToStringOptions provided. Example: std::vector<Foo> values(2);
// StringInspectable foo(std::move(values), inspect_node.CreateString("foo",
// ""),
//                       MakeContainerOfToStringInspectConvertFunction());
//
// This does not generate an node hierarchy based on the container contents and
// is more appropriate for sequential containers at leaves of the inspect tree.
// More complex data structures, especially associative ones, should export full
// inspect trees.
struct ContainerOfToStringOptions {
  const char* prologue = "{ ";
  const char* delimiter = ", ";
  const char* epilogue = " }";
};
inline auto MakeContainerOfToStringConvertFunction(
    ContainerOfToStringOptions options = {}) {
  return [options](auto value) {
    std::string out(options.prologue);
    for (auto iter = std::begin(value); iter != std::end(value);
         std::advance(iter, 1)) {
      out += iter->ToString();
      if (std::next(iter) == std::end(value)) {
        continue;
      }
      out += options.delimiter;
    }
    out += options.epilogue;
    return out;
  };
}

}  // namespace bt
