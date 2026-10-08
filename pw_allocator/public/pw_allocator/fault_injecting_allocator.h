// Copyright 2025 The Pigweed Authors
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

#include "pw_allocator/forwarding_allocator.h"

namespace pw::allocator::test {

/// @submodule{pw_allocator,impl_test}

/// Applies the given macro to each pair of names.
#define PW_ALLOCATOR_FIA_FOREACH(fn)               \
  fn(allocate, Allocate);                          \
  fn(resize, Resize);                              \
  fn(reallocate, Reallocate);                      \
  fn(get_capacity, GetCapacity);                   \
  fn(get_allocated, GetAllocated);                 \
  fn(measure_fragmentation, MeasureFragmentation); \
  fn(get_requested_layout, GetRequestedLayout);    \
  fn(get_usable_layout, GetUsableLayout);          \
  fn(get_allocated_layout, GetAllocatedLayout);    \
  fn(recognize, Recognizes)

#define PW_ALLOCATOR_ABSORB_SEMICOLON() static_assert(true)

/// For a name pair like `(foo_bar, FooBar)`, creates an accessor like:
/// `[[nodiscard]] constexpr bool can_foo_bar() const { return can_foo_bar_; }`
#define PW_ALLOCATOR_FIA_ACCESSOR(snake, Camel)                               \
  [[nodiscard]] constexpr bool can_##snake() const { return can_##snake##_; } \
  PW_ALLOCATOR_ABSORB_SEMICOLON()

/// For a name pair like `(foo_bar, FooBar)` and a boolean value, creates
/// statement like `can_foo_bar_ = true;`
#define PW_ALLOCATOR_FIA_SET_TRUE(snake, Camel) can_##snake##_ = true

/// For a name pair like `(foo_bar, FooBar)` and a boolean value, creates
/// statement like `can_foo_bar_ = false`
#define PW_ALLOCATOR_FIA_SET_FALSE(snake, Camel) can_##snake##_ = false

/// For a name pair like `(foo_bar, FooBar)`, creates a method like
/// `void EnableFooBar() { can_foo_bar_ = true; }`
#define PW_ALLOCATOR_FIA_ENABLE(snake, Camel)                       \
  void Enable##Camel() { PW_ALLOCATOR_FIA_SET_TRUE(snake, Camel); } \
  PW_ALLOCATOR_ABSORB_SEMICOLON()

/// For a name pair like `(foo_bar, FooBar)`, creates a method like
/// `void DisableFooBar() { can_foo_bar_ = false; }`
#define PW_ALLOCATOR_FIA_DISABLE(snake, Camel)                        \
  void Disable##Camel() { PW_ALLOCATOR_FIA_SET_FALSE(snake, Camel); } \
  PW_ALLOCATOR_ABSORB_SEMICOLON()

/// For a name pair like `(foo_bar, FooBar)`, creates a field like
/// `bool can_foo_bar_ = true`
#define PW_ALLOCATOR_FIA_FIELD(snake, Camel) \
  bool PW_ALLOCATOR_FIA_SET_TRUE(snake, Camel)

/// Forwarding allocator for injecting failures. Forwards calls to a real
/// allocator implementation, or artificially fails if requested.
///
/// @note `Allocate`, `Resize` and `Reallocate` are all fallible methods that
/// may fail at any time, and can therefore be enabled or disabled freely. All
/// others should only be configured during initial configuration. Some
/// allocators check whether these optional methods are implemented during
/// construction and assume they are callable thereafter. Disabling one of these
/// methods later may lead to undefined behavior.
///
/// @warning FaultInjectingAllocator is NOT thread safe, even if used with
/// `SynchronizedAllocator`.
class FaultInjectingAllocator : public ForwardingAllocator {
 private:
  using Base = ForwardingAllocator;

 public:
  constexpr FaultInjectingAllocator(const Capabilities& capabilities) noexcept
      : Base(capabilities) {}

  constexpr explicit FaultInjectingAllocator(Allocator& allocator) noexcept
      : Base(allocator) {}

  using Base::allocator;
  using Base::Init;

  // Create accessors that return the state of each flag.
  PW_ALLOCATOR_FIA_FOREACH(PW_ALLOCATOR_FIA_ACCESSOR);

  // Create methods to set each flag to forward the corresponding call to the
  // allocator.
  PW_ALLOCATOR_FIA_FOREACH(PW_ALLOCATOR_FIA_ENABLE);

  // Create methods to set each flag to return a value indicating failure from
  // the corresponding call.
  PW_ALLOCATOR_FIA_FOREACH(PW_ALLOCATOR_FIA_DISABLE);

  /// Forward all calls to the allocator.
  void EnableAll() { PW_ALLOCATOR_FIA_FOREACH(PW_ALLOCATOR_FIA_SET_TRUE); }

  /// Return errors for all calls without forwarding to the allocator.
  void DisableAll() { PW_ALLOCATOR_FIA_FOREACH(PW_ALLOCATOR_FIA_SET_FALSE); }

 protected:
  /// @copydoc Allocator::Allocate
  void* DoAllocate(Layout layout) override {
    return can_allocate_ ? Base::DoAllocate(layout) : nullptr;
  }

  /// @copydoc Deallocator::Deallocate
  void DoDeallocate(void* ptr) override { Base::DoDeallocate(ptr); }

  /// @copydoc Allocator::Resize
  bool DoResize(void* ptr, size_t new_size) override {
    return can_resize_ && Base::DoResize(ptr, new_size);
  }

  /// @copydoc Allocator::Reallocate
  void* DoReallocate(void* ptr, Layout new_layout) override {
    return can_reallocate_ ? Base::DoReallocate(ptr, new_layout) : nullptr;
  }

  /// @copydoc Allocator::GetAllocated
  size_t DoGetAllocated() const override {
    return can_get_allocated_ ? Base::DoGetAllocated() : size_t(-1);
  }

  /// @copydoc Allocator::MeasureFragmentation
  std::optional<Fragmentation> DoMeasureFragmentation() const override {
    return can_measure_fragmentation_ ? Base::DoMeasureFragmentation()
                                      : std::nullopt;
  }

  /// @copydoc Deallocator::GetInfo
  Result<Layout> DoGetInfo(InfoType info_type, const void* ptr) const override {
    bool can_get_info = false;
    switch (info_type) {
      case InfoType::kRequestedLayoutOf:
        can_get_info = can_get_requested_layout_;
        break;
      case InfoType::kUsableLayoutOf:
        can_get_info = can_get_usable_layout_;
        break;
      case InfoType::kAllocatedLayoutOf:
        can_get_info = can_get_allocated_layout_;
        break;
      case InfoType::kCapacity:
        can_get_info = can_get_capacity_;
        break;
      case InfoType::kRecognizes:
        can_get_info = can_recognize_;
        break;
    }
    return can_get_info ? Base::DoGetInfo(info_type, ptr)
                        : Status::Unimplemented();
  }

 private:
  // Flags for whether to allow calls to pass through.
  PW_ALLOCATOR_FIA_FOREACH(PW_ALLOCATOR_FIA_FIELD);
};

#undef PW_ALLOCATOR_FIA_FOREACH
#undef PW_ALLOCATOR_ABSORB_SEMICOLON
#undef PW_ALLOCATOR_FIA_ACCESSOR
#undef PW_ALLOCATOR_FIA_SET_TRUE
#undef PW_ALLOCATOR_FIA_SET_FALSE
#undef PW_ALLOCATOR_FIA_ENABLE
#undef PW_ALLOCATOR_FIA_DISABLE
#undef PW_ALLOCATOR_FIA_FIELD

/// @endsubmodule

}  // namespace pw::allocator::test
