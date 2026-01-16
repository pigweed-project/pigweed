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
#pragma once

#include "pw_allocator/unique_ptr.h"
#include "pw_function/function.h"
#include "pw_status/status.h"

namespace pw::grpc {

// Abstract interface for sending response buffers. Buffers are written to the
// response in the order they are queued.
class SendQueue {
 public:
  using ErrorHandler = pw::Function<void(pw::Status)>;

  virtual ~SendQueue() {}

  // Thread safe. Queues buffer to be sent on send thread. Returns false if
  // there was no queue space available from allocator.
  virtual bool QueueSend(UniquePtr<std::byte[]>&& buffer) = 0;

  // Sets callback to be called when write to socket fails. QueueSend must not
  // be called from this callback.
  virtual void set_on_error(ErrorHandler&& error_handler) = 0;

  // Runs this send queue.
  virtual void Run() = 0;

  // Stops the current Run() call.
  virtual void RequestStop() = 0;
};

// TODO(b/475261598): will be removed after transitioning
using SendQueueBase = SendQueue;

}  // namespace pw::grpc
