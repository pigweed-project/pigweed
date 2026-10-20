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
#include <cstdint>
#include <optional>

#include "pw_allocator/allocator.h"
#include "pw_async2/dispatcher.h"
#include "pw_async2/poll.h"
#include "pw_async2/value_future.h"
#include "pw_buf/buf.h"
#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_sync/interrupt_spin_lock.h"
#include "pw_sync/lock_annotations.h"
#include "pw_transport/socket.h"
#include "pw_transport/transport.h"

namespace pw::rpc2::size_report {

Allocator& GetAllocator();

class MockConnection : public transport::ReliableDatagramSocketImpl {
 public:
  MockConnection() : ReliableDatagramSocketImpl(GetAllocator(), 1500) {}
  explicit MockConnection(Allocator& allocator)
      : ReliableDatagramSocketImpl(allocator, 1500) {}
  ~MockConnection() override;

  async2::Poll<pw::ConstBuf> DoRead() override
      PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    return async2::Pending();
  }

  std::optional<transport::WriteReservation> DoTryReserveWrite(size_t) override
      PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    return std::nullopt;
  }

  void DoClose() override PW_EXCLUSIVE_LOCKS_REQUIRED(*this) { MarkClosed(); }

  void lock() const PW_EXCLUSIVE_LOCK_FUNCTION() override { lock_.lock(); }
  void unlock() const PW_UNLOCK_FUNCTION() override { lock_.unlock(); }

 protected:
  bool DoCommitWrite(pw::Buf&&) override PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    return true;
  }
  void DoCancelWrite(pw::Buf&&) override PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {}

 private:
  mutable sync::InterruptSpinLock lock_;
};

class MockListener : public transport::ReliableDatagramListener {
 public:
  ~MockListener() override;

  /// Republished so that size report binaries can mint socket handles for a
  /// `MockConnection`; `ReliableDatagramSocket`'s constructor is private to
  /// transport implementations.
  using transport::ReliableDatagramListener::WrapSocket;

  transport::ReliableDatagramListener::AcceptFuture Accept() override {
    return accept_provider_.Get();
  }

 private:
  async2::ValueProvider<Result<transport::ReliableDatagramSocket>>
      accept_provider_;
};

uint32_t SetBaseline(uint32_t mask);

}  // namespace pw::rpc2::size_report
