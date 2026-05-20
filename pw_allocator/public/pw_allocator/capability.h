// Copyright 2024 The Pigweed Authors
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

#include <cstdint>

namespace pw::allocator {

/// @submodule{pw_allocator,impl}

/// Hints about optional methods implemented or optional behaviors requested
/// by an allocator of a derived type.
///
/// Implementations can provide hints by passing capabilities to the base
/// class constructor. These capabilities can be constructed by combining
/// ``Capability``s using logical operations.
enum Capability : uint32_t {
  // clang-format off
  kCanAllocateArbitraryLayout   = 1 << 0,
  kImplementsGetRequestedLayout = 1 << 1,
  kImplementsGetUsableLayout    = 1 << 2,
  kImplementsGetAllocatedLayout = 1 << 3,
  kImplementsGetCapacity        = 1 << 4,
  kImplementsRecognizes         = 1 << 5,
  kSkipsDestroy                 = 1 << 6,
  // clang-format on
};

/// A collection of ``Capability``s.
///
/// Concrete allocators should declare a constant set of capabilities, and pass
/// it to the ``Allocator`` constructor.
///
/// @code{.cpp}
/// class MyConcreteAllocator : public Allocator {
///  public:
///   static constexpr Capabilities kCapabilities = kCapability1 | kCapability2;
///
///   MyConcreteAllocator() : Allocator(kCapabilities) {}
/// };
/// @endcode
///
/// Forwarding allocators should pass the underlying allocator's capabilities,
/// potentially with modifications:
///
/// @code{.cpp}
/// class MyForwardingAllocator : public Allocator {
///  public:
///   MyForwardingAllocator(Allocator& allocator)
///     : Allocator(allocator.capabilities() | kCapability3),
///       allocator_(allocator) {}
/// };
/// @endcode
class Capabilities {
 public:
  constexpr Capabilities() : capabilities_(0) {}
  constexpr Capabilities(uint32_t capabilities) : capabilities_(capabilities) {}

  constexpr bool has(Capability capability) const {
    return (capabilities_ & capability) == capability;
  }

  constexpr uint32_t get() const { return capabilities_; }

 private:
  const uint32_t capabilities_;
};

inline constexpr bool operator==(const Capabilities& lhs,
                                 const Capabilities& rhs) {
  return lhs.get() == rhs.get();
}

inline constexpr bool operator!=(const Capabilities& lhs,
                                 const Capabilities& rhs) {
  return lhs.get() != rhs.get();
}

inline constexpr Capabilities operator|(const Capabilities& lhs,
                                        const Capabilities& rhs) {
  return Capabilities(lhs.get() | rhs.get());
}

inline constexpr Capabilities operator&(const Capabilities& lhs,
                                        const Capabilities& rhs) {
  return Capabilities(lhs.get() & rhs.get());
}

inline constexpr Capabilities operator^(const Capabilities& lhs,
                                        const Capabilities& rhs) {
  return Capabilities(lhs.get() ^ rhs.get());
}

/// @endsubmodule

}  // namespace pw::allocator
