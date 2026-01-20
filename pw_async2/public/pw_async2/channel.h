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

#include <mutex>
#include <optional>

#include "pw_allocator/allocator.h"
#include "pw_async2/callback_task.h"
#include "pw_async2/dispatcher.h"
#include "pw_async2/future.h"
#include "pw_containers/deque.h"
#include "pw_numeric/checked_arithmetic.h"
#include "pw_result/result.h"
#include "pw_sync/interrupt_spin_lock.h"
#include "pw_sync/lock_annotations.h"
#include "pw_sync/timed_thread_notification.h"

namespace pw::async2 {

template <typename T>
class Receiver;

template <typename T>
class ReceiveFuture;

template <typename T>
class Sender;

template <typename T>
class SendFuture;

template <typename T>
class ReserveSendFuture;

template <typename T>
class SendReservation;

template <typename T, uint16_t kCapacity>
class ChannelStorage;

namespace internal {

template <typename T>
class Channel;

class BaseChannel;

class BaseChannelFuture {
 public:
  constexpr BaseChannelFuture() : channel_(nullptr) {}

  BaseChannelFuture(const BaseChannelFuture&) = delete;
  BaseChannelFuture& operator=(const BaseChannelFuture&) = delete;

  // Derived classes call MoveAssignFrom to move rather than use the operator.
  BaseChannelFuture& operator=(BaseChannelFuture&&) = delete;

  /// True if the `Pend` can be called on the future.
  [[nodiscard]] bool is_pendable() const { return core_.is_pendable(); }

  /// True if the future has returned `Ready()`.
  [[nodiscard]] bool is_complete() const { return core_.is_complete(); }

 protected:
  // Creates a new future, storing nullptr if `channel` is nullptr or if the
  // channel is closed.
  explicit BaseChannelFuture(BaseChannel* channel) PW_LOCKS_EXCLUDED(*channel);

  enum AllowClosed { kAllowClosed };

  // Creates a new future, but does NOT check if the channel is open.
  BaseChannelFuture(BaseChannel* channel, AllowClosed)
      PW_LOCKS_EXCLUDED(*channel)
      : core_(FutureState::kPending) {
    StoreAndAddRefIfNonnull(channel);
  }

  BaseChannelFuture(BaseChannelFuture&& other)
      PW_LOCKS_EXCLUDED(*channel_, *other.channel_)
      : channel_(other.channel_) {
    MoveFrom(other);
  }

  BaseChannelFuture& MoveAssignFrom(BaseChannelFuture& other)
      PW_LOCKS_EXCLUDED(*channel_, *other.channel_);

  // Unlists this future and removes a reference from the channel.
  void RemoveFromChannel() PW_LOCKS_EXCLUDED(*channel_);

  bool StoreWakerForReceiveIfOpen(Context& cx) PW_UNLOCK_FUNCTION(*channel_);

  void StoreWakerForSend(Context& cx) PW_UNLOCK_FUNCTION(*channel_);

  void StoreWakerForReserveSend(Context& cx) PW_UNLOCK_FUNCTION(*channel_);

  void MarkCompleted() { core_.MarkComplete(); }

  void Complete() PW_UNLOCK_FUNCTION(*channel_);

  BaseChannel* base_channel() PW_LOCK_RETURNED(channel_) { return channel_; }

 private:
  void StoreAndAddRefIfNonnull(BaseChannel* channel)
      PW_LOCKS_EXCLUDED(*channel);

  void MoveFrom(BaseChannelFuture& other) PW_LOCKS_EXCLUDED(*other.channel_);

  BaseChannel* channel_;
  FutureCore core_;

 public:
  using List = FutureList<&BaseChannelFuture::core_>;
};

// Adds a Pend function to BaseChannelFuture.
template <typename Derived, typename T, typename FutureValue>
class ChannelFuture : public BaseChannelFuture {
 public:
  using value_type = FutureValue;

  Poll<value_type> Pend(Context& cx) PW_LOCKS_EXCLUDED(*this->channel()) {
    Poll<value_type> result = static_cast<Derived&>(*this).DoPend(cx);
    if (result.IsReady()) {
      MarkCompleted();
    }
    return result;
  }

 protected:
  constexpr ChannelFuture() = default;

  explicit ChannelFuture(Channel<T>* channel) : BaseChannelFuture(channel) {}

  ChannelFuture(Channel<T>* channel, AllowClosed)
      : BaseChannelFuture(channel, kAllowClosed) {}

  ChannelFuture(ChannelFuture&& other) : BaseChannelFuture(std::move(other)) {}

  Channel<T>* channel() PW_LOCK_RETURNED(this->base_channel()) {
    return static_cast<Channel<T>*>(base_channel());
  }

 private:
  using BaseChannelFuture::base_channel;
  using BaseChannelFuture::MarkCompleted;
};

// Internal generic channel type. BaseChannel is not exposed to users. Its
// public interface is for internal consumption.
class PW_LOCKABLE("pw::async2::internal::BaseChannel") BaseChannel {
 public:
  static constexpr chrono::SystemClock::duration kWaitForever =
      chrono::SystemClock::duration::max();

  // Acquires the channel's lock.
  void lock() PW_EXCLUSIVE_LOCK_FUNCTION() { lock_.lock(); }

  // Releases the channel's lock.
  void unlock() PW_UNLOCK_FUNCTION() { lock_.unlock(); }

  [[nodiscard]] bool is_open() PW_LOCKS_EXCLUDED(*this) {
    std::lock_guard lock(*this);
    return is_open_locked();
  }

  bool is_open_locked() const PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    return !closed_;
  }

  [[nodiscard]] bool active_locked() const PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    return ref_count_ != 0;
  }

  // Removes a reference to this channel and destroys the channel if needed.
  void RemoveRefAndDestroyIfUnreferenced() PW_UNLOCK_FUNCTION();

  void Close() PW_LOCKS_EXCLUDED(*this) {
    std::lock_guard lock(*this);
    CloseLocked();
  }

  // Adds a SendFuture or ReserveSendFuture to the list of pending futures.
  void add_send_future(BaseChannelFuture& future)
      PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    send_futures_.Push(future);
  }

  // Adds a ReceiveFuture to the list of pending futures.
  void add_receive_future(BaseChannelFuture& future)
      PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    receive_futures_.Push(future);
  }

  void DropReservationAndRemoveRef() PW_LOCKS_EXCLUDED(*this);

  void add_receiver() PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    add_object(receiver_count_);
  }

  void add_sender() PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    add_object(sender_count_);
  }

  void add_handle() PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    add_object(handle_count_);
  }

  void add_reservation() PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    reservations_ += 1;
  }

  void remove_reservation() PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    PW_DASSERT(reservations_ > 0);
    reservations_ -= 1;
  }

  void remove_sender() PW_LOCKS_EXCLUDED(*this) {
    remove_object(&sender_count_);
  }

  void remove_receiver() PW_LOCKS_EXCLUDED(*this) {
    remove_object(&receiver_count_);
  }

  void remove_handle() PW_LOCKS_EXCLUDED(*this) {
    remove_object(&handle_count_);
  }

  void add_ref() PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    PW_ASSERT(CheckedIncrement(ref_count_, 1));
  }

 protected:
  constexpr BaseChannel() = default;

  ~BaseChannel();

  void WakeOneReceiver() PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    receive_futures_.ResolveOneIfAvailable();
  }

  void WakeOneSender() PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    send_futures_.ResolveOneIfAvailable();
  }

  uint16_t reservations() const PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    return reservations_;
  }

 private:
  void add_object(uint8_t& counter) PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    if (is_open_locked()) {
      PW_ASSERT(CheckedIncrement(counter, 1));
    }
    add_ref();
  }

  // Takes a pointer since otherwise Clang's thread safety analysis complains
  // about taking a reference without the lock held.
  void remove_object(uint8_t* counter) PW_LOCKS_EXCLUDED(*this);

  // Returns true if the channel should be closed following a reference
  // decrement.
  //
  // Handles can create new senders and receivers, so as long as one exists, the
  // channel should remain open. Without active handles, the channel closes when
  // either end fully hangs up.
  bool should_close() const PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    return handle_count_ == 0 && (sender_count_ == 0 || receiver_count_ == 0);
  }

  void CloseLocked() PW_EXCLUSIVE_LOCKS_REQUIRED(*this);

  // Destroys the channel if it is dynamically allocated.
  virtual void Destroy() {}

  BaseChannelFuture::List send_futures_ PW_GUARDED_BY(*this);
  BaseChannelFuture::List receive_futures_ PW_GUARDED_BY(*this);

  uint16_t reservations_ PW_GUARDED_BY(*this) = 0;
  bool closed_ PW_GUARDED_BY(*this) = false;
  mutable sync::InterruptSpinLock lock_;

  // Channels are reference counted in two ways:
  //
  // - Senders and receivers are tracked independently. Once either reaches
  //   zero, the channel is closed, but not destroyed. No new values can be
  //   sent, but any buffered values can still be read.
  //
  // - Overall object reference count, including senders, receivers, futures,
  //   and channel handles. Once this reaches zero, the channel is destroyed.
  //
  uint8_t sender_count_ PW_GUARDED_BY(*this) = 0;
  uint8_t receiver_count_ PW_GUARDED_BY(*this) = 0;
  uint8_t handle_count_ PW_GUARDED_BY(*this) = 0;
  uint16_t ref_count_ PW_GUARDED_BY(*this) = 0;
};

// Like BaseChannel, Channel is an internal class that is not exposed to users.
// Its public interface is for internal consumption.
template <typename T>
class Channel : public BaseChannel {
 public:
  Sender<T> CreateSender() PW_LOCKS_EXCLUDED(*this) {
    {
      std::lock_guard guard(*this);
      if (is_open_locked()) {
        return Sender<T>(*this);
      }
    }
    return Sender<T>();
  }

  Receiver<T> CreateReceiver() PW_LOCKS_EXCLUDED(*this) {
    {
      std::lock_guard guard(*this);
      if (is_open_locked()) {
        return Receiver<T>(*this);
      }
    }
    return Receiver<T>();
  }

  void PushAndWake(T&& value) PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    deque_.push_back(std::move(value));
    WakeOneReceiver();
  }

  void PushAndWake(const T& value) PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    deque_.push_back(value);
    WakeOneReceiver();
  }

  template <typename... Args>
  void EmplaceAndWake(Args&&... args) PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    deque_.emplace_back(std::forward<Args>(args)...);
    WakeOneReceiver();
  }

  T PopAndWake() PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    T value = std::move(deque_.front());
    deque_.pop_front();

    WakeOneSender();
    return value;
  }

  bool full() const PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    return remaining_capacity_locked() == 0;
  }

  uint16_t remaining_capacity() PW_LOCKS_EXCLUDED(*this) {
    std::lock_guard guard(*this);
    return remaining_capacity_locked();
  }

  uint16_t remaining_capacity_locked() const
      PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    return deque_.capacity() - deque_.size() - reservations();
  }

  uint16_t capacity() const PW_NO_LOCK_SAFETY_ANALYSIS {
    // SAFETY: The capacity of `deque_` cannot change.
    return deque_.capacity();
  }

  [[nodiscard]] bool empty() PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    return deque_.empty();
  }

  template <typename U>
  Status TrySend(U&& value) PW_LOCKS_EXCLUDED(*this) {
    std::lock_guard guard(*this);
    if (!is_open_locked()) {
      return Status::FailedPrecondition();
    }
    if (full()) {
      return Status::Unavailable();
    }
    PushAndWake(std::forward<U>(value));
    return OkStatus();
  }

  Result<T> TryReceive() PW_LOCKS_EXCLUDED(*this) {
    std::lock_guard guard(*this);
    if (deque_.empty()) {
      return is_open_locked() ? Status::Unavailable()
                              : Status::FailedPrecondition();
    }
    return Result(PopAndWake());
  }

  Result<SendReservation<T>> TryReserveSend() PW_LOCKS_EXCLUDED(*this) {
    std::lock_guard guard(*this);
    if (!is_open_locked()) {
      return Status::FailedPrecondition();
    }
    if (full()) {
      return Status::Unavailable();
    }
    add_reservation();
    return SendReservation<T>(*this);
  }

  template <typename... Args>
  void CommitReservationAndRemoveRef(Args&&... args) PW_LOCKS_EXCLUDED(*this) {
    lock();
    remove_reservation();
    if (is_open_locked()) {
      EmplaceAndWake(std::forward<Args>(args)...);
    }
    RemoveRefAndDestroyIfUnreferenced();
  }

 protected:
  constexpr explicit Channel(FixedDeque<T>&& deque)
      : deque_(std::move(deque)) {}

  template <size_t kAlignment, size_t kCapacity>
  explicit Channel(containers::Storage<kAlignment, kCapacity>& storage)
      : deque_(storage) {}

  ~Channel() = default;

  Deallocator* deallocator() const PW_NO_LOCK_SAFETY_ANALYSIS {
    // SAFETY: deque_.deallocator() cannot change.
    return deque_.deallocator();
  }

 private:
  FixedDeque<T> deque_ PW_GUARDED_BY(*this);
};

template <typename T>
class DynamicChannel final : public Channel<T> {
 public:
  static Channel<T>* Allocate(Allocator& alloc, uint16_t capacity) {
    FixedDeque<T> deque = FixedDeque<T>::TryAllocate(alloc, capacity);
    if (deque.capacity() == 0) {
      return nullptr;
    }
    return alloc.New<DynamicChannel<T>>(std::move(deque));
  }

  explicit DynamicChannel(FixedDeque<T>&& deque)
      : Channel<T>(std::move(deque)) {}

 private:
  ~DynamicChannel() = default;

  void Destroy() final PW_LOCKS_EXCLUDED(*this) {
    Deallocator* const deallocator = this->deallocator();
    this->~DynamicChannel();
    deallocator->Deallocate(this);
  }
};

/// A handle to a channel, used to create senders and receivers.
///
/// After all desired senders and receivers are created, the handle should be
/// released. The channel will remain allocated and open as long as at least
/// one sender and one receiver are alive.
class BaseChannelHandle {
 public:
  constexpr BaseChannelHandle() : channel_(nullptr) {}

  BaseChannelHandle(const BaseChannelHandle& other) PW_LOCKS_EXCLUDED(channel_);

  ~BaseChannelHandle() PW_LOCKS_EXCLUDED(channel_) { Release(); }

  [[nodiscard]] bool is_open() const PW_LOCKS_EXCLUDED(channel_) {
    return channel_ != nullptr && channel_->is_open();
  }

  /// Forces the channel to close, even if there are still active senders or
  /// receivers.
  void Close() PW_LOCKS_EXCLUDED(channel_);

  /// Drops the handle to the channel, preventing creation of new senders and
  /// receivers.
  ///
  /// This function should always be called when the handle is no longer
  /// needed. Holding onto an unreleased handle can prevent the channel from
  /// being closed (and deallocated if the channel is dynamic).
  void Release() PW_LOCKS_EXCLUDED(channel_);

 protected:
  explicit BaseChannelHandle(BaseChannel& channel)
      PW_EXCLUSIVE_LOCKS_REQUIRED(channel)
      : channel_(&channel) {
    channel_->add_handle();
  }

  BaseChannelHandle& operator=(const BaseChannelHandle& other)
      PW_LOCKS_EXCLUDED(channel_);

  BaseChannelHandle(BaseChannelHandle&& other) noexcept
      : channel_(std::exchange(other.channel_, nullptr)) {}

  BaseChannelHandle& operator=(BaseChannelHandle&& other) noexcept
      PW_LOCKS_EXCLUDED(channel_);

  constexpr BaseChannel* channel() const PW_LOCK_RETURNED(channel_) {
    return channel_;
  }

 private:
  BaseChannel* channel_;
};

}  // namespace internal

/// @submodule{pw_async2,channels}

/// Channel handle for a particular type `T`.
template <typename T>
class ChannelHandle : public internal::BaseChannelHandle {
 public:
  constexpr ChannelHandle() = default;

  ChannelHandle(const ChannelHandle&) = default;
  ChannelHandle& operator=(const ChannelHandle&) = default;

  ChannelHandle(ChannelHandle&&) = default;
  ChannelHandle& operator=(ChannelHandle&&) = default;

 protected:
  explicit ChannelHandle(internal::Channel<T>& channel)
      PW_EXCLUSIVE_LOCKS_REQUIRED(channel)
      : internal::BaseChannelHandle(channel) {}

  /// Creates a new sender for the channel, increasing the active sender count.
  /// Cannot be called following `Release`.
  Sender<T> CreateSender() {
    PW_ASSERT(channel() != nullptr);
    return static_cast<internal::Channel<T>&>(*channel()).CreateSender();
  }

  /// Creates a new receiver for the channel, increasing the active receiver
  /// count. Cannot be called following `Release`.
  Receiver<T> CreateReceiver() {
    PW_ASSERT(channel() != nullptr);
    return static_cast<internal::Channel<T>&>(*channel()).CreateReceiver();
  }
};

/// A handle to a multi-producer, multi-consumer channel.
template <typename T>
class MpmcChannelHandle final : public ChannelHandle<T> {
 public:
  constexpr MpmcChannelHandle() = default;

  using ChannelHandle<T>::CreateReceiver;
  using ChannelHandle<T>::CreateSender;

 private:
  explicit MpmcChannelHandle(internal::Channel<T>& channel)
      : ChannelHandle<T>(channel) {}

  template <typename U>
  friend std::optional<MpmcChannelHandle<U>> CreateMpmcChannel(Allocator&,
                                                               uint16_t);

  template <typename U, uint16_t kCapacity>
  friend MpmcChannelHandle<U> CreateMpmcChannel(
      ChannelStorage<U, kCapacity>& storage);
};

/// A handle to a multi-producer, single-consumer channel.
template <typename T>
class MpscChannelHandle final : public ChannelHandle<T> {
 public:
  constexpr MpscChannelHandle() = default;

  using ChannelHandle<T>::CreateSender;

 private:
  explicit MpscChannelHandle(internal::Channel<T>& channel)
      : ChannelHandle<T>(channel) {}

  template <typename U>
  friend std::optional<std::tuple<MpscChannelHandle<U>, Receiver<U>>>
  CreateMpscChannel(Allocator&, uint16_t);

  template <typename U, uint16_t kCapacity>
  friend std::tuple<MpscChannelHandle<U>, Receiver<U>> CreateMpscChannel(
      ChannelStorage<U, kCapacity>& storage);
};

/// A handle to a single-producer, multi-consumer channel.
template <typename T>
class SpmcChannelHandle final : public ChannelHandle<T> {
 public:
  constexpr SpmcChannelHandle() = default;

  using ChannelHandle<T>::CreateReceiver;

 private:
  explicit SpmcChannelHandle(internal::Channel<T>& channel)
      : ChannelHandle<T>(channel) {}

  template <typename U>
  friend std::optional<std::tuple<SpmcChannelHandle<U>, Sender<U>>>
  CreateSpmcChannel(Allocator&, uint16_t);

  template <typename U, uint16_t kCapacity>
  friend std::tuple<SpmcChannelHandle<U>, Sender<U>> CreateSpmcChannel(
      ChannelStorage<U, kCapacity>& storage);
};

/// A handle to a single-producer, single-consumer channel.
template <typename T>
class SpscChannelHandle final : public ChannelHandle<T> {
 public:
  constexpr SpscChannelHandle() = default;

 private:
  explicit SpscChannelHandle(internal::Channel<T>& channel)
      : ChannelHandle<T>(channel) {}

  template <typename U>
  friend std::optional<std::tuple<SpscChannelHandle<U>, Sender<U>, Receiver<U>>>
  CreateSpscChannel(Allocator&, uint16_t);

  template <typename U, uint16_t kCapacity>
  friend std::tuple<SpscChannelHandle<U>, Sender<U>, Receiver<U>>
  CreateSpscChannel(ChannelStorage<U, kCapacity>& storage);
};

/// Handle to a multi-producer channel, which may be either single or multi
/// consumer. Created from either a `MpmcChannelHandle` or a
/// `MpscChannelHandle`.
template <typename T>
class MpChannelHandle final : public ChannelHandle<T> {
 public:
  constexpr MpChannelHandle() = default;

  MpChannelHandle(const MpmcChannelHandle<T>& other)
      : ChannelHandle<T>(other) {}

  MpChannelHandle& operator=(const MpmcChannelHandle<T>& other) {
    ChannelHandle<T>::operator=(other);
    return *this;
  }

  MpChannelHandle(MpmcChannelHandle<T>&& other)
      : ChannelHandle<T>(std::move(other)) {}

  MpChannelHandle& operator=(MpmcChannelHandle<T>&& other) {
    ChannelHandle<T>::operator=(std::move(other));
    return *this;
  }

  MpChannelHandle(const MpscChannelHandle<T>& other)
      : ChannelHandle<T>(other) {}

  MpChannelHandle& operator=(const MpscChannelHandle<T>& other) {
    ChannelHandle<T>::operator=(other);
    return *this;
  }

  MpChannelHandle(MpscChannelHandle<T>&& other)
      : ChannelHandle<T>(std::move(other)) {}

  MpChannelHandle& operator=(MpscChannelHandle<T>&& other) {
    ChannelHandle<T>::operator=(std::move(other));
    return *this;
  }

  using ChannelHandle<T>::CreateSender;
};

/// Handle to a multi-consumer channel, which may be either single or multi
/// producer. Created from either a `MpmcChannelHandle` or a
/// `SpmcChannelHandle`.
template <typename T>
class McChannelHandle final : public ChannelHandle<T> {
 public:
  constexpr McChannelHandle() = default;

  McChannelHandle(const MpmcChannelHandle<T>& other)
      : ChannelHandle<T>(other) {}

  McChannelHandle& operator=(const MpmcChannelHandle<T>& other) {
    ChannelHandle<T>::operator=(other);
    return *this;
  }

  McChannelHandle(MpmcChannelHandle<T>&& other)
      : ChannelHandle<T>(std::move(other)) {}

  McChannelHandle& operator=(MpmcChannelHandle<T>&& other) {
    ChannelHandle<T>::operator=(std::move(other));
    return *this;
  }

  McChannelHandle(const SpmcChannelHandle<T>& other)
      : ChannelHandle<T>(other) {}

  McChannelHandle& operator=(const SpmcChannelHandle<T>& other) {
    ChannelHandle<T>::operator=(other);
    return *this;
  }

  McChannelHandle(SpmcChannelHandle<T>&& other)
      : ChannelHandle<T>(std::move(other)) {}

  McChannelHandle& operator=(SpmcChannelHandle<T>&& other) {
    ChannelHandle<T>::operator=(std::move(other));
    return *this;
  }

  using ChannelHandle<T>::CreateReceiver;
};

/// Fixed capacity storage for an asynchronous channel which supports multiple
/// producers and multiple consumers.
///
/// `ChannelStorage` must outlive the channel in which it is used.
template <typename T, uint16_t kCapacity>
class ChannelStorage final : private containers::StorageBaseFor<T, kCapacity>,
                             private internal::Channel<T> {
 public:
  template <typename U, uint16_t kCap>
  friend std::tuple<SpscChannelHandle<U>, Sender<U>, Receiver<U>>
  CreateSpscChannel(ChannelStorage<U, kCap>& storage);

  template <typename U, uint16_t kCap>
  friend std::tuple<MpscChannelHandle<U>, Receiver<U>> CreateMpscChannel(
      ChannelStorage<U, kCap>& storage);

  template <typename U, uint16_t kCap>
  friend std::tuple<SpmcChannelHandle<U>, Sender<U>> CreateSpmcChannel(
      ChannelStorage<U, kCap>& storage);

  template <typename U, uint16_t kCap>
  friend MpmcChannelHandle<U> CreateMpmcChannel(
      ChannelStorage<U, kCap>& storage);

  ChannelStorage() : internal::Channel<T>(this->storage()) {}

  ~ChannelStorage() = default;

  /// Returns true if this channel storage is in use.
  /// If `false`, the storage can either be reused or safely destroyed.
  [[nodiscard]] bool active() const PW_LOCKS_EXCLUDED(*this) {
    std::lock_guard lock(*this);
    return this->active_locked();
  }

  constexpr uint16_t capacity() const { return kCapacity; }
};

template <typename T>
class [[nodiscard]] ReceiveFuture final
    : public internal::ChannelFuture<ReceiveFuture<T>, T, std::optional<T>> {
 private:
  using Base = internal::ChannelFuture<ReceiveFuture, T, std::optional<T>>;

 public:
  constexpr ReceiveFuture() = default;

  ReceiveFuture(ReceiveFuture&& other)
      PW_LOCKS_EXCLUDED(*this->channel(), *other.channel())
      : Base(std::move(other)) {}

  ReceiveFuture& operator=(ReceiveFuture&& other)
      PW_LOCKS_EXCLUDED(*this->channel(), *other.channel()) {
    // NOLINTNEXTLINE(misc-unconventional-assign-operator)
    return static_cast<ReceiveFuture&>(this->MoveAssignFrom(other));
  }

  ~ReceiveFuture() PW_LOCKS_EXCLUDED(*this->channel()) {
    this->RemoveFromChannel();
  }

 private:
  friend Base;
  friend internal::Channel<T>;
  friend Receiver<T>;
  template <typename, typename>
  friend class CallbackTask;

  explicit ReceiveFuture(internal::Channel<T>* channel)
      PW_LOCKS_EXCLUDED(*channel)
      : Base(channel, this->kAllowClosed) {}

  PollOptional<T> DoPend(Context& cx) PW_LOCKS_EXCLUDED(*this->channel()) {
    if (this->channel() == nullptr) {
      PW_ASSERT(!this->is_complete());
      return Ready<std::optional<T>>(std::nullopt);
    }

    this->channel()->lock();
    if (this->channel()->empty()) {
      return this->StoreWakerForReceiveIfOpen(cx)
                 ? Pending()
                 : Ready<std::optional<T>>(std::nullopt);
    }

    auto result = Ready(this->channel()->PopAndWake());
    this->Complete();
    return result;
  }
};

/// A receiver which reads values from an asynchronous channel.
template <typename T>
class Receiver {
 public:
  constexpr Receiver() : channel_(nullptr) {}

  Receiver(const Receiver& other) = delete;
  Receiver& operator=(const Receiver& other) = delete;

  Receiver(Receiver&& other) noexcept
      : channel_(std::exchange(other.channel_, nullptr)) {}

  Receiver& operator=(Receiver&& other) noexcept {
    if (this == &other) {
      return *this;
    }
    if (channel_ != nullptr) {
      channel_->remove_receiver();
    }
    channel_ = std::exchange(other.channel_, nullptr);
    return *this;
  }

  ~Receiver() {
    if (channel_ != nullptr) {
      channel_->remove_receiver();
    }
  }

  /// Reads a value from the channel, blocking until it is available.
  ///
  /// @returns
  /// * @OK: A value was successfully read from the channel.
  /// * @FAILED_PRECONDITION: The channel is closed.
  /// * @DEADLINE_EXCEEDED: The operation timed out.
  ///
  /// This operation blocks the running thread until it is complete. It must
  /// not be called from an async context, or it will likely deadlock.
  Result<T> BlockingReceive(Dispatcher& dispatcher,
                            chrono::SystemClock::duration timeout =
                                internal::Channel<T>::kWaitForever)
      PW_LOCKS_EXCLUDED(*channel_) {
    if (channel_ == nullptr) {
      return Status::FailedPrecondition();
    }

    // Return immediately if a value is available or the channel is closed.
    if (Result<T> result = channel_->TryReceive();
        result.ok() || result.status().IsFailedPrecondition()) {
      return result;
    }

    std::optional<T> result;
    sync::TimedThreadNotification notification;

    CallbackTask task = CallbackTask<ReceiveFuture<T>>::Emplace(
        [&result, &notification](std::optional<T>&& val) {
          result = std::move(val);
          notification.release();
        },
        channel_);
    dispatcher.Post(task);

    if (timeout == internal::Channel<T>::kWaitForever) {
      notification.acquire();
      if (!result.has_value()) {
        return Status::FailedPrecondition();
      }
      return Result<T>(std::move(*result));
    }

    if (!notification.try_acquire_for(timeout)) {
      return Status::DeadlineExceeded();
    }

    if (!result.has_value()) {
      return Status::FailedPrecondition();
    }
    return Result<T>(std::move(*result));
  }

  /// Reads a value from the channel, blocking until it is available.
  ///
  /// Returns a `Future<std::optional<T>>` which resolves to a `T` value if
  /// the read is successful, or `std::nullopt` if the channel is closed.
  ///
  /// If there are multiple receivers for a channel, each of them compete for
  /// exclusive values.
  ReceiveFuture<T> Receive() PW_LOCKS_EXCLUDED(*channel_) {
    return ReceiveFuture<T>(channel_);
  }

  /// Reads a value from the channel if one is available.
  ///
  /// @returns
  /// * @OK: A value was successfully read from the channel.
  /// * @FAILED_PRECONDITION: The channel is closed.
  /// * @UNAVAILABLE: The channel is empty.
  Result<T> TryReceive() {
    if (channel_ == nullptr) {
      return Status::FailedPrecondition();
    }
    return channel_->TryReceive();
  }

  /// Removes this receiver from its channel, preventing the receiver from
  /// reading further values.
  ///
  /// The channel may remain open if other receivers exist.
  void Disconnect() {
    if (channel_ != nullptr) {
      channel_->remove_receiver();
      channel_ = nullptr;
    }
  }

  /// Returns true if the channel is open.
  [[nodiscard]] bool is_open() const {
    return channel_ != nullptr && channel_->is_open();
  }

 private:
  template <typename U>
  friend class internal::Channel;

  template <typename U>
  friend std::optional<std::tuple<MpscChannelHandle<U>, Receiver<U>>>
  CreateMpscChannel(Allocator&, uint16_t);

  template <typename U, uint16_t kCapacity>
  friend std::tuple<MpscChannelHandle<U>, Receiver<U>> CreateMpscChannel(
      ChannelStorage<U, kCapacity>& storage);

  template <typename U>
  friend std::optional<std::tuple<SpscChannelHandle<U>, Sender<U>, Receiver<U>>>
  CreateSpscChannel(Allocator&, uint16_t);

  template <typename U, uint16_t kCapacity>
  friend std::tuple<SpscChannelHandle<U>, Sender<U>, Receiver<U>>
  CreateSpscChannel(ChannelStorage<U, kCapacity>& storage);

  explicit Receiver(internal::Channel<T>& channel)
      PW_EXCLUSIVE_LOCKS_REQUIRED(channel)
      : channel_(&channel) {
    channel_->add_receiver();
  }

  internal::Channel<T>* channel_;
};

template <typename T>
class [[nodiscard]] SendFuture final
    : public internal::ChannelFuture<SendFuture<T>, T, bool> {
 private:
  using Base = internal::ChannelFuture<SendFuture, T, bool>;

 public:
  constexpr SendFuture() = default;

  SendFuture(SendFuture&& other) PW_LOCKS_EXCLUDED(*other.channel())
      : Base(static_cast<Base&&>(other)), value_(std::move(other.value_)) {}

  SendFuture& operator=(SendFuture&& other)
      PW_LOCKS_EXCLUDED(*this->channel(), *other.channel()) {
    value_ = std::move(other.value_);
    // NOLINTNEXTLINE(misc-unconventional-assign-operator)
    return static_cast<SendFuture&>(this->MoveAssignFrom(other));
  }

  ~SendFuture() PW_LOCKS_EXCLUDED(*this->channel()) {
    this->RemoveFromChannel();
  }

 private:
  friend Base;
  friend internal::Channel<T>;
  friend Sender<T>;

  SendFuture(internal::Channel<T>* channel, const T& value)
      PW_LOCKS_EXCLUDED(*channel)
      : Base(channel), value_(value) {}

  SendFuture(internal::Channel<T>* channel, T&& value)
      PW_LOCKS_EXCLUDED(*channel)
      : Base(channel), value_(std::move(value)) {}

  Poll<bool> DoPend(Context& cx) PW_LOCKS_EXCLUDED(*this->channel()) {
    if (this->channel() == nullptr) {
      PW_ASSERT(!this->is_complete());
      return Ready(false);
    }

    this->channel()->lock();
    if (!this->channel()->is_open_locked()) {
      this->Complete();
      return Ready(false);
    }

    if (this->channel()->full()) {
      this->StoreWakerForSend(cx);
      return Pending();
    }

    this->channel()->PushAndWake(std::move(*value_));
    this->Complete();
    return Ready(true);
  }

  std::optional<T> value_;
};

/// A reservation for sending values to a channel, returned from a
/// `ReserveSendFuture` once space is available in the channel.
///
/// The `SendReservation` must be used immediately once its future resolves.
/// If the reservation object is dropped, its reservation is released and the
/// space is made available for other senders.
template <typename T>
class SendReservation {
 public:
  SendReservation(const SendReservation& other) = delete;
  SendReservation& operator=(const SendReservation& other) = delete;

  SendReservation(SendReservation&& other)
      : channel_(std::exchange(other.channel_, nullptr)) {}

  SendReservation& operator=(SendReservation&& other) {
    if (this == &other) {
      return *this;
    }
    Cancel();
    channel_ = std::exchange(other.channel_, nullptr);
    return *this;
  }

  ~SendReservation() { Cancel(); }

  /// Commits a value to a reserved slot.
  template <typename... Args>
  void Commit(Args&&... args) {
    PW_ASSERT(channel_ != nullptr);
    channel_->CommitReservationAndRemoveRef(std::forward<Args>(args)...);
    channel_ = nullptr;
  }

  /// Releases the reservation, making the space available for other senders.
  void Cancel() {
    if (channel_ != nullptr) {
      channel_->DropReservationAndRemoveRef();
      channel_ = nullptr;
    }
  }

 private:
  friend internal::Channel<T>;
  friend class ReserveSendFuture<T>;
  friend class Sender<T>;

  explicit SendReservation(internal::Channel<T>& channel)
      PW_EXCLUSIVE_LOCKS_REQUIRED(channel)
      : channel_(&channel) {
    channel_->add_ref();
  }

  internal::Channel<T>* channel_;
};

template <typename T>
class [[nodiscard]] ReserveSendFuture final
    : public internal::ChannelFuture<ReserveSendFuture<T>,
                                     T,
                                     std::optional<SendReservation<T>>> {
 private:
  using Base = internal::
      ChannelFuture<ReserveSendFuture, T, std::optional<SendReservation<T>>>;

 public:
  constexpr ReserveSendFuture() = default;

  ReserveSendFuture(ReserveSendFuture&& other) : Base(std::move(other)) {}

  ReserveSendFuture& operator=(ReserveSendFuture&& other) {
    // NOLINTNEXTLINE(misc-unconventional-assign-operator)
    return static_cast<ReserveSendFuture&>(this->MoveAssignFrom(other));
  }

  ~ReserveSendFuture() PW_LOCKS_EXCLUDED(*this->channel()) {
    this->RemoveFromChannel();
  }

 private:
  friend Base;
  friend internal::Channel<T>;
  friend Sender<T>;

  explicit ReserveSendFuture(internal::Channel<T>* channel)
      PW_LOCKS_EXCLUDED(*channel)
      : Base(channel) {}

  PollOptional<SendReservation<T>> DoPend(Context& cx)
      PW_LOCKS_EXCLUDED(*this->channel()) {
    if (this->channel() == nullptr) {
      PW_ASSERT(!this->is_complete());
      return Ready<std::optional<SendReservation<T>>>(std::nullopt);
    }

    this->channel()->lock();
    if (!this->channel()->is_open_locked()) {
      this->Complete();
      return Ready<std::optional<SendReservation<T>>>(std::nullopt);
    }

    if (this->channel()->remaining_capacity_locked() == 0) {
      this->StoreWakerForReserveSend(cx);
      return Pending();
    }

    this->channel()->add_reservation();
    SendReservation<T> reservation(*this->channel());
    this->Complete();
    return reservation;
  }
};

/// A sender which writes values to an asynchronous channel.
template <typename T>
class Sender {
 public:
  constexpr Sender() : channel_(nullptr) {}

  Sender(const Sender& other) = delete;
  Sender& operator=(const Sender& other) = delete;

  Sender(Sender&& other) noexcept
      : channel_(std::exchange(other.channel_, nullptr)) {}

  Sender& operator=(Sender&& other) noexcept {
    if (this == &other) {
      return *this;
    }
    if (channel_ != nullptr) {
      channel_->remove_sender();
    }
    channel_ = std::exchange(other.channel_, nullptr);
    return *this;
  }

  ~Sender() {
    if (channel_ != nullptr) {
      channel_->remove_sender();
    }
  }

  /// Sends `value` through the channel, blocking until there is space.
  ///
  /// Returns a `Future<bool>` which resolves to `true` if the value was
  /// successfully sent to the channel, or `false` if the channel is closed.
  ///
  /// Note that a value being sent successfully does not guarantee that it
  /// will be read. If all corresponding receivers disconnect, any values
  /// still buffered in the channel are lost.
  template <typename U>
  SendFuture<T> Send(U&& value) {
    return SendFuture<T>(channel_, std::forward<U>(value));
  }

  /// Returns a `Future<std::optional<SendReservation>>` which resolves to a
  /// `SendReservation` which can be used to write a value directly into the
  /// channel when space is available.
  ///
  /// If the channel is closed, the future resolves to `nullopt`.
  ReserveSendFuture<T> ReserveSend() { return ReserveSendFuture<T>(channel_); }

  /// Synchronously attempts to reserve a slot in the channel.
  ///
  /// This operation is thread-safe and may be called from outside of an async
  /// context.
  ///
  /// @returns
  /// * @OK: A `SendReservation` was successfully created.
  /// * @FAILED_PRECONDITION: The channel is closed.
  /// * @UNAVAILABLE: The channel is full.
  Result<SendReservation<T>> TryReserveSend() {
    if (channel_ == nullptr) {
      return Status::FailedPrecondition();
    }
    return channel_->TryReserveSend();
  }

  /// Synchronously attempts to send `value` if there is space in the channel.
  ///
  /// This operation is thread-safe and may be called from outside of an async
  /// context.
  ///
  /// @returns
  /// * @OK: The value was successfully sent to the channel.
  /// * @FAILED_PRECONDITION: The channel is closed.
  /// * @UNAVAILABLE: The channel is full.
  Status TrySend(const T& value) {
    if (channel_ == nullptr) {
      return Status::FailedPrecondition();
    }
    return channel_->TrySend(value);
  }

  /// @copydoc TrySend
  Status TrySend(T&& value) {
    if (channel_ == nullptr) {
      return Status::FailedPrecondition();
    }
    return channel_->TrySend(std::move(value));
  }

  /// Synchronously attempts to send `value` to the channel, blocking until
  /// space is available or the channel is closed.
  ///
  /// This operation blocks the running thread until it is complete. It must
  /// not be called from an async context, or it will likely deadlock.
  ///
  /// @returns
  /// * @OK: The value was successfully sent to the channel.
  /// * @FAILED_PRECONDITION: The channel is closed.
  /// * @DEADLINE_EXCEEDED: The operation timed out.
  Status BlockingSend(Dispatcher& dispatcher,
                      const T& value,
                      chrono::SystemClock::duration timeout =
                          internal::Channel<T>::kWaitForever) {
    return BlockingSendMoveOrCopy(dispatcher, value, timeout);
  }

  /// @copydoc BlockingSend
  Status BlockingSend(Dispatcher& dispatcher,
                      T&& value,
                      chrono::SystemClock::duration timeout =
                          internal::Channel<T>::kWaitForever) {
    return BlockingSendMoveOrCopy(dispatcher, std::move(value), timeout);
  }

  /// Removes this sender from its channel, preventing it from writing further
  /// values.
  ///
  /// The channel may remain open if other senders and receivers exist.
  void Disconnect() {
    if (channel_ != nullptr) {
      channel_->remove_sender();
      channel_ = nullptr;
    }
  }

  /// Returns the remaining capacity of the channel.
  uint16_t remaining_capacity() const {
    return channel_ != nullptr ? channel_->remaining_capacity() : 0;
  }

  /// Returns the maximum capacity of the channel.
  uint16_t capacity() const {
    return channel_ != nullptr ? channel_->capacity() : 0;
  }

  /// Returns true if the channel is open.
  [[nodiscard]] bool is_open() const {
    return channel_ != nullptr && channel_->is_open();
  }

 private:
  template <typename U>
  friend class internal::Channel;

  template <typename U>
  friend std::optional<std::tuple<SpmcChannelHandle<U>, Sender<U>>>
  CreateSpmcChannel(Allocator&, uint16_t);

  template <typename U, uint16_t kCapacity>
  friend std::tuple<SpmcChannelHandle<U>, Sender<U>> CreateSpmcChannel(
      ChannelStorage<U, kCapacity>& storage);

  template <typename U>
  friend std::optional<std::tuple<SpscChannelHandle<U>, Sender<U>, Receiver<U>>>
  CreateSpscChannel(Allocator&, uint16_t);

  template <typename U, uint16_t kCapacity>
  friend std::tuple<SpscChannelHandle<U>, Sender<U>, Receiver<U>>
  CreateSpscChannel(ChannelStorage<U, kCapacity>& storage);

  explicit Sender(internal::Channel<T>& channel)
      PW_EXCLUSIVE_LOCKS_REQUIRED(channel)
      : channel_(&channel) {
    channel_->add_sender();
  }

  template <typename U>
  Status BlockingSendMoveOrCopy(Dispatcher& dispatcher,
                                U&& value,
                                chrono::SystemClock::duration timeout)
      PW_LOCKS_EXCLUDED(*channel_) {
    if (channel_ == nullptr) {
      return Status::FailedPrecondition();
    }

    if (Status status = channel_->TrySend(std::forward<U>(value));
        status.ok() || status.IsFailedPrecondition()) {
      return status;
    }

    return BlockingSendFuture(
        dispatcher,  // NOLINTNEXTLINE(bugprone-use-after-move)
        SendFuture<T>(channel_, std::forward<U>(value)),
        timeout);
  }

  Status BlockingSendFuture(Dispatcher& dispatcher,
                            SendFuture<T>&& future,
                            chrono::SystemClock::duration timeout)
      PW_LOCKS_EXCLUDED(*channel_) {
    Status status;
    sync::TimedThreadNotification notification;

    CallbackTask task(
        [&status, &notification](bool result) {
          status = result ? OkStatus() : Status::FailedPrecondition();
          notification.release();
        },
        std::move(future));
    dispatcher.Post(task);

    if (timeout == internal::Channel<T>::kWaitForever) {
      notification.acquire();
      return status;
    }

    if (!notification.try_acquire_for(timeout)) {
      task.Deregister();
      return Status::DeadlineExceeded();
    }
    return status;
  }

  internal::Channel<T>* channel_;
};

/// Creates a dynamically allocated multi-producer, multi-consumer channel
/// with a fixed storage capacity.
///
/// Returns a handle to the channel which may be used to create senders and
/// receivers. After all desired senders and receivers are created, the handle
/// can be dropped without affecting the channel.
///
/// All allocation occurs during the creation of the channel. After this
/// function returns, usage of the channel is guaranteed not to allocate.
/// If allocation fails, returns `std::nullopt`.
///
/// The channel remains open as long as at least either a handle, or at least
/// one sender and one receiver exist.
template <typename T>
std::optional<MpmcChannelHandle<T>> CreateMpmcChannel(Allocator& alloc,
                                                      uint16_t capacity) {
  auto channel = internal::DynamicChannel<T>::Allocate(alloc, capacity);
  if (channel == nullptr) {
    return std::nullopt;
  }
  std::lock_guard lock(*channel);
  return MpmcChannelHandle<T>(*channel);
}

/// Creates a multi-producer, multi-consumer channel with provided static
/// storage.
///
/// Returns a handle to the channel which may be used to create senders and
/// receivers. After all desired senders and receivers are created, the handle
/// can be dropped without affecting the channel.
///
/// The channel remains open as long as at least either a handle, or at least
/// one sender and one receiver exist.
///
/// The provided storage must outlive the channel.
template <typename T, uint16_t kCapacity>
MpmcChannelHandle<T> CreateMpmcChannel(ChannelStorage<T, kCapacity>& storage) {
  std::lock_guard lock(static_cast<internal::Channel<T>&>(storage));
  PW_DASSERT(!storage.active_locked());
  return MpmcChannelHandle<T>(storage);
}

/// Creates a dynamically allocated multi-producer, single-consumer channel
/// with a fixed storage capacity.
///
/// Returns a handle to the channel which may be used to create senders. After
/// all desired senders are created, the handle can be dropped without
/// affecting the channel.
///
/// All allocation occurs during the creation of the channel. After this
/// function returns, usage of the channel is guaranteed not to allocate.
/// If allocation fails, returns `std::nullopt`.
///
/// The channel remains open as long as at least either a handle, or at least
/// one sender and one receiver exist.
template <typename T>
std::optional<std::tuple<MpscChannelHandle<T>, Receiver<T>>> CreateMpscChannel(
    Allocator& alloc, uint16_t capacity) {
  auto channel = internal::DynamicChannel<T>::Allocate(alloc, capacity);
  if (channel == nullptr) {
    return std::nullopt;
  }
  std::lock_guard lock(*channel);
  return std::make_tuple(MpscChannelHandle<T>(*channel), Receiver<T>(*channel));
}

/// Creates a multi-producer, single-consumer channel with provided static
/// storage.
///
/// Returns a handle to the channel which may be used to create senders. After
/// all desired senders are created, the handle can be dropped without
/// affecting the channel.
///
/// The channel remains open as long as at least either a handle, or at least
/// one sender and one receiver exist.
///
/// The provided storage must outlive the channel.
template <typename T, uint16_t kCapacity>
std::tuple<MpscChannelHandle<T>, Receiver<T>> CreateMpscChannel(
    ChannelStorage<T, kCapacity>& storage) {
  std::lock_guard lock(static_cast<internal::Channel<T>&>(storage));
  PW_DASSERT(!storage.active_locked());
  return std::make_tuple(MpscChannelHandle<T>(storage), Receiver<T>(storage));
}

/// Creates a dynamically allocated single-producer, multi-consumer channel
/// with a fixed storage capacity.
///
/// Returns a handle to the channel which may be used to create receivers.
/// After all desired receivers are created, the handle can be dropped without
/// affecting the channel.
///
/// All allocation occurs during the creation of the channel. After this
/// function returns, usage of the channel is guaranteed not to allocate.
/// If allocation fails, returns `std::nullopt`.
///
/// The channel remains open as long as at least either a handle, or at least
/// one sender and one receiver exist.
template <typename T>
std::optional<std::tuple<SpmcChannelHandle<T>, Sender<T>>> CreateSpmcChannel(
    Allocator& alloc, uint16_t capacity) {
  auto channel = internal::DynamicChannel<T>::Allocate(alloc, capacity);
  if (channel == nullptr) {
    return std::nullopt;
  }
  std::lock_guard lock(*channel);
  return std::make_tuple(SpmcChannelHandle<T>(*channel), Sender<T>(*channel));
}

/// Creates a single-producer, multi-consumer channel with provided static
/// storage.
///
/// Returns a handle to the channel which may be used to create receivers.
/// After all desired receivers are created, the handle can be dropped without
/// affecting the channel.
///
/// The channel remains open as long as at least either a handle, or at least
/// one sender and one receiver exist.
///
/// The provided storage must outlive the channel.
template <typename T, uint16_t kCapacity>
std::tuple<SpmcChannelHandle<T>, Sender<T>> CreateSpmcChannel(
    ChannelStorage<T, kCapacity>& storage) {
  std::lock_guard lock(static_cast<internal::Channel<T>&>(storage));
  PW_DASSERT(!storage.active_locked());
  return std::make_tuple(SpmcChannelHandle<T>(storage), Sender<T>(storage));
}

/// Creates a dynamically allocated single-producer, single-consumer channel
/// with a fixed storage capacity.
///
/// Returns a handle to the channel alongside the sender and receiver. The
/// handle can be used to forcefully close the channel. If that is not
/// required, it can be dropped without affecting the channel.
///
/// All allocation occurs during the creation of the channel. After this
/// function returns, usage of the channel is guaranteed not to allocate.
/// If allocation fails, returns `std::nullopt`.
///
/// The channel remains open as long as at least either a handle, or at least
/// one sender and one receiver exist.
template <typename T>
std::optional<std::tuple<SpscChannelHandle<T>, Sender<T>, Receiver<T>>>
CreateSpscChannel(Allocator& alloc, uint16_t capacity) {
  auto channel = internal::DynamicChannel<T>::Allocate(alloc, capacity);
  if (channel == nullptr) {
    return std::nullopt;
  }
  std::lock_guard lock(*channel);
  return std::make_tuple(SpscChannelHandle<T>(*channel),
                         Sender<T>(*channel),
                         Receiver<T>(*channel));
}

/// Creates a single-producer, single-consumer channel with provided static
/// storage.
///
/// Returns a handle to the channel alongside the sender and receiver. The
/// handle can be used to forcefully close the channel. If that is not
/// required, it can be dropped without affecting the channel.
///
/// The channel remains open as long as at least either a handle, or at least
/// one sender and one receiver exist.
///
/// The provided storage must outlive the channel.
template <typename T, uint16_t kCapacity>
std::tuple<SpscChannelHandle<T>, Sender<T>, Receiver<T>> CreateSpscChannel(
    ChannelStorage<T, kCapacity>& storage) {
  std::lock_guard lock(static_cast<internal::Channel<T>&>(storage));
  PW_DASSERT(!storage.active_locked());
  return std::make_tuple(
      SpscChannelHandle<T>(storage), Sender<T>(storage), Receiver<T>(storage));
}

/// @endsubmodule

namespace internal {

inline void BaseChannelFuture::Complete() PW_UNLOCK_FUNCTION(*channel_) {
  channel_->RemoveRefAndDestroyIfUnreferenced();
  channel_ = nullptr;
}

}  // namespace internal
}  // namespace pw::async2
