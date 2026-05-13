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

#include <optional>

#include "pw_async2/await.h"
#include "pw_async2/channel.h"
#include "pw_containers/vector.h"

namespace pw::async2::test {

/// A move-only integer wrapper used for testing.
class MoveOnlyInt {
 public:
  constexpr MoveOnlyInt() = default;
  constexpr explicit MoveOnlyInt(int value) : value_(value) {}
  ~MoveOnlyInt() = default;

  MoveOnlyInt(const MoveOnlyInt&) = delete;
  MoveOnlyInt& operator=(const MoveOnlyInt&) = delete;
  MoveOnlyInt(MoveOnlyInt&&) = default;
  MoveOnlyInt& operator=(MoveOnlyInt&&) = default;

  constexpr operator int() const { return value_; }

  friend bool operator==(const MoveOnlyInt& lhs, const MoveOnlyInt& rhs) {
    return lhs.value_ == rhs.value_;
  }
  friend bool operator==(const MoveOnlyInt& lhs, int rhs) {
    return lhs.value_ == rhs;
  }
  friend bool operator==(int lhs, const MoveOnlyInt& rhs) {
    return lhs == rhs.value_;
  }

 private:
  int value_ = 0;
};

/// A task that sends a sequence of values to a channel.
template <typename T, typename U = T, size_t kCapacity = 10>
class SenderTask : public Task {
 public:
  SenderTask(Sender<T> sender, std::initializer_list<U> values)
      : Task(PW_ASYNC_TASK_NAME("SenderTask")),
        sender_(std::move(sender)),
        values_(values) {}

  bool succeeded() const { return next_ == values_.size(); }

 private:
  Poll<> DoPend(Context& cx) override {
    while (next_ < values_.size()) {
      if (!future_.is_pendable()) {
        future_ = sender_.Send(T(values_[next_]));
      }

      PW_AWAIT(bool sent, future_, cx);
      if (!sent) {
        return Ready();
      }

      next_++;
    }

    sender_.Disconnect();
    return Ready();
  }

  Sender<T> sender_;
  SendFuture<T> future_;
  pw::Vector<U, kCapacity> values_;
  size_t next_ = 0;
};

/// A task that receives values from a channel.
template <typename T, size_t kCapacity = 10>
class ReceiverTask : public Task {
 public:
  explicit ReceiverTask(
      Receiver<T> receiver,
      size_t disconnect_after = std::numeric_limits<size_t>::max())
      : Task(PW_ASYNC_TASK_NAME("ReceiverTask")),
        receiver_(std::move(receiver)),
        disconnect_after_(disconnect_after) {}

  const pw::Vector<T, kCapacity>& received() const { return received_; }

 private:
  Poll<> DoPend(Context& cx) override {
    while (disconnect_after_ > 0) {
      if (!future_.is_pendable()) {
        future_ = receiver_.Receive();
      }

      PW_AWAIT(std::optional<T> value, future_, cx);
      if (!value.has_value()) {
        break;
      }

      received_.push_back(std::move(*value));
      disconnect_after_--;
    }

    receiver_.Disconnect();
    return Ready();
  }

  Receiver<T> receiver_;
  ReceiveFuture<T> future_;
  pw::Vector<T, kCapacity> received_;
  size_t disconnect_after_;
};

/// A task that sends functions, each of which returns a single value from a
/// sequence.
template <typename T, typename U, size_t kCapacity>
class SenderTask<pw::InlineFunction<T()>, U, kCapacity> : public Task {
 public:
  SenderTask(Sender<pw::InlineFunction<T()>> sender,
             std::initializer_list<T> values)
      : Task(PW_ASYNC_TASK_NAME("SenderTask")),
        sender_(std::move(sender)),
        values_(values) {}

  bool succeeded() const { return next_ == values_.size(); }

 private:
  Poll<> DoPend(Context& cx) override {
    while (next_ < values_.size()) {
      if (!future_.is_pendable()) {
        future_ =
            sender_.Send([value = values_[next_]]() -> T { return T(value); });
      }

      PW_AWAIT(bool sent, future_, cx);
      if (!sent) {
        return Ready();
      }

      next_++;
    }

    sender_.Disconnect();
    return Ready();
  }

  Sender<pw::InlineFunction<T()>> sender_;
  SendFuture<pw::InlineFunction<T()>> future_;
  pw::Vector<T, kCapacity> values_;
  size_t next_ = 0;
};

/// A task that sends a sequence of notifications to a channel.
template <>
class SenderTask<void> : public Task {
 public:
  SenderTask(Sender<void> sender, std::initializer_list<int> values)
      : Task(PW_ASYNC_TASK_NAME("NotificationSenderTask")),
        sender_(std::move(sender)),
        remaining_(values.size()) {}

  bool succeeded() const { return success_; }

 private:
  Poll<> DoPend(Context& cx) override {
    while (remaining_ > 0) {
      if (!future_.is_pendable()) {
        future_ = sender_.Send();
      }

      PW_AWAIT(bool sent, future_, cx);
      if (!sent) {
        success_ = false;
        return Ready();
      }

      remaining_--;
    }

    success_ = true;
    sender_.Disconnect();
    return Ready();
  }

  Sender<void> sender_;
  SendFuture<void> future_;
  bool success_ = false;
  size_t remaining_ = 0;
};

/// A task that receives notifications from a channel.
template <>
class ReceiverTask<void> : public Task {
 public:
  explicit ReceiverTask(
      Receiver<void> receiver,
      size_t disconnect_after = std::numeric_limits<size_t>::max())
      : Task(PW_ASYNC_TASK_NAME("NotificationReceiverTask")),
        receiver_(std::move(receiver)),
        disconnect_after_(disconnect_after) {}

  size_t received() const { return received_count_; }

 private:
  Poll<> DoPend(Context& cx) override {
    while (disconnect_after_ > 0) {
      if (!future_.is_pendable()) {
        future_ = receiver_.Receive();
      }

      PW_AWAIT(bool received, future_, cx);
      if (!received) {
        break;
      }

      received_count_++;
      disconnect_after_--;
    }

    receiver_.Disconnect();
    return Ready();
  }

  Receiver<void> receiver_;
  ReceiveFuture<void> future_;
  size_t received_count_ = 0;
  size_t disconnect_after_;
};

/// A task that reserves space and then sends a sequence of values to a channel.
template <typename T, typename U = T, size_t kCapacity = 10>
class ReservedSenderTask : public Task {
 public:
  ReservedSenderTask(Sender<T> sender, std::initializer_list<U> values)
      : Task(PW_ASYNC_TASK_NAME("ReservedSenderTask")),
        sender_(std::move(sender)),
        values_(values) {}

  bool succeeded() const { return next_ == values_.size(); }

 private:
  Poll<> DoPend(Context& cx) override {
    while (next_ < values_.size()) {
      if (!future_.is_pendable()) {
        future_ = sender_.ReserveSend();
      }

      PW_AWAIT(auto reservation, future_, cx);
      if (!reservation.has_value()) {
        return Ready();
      }

      reservation->Commit(T(values_[next_]));
      next_++;
    }

    sender_.Disconnect();
    return Ready();
  }

  Sender<T> sender_;
  ReserveSendFuture<T> future_;
  size_t next_ = 0;
  pw::Vector<U, kCapacity> values_;
};

/// A task that reserves space and then sends functions which return a single
/// value from a sequence to a channel.
template <typename T, typename U, size_t kCapacity>
class ReservedSenderTask<pw::InlineFunction<T()>, U, kCapacity> : public Task {
 public:
  ReservedSenderTask(Sender<pw::InlineFunction<T()>> sender,
                     std::initializer_list<U> values)
      : Task(PW_ASYNC_TASK_NAME("ReservedSenderTask")),
        sender_(std::move(sender)),
        values_(values) {}

  bool succeeded() const { return next_ == values_.size(); }

 private:
  Poll<> DoPend(Context& cx) override {
    while (next_ < values_.size()) {
      if (!future_.is_pendable()) {
        future_ = sender_.ReserveSend();
      }

      PW_AWAIT(auto reservation, future_, cx);
      if (!reservation.has_value()) {
        return Ready();
      }

      reservation->Commit([value = values_[next_]]() -> T { return T(value); });
      next_++;
    }

    sender_.Disconnect();
    return Ready();
  }

  Sender<pw::InlineFunction<T()>> sender_;
  ReserveSendFuture<pw::InlineFunction<T()>> future_;
  size_t next_ = 0;
  pw::Vector<U, kCapacity> values_;
};

/// A task that reserves space and then sends a sequence of notifications to a
/// channel.
template <>
class ReservedSenderTask<void> : public Task {
 public:
  ReservedSenderTask(Sender<void> sender, std::initializer_list<int> values)
      : Task(PW_ASYNC_TASK_NAME("ReservedNotificationSenderTask")),
        sender_(std::move(sender)),
        remaining_(values.size()) {}

  bool succeeded() const { return success_; }

 private:
  Poll<> DoPend(Context& cx) override {
    while (remaining_ > 0) {
      if (!future_.is_pendable()) {
        future_ = sender_.ReserveSend();
      }

      PW_AWAIT(auto reservation, future_, cx);
      if (!reservation.has_value()) {
        success_ = false;
        return Ready();
      }

      reservation->CommitNotification();
      remaining_--;
    }

    success_ = true;
    sender_.Disconnect();
    return Ready();
  }

  Sender<void> sender_;
  ReserveSendFuture<void> future_;
  bool success_ = false;
  size_t remaining_ = 0;
};

}  // namespace pw::async2::test
