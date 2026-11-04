// Copyright 2026 The Pigweed Authors
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
#include <memory>
#include <type_traits>
#include <utility>

#include "pw_rpc/internal/call.h"
#include "pw_rpc/internal/config.h"

// DynamicCallPtr requires PW_RPC_DYNAMIC_ALLOCATION. Doxygen defines
// PW_EXCLUDE_FROM_DOXYGEN, so this is also enabled when generating docs. A
// PREDEFINED PW_RPC_DYNAMIC_ALLOCATION=1 in the Doxyfile would instead hide the
// documented default definition of PW_RPC_DYNAMIC_ALLOCATION in config.h.
#if PW_RPC_DYNAMIC_ALLOCATION || defined(PW_EXCLUDE_FROM_DOXYGEN)

#include PW_RPC_MAKE_UNIQUE_PTR_INCLUDE

namespace pw::rpc {

/// @module{pw_rpc}

/// Owns a heap-allocated RPC call object, such as a `RawServerReaderWriter` or
/// `PwpbClientReader`.
///
/// Destroying a call object waits for its running callbacks to finish, which
/// deadlocks if a callback destroys its own call, or if a thread destroys a
/// call while holding a lock that the call's running callback is waiting for.
/// Resetting or destroying a `DynamicCallPtr` closes the call immediately
/// instead. If one of the call's callbacks is running, the call object is
/// deleted when that callback finishes rather than waiting for it.
///
/// Closing the call works as when a call object is destroyed: server calls are
/// completed with an OK status and client calls are abandoned. Unlike
/// destroying a call object, a pending `on_error` from an earlier abort or
/// cancellation is discarded and not invoked.
///
/// A `DynamicCallPtr` is not thread safe. Synchronize access to a
/// `DynamicCallPtr` that is used from multiple threads. To avoid destroying
/// callbacks while holding a lock, move the `DynamicCallPtr` out of the
/// lock's scope before destroying it.
///
/// Requires `PW_RPC_DYNAMIC_ALLOCATION`.
///
/// @tparam CallType The RPC call object type.
/// @tparam UniquePtr The owning smart pointer for the call object. Defaults to
///   the type returned by `PW_RPC_MAKE_UNIQUE_PTR`. Other smart pointers, such
///   as a `std::unique_ptr` with a custom deleter, may be used if they are no
///   larger than that type.
template <typename CallType,
          typename UniquePtr = decltype(PW_RPC_MAKE_UNIQUE_PTR(
              CallType, std::declval<CallType>()))>
class DynamicCallPtr {
 public:
  static_assert(std::is_base_of_v<internal::Call, CallType>,
                "DynamicCallPtr may only be used with pw::rpc call types");
  static_assert(!std::is_pointer_v<UniquePtr>,
                "DynamicCallPtr requires an owning smart pointer");

  /// Creates an empty `DynamicCallPtr`.
  constexpr DynamicCallPtr() = default;
  constexpr DynamicCallPtr(std::nullptr_t) : ptr_(nullptr) {}

  /// Takes ownership of a heap-allocated call object.
  DynamicCallPtr(UniquePtr ptr) : ptr_(std::move(ptr)) {}

  DynamicCallPtr(const DynamicCallPtr&) = delete;
  DynamicCallPtr& operator=(const DynamicCallPtr&) = delete;

  DynamicCallPtr(DynamicCallPtr&& other) noexcept
      : ptr_(std::move(other.ptr_)) {}

  /// Closes and releases the currently owned call, if any, and takes ownership
  /// of `other`'s call.
  DynamicCallPtr& operator=(DynamicCallPtr&& other) noexcept {
    if (this != &other) {
      UniquePtr old_ptr = std::exchange(ptr_, std::move(other.ptr_));
      if (old_ptr != nullptr) {
        internal::Call::DeleteDynamicCall(std::move(old_ptr));
      }
    }
    return *this;
  }

  DynamicCallPtr& operator=(std::nullptr_t) noexcept {
    reset();
    return *this;
  }

  ~DynamicCallPtr() { reset(); }

  /// Closes and releases the owned call, if any. Server calls are completed
  /// with an OK status and client calls are abandoned, as when a call object is
  /// destroyed. A pending `on_error` from an earlier abort or cancellation is
  /// discarded and not invoked.
  ///
  /// If none of the call's callbacks are running, the call object and its
  /// callbacks are destroyed before this returns. Otherwise, they are destroyed
  /// when the last running callback finishes.
  void reset() noexcept {
    UniquePtr old_ptr = std::exchange(ptr_, nullptr);
    if (old_ptr != nullptr) {
      internal::Call::DeleteDynamicCall(std::move(old_ptr));
    }
  }

  CallType* get() const noexcept { return ptr_ != nullptr ? &*ptr_ : nullptr; }
  CallType& operator*() const noexcept { return *ptr_; }
  CallType* operator->() const noexcept { return &*ptr_; }
  explicit operator bool() const noexcept { return ptr_ != nullptr; }

  friend bool operator==(const DynamicCallPtr& lhs, std::nullptr_t) noexcept {
    return lhs.ptr_ == nullptr;
  }
  friend bool operator==(std::nullptr_t, const DynamicCallPtr& rhs) noexcept {
    return rhs.ptr_ == nullptr;
  }
  friend bool operator!=(const DynamicCallPtr& lhs, std::nullptr_t) noexcept {
    return lhs.ptr_ != nullptr;
  }
  friend bool operator!=(std::nullptr_t, const DynamicCallPtr& rhs) noexcept {
    return rhs.ptr_ != nullptr;
  }

 private:
  UniquePtr ptr_;
};

/// Deduces the call type from an owning smart pointer, such as the pointer
/// returned by the generated pw_protobuf `DynamicClient` methods.
template <typename Ptr, typename = std::enable_if_t<!std::is_pointer_v<Ptr>>>
DynamicCallPtr(Ptr)
    -> DynamicCallPtr<typename std::pointer_traits<Ptr>::element_type, Ptr>;

/// Moves a call object into a heap-allocated `DynamicCallPtr` using
/// `PW_RPC_MAKE_UNIQUE_PTR`.
///
/// @returns A `DynamicCallPtr` that owns the call, or an empty `DynamicCallPtr`
///   if `PW_RPC_MAKE_UNIQUE_PTR` fails to allocate the call. If allocation
///   fails, the call is closed.
template <typename CallType>
[[nodiscard]] DynamicCallPtr<CallType> MakeDynamicCall(CallType call) {
  return DynamicCallPtr<CallType>(
      PW_RPC_MAKE_UNIQUE_PTR(CallType, std::move(call)));
}

}  // namespace pw::rpc

#endif  // PW_RPC_DYNAMIC_ALLOCATION || defined(PW_EXCLUDE_FROM_DOXYGEN)
