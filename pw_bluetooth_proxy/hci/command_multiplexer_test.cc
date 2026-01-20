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

#include "pw_bluetooth_proxy/hci/command_multiplexer.h"

#include <chrono>
#include <cstdint>

#include "pw_allocator/testing.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/pend_func_task.h"
#include "pw_async2/simulated_time_provider.h"
#include "pw_chrono/system_clock.h"
#include "pw_containers/vector.h"
#include "pw_function/function.h"
#include "pw_unit_test/framework.h"

namespace pw::bluetooth::proxy::hci {
namespace {

constexpr size_t kAllocatorSize = 256;

TEST(IdentifierTest, UniqueIdentifier) {
  using Int = uint8_t;
  constexpr size_t kMax = std::numeric_limits<Int>::max();

  IdentifierMint<uint8_t> mint;
  pw::Vector<Identifier<Int>, kMax> ids;
  for (size_t i = 0; i < kMax; ++i) {
    auto new_id = mint.MintId([&](Int candidate) {
      return std::find(ids.begin(), ids.end(), candidate) != ids.end();
    });
    ASSERT_TRUE(new_id.has_value());
    ASSERT_TRUE(new_id->is_valid());

    EXPECT_EQ(new_id->value(), i + 1);
    EXPECT_EQ(std::find(ids.begin(), ids.end(), new_id->value()), ids.end());

    ids.push_back(std::move(*new_id));
    EXPECT_FALSE(new_id->is_valid());
  }

  {
    // Allocation exhausted, should return std::nullopt.
    auto result = mint.MintId([&](Int candidate) {
      return std::find(ids.begin(), ids.end(), candidate) != ids.end();
    });
    EXPECT_FALSE(result.has_value());
  }

  ids.erase(std::remove(ids.begin(), ids.end(), 42), ids.end());

  {
    // Allocation opened up at 42, confirm allocation.
    auto result = mint.MintId([&](Int candidate) {
      return std::find(ids.begin(), ids.end(), candidate) != ids.end();
    });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->value(), 42);
    ids.push_back(std::move(*result));
  }

  {
    // Allocation exhausted again, should return std::nullopt.
    auto result = mint.MintId([&](Int candidate) {
      return std::find(ids.begin(), ids.end(), candidate) != ids.end();
    });
    EXPECT_FALSE(result.has_value());
  }
}

class CommandMultiplexerTest : public ::testing::Test {
 public:
  static constexpr chrono::SystemClock::duration kTestTimeoutDuration =
      std::chrono::milliseconds(2);

  // Because we want to test both kinds of CommandMultiplexer (async vs cb),
  // some tests are implemented outside the fixture, this provides access to the
  // appropriate properties and functions.
  //
  // This is required because not all pw_unit_test backends support `TEST_P`
  // style test parameterization.
  class Accessor {
   public:
    CommandMultiplexer& hci_cmd_mux() { return hci_cmd_mux_; }
    Allocator& allocator() { return test_.allocator(); }
    async2::DispatcherForTest& dispatcher() { return test_.dispatcher(); }

    pw::DynamicDeque<MultiBuf::Instance>& packets_to_host() {
      return test_.packets_to_host();
    }
    pw::DynamicDeque<MultiBuf::Instance>& packets_to_controller() {
      return test_.packets_to_controller();
    }

   private:
    Accessor(CommandMultiplexerTest& test, CommandMultiplexer& hci_cmd_mux)
        : test_(test), hci_cmd_mux_(hci_cmd_mux) {}
    friend CommandMultiplexerTest;

    CommandMultiplexerTest& test_;
    CommandMultiplexer& hci_cmd_mux_;
  };

 protected:
  CommandMultiplexer& hci_cmd_mux_async2() {
    if (!hci_cmd_mux_async2_.has_value()) {
      hci_cmd_mux_async2_.emplace(allocator_,
                                  make_send_to_host_cb(),
                                  make_send_to_controller_cb(),
                                  time_provider_);
    }
    return *hci_cmd_mux_async2_;
  }

  CommandMultiplexer& hci_cmd_mux_timer() {
    if (!hci_cmd_mux_async2_.has_value()) {
      Function<void()> timeout_fn = [this] { OnTimeout(); };
      hci_cmd_mux_async2_.emplace(allocator_,
                                  make_send_to_host_cb(),
                                  make_send_to_controller_cb(),
                                  std::move(timeout_fn),
                                  kTestTimeoutDuration);
    }
    return *hci_cmd_mux_async2_;
  }

  Allocator& allocator() { return allocator_; }
  async2::DispatcherForTest& dispatcher() { return dispatcher_; }

  Accessor accessor_async2() { return Accessor(*this, hci_cmd_mux_async2()); }

  Accessor accessor_timer() { return Accessor(*this, hci_cmd_mux_timer()); }

  pw::DynamicDeque<MultiBuf::Instance>& packets_to_host() {
    return packets_to_host_;
  }
  pw::DynamicDeque<MultiBuf::Instance>& packets_to_controller() {
    return packets_to_controller_;
  }

 private:
  void OnTimeout() {}

  Function<void(MultiBuf::Instance&&)> make_send_to_host_cb() {
    return [this](MultiBuf::Instance&& packet) {
      // Intentionally fail assert if allocation fails, this is test code.
      packets_to_host_.push_back(std::move(packet));
    };
  }

  Function<void(MultiBuf::Instance&&)> make_send_to_controller_cb() {
    return [this](MultiBuf::Instance&& packet) {
      // Intentionally fail assert if allocation fails, this is test code.
      packets_to_controller_.push_back(std::move(packet));
    };
  }

  async2::DispatcherForTest dispatcher_{};
  async2::SimulatedTimeProvider<chrono::SystemClock> time_provider_{};
  pw::allocator::test::AllocatorForTest<kAllocatorSize> allocator_{};

  pw::DynamicDeque<MultiBuf::Instance> packets_to_host_{allocator_};
  pw::DynamicDeque<MultiBuf::Instance> packets_to_controller_{allocator_};

  std::optional<CommandMultiplexer> hci_cmd_mux_async2_{std::nullopt};
  std::optional<CommandMultiplexer> hci_cmd_mux_timer_{std::nullopt};
};

using Accessor = CommandMultiplexerTest::Accessor;

TEST_F(CommandMultiplexerTest, AsyncTimeout) {
  auto& hci_cmd_mux = hci_cmd_mux_async2();

  std::optional<Result<async2::Poll<>>> pend_result;
  async2::PendFuncTask task{[&](async2::Context& cx) {
    pend_result = hci_cmd_mux.PendCommandTimeout(cx);
    return async2::Ready();
  }};

  dispatcher().Post(task);
  dispatcher().RunToCompletion();

  ASSERT_TRUE(pend_result.has_value());
  ASSERT_FALSE(pend_result->ok());
  // Not yet implemented.
  EXPECT_EQ(pend_result->status(), Status::Unimplemented());
}

TEST_F(CommandMultiplexerTest, AsyncTimeoutFailsSync) {
  auto& hci_cmd_mux = hci_cmd_mux_timer();

  std::optional<Result<async2::Poll<>>> pend_result;
  async2::PendFuncTask task{[&](async2::Context& cx) {
    pend_result = hci_cmd_mux.PendCommandTimeout(cx);
    return async2::Ready();
  }};

  dispatcher().Post(task);
  dispatcher().RunToCompletion();

  ASSERT_TRUE(pend_result.has_value());
  ASSERT_FALSE(pend_result->ok());
  EXPECT_EQ(pend_result->status(), Status::Unimplemented());
}

void TestSendCommand(Accessor test) {
  MultiBuf::Instance buffer(test.allocator());
  // Not yet implemented.
  auto result = test.hci_cmd_mux().SendCommand({std::move(buffer)}, nullptr);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().status(), Status::Unimplemented());
}

TEST_F(CommandMultiplexerTest, SendCommandAsync) {
  TestSendCommand(accessor_async2());
}
TEST_F(CommandMultiplexerTest, SendCommandTimer) {
  TestSendCommand(accessor_timer());
}

void TestSendEvent(Accessor test) {
  MultiBuf::Instance buffer(test.allocator());
  // Not yet implemented.
  auto result = test.hci_cmd_mux().SendEvent({std::move(buffer)});
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().status(), Status::Unimplemented());
}

TEST_F(CommandMultiplexerTest, SendEventAsync) {
  TestSendEvent(accessor_async2());
}
TEST_F(CommandMultiplexerTest, SendEventTimer) {
  TestSendEvent(accessor_timer());
}

void TestRegisterEventInterceptor(Accessor test) {
  // Register an interceptor.
  auto result1 = test.hci_cmd_mux().RegisterEventInterceptor(
      emboss::EventCode::HARDWARE_ERROR, nullptr);
  EXPECT_TRUE(result1.ok());

  // Register a second interceptor.
  auto result2 = test.hci_cmd_mux().RegisterEventInterceptor(
      emboss::EventCode::INQUIRY_COMPLETE, nullptr);
  EXPECT_TRUE(result2.ok());

  // Ensure we can't register to an already-existing interceptor.
  auto result3 = test.hci_cmd_mux().RegisterEventInterceptor(
      emboss::EventCode::HARDWARE_ERROR, nullptr);
  ASSERT_FALSE(result3.ok());
  EXPECT_EQ(result3.status(), Status::AlreadyExists());

  // Reset result1, allowing us to register a different interceptor for the
  // same code.
  result1 = Status::Cancelled();
  auto result4 = test.hci_cmd_mux().RegisterEventInterceptor(
      emboss::EventCode::HARDWARE_ERROR, nullptr);
  EXPECT_TRUE(result4.ok());

  // Reset both active interceptors, clearing the maps.
  result2 = Status::Cancelled();
  result4 = Status::Cancelled();

  // Now register two more interceptors to ensure clearing the maps worked.
  auto result5 = test.hci_cmd_mux().RegisterEventInterceptor(
      emboss::EventCode::HARDWARE_ERROR, nullptr);
  EXPECT_TRUE(result5.ok());
  auto result6 = test.hci_cmd_mux().RegisterEventInterceptor(
      emboss::EventCode::INQUIRY_COMPLETE, nullptr);
  EXPECT_TRUE(result6.ok());
}

TEST_F(CommandMultiplexerTest, RegisterEventInterceptorAsync) {
  TestRegisterEventInterceptor(accessor_async2());
}
TEST_F(CommandMultiplexerTest, RegisterEventInterceptorTimer) {
  TestRegisterEventInterceptor(accessor_timer());
}

void TestRegisterCommandInterceptor(Accessor test) {
  // Register an interceptor.
  auto result1 = test.hci_cmd_mux().RegisterCommandInterceptor(
      emboss::OpCode::INQUIRY, nullptr);
  EXPECT_TRUE(result1.ok());

  // Register a second interceptor.
  auto result2 = test.hci_cmd_mux().RegisterCommandInterceptor(
      emboss::OpCode::DISCONNECT, nullptr);
  EXPECT_TRUE(result2.ok());

  // Ensure we can't register to an already-existing interceptor.
  auto result3 = test.hci_cmd_mux().RegisterCommandInterceptor(
      emboss::OpCode::INQUIRY, nullptr);
  ASSERT_FALSE(result3.ok());
  EXPECT_EQ(result3.status(), Status::AlreadyExists());

  // Reset result1, allowing us to register a different interceptor for the
  // same code.
  result1 = Status::Cancelled();
  auto result4 = test.hci_cmd_mux().RegisterCommandInterceptor(
      emboss::OpCode::INQUIRY, nullptr);
  EXPECT_TRUE(result4.ok());

  // Reset both active interceptors, clearing the maps.
  result2 = Status::Cancelled();
  result4 = Status::Cancelled();

  // Now register two more interceptors to ensure clearing the maps worked.
  auto result5 = test.hci_cmd_mux().RegisterCommandInterceptor(
      emboss::OpCode::INQUIRY, nullptr);
  EXPECT_TRUE(result5.ok());
  auto result6 = test.hci_cmd_mux().RegisterCommandInterceptor(
      emboss::OpCode::DISCONNECT, nullptr);
  EXPECT_TRUE(result6.ok());
}

TEST_F(CommandMultiplexerTest, RegisterCommandInterceptorAsync) {
  TestRegisterCommandInterceptor(accessor_async2());
}
TEST_F(CommandMultiplexerTest, RegisterCommandInterceptorTimer) {
  TestRegisterCommandInterceptor(accessor_timer());
}

void TestInterceptCommands(Accessor test) {
  {
    std::array<std::byte, 4> reset_packet_bytes{
        // Packet type (command)
        std::byte(0x01),
        // OpCode (Reset)
        std::byte(0x03),
        std::byte(0x0C),
        // Parameter size
        std::byte(0x00),
    };

    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), reset_packet_bytes);
    test.hci_cmd_mux().HandleH4FromHost(std::move(buf));

    // Forwards packets if no interceptor.
    ASSERT_EQ(test.packets_to_controller().size(), 1u);
    std::array<std::byte, 4> out;
    test.packets_to_controller().front()->CopyTo(out);
    EXPECT_EQ(reset_packet_bytes, out);
    test.packets_to_controller().pop_front();
  }

  // Register an interceptor (takes buffer, continues intercepting)
  std::optional<MultiBuf::Instance> intercepted;
  auto result = test.hci_cmd_mux().RegisterCommandInterceptor(
      emboss::OpCode::RESET,
      [&](CommandPacket&& packet)
          -> CommandMultiplexer::CommandInterceptorReturn {
        intercepted = std::move(packet.buffer);
        return {};
      });
  ASSERT_TRUE(result.ok());

  {
    std::array<std::byte, 4> reset_packet_bytes{
        // Packet type (command)
        std::byte(0x01),
        // OpCode (Reset)
        std::byte(0x03),
        std::byte(0x0C),
        // Parameter size
        std::byte(0x00),
    };

    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), reset_packet_bytes);
    test.hci_cmd_mux().HandleH4FromHost(std::move(buf));

    // Ensure not forwarded.
    EXPECT_TRUE(test.packets_to_controller().empty());
    ASSERT_TRUE(intercepted.has_value());
    std::array<std::byte, 4> out;
    intercepted.value()->CopyTo(out);
    EXPECT_EQ(reset_packet_bytes, out);
  }

  // Clear the result, but keep interceptor active.
  intercepted = std::nullopt;

  // Ensure continuing to intercept.
  {
    std::array<std::byte, 4> reset_packet_bytes{
        // Packet type (command)
        std::byte(0x01),
        // OpCode (Reset)
        std::byte(0x03),
        std::byte(0x0C),
        // Parameter size
        std::byte(0x00),
    };

    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), reset_packet_bytes);
    test.hci_cmd_mux().HandleH4FromHost(std::move(buf));

    // Ensure not forwarded.
    EXPECT_TRUE(test.packets_to_controller().empty());
    ASSERT_TRUE(intercepted.has_value());
    std::array<std::byte, 4> out;
    intercepted.value()->CopyTo(out);
    EXPECT_EQ(reset_packet_bytes, out);
  }

  result = Status::Cancelled();  // Unregister the interceptor.
  intercepted = std::nullopt;

  {
    std::array<std::byte, 4> reset_packet_bytes{
        // Packet type (command)
        std::byte(0x01),
        // OpCode (Reset)
        std::byte(0x03),
        std::byte(0x0C),
        // Parameter size
        std::byte(0x00),
    };

    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), reset_packet_bytes);
    test.hci_cmd_mux().HandleH4FromHost(std::move(buf));

    // Forwards packets if no interceptor.
    ASSERT_EQ(test.packets_to_controller().size(), 1u);
    EXPECT_FALSE(intercepted.has_value());
    std::array<std::byte, 4> out;
    test.packets_to_controller().front()->CopyTo(out);
    EXPECT_EQ(reset_packet_bytes, out);
    test.packets_to_controller().pop_front();
  }

  // Register an interceptor (Does not take buffer, continues intercepting)
  bool peeked = false;
  result = test.hci_cmd_mux().RegisterCommandInterceptor(
      emboss::OpCode::RESET,
      [&](CommandPacket&& packet)
          -> CommandMultiplexer::CommandInterceptorReturn {
        peeked = true;
        return {std::move(packet)};
      });
  ASSERT_TRUE(result.ok());

  {
    std::array<std::byte, 4> reset_packet_bytes{
        // Packet type (command)
        std::byte(0x01),
        // OpCode (Reset)
        std::byte(0x03),
        std::byte(0x0C),
        // Parameter size
        std::byte(0x00),
    };

    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), reset_packet_bytes);
    test.hci_cmd_mux().HandleH4FromHost(std::move(buf));

    // Forwards packets returned from interceptor.
    ASSERT_EQ(test.packets_to_controller().size(), 1u);
    EXPECT_TRUE(peeked);
    std::array<std::byte, 4> out;
    test.packets_to_controller().front()->CopyTo(out);
    EXPECT_EQ(reset_packet_bytes, out);
    test.packets_to_controller().pop_front();
  }

  peeked = false;

  // Ensure continuing to intercept.
  {
    std::array<std::byte, 4> reset_packet_bytes{
        // Packet type (command)
        std::byte(0x01),
        // OpCode (Reset)
        std::byte(0x03),
        std::byte(0x0C),
        // Parameter size
        std::byte(0x00),
    };

    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), reset_packet_bytes);
    test.hci_cmd_mux().HandleH4FromHost(std::move(buf));

    // Forwards packets returned from interceptor.
    ASSERT_EQ(test.packets_to_controller().size(), 1u);
    EXPECT_TRUE(peeked);
    std::array<std::byte, 4> out;
    test.packets_to_controller().front()->CopyTo(out);
    EXPECT_EQ(reset_packet_bytes, out);
    test.packets_to_controller().pop_front();
  }

  // Replace with an interceptor that removes itself.
  result = Status::Cancelled();
  struct {
    decltype(result)& result_;
    decltype(intercepted)& intercepted_;
  } capture{
      .result_ = result,
      .intercepted_ = intercepted,
  };
  result = test.hci_cmd_mux().RegisterCommandInterceptor(
      emboss::OpCode::RESET,
      [&capture](CommandPacket&& packet)
          -> CommandMultiplexer::CommandInterceptorReturn {
        capture.intercepted_ = std::move(packet.buffer);
        return {.action = CommandMultiplexer::RemoveThisInterceptor{
                    std::move(capture.result_.value().id())}};
      });

  {
    std::array<std::byte, 4> reset_packet_bytes{
        // Packet type (command)
        std::byte(0x01),
        // OpCode (Reset)
        std::byte(0x03),
        std::byte(0x0C),
        // Parameter size
        std::byte(0x00),
    };

    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), reset_packet_bytes);
    test.hci_cmd_mux().HandleH4FromHost(std::move(buf));

    // Ensure not forwarded.
    EXPECT_TRUE(test.packets_to_controller().empty());
    ASSERT_TRUE(intercepted.has_value());
    std::array<std::byte, 4> out;
    intercepted.value()->CopyTo(out);
    EXPECT_EQ(reset_packet_bytes, out);
  }

  intercepted = std::nullopt;

  // Ensure the next one is not intercepted.
  {
    std::array<std::byte, 4> reset_packet_bytes{
        // Packet type (command)
        std::byte(0x01),
        // OpCode (Reset)
        std::byte(0x03),
        std::byte(0x0C),
        // Parameter size
        std::byte(0x00),
    };

    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), reset_packet_bytes);
    test.hci_cmd_mux().HandleH4FromHost(std::move(buf));

    // Forwards packets if no interceptor.
    ASSERT_EQ(test.packets_to_controller().size(), 1u);
    EXPECT_FALSE(intercepted.has_value());
    std::array<std::byte, 4> out;
    test.packets_to_controller().front()->CopyTo(out);
    EXPECT_EQ(reset_packet_bytes, out);
    test.packets_to_controller().pop_front();
  }
}

TEST_F(CommandMultiplexerTest, InterceptCommandsAsync) {
  TestInterceptCommands(accessor_async2());
}
TEST_F(CommandMultiplexerTest, InterceptCommandsTimer) {
  TestInterceptCommands(accessor_timer());
}

void TestInterceptEvents(Accessor test) {
  {
    std::array<std::byte, 6> command_complete_packet_bytes{
        // Packet type (event)
        std::byte(0x04),
        // Event code (Command Complete)
        std::byte(0x0E),
        // Parameter size
        std::byte(0x03),
        // Num_HCI_Command_Packets
        std::byte(0x01),
        // OpCode (Reset)
        std::byte(0x03),
        std::byte(0x0C),
    };

    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), command_complete_packet_bytes);
    test.hci_cmd_mux().HandleH4FromController(std::move(buf));

    // Forwards packets if no interceptor.
    ASSERT_EQ(test.packets_to_host().size(), 1u);
    std::array<std::byte, 6> out;
    test.packets_to_host().front()->CopyTo(out);
    EXPECT_EQ(command_complete_packet_bytes, out);
    test.packets_to_host().pop_front();
  }

  // Register an interceptor (takes buffer, continues intercepting)
  std::optional<MultiBuf::Instance> intercepted;
  auto result = test.hci_cmd_mux().RegisterEventInterceptor(
      CommandCompleteOpcode{emboss::OpCode::RESET},
      [&](EventPacket&& packet) -> CommandMultiplexer::EventInterceptorReturn {
        intercepted = std::move(packet.buffer);
        return {};
      });
  ASSERT_TRUE(result.ok());

  {
    std::array<std::byte, 6> command_complete_packet_bytes{
        // Packet type (event)
        std::byte(0x04),
        // Event code (Command Complete)
        std::byte(0x0E),
        // Parameter size
        std::byte(0x03),
        // Num_HCI_Command_Packets
        std::byte(0x01),
        // OpCode (Reset)
        std::byte(0x03),
        std::byte(0x0C),
    };

    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), command_complete_packet_bytes);
    test.hci_cmd_mux().HandleH4FromController(std::move(buf));

    // Ensure not forwarded.
    EXPECT_TRUE(test.packets_to_host().empty());
    ASSERT_TRUE(intercepted.has_value());
    std::array<std::byte, 6> out;
    intercepted.value()->CopyTo(out);
    EXPECT_EQ(command_complete_packet_bytes, out);
  }

  // Clear the result, but keep interceptor active.
  intercepted = std::nullopt;

  // Ensure continuing to intercept.
  {
    std::array<std::byte, 6> command_complete_packet_bytes{
        // Packet type (event)
        std::byte(0x04),
        // Event code (Command Complete)
        std::byte(0x0E),
        // Parameter size
        std::byte(0x03),
        // Num_HCI_Command_Packets
        std::byte(0x01),
        // OpCode (Reset)
        std::byte(0x03),
        std::byte(0x0C),
    };

    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), command_complete_packet_bytes);
    test.hci_cmd_mux().HandleH4FromController(std::move(buf));

    // Ensure not forwarded.
    EXPECT_TRUE(test.packets_to_host().empty());
    ASSERT_TRUE(intercepted.has_value());
    std::array<std::byte, 6> out;
    intercepted.value()->CopyTo(out);
    EXPECT_EQ(command_complete_packet_bytes, out);
  }

  result = Status::Cancelled();  // Unregister the interceptor.
  intercepted = std::nullopt;

  {
    std::array<std::byte, 6> command_complete_packet_bytes{
        // Packet type (event)
        std::byte(0x04),
        // Event code (Command Complete)
        std::byte(0x0E),
        // Parameter size
        std::byte(0x03),
        // Num_HCI_Command_Packets
        std::byte(0x01),
        // OpCode (Reset)
        std::byte(0x03),
        std::byte(0x0C),
    };

    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), command_complete_packet_bytes);
    test.hci_cmd_mux().HandleH4FromController(std::move(buf));

    // Forwards packets if no interceptor.
    ASSERT_EQ(test.packets_to_host().size(), 1u);
    EXPECT_FALSE(intercepted.has_value());
    std::array<std::byte, 6> out;
    test.packets_to_host().front()->CopyTo(out);
    EXPECT_EQ(command_complete_packet_bytes, out);
    test.packets_to_host().pop_front();
  }

  // Register an interceptor (Does not take buffer, continues intercepting)
  bool peeked = false;
  result = test.hci_cmd_mux().RegisterEventInterceptor(
      CommandCompleteOpcode{emboss::OpCode::RESET},
      [&](EventPacket&& packet) -> CommandMultiplexer::EventInterceptorReturn {
        peeked = true;
        return {std::move(packet)};
      });
  ASSERT_TRUE(result.ok());

  {
    std::array<std::byte, 6> command_complete_packet_bytes{
        // Packet type (event)
        std::byte(0x04),
        // Event code (Command Complete)
        std::byte(0x0E),
        // Parameter size
        std::byte(0x03),
        // Num_HCI_Command_Packets
        std::byte(0x01),
        // OpCode (Reset)
        std::byte(0x03),
        std::byte(0x0C),
    };

    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), command_complete_packet_bytes);
    test.hci_cmd_mux().HandleH4FromController(std::move(buf));

    // Forwards packets returned from interceptor.
    ASSERT_EQ(test.packets_to_host().size(), 1u);
    EXPECT_TRUE(peeked);
    std::array<std::byte, 6> out;
    test.packets_to_host().front()->CopyTo(out);
    EXPECT_EQ(command_complete_packet_bytes, out);
    test.packets_to_host().pop_front();
  }

  peeked = false;

  // Ensure continuing to intercept.
  {
    std::array<std::byte, 6> command_complete_packet_bytes{
        // Packet type (event)
        std::byte(0x04),
        // Event code (Command Complete)
        std::byte(0x0E),
        // Parameter size
        std::byte(0x03),
        // Num_HCI_Command_Packets
        std::byte(0x01),
        // OpCode (Reset)
        std::byte(0x03),
        std::byte(0x0C),
    };

    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), command_complete_packet_bytes);
    test.hci_cmd_mux().HandleH4FromController(std::move(buf));

    // Forwards packets returned from interceptor.
    ASSERT_EQ(test.packets_to_host().size(), 1u);
    EXPECT_TRUE(peeked);
    std::array<std::byte, 6> out;
    test.packets_to_host().front()->CopyTo(out);
    EXPECT_EQ(command_complete_packet_bytes, out);
    test.packets_to_host().pop_front();
  }

  // Replace with an interceptor that removes itself.
  result = Status::Cancelled();
  struct {
    decltype(result)& result_;
    decltype(intercepted)& intercepted_;
  } capture{
      .result_ = result,
      .intercepted_ = intercepted,
  };
  result = test.hci_cmd_mux().RegisterEventInterceptor(
      CommandCompleteOpcode{emboss::OpCode::RESET},
      [&capture](
          EventPacket&& packet) -> CommandMultiplexer::EventInterceptorReturn {
        capture.intercepted_ = std::move(packet.buffer);
        return {.action = CommandMultiplexer::RemoveThisInterceptor{
                    std::move(capture.result_.value().id())}};
      });

  {
    std::array<std::byte, 6> command_complete_packet_bytes{
        // Packet type (event)
        std::byte(0x04),
        // Event code (Command Complete)
        std::byte(0x0E),
        // Parameter size
        std::byte(0x03),
        // Num_HCI_Command_Packets
        std::byte(0x01),
        // OpCode (Reset)
        std::byte(0x03),
        std::byte(0x0C),
    };

    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), command_complete_packet_bytes);
    test.hci_cmd_mux().HandleH4FromController(std::move(buf));

    // Ensure not forwarded.
    EXPECT_TRUE(test.packets_to_host().empty());
    ASSERT_TRUE(intercepted.has_value());
    std::array<std::byte, 6> out;
    intercepted.value()->CopyTo(out);
    EXPECT_EQ(command_complete_packet_bytes, out);
  }

  intercepted = std::nullopt;

  // Ensure the next one is not intercepted.
  {
    std::array<std::byte, 6> command_complete_packet_bytes{
        // Packet type (event)
        std::byte(0x04),
        // Event code (Command Complete)
        std::byte(0x0E),
        // Parameter size
        std::byte(0x03),
        // Num_HCI_Command_Packets
        std::byte(0x01),
        // OpCode (Reset)
        std::byte(0x03),
        std::byte(0x0C),
    };

    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), command_complete_packet_bytes);
    test.hci_cmd_mux().HandleH4FromController(std::move(buf));

    // Forwards packets if no interceptor.
    ASSERT_EQ(test.packets_to_host().size(), 1u);
    EXPECT_FALSE(intercepted.has_value());
    std::array<std::byte, 6> out;
    test.packets_to_host().front()->CopyTo(out);
    EXPECT_EQ(command_complete_packet_bytes, out);
    test.packets_to_host().pop_front();
  }

  result = Status::Cancelled();  // Unregister the interceptor.
  // Try to register an interceptor for Vendor Debug.
  result = test.hci_cmd_mux().RegisterEventInterceptor(
      emboss::EventCode::VENDOR_DEBUG,
      [&](EventPacket&& packet) -> CommandMultiplexer::EventInterceptorReturn {
        intercepted = std::move(packet.buffer);
        return {};
      });

  // Should fail, not allowed.
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.status(), Status::InvalidArgument());

  {
    std::array<std::byte, 6> vendor_debug_packet_bytes{
        // Packet type (event)
        std::byte(0x04),
        // Event code (Vendor Debug)
        std::byte(0xFF),
        // Parameter size
        std::byte(0x03),
        // Subevent code (0x01)
        std::byte(0x01),
        std::byte(0x00),
        std::byte(0x00),
    };

    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), vendor_debug_packet_bytes);
    test.hci_cmd_mux().HandleH4FromController(std::move(buf));

    // Forwards packets if no interceptor.
    ASSERT_EQ(test.packets_to_host().size(), 1u);
    std::array<std::byte, 6> out;
    test.packets_to_host().front()->CopyTo(out);
    EXPECT_EQ(vendor_debug_packet_bytes, out);
    test.packets_to_host().pop_front();
  }

  result = Status::Cancelled();  // Unregister the interceptor.
  // Register an interceptor for vendor debug subevent 0x01.
  result = test.hci_cmd_mux().RegisterEventInterceptor(
      VendorDebugSubEventCode{0x01},
      [&](EventPacket&& packet) -> CommandMultiplexer::EventInterceptorReturn {
        intercepted = std::move(packet.buffer);
        return {};
      });
  ASSERT_TRUE(result.ok());

  {
    std::array<std::byte, 6> vendor_debug_packet_bytes{
        // Packet type (event)
        std::byte(0x04),
        // Event code (Vendor Debug)
        std::byte(0xFF),
        // Parameter size
        std::byte(0x03),
        // Subevent code (0x01)
        std::byte(0x01),
        std::byte(0x00),
        std::byte(0x00),
    };

    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), vendor_debug_packet_bytes);
    test.hci_cmd_mux().HandleH4FromController(std::move(buf));

    // Ensure not forwarded.
    EXPECT_TRUE(test.packets_to_host().empty());
    ASSERT_TRUE(intercepted.has_value());
    std::array<std::byte, 6> out;
    intercepted.value()->CopyTo(out);
    EXPECT_EQ(vendor_debug_packet_bytes, out);
  }

  intercepted = std::nullopt;

  // Test a different vendor debug subevent code (should not be intercepted).
  {
    std::array<std::byte, 6> vendor_debug_packet_bytes{
        // Packet type (event)
        std::byte(0x04),
        // Event code (Vendor Debug)
        std::byte(0xFF),
        // Parameter size
        std::byte(0x03),
        // Subevent code (0x02)
        std::byte(0x02),
        std::byte(0x00),
        std::byte(0x00),
    };

    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), vendor_debug_packet_bytes);
    test.hci_cmd_mux().HandleH4FromController(std::move(buf));

    // Forwards packets if no interceptor.
    ASSERT_EQ(test.packets_to_host().size(), 1u);
    std::array<std::byte, 6> out;
    test.packets_to_host().front()->CopyTo(out);
    EXPECT_EQ(vendor_debug_packet_bytes, out);
    test.packets_to_host().pop_front();
  }

  // Test LE Meta Event subevent code.
  {
    std::array<std::byte, 6> le_meta_event_packet_bytes{
        // Packet type (event)
        std::byte(0x04),
        // Event code
        std::byte(0x3E),
        // Parameter size
        std::byte(0x03),
        // Subevent code (0x01)
        std::byte(0x01),
        std::byte(0x00),
        std::byte(0x00),
    };

    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), le_meta_event_packet_bytes);
    test.hci_cmd_mux().HandleH4FromController(std::move(buf));

    // Forwards packets if no interceptor.
    ASSERT_EQ(test.packets_to_host().size(), 1u);
    std::array<std::byte, 6> out;
    test.packets_to_host().front()->CopyTo(out);
    EXPECT_EQ(le_meta_event_packet_bytes, out);
    test.packets_to_host().pop_front();
  }

  result = Status::Cancelled();  // Unregister the interceptor.
  // Try to register an interceptor for LE Meta Event.
  result = test.hci_cmd_mux().RegisterEventInterceptor(
      emboss::EventCode::LE_META_EVENT,
      [&](EventPacket&& packet) -> CommandMultiplexer::EventInterceptorReturn {
        intercepted = std::move(packet.buffer);
        return {};
      });

  // Should fail, not allowed.
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.status(), Status::InvalidArgument());

  result = Status::Cancelled();
  // Register an interceptor for LE Meta Event subevent 0x01.
  result = test.hci_cmd_mux().RegisterEventInterceptor(
      emboss::LeSubEventCode::CONNECTION_COMPLETE,
      [&](EventPacket&& packet) -> CommandMultiplexer::EventInterceptorReturn {
        intercepted = std::move(packet.buffer);
        return {};
      });
  ASSERT_TRUE(result.ok());

  {
    std::array<std::byte, 6> le_meta_event_packet_bytes{
        // Packet type (event)
        std::byte(0x04),
        // Event code
        std::byte(0x3E),
        // Parameter size
        std::byte(0x03),
        // Subevent code (0x01)
        std::byte(0x01),
        std::byte(0x00),
        std::byte(0x00),
    };

    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), le_meta_event_packet_bytes);
    test.hci_cmd_mux().HandleH4FromController(std::move(buf));

    // Ensure not forwarded.
    EXPECT_TRUE(test.packets_to_host().empty());
    ASSERT_TRUE(intercepted.has_value());
    std::array<std::byte, 6> out;
    intercepted.value()->CopyTo(out);
    EXPECT_EQ(le_meta_event_packet_bytes, out);
  }

  intercepted = std::nullopt;

  // Test a different LE Meta Event subevent code (should not be intercepted).
  {
    std::array<std::byte, 6> le_meta_event_packet_bytes{
        // Packet type (event)
        std::byte(0x04),
        // Event code
        std::byte(0x3E),
        // Parameter size
        std::byte(0x03),
        // Subevent code (0x02)
        std::byte(0x02),
        std::byte(0x00),
        std::byte(0x00),
    };

    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), le_meta_event_packet_bytes);
    test.hci_cmd_mux().HandleH4FromController(std::move(buf));

    // Packet should not be intercepted.
    ASSERT_EQ(test.packets_to_host().size(), 1u);
    std::array<std::byte, 6> out;
    test.packets_to_host().front()->CopyTo(out);
    EXPECT_EQ(le_meta_event_packet_bytes, out);
    test.packets_to_host().pop_front();
    EXPECT_FALSE(intercepted.has_value());
  }

  intercepted = std::nullopt;
  result = Status::Cancelled();  // Unregister the interceptor.

  // Try to register an interceptor for Command Complete event.
  result = test.hci_cmd_mux().RegisterEventInterceptor(
      emboss::EventCode::COMMAND_COMPLETE,
      [&](EventPacket&& packet) -> CommandMultiplexer::EventInterceptorReturn {
        intercepted = std::move(packet.buffer);
        return {};
      });

  // Should fail, not allowed.
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.status(), Status::InvalidArgument());

  {
    std::array<std::byte, 6> command_complete_packet_bytes{
        // Packet type (event)
        std::byte(0x04),
        // Event code (Command Complete)
        std::byte(0x0E),
        // Parameter size
        std::byte(0x03),
        // Num_HCI_Command_Pack
        std::byte(0x01),
        // OpCode (Inquiry)
        std::byte(0x01),
        std::byte(0x04),
    };

    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), command_complete_packet_bytes);
    test.hci_cmd_mux().HandleH4FromController(std::move(buf));

    // Forwards packets if no interceptor.
    ASSERT_EQ(test.packets_to_host().size(), 1u);
    std::array<std::byte, 6> out;
    test.packets_to_host().front()->CopyTo(out);
    EXPECT_EQ(command_complete_packet_bytes, out);
    test.packets_to_host().pop_front();
  }

  intercepted = std::nullopt;
  result = Status::Cancelled();  // Unregister the interceptor.
  // Try to register an interceptor for Command Status event.
  result = test.hci_cmd_mux().RegisterEventInterceptor(
      emboss::EventCode::COMMAND_STATUS,
      [&](EventPacket&& packet) -> CommandMultiplexer::EventInterceptorReturn {
        intercepted = std::move(packet.buffer);
        return {};
      });

  // Should fail, not allowed.
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.status(), Status::InvalidArgument());

  {
    std::array<std::byte, 7> command_status_packet_bytes{
        // Packet type (event)
        std::byte(0x04),
        // Event code (Command Status)
        std::byte(0x0F),
        // Parameter size
        std::byte(0x04),
        // Status (Success)
        std::byte(0x00),
        // Num_HCI_Command_Packets
        std::byte(0x01),
        // OpCode (Inquiry)
        std::byte(0x01),
        std::byte(0x04),
    };

    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), command_status_packet_bytes);
    test.hci_cmd_mux().HandleH4FromController(std::move(buf));

    // Forwards packets if no interceptor.
    ASSERT_EQ(test.packets_to_host().size(), 1u);
    std::array<std::byte, 7> out;
    test.packets_to_host().front()->CopyTo(out);
    EXPECT_EQ(command_status_packet_bytes, out);
    test.packets_to_host().pop_front();
  }

  result = Status::Cancelled();  // Unregister the interceptor.
  // Register an interceptor for Command Status with a specific opcode.
  result = test.hci_cmd_mux().RegisterEventInterceptor(
      CommandStatusOpcode{emboss::OpCode::INQUIRY},
      [&](EventPacket&& packet) -> CommandMultiplexer::EventInterceptorReturn {
        intercepted = std::move(packet.buffer);
        return {};
      });
  ASSERT_TRUE(result.ok());

  {
    std::array<std::byte, 7> command_status_packet_bytes{
        // Packet type
        std::byte(0x04),
        // Event code (Command Status)
        std::byte(0x0F),
        // Parameter size
        std::byte(0x04),
        // Status (Success)
        std::byte(0x00),
        // Num_HCI_Command_Packets
        std::byte(0x01),
        // OpCode (Inquiry)
        std::byte(0x01),
        std::byte(0x04),
    };

    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), command_status_packet_bytes);
    test.hci_cmd_mux().HandleH4FromController(std::move(buf));

    // Ensure not forwarded.
    EXPECT_TRUE(test.packets_to_host().empty());
    ASSERT_TRUE(intercepted.has_value());
    std::array<std::byte, 7> out;
    intercepted.value()->CopyTo(out);
    EXPECT_EQ(command_status_packet_bytes, out);
  }

  intercepted = std::nullopt;

  // Test a different Command Status opcode (should not be intercepted).
  {
    std::array<std::byte, 7> command_status_packet_bytes{
        // Packet type
        std::byte(0x04),
        // Event code (Command Status)
        std::byte(0x0F),
        // Parameter size
        std::byte(0x04),
        // Status (Success)
        std::byte(0x00),
        // Num_HCI_Command_Packets
        std::byte(0x01),
        // OpCode (Disconnect)
        std::byte(0x06),
        std::byte(0x0C),
    };

    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), command_status_packet_bytes);
    test.hci_cmd_mux().HandleH4FromController(std::move(buf));

    // Forwards packets if no interceptor.
    ASSERT_EQ(test.packets_to_host().size(), 1u);
    std::array<std::byte, 7> out;
    test.packets_to_host().front()->CopyTo(out);
    EXPECT_EQ(command_status_packet_bytes, out);
    test.packets_to_host().pop_front();
  }
}

TEST_F(CommandMultiplexerTest, InterceptEventsAsync) {
  TestInterceptEvents(accessor_async2());
}
TEST_F(CommandMultiplexerTest, InterceptEventsTimer) {
  TestInterceptEvents(accessor_timer());
}

}  // namespace
}  // namespace pw::bluetooth::proxy::hci
