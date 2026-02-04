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

#include "pw_allocator/testing.h"
#include "pw_async2/dispatcher.h"
#include "pw_status/status.h"

// DOCSTAG: [pw_async2-examples-basic-manual]
#include "pw_async2/channel.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/poll.h"
#include "pw_async2/try.h"
#include "pw_log/log.h"

namespace {

using ::pw::async2::Context;
using ::pw::async2::Poll;
using ::pw::async2::Ready;
using ::pw::async2::ReceiveFuture;
using ::pw::async2::Receiver;
using ::pw::async2::Sender;
using ::pw::async2::SendFuture;
using ::pw::async2::Task;

// Receive then send that data asynchronously. If the receiver or sender
// isn't ready, the task suspends when `PW_TRY_READY_ASSIGN` returns
// `Pending()`.
class ForwardingTask final : public Task {
 public:
  ForwardingTask(Receiver<int> receiver, Sender<int> sender)
      : receiver_(std::move(receiver)),
        sender_(std::move(sender)),
        state_(kReceiving) {}

  Poll<> DoPend(Context& cx) final {
    receive_future_ = receiver_.Receive();

    switch (state_) {
      case kReceiving: {
        PW_TRY_READY_ASSIGN(std::optional<int> new_data,
                            receive_future_.Pend(cx));
        if (!new_data.has_value()) {
          PW_LOG_ERROR("Receive failed: channel has closed");
          return Ready();  // Completes the task.
        }
        // Start transmitting and switch to transmitting state.
        send_future_ = sender_.Send(*new_data);
        state_ = kTransmitting;
      }
        [[fallthrough]];
      case kTransmitting: {
        PW_TRY_READY_ASSIGN(bool sent, send_future_.Pend(cx));
        if (!sent) {
          PW_LOG_ERROR("Send failed: channel has closed");
        }
        return Ready();  // Completes the task.
      }
    }
  }

 private:
  // Can receive data async, tracking state in a future.
  Receiver<int> receiver_;
  ReceiveFuture<int> receive_future_;
  // Can send data async, tracking state in a future.
  Sender<int> sender_;
  SendFuture<int> send_future_;

  enum State { kReceiving, kTransmitting };
  State state_;
};

}  // namespace
// DOCSTAG: [pw_async2-examples-basic-manual]

namespace {

using ::pw::async2::ChannelStorage;
using ::pw::async2::CreateSpscChannel;
using ::pw::async2::DispatcherForTest;

TEST(ManualExample, Runs) {
  // DOCSTAG: [pw_async2-examples-basic-dispatcher]
  ChannelStorage<int, 1> storage_a;
  auto [handle_a, sender_a, receiver_a] = CreateSpscChannel(storage_a);
  ChannelStorage<int, 1> storage_b;
  auto [handle_b, sender_b, receiver_b] = CreateSpscChannel(storage_b);

  ForwardingTask task(std::move(receiver_a), std::move(sender_b));

  DispatcherForTest dispatcher;
  // Registers `task` to run on the dispatcher.
  dispatcher.Post(task);
  // Runs the dispatcher until all `Post`ed tasks are blocked.
  dispatcher.RunUntilStalled();
  // DOCSTAG: [pw_async2-examples-basic-dispatcher]

  EXPECT_EQ(sender_a.TrySend(42), pw::OkStatus());
  dispatcher.AllowBlocking();
  dispatcher.RunToCompletion();
  handle_a.Release();
  handle_b.Release();
  EXPECT_EQ(receiver_b.TryReceive().value(), 42);
}

}  // namespace
