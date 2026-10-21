// Copyright 2023 The Pigweed Authors
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

#include "pw_rpc/fuzz/engine.h"

#include <chrono>

#include "pw_containers/vector.h"
#include "pw_log/log.h"
#include "pw_rpc/benchmark.h"
#include "pw_rpc/internal/client_server_testing.h"
#include "pw_rpc/internal/client_server_testing_threaded.h"
#include "pw_rpc/internal/fake_channel_output.h"
#include "pw_thread/non_portable_test_thread_options.h"
#include "pw_unit_test/framework.h"

namespace pw::rpc::fuzz {
namespace {

using namespace std::literals::chrono_literals;

// Maximum time, in milliseconds, that can elapse without a call completing or
// being dropped in some way..
const chrono::SystemClock::duration kTimeout = 5s;

// These are fairly tight constraints in order to fit within the default
// `PW_UNIT_TEST_CONFIG_MEMORY_POOL_SIZE`.
constexpr size_t kMaxPackets = 128;
constexpr size_t kMaxPayloadSize = 64;

using BufferedChannelOutputBase =
    internal::test::FakeChannelOutputBuffer<kMaxPackets, kMaxPayloadSize>;

/// Channel output backed by a fixed buffer.
class BufferedChannelOutput : public BufferedChannelOutputBase {
 public:
  BufferedChannelOutput() : BufferedChannelOutputBase() {}
};

using FuzzerChannelOutputBase =
    internal::WatchableChannelOutput<BufferedChannelOutput,
                                     kMaxPayloadSize,
                                     kMaxPackets,
                                     kMaxPayloadSize>;

/// Channel output that can be waited on by the server.
class FuzzerChannelOutput : public FuzzerChannelOutputBase {
 public:
  explicit FuzzerChannelOutput(
      TestPacketProcessor&& server_packet_processor = nullptr,
      TestPacketProcessor&& client_packet_processor = nullptr)
      : FuzzerChannelOutputBase(std::move(server_packet_processor),
                                std::move(client_packet_processor)) {}
};

using FuzzerContextBase =
    internal::ClientServerTestContextThreaded<FuzzerChannelOutput,
                                              kMaxPayloadSize,
                                              kMaxPackets,
                                              kMaxPayloadSize>;
class FuzzerContext : public FuzzerContextBase {
 public:
  static constexpr uint32_t kChannelId = 1;

  explicit FuzzerContext(
      TestPacketProcessor&& server_packet_processor = nullptr,
      TestPacketProcessor&& client_packet_processor = nullptr)
      // TODO: b/290860904 - Replace TestOptionsThread0 with TestThreadContext.
      : FuzzerContextBase(thread::test::TestOptionsThread0(),
                          std::move(server_packet_processor),
                          std::move(client_packet_processor)) {}
};

class RpcFuzzTestingTest : public testing::Test {
 protected:
  void SetUp() override { context_.server().RegisterService(service_); }

  void Add(Action::Op op, size_t target, uint16_t value) {
    actions_.push_back(Action(op, target, value).Encode());
  }

  void Add(Action::Op op, size_t target, char val, size_t len) {
    actions_.push_back(Action(op, target, val, len).Encode());
  }

  void NextThread() { actions_.push_back(0); }

  void Run() {
    Fuzzer fuzzer(context_.client(), FuzzerContext::kChannelId);
    fuzzer.set_verbose(true);
    fuzzer.set_timeout(kTimeout);
    fuzzer.Run(actions_);
  }

  void TearDown() override { context_.server().UnregisterService(service_); }

 private:
  FuzzerContext context_;
  BenchmarkService service_;
  Vector<uint32_t, Fuzzer::kMaxActions> actions_;
};

// TODO: b/274437709 - Re-enable.
TEST_F(RpcFuzzTestingTest, DISABLED_SequentialRequests) {
  // Callback thread
  Add(Action::kWriteStream, 1, 'B', 1);
  Add(Action::kSkip, 0, 0);
  Add(Action::kWriteStream, 2, 'B', 2);
  Add(Action::kSkip, 0, 0);
  Add(Action::kWriteStream, 3, 'B', 3);
  Add(Action::kSkip, 0, 0);
  NextThread();

  // Thread 1
  Add(Action::kWriteStream, 0, 'A', 2);
  Add(Action::kWait, 1, 0);
  Add(Action::kWriteStream, 1, 'A', 4);
  NextThread();

  // Thread 2
  NextThread();
  Add(Action::kWait, 2, 0);
  Add(Action::kWriteStream, 2, 'A', 6);

  // Thread 3
  NextThread();
  Add(Action::kWait, 3, 0);

  Run();
}

// TODO: b/274437709 - Re-enable.
TEST_F(RpcFuzzTestingTest, DISABLED_SimultaneousRequests) {
  // Callback thread
  NextThread();

  // Thread 1
  Add(Action::kWriteUnary, 1, 'A', 1);
  Add(Action::kWait, 2, 0);
  NextThread();

  // Thread 2
  Add(Action::kWriteUnary, 2, 'B', 2);
  Add(Action::kWait, 3, 0);
  NextThread();

  // Thread 3
  Add(Action::kWriteUnary, 3, 'C', 3);
  Add(Action::kWait, 1, 0);
  NextThread();

  Run();
}

// TODO: b/274437709 - This test currently does not pass as it exhausts the fake
// channel. It will be re-enabled when the underlying stream is swapped for
// a pw_ring_buffer-based approach.
TEST_F(RpcFuzzTestingTest, DISABLED_CanceledRequests) {
  // Callback thread
  NextThread();

  // Thread 1
  for (size_t i = 0; i < 10; ++i) {
    Add(Action::kWriteUnary, i % 3, 'A', i);
  }
  Add(Action::kWait, 0, 0);
  Add(Action::kWait, 1, 0);
  Add(Action::kWait, 2, 0);
  NextThread();

  // Thread 2
  for (size_t i = 0; i < 10; ++i) {
    Add(Action::kCancel, i % 3, 0);
  }
  NextThread();

  // Thread 3
  NextThread();

  Run();
}

// TODO: b/274437709 - This test currently does not pass as it exhausts the fake
// channel. It will be re-enabled when the underlying stream is swapped for
// a pw_ring_buffer-based approach.
TEST_F(RpcFuzzTestingTest, DISABLED_AbandonedRequests) {
  // Callback thread
  NextThread();

  // Thread 1
  for (size_t i = 0; i < 10; ++i) {
    Add(Action::kWriteUnary, i % 3, 'A', i);
  }
  Add(Action::kWait, 0, 0);
  Add(Action::kWait, 1, 0);
  Add(Action::kWait, 2, 0);
  NextThread();

  // Thread 2
  for (size_t i = 0; i < 10; ++i) {
    Add(Action::kAbandon, i % 3, 0);
  }
  NextThread();

  // Thread 3
  NextThread();

  Run();
}

// TODO: b/274437709 - This test currently does not pass as it exhausts the fake
// channel. It will be re-enabled when the underlying stream is swapped for
// a pw_ring_buffer-based approach.
TEST_F(RpcFuzzTestingTest, DISABLED_SwappedRequests) {
  Vector<uint32_t, Fuzzer::kMaxActions> actions;
  // Callback thread
  NextThread();
  // Thread 1
  for (size_t i = 0; i < 10; ++i) {
    Add(Action::kWriteUnary, i % 3, 'A', i);
  }
  Add(Action::kWait, 0, 0);
  Add(Action::kWait, 1, 0);
  Add(Action::kWait, 2, 0);
  NextThread();
  // Thread 2
  for (size_t i = 0; i < 100; ++i) {
    auto j = i % 3;
    Add(Action::kSwap, j, static_cast<uint16_t>(j + 1));
  }
  NextThread();
  // Thread 3
  NextThread();

  Run();
}

// TODO: b/274437709 - This test currently does not pass as it exhausts the fake
// channel. It will be re-enabled when the underlying stream is swapped for
// a pw_ring_buffer-based approach.
TEST_F(RpcFuzzTestingTest, DISABLED_DestroyedRequests) {
  // Callback thread
  NextThread();

  // Thread 1
  for (size_t i = 0; i < 100; ++i) {
    Add(Action::kWriteUnary, i % 3, 'A', i);
  }
  Add(Action::kWait, 0, 0);
  Add(Action::kWait, 1, 0);
  Add(Action::kWait, 2, 0);
  NextThread();

  // Thread 2
  for (size_t i = 0; i < 100; ++i) {
    Add(Action::kDestroy, i % 3, 0);
  }
  NextThread();

  // Thread 3
  NextThread();

  Run();
}

}  // namespace
}  // namespace pw::rpc::fuzz
