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

#include "pw_async2/channel.h"

#include "pw_assert/check.h"

namespace pw::async2::internal {

BaseChannelFuture::BaseChannelFuture(BaseChannel* channel)
    : core_(FutureState::kPending) {
  if (channel != nullptr) {
    std::lock_guard lock(*channel);
    if (channel->is_open_locked()) {
      channel->add_ref();
      channel_ = channel;
      return;
    }
  }
  channel_ = nullptr;  // channel is nullptr or closed
}

BaseChannelFuture& BaseChannelFuture::MoveAssignFrom(BaseChannelFuture& other) {
  if (this != &other) {
    RemoveFromChannel();
    channel_ = nullptr;
    MoveFrom(other);
  }
  return *this;
}

void BaseChannelFuture::StoreAndAddRefIfNonnull(BaseChannel* channel) {
  channel_ = channel;
  if (channel_ != nullptr) {
    std::lock_guard lock(*channel);
    channel->add_ref();
  }
}

void BaseChannelFuture::MoveFrom(BaseChannelFuture& other) {
  core_ = std::move(other.core_);
  if (other.channel_ == nullptr) {
    return;
  }
  std::lock_guard lock(*other.channel_);
  channel_ = std::exchange(other.channel_, nullptr);
}

void BaseChannelFuture::RemoveFromChannel() {
  if (channel_ != nullptr) {
    channel_->lock();
    core_.Unlist();
    channel_->RemoveRefAndDestroyIfUnreferenced();
  }
}

bool BaseChannelFuture::StoreWakerForReceiveIfOpen(Context& cx) {
  if (!channel_->is_open_locked()) {
    Complete();
    return false;
  }

  PW_ASYNC_STORE_WAKER(cx, core_.waker(), "Receiver::Receive");
  if (!core_.in_list()) {
    channel_->add_receive_future(*this);
  }
  channel_->unlock();
  return true;
}

void BaseChannelFuture::StoreWakerForSend(Context& cx) {
  PW_ASYNC_STORE_WAKER(cx, core_.waker(), "Sender::Send");
  if (!core_.in_list()) {
    channel_->add_send_future(*this);
  }
  channel_->unlock();
}

void BaseChannelFuture::StoreWakerForReserveSend(Context& cx) {
  PW_ASYNC_STORE_WAKER(cx, core_.waker(), "Sender::ReserveSend");
  if (!core_.in_list()) {
    channel_->add_send_future(*this);
  }
  channel_->unlock();
}

BaseChannel::~BaseChannel() { PW_CHECK_UINT_EQ(ref_count_, 0); }

void BaseChannel::RemoveRefAndDestroyIfUnreferenced() {
  const auto ref_count = --ref_count_;
  unlock();

  // Deallocate if this channel is allocated and it has no references.
  if (ref_count == 0u) {
    Destroy();
  }
}

void BaseChannel::CloseLocked() {
  closed_ = true;
  send_futures_.ResolveAll();
  receive_futures_.ResolveAll();
}

void BaseChannel::DropReservationAndRemoveRef() {
  lock();
  PW_DASSERT(reservations_ > 0);
  reservations_--;
  if (is_open_locked()) {
    WakeOneSender();
  }
  RemoveRefAndDestroyIfUnreferenced();
}

void BaseChannel::remove_object(uint8_t* counter) {
  lock();
  if (is_open_locked()) {
    PW_CHECK_UINT_GT(*counter, 0);
    *counter -= 1;
    if (should_close()) {
      CloseLocked();
    }
  }
  RemoveRefAndDestroyIfUnreferenced();
}

BaseChannelHandle::BaseChannelHandle(const BaseChannelHandle& other)
    : channel_(other.channel_) {
  if (channel_ != nullptr) {
    std::lock_guard lock(*channel_);
    channel_->add_handle();
  }
}

BaseChannelHandle& BaseChannelHandle::operator=(
    const BaseChannelHandle& other) {
  if (channel_ != nullptr) {
    channel_->remove_handle();
  }
  channel_ = other.channel_;
  if (channel_ != nullptr) {
    std::lock_guard lock(*channel_);
    channel_->add_handle();
  }
  return *this;
}

BaseChannelHandle& BaseChannelHandle::operator=(
    BaseChannelHandle&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  if (channel_ != nullptr) {
    channel_->remove_handle();
  }
  channel_ = std::exchange(other.channel_, nullptr);
  return *this;
}

void BaseChannelHandle::Close() {
  if (channel_ != nullptr) {
    channel_->Close();
  }
}

void BaseChannelHandle::Release() {
  if (channel_ != nullptr) {
    channel_->remove_handle();
    channel_ = nullptr;
  }
}

}  // namespace pw::async2::internal
