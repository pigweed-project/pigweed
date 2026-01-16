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

#include "pw_allocator/allocator.h"
#include "pw_async/dispatcher.h"
#include "pw_async_basic/dispatcher.h"
#include "pw_containers/dynamic_deque.h"
#include "pw_function/function.h"
#include "pw_grpc/send_queue.h"
#include "pw_status/status.h"
#include "pw_stream/stream.h"
#include "pw_sync/lock_annotations.h"
#include "pw_sync/mutex.h"
#include "pw_thread/thread_core.h"

namespace pw::grpc {

// DefaultSendQueue is a default implementation of SendQueueBase that uses a
// queue+thread to serialize buffer write onto a blocking stream. The queue
// length is implicitly limited by the allocator that creates the buffers to
// send.
class DefaultSendQueue : public SendQueue {
 public:
  DefaultSendQueue(stream::ReaderWriter& socket, Allocator& allocator)
      : socket_(socket),
        send_task_(pw::bind_member<&DefaultSendQueue::ProcessSendQueue>(this)),
        queue_(allocator) {}

  // Thread safe. Queues buffer to be sent on send thread. Returns false if
  // there was no queue space available from allocator.
  bool QueueSend(UniquePtr<std::byte[]>&& buffer) override
      PW_LOCKS_EXCLUDED(send_mutex_);

  // ThreadCore impl.
  void Run() override { send_dispatcher_.Run(); }
  // Call before attempting to join thread.
  void RequestStop() override { send_dispatcher_.RequestStop(); }

  // Set callback to be called when write to socket fails. QueueSend should not
  // be called from this callback.
  void set_on_error(ErrorHandler&& error_handler) override
      PW_LOCKS_EXCLUDED(send_mutex_);

 private:
  void ProcessSendQueue(async::Context& context, Status status)
      PW_LOCKS_EXCLUDED(send_mutex_);

  UniquePtr<std::byte[]> PopNext() PW_LOCKS_EXCLUDED(send_mutex_);
  void NotifyOnError(Status status) PW_LOCKS_EXCLUDED(send_mutex_);

  stream::ReaderWriter& socket_;
  async::BasicDispatcher send_dispatcher_;
  async::Task send_task_;
  ErrorHandler on_error_;
  sync::Mutex send_mutex_;
  DynamicDeque<UniquePtr<std::byte[]>> queue_ PW_GUARDED_BY(send_mutex_);
};

}  // namespace pw::grpc
