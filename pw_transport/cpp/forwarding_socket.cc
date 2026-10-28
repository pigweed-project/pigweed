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

#include "pw_transport/forwarding_socket.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

#include "pw_assert/assert.h"
#include "pw_assert/check.h"
#include "pw_async2/poll.h"
#include "pw_buf/buf.h"
#include "pw_containers/queue.h"
#include "pw_intrusive_ptr/intrusive_ptr.h"
#include "pw_intrusive_ptr/recyclable.h"
#include "pw_intrusive_ptr/ref_counted.h"
#include "pw_status/status.h"
#include "pw_sync/interrupt_spin_lock.h"
#include "pw_sync/lock_annotations.h"

namespace pw::transport {
namespace {

// Lock shared by both sockets in a pair. Each socket reads and modifies its
// sibling's state, so guarding both with one lock avoids AB-BA deadlocks
// between them. It is reference counted so that it outlives whichever socket
// is destroyed last.
class SharedLock final : public RefCounted<SharedLock>,
                         public Recyclable<SharedLock> {
 public:
  explicit SharedLock(Allocator& allocator) : allocator_(allocator) {}

  void lock() PW_NO_LOCK_SAFETY_ANALYSIS { lock_.lock(); }
  void unlock() PW_NO_LOCK_SAFETY_ANALYSIS { lock_.unlock(); }

 private:
  friend class Recyclable<SharedLock>;

  void pw_recycle() { allocator_.Delete(this); }

  Allocator& allocator_;
  sync::InterruptSpinLock lock_;
};

// One endpoint of a `ForwardingDatagramSocketPair`.
//
// Both sockets in a pair use the same `SharedLock`, so holding `*this` also
// guards `*sibling_`. Clang's thread safety analysis cannot express that the
// two capabilities alias, so methods that touch the sibling's state disable it.
class ForwardingDatagramSocket final : public ReliableSocketImpl {
 public:
  ForwardingDatagramSocket(Allocator& allocator,
                           IntrusivePtr<SharedLock> lock,
                           FixedQueue<ConstBuf>&& read_queue,
                           size_t max_write_message_size_bytes,
                           size_t max_read_message_size_bytes)
      : ReliableSocketImpl(allocator,
                           max_write_message_size_bytes,
                           max_read_message_size_bytes),
        allocator_(allocator),
        lock_(std::move(lock)),
        read_queue_(std::move(read_queue)) {}

  ~ForwardingDatagramSocket() override {
    std::lock_guard lock(*this);
    DisconnectLocked();
  }

  void SetSibling(ForwardingDatagramSocket* sibling)
      PW_NO_LOCK_SAFETY_ANALYSIS {
    sibling_ = sibling;
  }

  using ReliableSocketImpl::WrapSocket;

  void lock() const
      PW_EXCLUSIVE_LOCK_FUNCTION() PW_NO_LOCK_SAFETY_ANALYSIS override {
    lock_->lock();
  }

  void unlock() const PW_UNLOCK_FUNCTION() PW_NO_LOCK_SAFETY_ANALYSIS override {
    lock_->unlock();
  }

 private:
  async2::Poll<ConstBuf> DoRead() override
      PW_EXCLUSIVE_LOCKS_REQUIRED(*this) PW_NO_LOCK_SAFETY_ANALYSIS {
    if (read_queue_.empty()) {
      return async2::Pending();
    }

    ConstBuf datagram = std::move(read_queue_.front());
    read_queue_.pop();

    if (sibling_ == nullptr) {
      if (read_queue_.empty()) {
        ShutDownLocked();
      }
    } else if (sibling_->HasWriteCapacityLocked()) {
      sibling_->WakeOneWriter();
    }

    return async2::Ready(std::move(datagram));
  }

  std::optional<WriteReservation> DoTryReserveWrite(size_t min_size_bytes)
      override PW_EXCLUSIVE_LOCKS_REQUIRED(*this) PW_NO_LOCK_SAFETY_ANALYSIS {
    if (!is_open() || !HasWriteCapacityLocked()) {
      return std::nullopt;
    }
    Buf buffer = Buf::TryAllocate(allocator_, min_size_bytes);
    if (buffer == nullptr) {
      return std::nullopt;
    }
    ++active_reservations_;
    return CreateReservation(std::move(buffer));
  }

  void DoClose() override
      PW_EXCLUSIVE_LOCKS_REQUIRED(*this) PW_NO_LOCK_SAFETY_ANALYSIS {
    DisconnectLocked();
  }

  bool DoCommitWrite(Buf&& buffer) override
      PW_EXCLUSIVE_LOCKS_REQUIRED(*this) PW_NO_LOCK_SAFETY_ANALYSIS {
    ReleaseReservationLocked();

    if (!is_open() || sibling_ == nullptr) {
      // Free the buffer before waking a writer so that its memory is available
      // to the woken writer's allocation.
      buffer = nullptr;
      WakeOneWriter();
      return false;
    }

    // The reservation held a slot in the sibling's queue, so there is room.
    PW_DASSERT(!sibling_->read_queue_.full());
    sibling_->read_queue_.push(std::move(buffer));
    sibling_->WakeReader();
    return true;
  }

  void DoCancelWrite(Buf&& buffer) override PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    // `CancelWrite()` wakes a writer after this returns. Free the buffer now so
    // that its memory is available to the woken writer's allocation.
    buffer = nullptr;
    ReleaseReservationLocked();
  }

  void ReleaseReservationLocked() PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    PW_DASSERT(active_reservations_ > 0);
    --active_reservations_;
  }

  // Active reservations count against the sibling's read queue, since each may
  // be committed into it. After the sibling disconnects, reservations are still
  // granted (and fail to commit) so that writers observe the disconnection,
  // but they remain limited to the queue depth.
  //
  // Both directions have the same queue depth, so this socket's own read queue
  // capacity is the limit. It is used instead of the sibling's because the
  // sibling may be gone, and `clear()` does not release the queue's storage.
  bool HasWriteCapacityLocked() const
      PW_EXCLUSIVE_LOCKS_REQUIRED(*this) PW_NO_LOCK_SAFETY_ANALYSIS {
    const size_t unread =
        sibling_ == nullptr ? 0 : sibling_->read_queue_.size();
    return active_reservations_ + unread < read_queue_.capacity();
  }

  void DisconnectLocked()
      PW_EXCLUSIVE_LOCKS_REQUIRED(*this) PW_NO_LOCK_SAFETY_ANALYSIS {
    ShutDownLocked();
    if (sibling_ != nullptr) {
      ForwardingDatagramSocket* peer = std::exchange(sibling_, nullptr);
      peer->sibling_ = nullptr;
      peer->OnPeerClosedLocked();
    }
  }

  void OnPeerClosedLocked() PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    if (!is_open() || read_queue_.empty()) {
      ShutDownLocked();
      return;
    }
    // `DoRead()` closes the socket after the last queued datagram is read.
    // Until then, wake waiting writers so they can acquire a reservation and
    // observe the disconnected state when `Commit()` returns false.
    WakeAllWriters();
  }

  void ShutDownLocked() PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    read_queue_.clear();
    MarkClosed();
  }

  Allocator& allocator_;
  IntrusivePtr<SharedLock> lock_;
  ForwardingDatagramSocket* sibling_ PW_GUARDED_BY(*this) = nullptr;

  FixedQueue<ConstBuf> read_queue_ PW_GUARDED_BY(*this);
  uint16_t active_reservations_ PW_GUARDED_BY(*this) = 0;
};

}  // namespace

ForwardingDatagramSocketPair ForwardingDatagramSocketPair::Create(
    Allocator& allocator, const ForwardingDatagramSocketOptions& options) {
  Result<ForwardingDatagramSocketPair> result = TryCreate(allocator, options);
  PW_CHECK_OK(result.status(),
              "Failed to allocate ForwardingDatagramSocketPair");
  return std::move(*result);
}

Result<ForwardingDatagramSocketPair> ForwardingDatagramSocketPair::TryCreate(
    Allocator& allocator, const ForwardingDatagramSocketOptions& options) {
  const uint16_t queue_depth = options.queue_depth;
  PW_CHECK_UINT_GT(queue_depth, 0u);

  IntrusivePtr<SharedLock> lock(allocator.New<SharedLock>(allocator));
  if (lock == nullptr) {
    return Status::ResourceExhausted();
  }

  FixedQueue<ConstBuf> first_queue =
      FixedQueue<ConstBuf>::TryAllocate(allocator, queue_depth);
  if (first_queue.capacity() == 0) {
    return Status::ResourceExhausted();
  }

  FixedQueue<ConstBuf> second_queue =
      FixedQueue<ConstBuf>::TryAllocate(allocator, queue_depth);
  if (second_queue.capacity() == 0) {
    return Status::ResourceExhausted();
  }

  ForwardingDatagramSocket* first = allocator.New<ForwardingDatagramSocket>(
      allocator,
      lock,
      std::move(first_queue),
      options.max_first_to_second_size_bytes,
      options.max_second_to_first_size_bytes);
  if (first == nullptr) {
    return Status::ResourceExhausted();
  }

  ForwardingDatagramSocket* second = allocator.New<ForwardingDatagramSocket>(
      allocator,
      std::move(lock),
      std::move(second_queue),
      options.max_second_to_first_size_bytes,
      options.max_first_to_second_size_bytes);
  if (second == nullptr) {
    allocator.Delete(first);
    return Status::ResourceExhausted();
  }

  first->SetSibling(second);
  second->SetSibling(first);
  return ForwardingDatagramSocketPair(first->WrapSocket(),
                                      second->WrapSocket());
}

}  // namespace pw::transport
