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

#include <cstddef>

#include "pw_async2/dispatcher.h"
#include "pw_channel/loopback_channel.h"
#include "pw_log/log.h"
#include "pw_multibuf/simple_allocator.h"
#include "pw_system/system.h"
#include "pw_unit_test/framework.h"

namespace {

class : public pw::async2::Task {
  pw::async2::Poll<> DoPend(pw::async2::Context&) override {
    return pw::async2::Ready();
  }
} my_task;

// DOCSTAG: [pw_system-async-example-main]
pw::channel::ByteReaderWriter& Init() {
  // First, do any required low-level platform initialization.

  // Initialize a channel to handle pw_system communications, including for
  // pw_rpc. This example uses LoopbackByteChannel, but a channel that actually
  // transmits data should be used instead.
  static std::byte channel_buffer[128];
  pw::multibuf::SimpleAllocator alloc(channel_buffer, pw::System().allocator());
  static pw::channel::LoopbackByteChannel channel(alloc);

  // Post any async tasks that should run. These will execute after calling
  // pw::SystemStart.
  pw::System().dispatcher().Post(my_task);

  // As needed, start threads to run user code. Or, register a task to start
  // threads after pw::SystemStart.

  // When all initialization is done, return a reference to the channel.
  return channel.channel();
}

int main() {
  // Perform all initialization in a function. It returns back a reference to
  // a non-stack allocated io_channel to use. By doing all initialization in
  // another function like this, we help ensure that the stack contains almost
  // nothing that can be trashed by the next call.
  pw::channel::ByteReaderWriter& io_channel = Init();

  // Start running the pw::System threads and dispatcher. This function call
  // never return, and because of that, some backends may trash the current
  // stack contents, including whatever is on it.
  pw::system::StartAndClobberTheStack(io_channel);

  return 0;
}
// DOCSTAG: [pw_system-async-example-main]

TEST(PwSystem, ReferToExampleMain) {
  if (false) {
    main();
  }
}

}  // namespace
