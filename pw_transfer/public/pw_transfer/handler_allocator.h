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

#include <limits>

#include "pw_allocator/allocator.h"
#include "pw_assert/assert.h"
#include "pw_containers/dynamic_deque.h"
#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_stream/stream.h"
#include "pw_sync/lock_annotations.h"
#include "pw_sync/mutex.h"
#include "pw_transfer/handler.h"
#include "pw_transfer/transfer.h"

namespace pw::transfer {

/// @module{pw_transfer}

class TransferHandlerAllocator;

/// A RAII handle for a registered transfer handler. The handler is closed and
/// deallocated when this object goes out of scope.
class TransferResource {
 public:
  /// Creates an invalid handle.
  TransferResource() = default;

  /// Constructs a `TransferResource` owned by `allocator` for `resource_id`.
  ///
  /// @param[in] allocator The `TransferHandlerAllocator` managing this
  /// resource.
  /// @param[in] resource_id Assigned non-zero resource ID.
  TransferResource(TransferHandlerAllocator& allocator, uint32_t resource_id)
      : allocator_(&allocator), resource_id_(resource_id) {}

  TransferResource(TransferResource&& other) noexcept {
    *this = std::move(other);
  }

  TransferResource& operator=(TransferResource&& other) noexcept {
    Close();
    allocator_ = other.allocator_;
    resource_id_ = other.resource_id_;
    other.allocator_ = nullptr;
    other.resource_id_ = 0;
    return *this;
  }

  ~TransferResource() { Close(); }

  /// Returns the resource ID associated with this handle, or `0` if invalid.
  ///
  /// @returns The assigned resource ID, or `0` if this handle is invalid.
  uint32_t resource_id() const { return resource_id_; }

  /// Checks whether this handle refers to an active registered resource.
  ///
  /// @returns `true` if this handle is valid, `false` otherwise.
  explicit operator bool() const { return resource_id_ != 0; }

  /// Unregisters and deallocates the handler if this is a valid handle.
  void Close();

 private:
  TransferHandlerAllocator* allocator_ = nullptr;
  uint32_t resource_id_ = 0;
};

/// Dynamically allocates and registers `Handler` instances for ephemeral
/// transfer resources.
///
/// The `resource_id` is dynamically assigned and registered with the
/// `TransferService` on creation and wrapped in a `TransferResource`. It is
/// unregistered and deallocated when the `TransferResource` is destroyed or
/// closed.
///
/// A range of resource IDs can be specified to allocate from. The range must
/// be between `1` and `std::numeric_limits<uint32_t>::max() - 1` inclusive.
///
/// Takes a `pw::Allocator` to allocate memory for the `Handler` instances and
/// the list tracking active handlers.
class TransferHandlerAllocator {
 public:
  /// Calculates the allocator buffer size needed to support `max_handlers`
  /// concurrent handlers.
  ///
  /// @param[in] max_handlers Maximum number of concurrent active handlers.
  /// @param[in] allocator_block_overhead Per-allocation overhead in bytes for
  /// the backing `pw::Allocator` (e.g. `SmallBlock::kBlockOverhead`).
  ///
  /// @returns Required allocator capacity in bytes.
  static constexpr size_t GetAllocatorSize(size_t max_handlers,
                                           size_t allocator_block_overhead) {
    constexpr auto handler_entry_layout = allocator::Layout::Of<HandlerEntry>();
    static_assert(sizeof(Handler) == sizeof(ReadWriteHandler));
    static_assert(sizeof(Handler) == sizeof(ReadOnlyHandler));
    static_assert(sizeof(Handler) == sizeof(WriteOnlyHandler));
    constexpr auto handler_layout = allocator::Layout::Of<Handler>();
    return max_handlers *
               (handler_entry_layout.size() + handler_entry_layout.alignment() +
                handler_layout.size() + handler_layout.alignment() +
                allocator_block_overhead) +
           // Max expected overhead of active_handlers_ allocation.
           2 * allocator_block_overhead;
  }

  /// Constructs a `TransferHandlerAllocator` bound to `transfer_service`.
  ///
  /// @param[in] transfer_service Service with which allocated handlers are
  /// registered.
  /// @param[in] allocator Memory allocator used for handlers and tracking
  /// entries.
  /// @param[in] min_resource_id Minimum resource ID to assign (inclusive).
  /// @param[in] max_resource_id Maximum resource ID to assign (inclusive).
  ///
  /// @pre `min_resource_id != 0`, `max_resource_id >= min_resource_id`, and
  /// `max_resource_id < std::numeric_limits<uint32_t>::max()`.
  TransferHandlerAllocator(
      TransferService& transfer_service,
      Allocator& allocator,
      uint32_t min_resource_id = 1,
      uint32_t max_resource_id = std::numeric_limits<uint32_t>::max() - 1)
      : transfer_service_(transfer_service),
        allocator_(allocator),
        active_handlers_(allocator),
        min_resource_id_(min_resource_id),
        max_resource_id_(max_resource_id),
        last_resource_id_(min_resource_id - 1) {
    PW_ASSERT(min_resource_id_ != 0);
    PW_ASSERT(max_resource_id_ >= min_resource_id_);
    PW_ASSERT(max_resource_id_ < std::numeric_limits<uint32_t>::max());
  }

  /// Allocates and registers a `ReadOnlyHandler` for the given `reader`.
  ///
  /// @param[in] reader Stream from which transfer data will be read.
  ///
  /// @returns @Result{a `TransferResource` handle for the registered handler}
  /// * @RESOURCE_EXHAUSTED: No resource IDs are available in the configured
  ///   range, or the allocator is out of memory.
  /// * @FAILED_PRECONDITION: Failed to register the handler with the
  ///   `TransferService`.
  Result<TransferResource> AllocateReader(stream::Reader& reader);

  /// Allocates and registers a `WriteOnlyHandler` for the given `writer`.
  ///
  /// @param[in] writer Stream to which transfer data will be written.
  ///
  /// @returns @Result{a `TransferResource` handle for the registered handler}
  /// * @RESOURCE_EXHAUSTED: No resource IDs are available in the configured
  ///   range, or the allocator is out of memory.
  /// * @FAILED_PRECONDITION: Failed to register the handler with the
  ///   `TransferService`.
  Result<TransferResource> AllocateWriter(stream::Writer& writer);

  /// Allocates and registers a `ReadWriteHandler` for the given
  /// `reader_writer`.
  ///
  /// @param[in] reader_writer Stream used for both reading and writing.
  ///
  /// @returns @Result{a `TransferResource` handle for the registered handler}
  /// * @RESOURCE_EXHAUSTED: No resource IDs are available in the configured
  ///   range, or the allocator is out of memory.
  /// * @FAILED_PRECONDITION: Failed to register the handler with the
  ///   `TransferService`.
  Result<TransferResource> AllocateReadWriter(
      stream::ReaderWriter& reader_writer);

 private:
  friend class TransferResource;

  struct HandlerEntry {
    uint32_t resource_id;
    UniquePtr<Handler> handler;
  };

  // Closes and deallocates the handler associated with the given resource_id.
  void Close(uint32_t resource_id);

  bool IsIdInUse(uint32_t resource_id) PW_EXCLUSIVE_LOCKS_REQUIRED(lock_);

  template <typename HandlerType, typename... Args>
  Result<TransferResource> AllocateHandler(Args&... args);

  TransferService& transfer_service_;
  Allocator& allocator_ PW_GUARDED_BY(lock_);
  DynamicDeque<HandlerEntry> active_handlers_ PW_GUARDED_BY(lock_);
  const uint32_t min_resource_id_;
  const uint32_t max_resource_id_;
  uint32_t last_resource_id_ PW_GUARDED_BY(lock_);
  sync::Mutex lock_;
};

/// @endmodule

}  // namespace pw::transfer
