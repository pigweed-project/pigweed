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

#include "pw_channel/forwarding_channel.h"

namespace pw::channel::internal {

async2::PollResult<multibuf::MultiBuf>
ForwardingChannel<DataType::kDatagram>::DoPendRead(async2::Context& cx)
    PW_NO_LOCK_SAFETY_ANALYSIS {
  std::lock_guard lock(pair_.mutex_);

  // Close this channel if the sibling is closed, but return any remaining data.
  if (!sibling_.is_write_open()) {
    set_read_closed();
    if (!read_queue_.has_value()) {
      read_waker_.Clear();
      return Status::FailedPrecondition();
    }
  } else if (!read_queue_.has_value()) {
    PW_ASYNC_STORE_WAKER(
        cx,
        read_waker_,
        "ForwardingChannel is waiting for incoming data from its peer");
    return async2::Pending();
  }

  read_waker_.Clear();
  auto read_data = std::move(*read_queue_);
  read_queue_.reset();
  sibling_.write_waker_.Wake();
  return read_data;
}

async2::Poll<Status> ForwardingChannel<DataType::kDatagram>::DoPendReadyToWrite(
    async2::Context& cx) PW_NO_LOCK_SAFETY_ANALYSIS {
  std::lock_guard lock(pair_.mutex_);
  if (sibling_.read_queue_.has_value()) {
    PW_ASYNC_STORE_WAKER(
        cx,
        write_waker_,
        "ForwardingChannel is waiting for its peer to read the data "
        "it enqueued");
    return async2::Pending();
  }
  write_waker_.Clear();
  return async2::Ready(OkStatus());
}

Status ForwardingChannel<DataType::kDatagram>::DoStageWrite(
    multibuf::MultiBuf&& data) PW_NO_LOCK_SAFETY_ANALYSIS {
  std::lock_guard lock(pair_.mutex_);
  PW_DASSERT(!sibling_.read_queue_.has_value());
  sibling_.read_queue_ = std::move(data);

  sibling_.read_waker_.Wake();
  return OkStatus();
}

async2::Poll<Status> ForwardingChannel<DataType::kDatagram>::DoPendClose(
    async2::Context&) PW_NO_LOCK_SAFETY_ANALYSIS {
  std::lock_guard lock(pair_.mutex_);
  sibling_.set_write_closed();  // No more writes from the other end
  read_queue_.reset();
  read_waker_.Clear();
  write_waker_.Clear();
  sibling_.read_waker_.Wake();
  sibling_.write_waker_.Wake();
  return OkStatus();
}

async2::PollResult<multibuf::MultiBuf>
ForwardingChannel<DataType::kByte>::DoPendRead(async2::Context& cx) {
  std::lock_guard lock(pair_.mutex_);

  // Close this channel if the sibling is closed, but return any remaining data.
  if (!sibling_.is_write_open()) {
    set_read_closed();
    if (read_queue_.empty()) {
      read_waker_.Clear();
      return Status::FailedPrecondition();
    }
  } else if (read_queue_.empty()) {
    PW_ASYNC_STORE_WAKER(
        cx,
        read_waker_,
        "ForwardingChannel is waiting for incoming data from its peer");
    return async2::Pending();
  }

  read_waker_.Clear();
  auto read_data = std::move(read_queue_);
  read_queue_ = {};
  return read_data;
}

Status ForwardingChannel<DataType::kByte>::DoStageWrite(
    multibuf::MultiBuf&& data) PW_NO_LOCK_SAFETY_ANALYSIS {
  std::lock_guard lock(pair_.mutex_);
  if (data.empty()) {
    return OkStatus();  // no data, nothing to do
  }

  sibling_.read_queue_.PushSuffix(std::move(data));
  sibling_.read_waker_.Wake();
  return OkStatus();
}

async2::Poll<Status> ForwardingChannel<DataType::kByte>::DoPendClose(
    async2::Context&) PW_NO_LOCK_SAFETY_ANALYSIS {
  std::lock_guard lock(pair_.mutex_);
  sibling_.set_write_closed();  // No more writes from the other end
  read_queue_.Release();
  read_waker_.Clear();
  sibling_.read_waker_.Wake();
  return OkStatus();
}

}  // namespace pw::channel::internal
