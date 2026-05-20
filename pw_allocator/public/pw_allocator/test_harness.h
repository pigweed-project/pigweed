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

#include <cstddef>
#include <optional>
#include <variant>

#include "pw_allocator/allocator.h"
#include "pw_containers/intrusive_list.h"
#include "pw_containers/vector.h"
#include "pw_random/xor_shift.h"

namespace pw::allocator::test {

/// @submodule{pw_allocator,impl_test}

/// Represents a request to allocate some memory.
struct AllocationRequest {
  size_t size = 0;
  size_t alignment = 1;
};

/// Represents a request to free some allocated memory.
struct DeallocationRequest {
  size_t index = 0;
};

/// Represents a request to reallocate allocated memory with a new size.
struct ReallocationRequest {
  size_t index = 0;
  size_t new_size = 0;
};

using Request =
    std::variant<AllocationRequest, DeallocationRequest, ReallocationRequest>;

/// Helper function to produce a valid alignment for a given `size` from an
/// arbitrary left shift amount.
size_t AlignmentFromLShift(size_t lshift, size_t size);

/// Associates an `Allocator` with a list to store allocated pointers.
///
/// This class facilitates performing allocations from generated
/// `Request`s, enabling the creation of performance, stress, and fuzz
/// tests for various allocators.
///
/// This class does NOT implement `TestHarness::Init`. It must be extended
/// further with a method that provides an initialized allocator.
///
/// For example, one can create a fuzzer for `MyAllocator` that verifies it
/// never crashes by adding the following class, function, and macro:
/// @code{.cpp}
///   void MyAllocatorNeverCrashes(const Vector<Request>& requests) {
///     static MyAllocator allocator;
///     static TestHarness fuzzer(allocator);
///     fuzzer.HandleRequests(requests);
///   }
///
///   FUZZ_TEST(MyAllocator, MyAllocatorNeverCrashes)
///     .WithDomains(DefaultArbitraryRequests());
/// @endcode
class TestHarness {
 public:
  /// Associates a pointer to memory with the `Layout` used to allocate it.
  struct Allocation : public IntrusiveList<Allocation>::Item {
    Layout layout;

    explicit Allocation(Layout layout_) : layout(layout_) {}
  };

  TestHarness() = default;
  explicit TestHarness(pw::Allocator& allocator) : allocator_(&allocator) {}
  virtual ~TestHarness() = default;

  size_t num_allocations() const { return num_allocations_; }
  size_t allocated() const { return allocated_; }

  void set_allocator(pw::Allocator* allocator) { allocator_ = allocator; }
  void set_prng_seed(uint64_t seed) { prng_ = random::XorShiftStarRng64(seed); }
  void set_available(size_t available) { available_ = available; }

  /// Generates and handles a sequence of allocation requests.
  ///
  /// This method will use the given PRNG to generate `num_requests` allocation
  /// requests and pass each in turn to `HandleRequest`. It will call `Reset`
  /// before returning.
  void GenerateRequests(size_t max_size, size_t num_requests);

  /// Generate and handle an allocation requests.
  ///
  /// This method will use the given PRNG to generate an allocation request
  /// and pass it to `HandleRequest`. Callers *MUST* call `Reset` when no more
  /// requests remain to be generated.
  void GenerateRequest(size_t max_size);

  /// Handles a sequence of allocation requests.
  ///
  /// This method is useful for processing externally generated requests, e.g.
  /// from FuzzTest. It will call `Reset` before returning.
  void HandleRequests(const Vector<Request>& requests);

  /// Handles an allocator request.
  ///
  /// This method is stateful, and modifies the vector of allocated pointers.
  /// It will call `Init` if it has not yet been called.
  ///
  /// If the request is an allocation request:
  /// * If the vector of previous allocations is full, ignores the request.
  /// * Otherwise, allocates memory and stores the pointer in the vector.
  ///
  /// If the request is a deallocation request:
  /// * If the vector of previous allocations is empty, ignores the request.
  /// * Otherwise, removes a pointer from the vector and deallocates it.
  ///
  /// If the request is a reallocation request:
  /// * If the vector of previous allocations is empty, reallocates a `nullptr`.
  /// * Otherwise, removes a pointer from the vector and reallocates it.
  ///
  /// Returns whether the request was handled. This is different from whether
  /// the request succeeded, e.g. a DeallocationRequest cannot be handled when
  /// there are no current allocations and will return false. By contrast, an
  /// AllocationRequest may be handled, but fail due to insufficient memory, and
  /// will return true.
  bool HandleRequest(const Request& request);

  /// Deallocates any pointers stored in the vector of allocated pointers.
  void Reset();

 private:
  /// Generate requests. `set_prng_seed` must have been called first.
  AllocationRequest GenerateAllocationRequest(size_t max_size);
  DeallocationRequest GenerateDeallocationRequest();
  ReallocationRequest GenerateReallocationRequest(size_t max_size);
  size_t GenerateSize(size_t max_size);

  /// Derived classes may add callbacks that are invoked before and after each
  /// de/re/allocation to record additional data about the allocator.
  virtual void BeforeAllocate(const Layout&) {}
  virtual void AfterAllocate(const void*) {}
  virtual void BeforeReallocate(const Layout&) {}
  virtual void AfterReallocate(const void*) {}
  virtual void BeforeDeallocate(const void*) {}
  virtual void AfterDeallocate() {}

  /// Adds a pointer to the vector of allocated pointers.
  ///
  /// The `ptr` must not be null, and the vector of allocated pointers must not
  /// be full. To aid in detecting memory corruptions and in debugging, the
  /// pointed-at memory will be filled with as much of the following sequence as
  /// will fit:
  /// * The request number.random::RandomGenerator& prng
  /// * The request size.
  /// * The byte "0x5a", repeating.
  void AddAllocation(void* ptr, Layout layout);

  /// Removes and returns a previously allocated pointer.
  ///
  /// The vector of allocated pointers must not be empty.
  Allocation* RemoveAllocation(size_t index);

  /// An allocator used to manage memory.
  pw::Allocator* allocator_ = nullptr;

  /// A list of allocated pointers.
  IntrusiveList<Allocation> allocations_;

  /// The number of outstanding active allocation by this object.
  size_t num_allocations_ = 0;

  /// The total memory allocated.
  size_t allocated_ = 0;

  /// An optional amount of memory available. If set, this is used to adjust the
  /// likelihood of what requests are generated based on how much of the
  /// available memory has been used.
  std::optional<size_t> available_;

  /// Pseudorandom number generator.
  std::optional<random::XorShiftStarRng64> prng_;

  /// If an allocation fails, the next generated request is limited to half the
  /// previous request's size.
  std::optional<size_t> max_size_;
};

/// @endsubmodule

}  // namespace pw::allocator::test
