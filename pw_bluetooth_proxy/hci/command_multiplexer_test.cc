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

#include <algorithm>
#include <chrono>
#include <cstdint>

#include "pw_allocator/testing.h"
#include "pw_assert/check.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/func_task.h"
#include "pw_async2/simulated_time_provider.h"
#include "pw_bluetooth/hci_h4.emb.h"
#include "pw_chrono/system_clock.h"
#include "pw_containers/vector.h"
#include "pw_function/function.h"
#include "pw_status/try.h"
#include "pw_unit_test/framework.h"

namespace pw::bluetooth::proxy::hci {
namespace {

constexpr size_t kCommandCompleteHeaderSize = 6;
constexpr size_t kCommandCompleteParameterSize = 3;
constexpr size_t kAllocatorSize = 2048;

struct GiveCommandCreditParams {
  // An opcode of 0 is used to indicate commands can/cannot be sent without
  // necessarily being associated with a particular command.
  uint16_t opcode = 0;

  // By default allow a single additional command to be sent.
  uint8_t count = 1;

  // By default, remove the resulting event from the host buffer.
  bool remove_from_host_buffer = true;
};

template <size_t kPayloadSize = 0>
std::array<std::byte, kCommandCompleteHeaderSize + kPayloadSize>
MakeCommandCompletePacket(uint16_t opcode,
                          uint8_t credits,
                          span<const std::byte, kPayloadSize> payload = {}) {
  static_assert(kPayloadSize + kCommandCompleteParameterSize <=
                std::numeric_limits<uint8_t>::max());
  std::byte opcode_lower{static_cast<uint8_t>(opcode >> 0 & 0xFF)};
  std::byte opcode_upper{static_cast<uint8_t>(opcode >> 8 & 0xFF)};
  std::array<std::byte, kCommandCompleteHeaderSize + kPayloadSize> packet{
      // Packet type (event)
      std::byte(0x04),
      // Event code (Command Complete)
      std::byte(0x0E),
      // Parameter size
      std::byte(kCommandCompleteParameterSize + kPayloadSize),
      // Num_HCI_Command_Packets
      std::byte(credits),
      // OpCode
      opcode_lower,
      opcode_upper,
  };
  std::copy_n(payload.begin(),
              payload.size(),
              packet.begin() + kCommandCompleteHeaderSize);
  return packet;
}

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
    allocator::test::AllocatorForTest<kAllocatorSize>& allocator() {
      return test_.allocator();
    }
    async2::DispatcherForTest& dispatcher() { return test_.dispatcher(); }

    void set_auto_command_complete(bool value = true) {
      auto_command_complete_ = value;
    }

    pw::DynamicDeque<MultiBuf::Instance>& packets_to_host() {
      return test_.packets_to_host();
    }
    pw::DynamicDeque<MultiBuf::Instance>& packets_to_controller() {
      return test_.packets_to_controller();
    }

    void SendFromHost(MultiBuf::Instance&& packet) {
      bool give_credit =
          auto_command_complete_ && *packet->begin() == std::byte(0x01);
      hci_cmd_mux_.HandleH4FromHost(std::move(packet));
      if (give_credit) {
        GiveCommandCredit();
      }
    }

    void GiveCommandCredit(GiveCommandCreditParams params = {}) {
      return test_.GiveCommandCredit(params);
    }

   private:
    Accessor(CommandMultiplexerTest& test, CommandMultiplexer& hci_cmd_mux)
        : test_(test), hci_cmd_mux_(hci_cmd_mux) {}
    friend CommandMultiplexerTest;

    bool auto_command_complete_{false};
    CommandMultiplexerTest& test_;
    CommandMultiplexer& hci_cmd_mux_;
  };

 protected:
  CommandMultiplexer& hci_cmd_mux_async2() {
    PW_CHECK(is_async_.value_or(true));
    is_async_ = true;
    if (!cmd_mux_.has_value()) {
      cmd_mux_.emplace(allocator_,
                       make_send_to_host_cb(),
                       make_send_to_controller_cb(),
                       time_provider_);
    }
    return *cmd_mux_;
  }

  CommandMultiplexer& hci_cmd_mux_timer() {
    PW_CHECK(!is_async_.value_or(false));
    is_async_ = false;
    if (!cmd_mux_.has_value()) {
      Function<void()> timeout_fn = [this] { OnTimeout(); };
      cmd_mux_.emplace(allocator_,
                       make_send_to_host_cb(),
                       make_send_to_controller_cb(),
                       std::move(timeout_fn),
                       kTestTimeoutDuration);
    }
    return *cmd_mux_;
  }

  allocator::test::AllocatorForTest<kAllocatorSize>& allocator() {
    return allocator_;
  }
  async2::DispatcherForTest& dispatcher() { return dispatcher_; }

  Accessor accessor_async2() { return Accessor(*this, hci_cmd_mux_async2()); }

  Accessor accessor_timer() { return Accessor(*this, hci_cmd_mux_timer()); }

  pw::DynamicDeque<MultiBuf::Instance>& packets_to_host() {
    return packets_to_host_;
  }
  pw::DynamicDeque<MultiBuf::Instance>& packets_to_controller() {
    return packets_to_controller_;
  }

  pw::Result<MultiBuf::Instance> AllocBuf(ConstByteSpan span) {
    MultiBuf::Instance buf(allocator());
    if (!buf->TryReserveForPushBack()) {
      return Status::ResourceExhausted();
    }

    auto alloc = allocator().MakeUnique<std::byte[]>(span.size());
    if (alloc == nullptr) {
      return Status::ResourceExhausted();
    }

    std::memcpy(alloc.get(), span.data(), span.size());
    buf->PushBack(std::move(alloc));
    return buf;
  }

  void GiveCommandCredit(GiveCommandCreditParams params) {
    auto host_packets_count = packets_to_host_.size();
    PW_TEST_ASSERT_OK_AND_ASSIGN(
        auto buf,
        AllocBuf(MakeCommandCompletePacket(params.opcode, params.count)));
    cmd_mux_->HandleH4FromController(std::move(buf));

    PW_CHECK(packets_to_host_.size() == host_packets_count + 1);
    if (params.remove_from_host_buffer) {
      packets_to_host_.pop_back();
    }
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

  std::optional<CommandMultiplexer> cmd_mux_{std::nullopt};
  std::optional<bool> is_async_{std::nullopt};
};

using Accessor = CommandMultiplexerTest::Accessor;

TEST_F(CommandMultiplexerTest, AsyncTimeout) {
  auto& hci_cmd_mux = hci_cmd_mux_async2();

  std::optional<Result<async2::Poll<>>> pend_result;
  async2::FuncTask task{[&](async2::Context& cx) {
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
  async2::FuncTask task{[&](async2::Context& cx) {
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
  static constexpr std::array<std::byte, 5> command_complete_packet_bytes{
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

  static constexpr std::array<std::byte, 4> hardware_error_packet_bytes{
      // Packet type (event)
      std::byte(0x04),
      // Event code (Hardware Error)
      std::byte(0x10),
      // Parameter size
      std::byte(0x01),
      // Hardware_Code
      std::byte(0x01),
  };

  // Try sending empty buffer.
  {
    MultiBuf::Instance buffer(test.allocator());

    auto result = test.hci_cmd_mux().SendEvent({std::move(buffer)});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().status(), Status::InvalidArgument());
  }

  // Try sending buffer containing a valid event.
  {
    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), command_complete_packet_bytes);

    auto result = test.hci_cmd_mux().SendEvent({std::move(buf)});
    EXPECT_TRUE(result.has_value());
    ASSERT_EQ(test.packets_to_host().size(), 1u);
    std::array<std::byte, 1> out_h4_header{};
    std::array<std::byte, command_complete_packet_bytes.size()> out_payload{};
    test.packets_to_host().front()->CopyTo(out_h4_header);
    test.packets_to_host().front()->CopyTo(out_payload, 1);
    EXPECT_EQ(out_h4_header[0], std::byte(emboss::H4PacketType::EVENT));
    EXPECT_EQ(command_complete_packet_bytes, out_payload);
    test.packets_to_host().pop_front();
  }

  // Register an event interceptor and ensure that SendEvent bypasses it.
  {
    std::optional<EventPacket> intercepted;
    auto result = test.hci_cmd_mux().RegisterEventInterceptor(
        emboss::EventCode::HARDWARE_ERROR,
        [&](EventPacket&& packet)
            -> CommandMultiplexer::EventInterceptorReturn {
          intercepted = std::move(packet);
          return {};
        });
    EXPECT_EQ(result.status(), OkStatus());
    EXPECT_TRUE(result.ok());

    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), hardware_error_packet_bytes);

    auto send_result = test.hci_cmd_mux().SendEvent({std::move(buf)});
    EXPECT_TRUE(send_result.has_value());

    // SendEvent should bypass interceptors.
    EXPECT_FALSE(intercepted.has_value());
    ASSERT_EQ(test.packets_to_host().size(), 1u);
    std::array<std::byte, 1> out_h4_header{};
    std::array<std::byte, hardware_error_packet_bytes.size()> out_payload{};
    test.packets_to_host().front()->CopyTo(out_h4_header);
    test.packets_to_host().front()->CopyTo(out_payload, 1);
    EXPECT_EQ(out_h4_header[0], std::byte(emboss::H4PacketType::EVENT));
    EXPECT_EQ(hardware_error_packet_bytes, out_payload);
    test.packets_to_host().pop_front();
  }
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
  static constexpr std::array<std::byte, 4> reset_packet_bytes{
      // Packet type (command)
      std::byte(0x01),
      // OpCode (Reset)
      std::byte(0x03),
      std::byte(0x0C),
      // Parameter size
      std::byte(0x00),
  };

  test.set_auto_command_complete();

  {
    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), reset_packet_bytes);
    test.SendFromHost(std::move(buf));

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
  ASSERT_EQ(result.status(), OkStatus());

  {
    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), reset_packet_bytes);
    test.SendFromHost(std::move(buf));

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
    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), reset_packet_bytes);
    test.SendFromHost(std::move(buf));

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
    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), reset_packet_bytes);
    test.SendFromHost(std::move(buf));

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
    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), reset_packet_bytes);
    test.SendFromHost(std::move(buf));

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
    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), reset_packet_bytes);
    test.SendFromHost(std::move(buf));

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
    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), reset_packet_bytes);
    test.SendFromHost(std::move(buf));

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
    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), reset_packet_bytes);
    test.SendFromHost(std::move(buf));

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
  static constexpr std::array<std::byte, 6> reset_command_complete_packet_bytes{
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
  static constexpr std::array<std::byte, 6> vendor_debug_subevent1_packet_bytes{
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
  static constexpr std::array<std::byte, 6> vendor_debug_subevent2_packet_bytes{
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
  static constexpr std::array<std::byte, 6> le_meta_subevent1_packet_bytes{
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
  static constexpr std::array<std::byte, 6> le_meta_subevent2_packet_bytes{
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
  static constexpr std::array<std::byte, 6>
      inquiry_command_complete_packet_bytes{
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
  static constexpr std::array<std::byte, 7> inquiry_command_status_packet_bytes{
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
  static constexpr std::array<std::byte, 7>
      disconnect_command_status_packet_bytes{
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

  {
    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), reset_command_complete_packet_bytes);
    test.hci_cmd_mux().HandleH4FromController(std::move(buf));

    // Forwards packets if no interceptor.
    ASSERT_EQ(test.packets_to_host().size(), 1u);
    std::array<std::byte, 6> out;
    test.packets_to_host().front()->CopyTo(out);
    EXPECT_EQ(reset_command_complete_packet_bytes, out);
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
    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), reset_command_complete_packet_bytes);
    test.hci_cmd_mux().HandleH4FromController(std::move(buf));

    // Ensure not forwarded.
    EXPECT_TRUE(test.packets_to_host().empty());
    ASSERT_TRUE(intercepted.has_value());
    std::array<std::byte, 6> out;
    intercepted.value()->CopyTo(out);
    EXPECT_EQ(reset_command_complete_packet_bytes, out);
  }

  // Clear the result, but keep interceptor active.
  intercepted = std::nullopt;

  // Ensure continuing to intercept.
  {
    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), reset_command_complete_packet_bytes);
    test.hci_cmd_mux().HandleH4FromController(std::move(buf));

    // Ensure not forwarded.
    EXPECT_TRUE(test.packets_to_host().empty());
    ASSERT_TRUE(intercepted.has_value());
    std::array<std::byte, 6> out;
    intercepted.value()->CopyTo(out);
    EXPECT_EQ(reset_command_complete_packet_bytes, out);
  }

  result = Status::Cancelled();  // Unregister the interceptor.
  intercepted = std::nullopt;

  {
    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), reset_command_complete_packet_bytes);
    test.hci_cmd_mux().HandleH4FromController(std::move(buf));

    // Forwards packets if no interceptor.
    ASSERT_EQ(test.packets_to_host().size(), 1u);
    EXPECT_FALSE(intercepted.has_value());
    std::array<std::byte, 6> out;
    test.packets_to_host().front()->CopyTo(out);
    EXPECT_EQ(reset_command_complete_packet_bytes, out);
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
    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), reset_command_complete_packet_bytes);
    test.hci_cmd_mux().HandleH4FromController(std::move(buf));

    // Forwards packets returned from interceptor.
    ASSERT_EQ(test.packets_to_host().size(), 1u);
    EXPECT_TRUE(peeked);
    std::array<std::byte, 6> out;
    test.packets_to_host().front()->CopyTo(out);
    EXPECT_EQ(reset_command_complete_packet_bytes, out);
    test.packets_to_host().pop_front();
  }

  peeked = false;

  // Ensure continuing to intercept.
  {
    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), reset_command_complete_packet_bytes);
    test.hci_cmd_mux().HandleH4FromController(std::move(buf));

    // Forwards packets returned from interceptor.
    ASSERT_EQ(test.packets_to_host().size(), 1u);
    EXPECT_TRUE(peeked);
    std::array<std::byte, 6> out;
    test.packets_to_host().front()->CopyTo(out);
    EXPECT_EQ(reset_command_complete_packet_bytes, out);
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
    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), reset_command_complete_packet_bytes);
    test.hci_cmd_mux().HandleH4FromController(std::move(buf));

    // Ensure not forwarded.
    EXPECT_TRUE(test.packets_to_host().empty());
    ASSERT_TRUE(intercepted.has_value());
    std::array<std::byte, 6> out;
    intercepted.value()->CopyTo(out);
    EXPECT_EQ(reset_command_complete_packet_bytes, out);
  }

  intercepted = std::nullopt;

  // Ensure the next one is not intercepted.
  {
    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), reset_command_complete_packet_bytes);
    test.hci_cmd_mux().HandleH4FromController(std::move(buf));

    // Forwards packets if no interceptor.
    ASSERT_EQ(test.packets_to_host().size(), 1u);
    EXPECT_FALSE(intercepted.has_value());
    std::array<std::byte, 6> out;
    test.packets_to_host().front()->CopyTo(out);
    EXPECT_EQ(reset_command_complete_packet_bytes, out);
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
    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), vendor_debug_subevent1_packet_bytes);
    test.hci_cmd_mux().HandleH4FromController(std::move(buf));

    // Forwards packets if no interceptor.
    ASSERT_EQ(test.packets_to_host().size(), 1u);
    std::array<std::byte, 6> out;
    test.packets_to_host().front()->CopyTo(out);
    EXPECT_EQ(vendor_debug_subevent1_packet_bytes, out);
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
    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), vendor_debug_subevent1_packet_bytes);
    test.hci_cmd_mux().HandleH4FromController(std::move(buf));

    // Ensure not forwarded.
    EXPECT_TRUE(test.packets_to_host().empty());
    ASSERT_TRUE(intercepted.has_value());
    std::array<std::byte, 6> out;
    intercepted.value()->CopyTo(out);
    EXPECT_EQ(vendor_debug_subevent1_packet_bytes, out);
  }

  intercepted = std::nullopt;

  // Test a different vendor debug subevent code (should not be intercepted).
  {
    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), vendor_debug_subevent2_packet_bytes);
    test.hci_cmd_mux().HandleH4FromController(std::move(buf));

    // Forwards packets if no interceptor.
    ASSERT_EQ(test.packets_to_host().size(), 1u);
    std::array<std::byte, 6> out;
    test.packets_to_host().front()->CopyTo(out);
    EXPECT_EQ(vendor_debug_subevent2_packet_bytes, out);
    test.packets_to_host().pop_front();
  }

  // Test LE Meta Event subevent code.
  {
    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), le_meta_subevent1_packet_bytes);
    test.hci_cmd_mux().HandleH4FromController(std::move(buf));

    // Forwards packets if no interceptor.
    ASSERT_EQ(test.packets_to_host().size(), 1u);
    std::array<std::byte, 6> out;
    test.packets_to_host().front()->CopyTo(out);
    EXPECT_EQ(le_meta_subevent1_packet_bytes, out);
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
    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), le_meta_subevent1_packet_bytes);
    test.hci_cmd_mux().HandleH4FromController(std::move(buf));

    // Ensure not forwarded.
    EXPECT_TRUE(test.packets_to_host().empty());
    ASSERT_TRUE(intercepted.has_value());
    std::array<std::byte, 6> out;
    intercepted.value()->CopyTo(out);
    EXPECT_EQ(le_meta_subevent1_packet_bytes, out);
  }

  intercepted = std::nullopt;

  // Test a different LE Meta Event subevent code (should not be intercepted).
  {
    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), le_meta_subevent2_packet_bytes);
    test.hci_cmd_mux().HandleH4FromController(std::move(buf));

    // Packet should not be intercepted.
    ASSERT_EQ(test.packets_to_host().size(), 1u);
    std::array<std::byte, 6> out;
    test.packets_to_host().front()->CopyTo(out);
    EXPECT_EQ(le_meta_subevent2_packet_bytes, out);
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
    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), inquiry_command_complete_packet_bytes);
    test.hci_cmd_mux().HandleH4FromController(std::move(buf));

    // Forwards packets if no interceptor.
    ASSERT_EQ(test.packets_to_host().size(), 1u);
    std::array<std::byte, 6> out;
    test.packets_to_host().front()->CopyTo(out);
    EXPECT_EQ(inquiry_command_complete_packet_bytes, out);
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
    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), inquiry_command_status_packet_bytes);
    test.hci_cmd_mux().HandleH4FromController(std::move(buf));

    // Forwards packets if no interceptor.
    ASSERT_EQ(test.packets_to_host().size(), 1u);
    std::array<std::byte, 7> out;
    test.packets_to_host().front()->CopyTo(out);
    EXPECT_EQ(inquiry_command_status_packet_bytes, out);
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
    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), inquiry_command_status_packet_bytes);
    test.hci_cmd_mux().HandleH4FromController(std::move(buf));

    // Ensure not forwarded.
    EXPECT_TRUE(test.packets_to_host().empty());
    ASSERT_TRUE(intercepted.has_value());
    std::array<std::byte, 7> out;
    intercepted.value()->CopyTo(out);
    EXPECT_EQ(inquiry_command_status_packet_bytes, out);
  }

  intercepted = std::nullopt;

  // Test a different Command Status opcode (should not be intercepted).
  {
    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), disconnect_command_status_packet_bytes);
    test.hci_cmd_mux().HandleH4FromController(std::move(buf));

    // Forwards packets if no interceptor.
    ASSERT_EQ(test.packets_to_host().size(), 1u);
    std::array<std::byte, 7> out;
    test.packets_to_host().front()->CopyTo(out);
    EXPECT_EQ(disconnect_command_status_packet_bytes, out);
    test.packets_to_host().pop_front();
  }
}

TEST_F(CommandMultiplexerTest, InterceptEventsAsync) {
  TestInterceptEvents(accessor_async2());
}
TEST_F(CommandMultiplexerTest, InterceptEventsTimer) {
  TestInterceptEvents(accessor_timer());
}

void TestQueueCommands(Accessor test) {
  static constexpr std::array<std::byte, 4> reset_packet_bytes{
      // Packet type (command)
      std::byte(0x01),
      // OpCode (Reset)
      std::byte(0x03),
      std::byte(0x0C),
      // Parameter size
      std::byte(0x00),
  };
  static constexpr std::array<std::byte, 4> inquiry_packet_bytes{
      // Packet type (command)
      std::byte(0x01),
      // OpCode (Inquiry)
      std::byte(0x01),
      std::byte(0x04),
      // Parameter size
      std::byte(0x00),
  };
  static constexpr std::array<std::byte, 7> disconnect_packet_bytes{
      // Packet type (command)
      std::byte(0x01),
      // OpCode (Disconnect)
      std::byte(0x06),
      std::byte(0x04),
      // Parameter size
      std::byte(0x03),
      // Command handle
      std::byte(0x00),
      std::byte(0x00),
      // Status code
      std::byte(0x00),
  };

  {
    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), reset_packet_bytes);
    test.SendFromHost(std::move(buf));

    // Packet should have been sent.
    ASSERT_EQ(test.packets_to_controller().size(), 1u);
    std::array<std::byte, reset_packet_bytes.size()> out;
    test.packets_to_controller().front()->CopyTo(out);
    EXPECT_EQ(reset_packet_bytes, out);
    test.packets_to_controller().pop_front();
  }

  {
    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), inquiry_packet_bytes);
    test.SendFromHost(std::move(buf));
  }

  {
    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), disconnect_packet_bytes);
    test.SendFromHost(std::move(buf));
  }

  // Packet should have been queued and not yet sent to the controller.
  ASSERT_EQ(test.packets_to_controller().size(), 0u);

  // Give credit, confirm the queued packet was sent.
  test.GiveCommandCredit({.opcode = 0x0C03});
  ASSERT_EQ(test.packets_to_controller().size(), 1u);
  std::array<std::byte, inquiry_packet_bytes.size()> out;
  EXPECT_EQ(test.packets_to_controller().front()->size(), 4u);
  test.packets_to_controller().front()->CopyTo(out);
  EXPECT_EQ(inquiry_packet_bytes, out);
  test.packets_to_controller().pop_front();

  // Give last credit, confirm the second queued packet was sent.
  test.GiveCommandCredit({.opcode = 0x0401});
  ASSERT_EQ(test.packets_to_controller().size(), 1u);
  std::array<std::byte, disconnect_packet_bytes.size()> out2;
  EXPECT_EQ(test.packets_to_controller().front()->size(), 7u);
  test.packets_to_controller().front()->CopyTo(out2);
  EXPECT_EQ(disconnect_packet_bytes, out2);
  test.packets_to_controller().pop_front();

  // Give four credits.
  test.GiveCommandCredit({.opcode = 0x0406, .count = 4});
  ASSERT_EQ(test.packets_to_controller().size(), 0u);

  // Send a couple commands, ensuring they get sent immediately.
  {
    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), inquiry_packet_bytes);
    test.SendFromHost(std::move(buf));
  }

  ASSERT_EQ(test.packets_to_controller().size(), 1u);
  std::array<std::byte, 4> out3;
  EXPECT_EQ(test.packets_to_controller().front()->size(), 4u);
  test.packets_to_controller().front()->CopyTo(out3);
  EXPECT_EQ(inquiry_packet_bytes, out3);
  test.packets_to_controller().pop_front();

  {
    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), disconnect_packet_bytes);
    test.SendFromHost(std::move(buf));
  }

  ASSERT_EQ(test.packets_to_controller().size(), 1u);
  std::array<std::byte, 7> out4;
  EXPECT_EQ(test.packets_to_controller().front()->size(), 7u);
  test.packets_to_controller().front()->CopyTo(out4);
  EXPECT_EQ(disconnect_packet_bytes, out4);
  test.packets_to_controller().pop_front();

  // Send a command credit packet with 0 credits, ensuring it is overridden.
  test.GiveCommandCredit({.count = 0});

  {
    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), reset_packet_bytes);
    test.SendFromHost(std::move(buf));
  }

  // Packet should have been queued and not yet sent to the controller.
  ASSERT_EQ(test.packets_to_controller().size(), 0u);

  // Give credit, confirm the queued packet was sent.
  test.GiveCommandCredit({.opcode = 0x0C03});
  ASSERT_EQ(test.packets_to_controller().size(), 1u);
  std::array<std::byte, reset_packet_bytes.size()> out5;
  EXPECT_EQ(test.packets_to_controller().front()->size(), 4u);
  test.packets_to_controller().front()->CopyTo(out5);
  EXPECT_EQ(reset_packet_bytes, out5);
  test.packets_to_controller().pop_front();
}

TEST_F(CommandMultiplexerTest, QueueCommandsAsync) {
  TestQueueCommands(accessor_async2());
}
TEST_F(CommandMultiplexerTest, QueueCommandsTimer) {
  TestQueueCommands(accessor_timer());
}

void TestNumHciCommands(Accessor test) {
  constexpr uint8_t kTestNumHciCommands = 5;
  constexpr size_t kNumHciCommandsByte = 3;
  static constexpr std::array<std::byte, 4> reset_packet_bytes{
      // Packet type (command)
      std::byte(0x01),
      // OpCode (Reset)
      std::byte(0x03),
      std::byte(0x0C),
      // Parameter size
      std::byte(0x00),
  };

  {
    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), reset_packet_bytes);
    test.hci_cmd_mux().HandleH4FromHost(std::move(buf));
  }

  // Packet should have been sent.
  ASSERT_EQ(test.packets_to_controller().size(), 1u);
  std::array<std::byte, reset_packet_bytes.size()> out;
  test.packets_to_controller().front()->CopyTo(out);
  EXPECT_EQ(reset_packet_bytes, out);
  test.packets_to_controller().pop_front();

  // Give credit, confirm the queued packet was sent.
  test.GiveCommandCredit({.opcode = 0x0C03,
                          .count = kTestNumHciCommands,
                          .remove_from_host_buffer = false});
  ASSERT_EQ(test.packets_to_controller().size(), 0u);

  // Check that the num_hci_commands field of the command complete packet is >=
  // the value sent from.
  ASSERT_EQ(test.packets_to_host().size(), 1u);
  std::array<std::byte, kCommandCompleteHeaderSize> command_complete_header;
  test.packets_to_host().front()->CopyTo(command_complete_header);
  uint8_t actual_num_hci_commands =
      static_cast<uint8_t>(command_complete_header[kNumHciCommandsByte]);
  EXPECT_GE(actual_num_hci_commands, kTestNumHciCommands);
  test.packets_to_host().pop_front();

  for (size_t i = 0; i < kTestNumHciCommands; ++i) {
    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), reset_packet_bytes);
    test.hci_cmd_mux().HandleH4FromHost(std::move(buf));
    ASSERT_EQ(test.packets_to_controller().size(), 1);
    std::array<std::byte, reset_packet_bytes.size()> out_packet;
    test.packets_to_controller().front()->CopyTo(out_packet);
    EXPECT_EQ(reset_packet_bytes, out_packet);
    test.packets_to_controller().clear();
  }

  if (actual_num_hci_commands > kTestNumHciCommands) {
    MultiBuf::Instance buf(test.allocator());
    buf->Insert(buf->end(), reset_packet_bytes);
    test.hci_cmd_mux().HandleH4FromHost(std::move(buf));
    // All further packets should be buffered.
    EXPECT_EQ(test.packets_to_controller().size(), 0u);
  }
}

TEST_F(CommandMultiplexerTest, NumHciCommandsAsync) {
  TestNumHciCommands(accessor_async2());
}
TEST_F(CommandMultiplexerTest, NumHciCommandsTimer) {
  TestNumHciCommands(accessor_timer());
}

void TestDeadlock(Accessor test) {
  static constexpr std::array<std::byte, 5> command_complete_packet_bytes{
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

  // Used to make sure the callback only takes one local reference
  struct CallbackData {
    MultiBuf::Instance event_buf;
    Accessor& test;
    std::optional<MultiBuf::Instance> intercepted;
  };

  CallbackData callback_data{
      .event_buf = MultiBuf::Instance(test.allocator()),
      .test = test,
      .intercepted = std::nullopt,
  };
  auto& intercepted = callback_data.intercepted;

  callback_data.event_buf->Insert(callback_data.event_buf->end(),
                                  command_complete_packet_bytes);

  auto result = test.hci_cmd_mux().RegisterCommandInterceptor(
      emboss::OpCode::RESET,
      [&callback_data](CommandPacket&& packet)
          -> CommandMultiplexer::CommandInterceptorReturn {
        // This will acquire `mutex_` to send an event.
        EXPECT_TRUE(callback_data.test.hci_cmd_mux()
                        .SendEvent({std::move(callback_data.event_buf)})
                        .has_value());
        callback_data.intercepted = std::move(packet.buffer);
        return {};
      });
  ASSERT_EQ(result.status(), OkStatus());

  static constexpr std::array<std::byte, 4> reset_packet_bytes{
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
  test.SendFromHost(std::move(buf));

  // Ensure not forwarded.
  EXPECT_TRUE(test.packets_to_controller().empty());
  ASSERT_TRUE(intercepted.has_value());
  std::array<std::byte, 4> out;
  intercepted.value()->CopyTo(out);
  EXPECT_EQ(reset_packet_bytes, out);

  // Ensure event was sent.
  ASSERT_EQ(test.packets_to_host().size(), 1u);
  std::array<std::byte, 1> h4_header{};
  test.packets_to_host().front()->CopyTo(h4_header);
  EXPECT_EQ(h4_header[0], std::byte(emboss::H4PacketType::EVENT));
  std::array<std::byte, command_complete_packet_bytes.size()> event_payload{};
  test.packets_to_host().front()->CopyTo(event_payload, 1);
  EXPECT_EQ(command_complete_packet_bytes, event_payload);
  test.packets_to_host().pop_front();
}

TEST_F(CommandMultiplexerTest, DeadlockAsync) {
  TestDeadlock(accessor_async2());
}
TEST_F(CommandMultiplexerTest, DeadlockTimer) {
  TestDeadlock(accessor_timer());
}

}  // namespace
}  // namespace pw::bluetooth::proxy::hci
