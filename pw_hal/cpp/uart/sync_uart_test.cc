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

#include "pw_hal/uart/sync_uart.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <memory>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "pw_bytes/array.h"
#include "pw_bytes/span.h"
#include "pw_chrono/system_clock.h"
#include "pw_hal/uart/buffered_uart_driver_gmock.h"
#include "pw_status/status.h"
#include "pw_thread/sleep.h"
#include "pw_thread/test_thread_context.h"
#include "pw_thread/thread.h"

namespace pw::hal::uart {
namespace {

using namespace std::chrono_literals;
using ::testing::_;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::StrictMock;

TEST(SyncBufferedUartTest, ConstructAndDestruct) {
  auto mock = std::make_unique<StrictMock<GmockBufferedUartDriver>>();

  {
    ::testing::InSequence seq;
    EXPECT_CALL(*mock, Enable()).WillOnce(Return(OkStatus()));
    EXPECT_CALL(*mock, SetTxReadyNotifyLevel(1)).WillOnce(Return(OkStatus()));
    EXPECT_CALL(*mock, SetRxReadyNotifyLevel(1)).WillOnce(Return(OkStatus()));
    EXPECT_CALL(*mock, SetTxReadyNotifier(_)).WillOnce(Return(OkStatus()));
    EXPECT_CALL(*mock, SetRxReadyNotifier(_)).WillOnce(Return(OkStatus()));
    EXPECT_CALL(*mock, SetTxReadyNotifier(_)).WillOnce(Return(OkStatus()));
    EXPECT_CALL(*mock, SetRxReadyNotifier(_)).WillOnce(Return(OkStatus()));
    EXPECT_CALL(*mock, Disable()).WillOnce(Return(OkStatus()));
  }

  {
    SyncBufferedUart sync_uart(std::move(mock));
  }
}

TEST(SyncBufferedUartTest, SendForSuccess) {
  auto mock = std::make_unique<NiceMock<GmockBufferedUartDriver>>();
  const auto data = pw::bytes::Array<1, 2, 3, 4, 5>();

  EXPECT_CALL(*mock, Send(_)).WillOnce([&data](pw::span<const std::byte> tx) {
    EXPECT_EQ(tx.size(), data.size());
    EXPECT_TRUE(std::equal(tx.begin(), tx.end(), data.begin()));
    return StatusWithSize(OkStatus(), data.size());
  });

  SyncBufferedUart sync_uart(std::move(mock));
  EXPECT_EQ(sync_uart.SendFor(data, 100ms), OkStatus());
}

TEST(SyncBufferedUartTest, SendForEmptyBuffer) {
  auto mock = std::make_unique<StrictMock<GmockBufferedUartDriver>>();
  EXPECT_CALL(*mock, Enable()).WillOnce(Return(OkStatus()));
  EXPECT_CALL(*mock, SetTxReadyNotifyLevel(1)).WillOnce(Return(OkStatus()));
  EXPECT_CALL(*mock, SetRxReadyNotifyLevel(1)).WillOnce(Return(OkStatus()));
  EXPECT_CALL(*mock, SetTxReadyNotifier(_)).WillRepeatedly(Return(OkStatus()));
  EXPECT_CALL(*mock, SetRxReadyNotifier(_)).WillRepeatedly(Return(OkStatus()));
  EXPECT_CALL(*mock, Disable()).WillOnce(Return(OkStatus()));

  SyncBufferedUart sync_uart(std::move(mock));
  EXPECT_EQ(sync_uart.SendFor(pw::ConstByteSpan(), 100ms), OkStatus());
}

TEST(SyncBufferedUartTest, SendUntilSuccess) {
  auto mock = std::make_unique<NiceMock<GmockBufferedUartDriver>>();
  const auto data = pw::bytes::Array<10, 20, 30>();

  EXPECT_CALL(*mock, Send(_)).WillOnce([&data](pw::span<const std::byte> tx) {
    EXPECT_EQ(tx.size(), data.size());
    return StatusWithSize(OkStatus(), data.size());
  });

  SyncBufferedUart sync_uart(std::move(mock));
  EXPECT_EQ(sync_uart.SendUntil(data, chrono::SystemClock::now() + 500ms),
            OkStatus());
}

TEST(SyncBufferedUartTest, SendUntilExpiredDeadline) {
  auto mock = std::make_unique<NiceMock<GmockBufferedUartDriver>>();
  SyncBufferedUart sync_uart(std::move(mock));

  const auto data = pw::bytes::Array<1, 2, 3>();
  EXPECT_EQ(sync_uart.SendUntil(data, chrono::SystemClock::now() - 100ms),
            Status::DeadlineExceeded());
}

TEST(SyncBufferedUartTest, SendForDriverError) {
  auto mock = std::make_unique<NiceMock<GmockBufferedUartDriver>>();
  EXPECT_CALL(*mock, Send(_))
      .WillOnce(Return(StatusWithSize(Status::Unavailable(), 0)));

  SyncBufferedUart sync_uart(std::move(mock));
  const auto data = pw::bytes::Array<1, 2, 3>();
  EXPECT_EQ(sync_uart.SendFor(data, 100ms), Status::Unavailable());
}

TEST(SyncBufferedUartTest, SendForTimeoutWhenFull) {
  auto mock = std::make_unique<NiceMock<GmockBufferedUartDriver>>();
  EXPECT_CALL(*mock, Send(_))
      .WillRepeatedly(Return(StatusWithSize(Status::ResourceExhausted(), 0)));

  SyncBufferedUart sync_uart(std::move(mock));
  const auto data = pw::bytes::Array<1, 2, 3, 4>();
  EXPECT_EQ(sync_uart.SendFor(data, 20ms), Status::DeadlineExceeded());
}

TEST(SyncBufferedUartTest, SendForBlockingSuccess) {
  auto mock = std::make_unique<NiceMock<GmockBufferedUartDriver>>();
  pw::Function<void(size_t)> tx_ready_notifier;

  EXPECT_CALL(*mock, SetTxReadyNotifier(_))
      .WillRepeatedly(
          [&tx_ready_notifier](pw::Function<void(size_t)> on_ready) {
            tx_ready_notifier = std::move(on_ready);
            return OkStatus();
          });

  EXPECT_CALL(*mock, Send(_))
      .WillOnce(Return(StatusWithSize(Status::ResourceExhausted(), 4)))
      .WillOnce(Return(StatusWithSize(OkStatus(), 4)));

  SyncBufferedUart sync_uart(std::move(mock));
  const auto data = pw::bytes::Array<1, 2, 3, 4, 5, 6, 7, 8>();

  pw::thread::test::TestThreadContext thread_ctx;
  pw::Thread notifier_thread(thread_ctx.options(), [&]() {
    pw::this_thread::sleep_for(20ms);
    if (tx_ready_notifier) {
      tx_ready_notifier(4);
    }
  });

  EXPECT_EQ(sync_uart.SendFor(data, 1000ms), OkStatus());
  notifier_thread.join();
}

TEST(SyncBufferedUartTest, ReceiveForSuccessPreloaded) {
  auto mock = std::make_unique<NiceMock<GmockBufferedUartDriver>>();
  const auto expected = pw::bytes::Array<42, 43, 44>();

  EXPECT_CALL(*mock, Receive(_)).WillOnce([&expected](pw::span<std::byte> rx) {
    std::copy(expected.begin(), expected.end(), rx.begin());
    return StatusWithSize(OkStatus(), expected.size());
  });

  SyncBufferedUart sync_uart(std::move(mock));
  std::array<std::byte, 3> rx_buffer{};
  EXPECT_EQ(sync_uart.ReceiveFor(rx_buffer, 100ms), OkStatus());
  EXPECT_EQ(rx_buffer[0], std::byte(42));
  EXPECT_EQ(rx_buffer[1], std::byte(43));
  EXPECT_EQ(rx_buffer[2], std::byte(44));
}

TEST(SyncBufferedUartTest, ReceiveForEmptyBuffer) {
  auto mock = std::make_unique<StrictMock<GmockBufferedUartDriver>>();
  EXPECT_CALL(*mock, Enable()).WillOnce(Return(OkStatus()));
  EXPECT_CALL(*mock, SetTxReadyNotifyLevel(1)).WillOnce(Return(OkStatus()));
  EXPECT_CALL(*mock, SetRxReadyNotifyLevel(1)).WillOnce(Return(OkStatus()));
  EXPECT_CALL(*mock, SetTxReadyNotifier(_)).WillRepeatedly(Return(OkStatus()));
  EXPECT_CALL(*mock, SetRxReadyNotifier(_)).WillRepeatedly(Return(OkStatus()));
  EXPECT_CALL(*mock, Disable()).WillOnce(Return(OkStatus()));

  SyncBufferedUart sync_uart(std::move(mock));
  EXPECT_EQ(sync_uart.ReceiveFor(pw::ByteSpan(), 100ms), OkStatus());
}

TEST(SyncBufferedUartTest, ReceiveUntilSuccess) {
  auto mock = std::make_unique<NiceMock<GmockBufferedUartDriver>>();
  const auto expected = pw::bytes::Array<7, 8, 9>();

  EXPECT_CALL(*mock, Receive(_)).WillOnce([&expected](pw::span<std::byte> rx) {
    std::copy(expected.begin(), expected.end(), rx.begin());
    return StatusWithSize(OkStatus(), expected.size());
  });

  SyncBufferedUart sync_uart(std::move(mock));
  std::array<std::byte, 3> rx_buffer{};
  EXPECT_EQ(
      sync_uart.ReceiveUntil(rx_buffer, chrono::SystemClock::now() + 500ms),
      OkStatus());
  EXPECT_EQ(rx_buffer[0], std::byte(7));
  EXPECT_EQ(rx_buffer[1], std::byte(8));
  EXPECT_EQ(rx_buffer[2], std::byte(9));
}

TEST(SyncBufferedUartTest, ReceiveUntilExpiredDeadline) {
  auto mock = std::make_unique<NiceMock<GmockBufferedUartDriver>>();
  SyncBufferedUart sync_uart(std::move(mock));

  std::array<std::byte, 3> rx_buffer{};
  EXPECT_EQ(
      sync_uart.ReceiveUntil(rx_buffer, chrono::SystemClock::now() - 100ms),
      Status::DeadlineExceeded());
}

TEST(SyncBufferedUartTest, ReceiveForDriverError) {
  auto mock = std::make_unique<NiceMock<GmockBufferedUartDriver>>();
  EXPECT_CALL(*mock, Receive(_))
      .WillOnce(Return(StatusWithSize(Status::Unavailable(), 0)));

  SyncBufferedUart sync_uart(std::move(mock));
  std::array<std::byte, 3> rx_buffer{};
  EXPECT_EQ(sync_uart.ReceiveFor(rx_buffer, 100ms), Status::Unavailable());
}

TEST(SyncBufferedUartTest, ReceiveForTimeout) {
  auto mock = std::make_unique<NiceMock<GmockBufferedUartDriver>>();
  EXPECT_CALL(*mock, Receive(_))
      .WillRepeatedly(Return(StatusWithSize(Status::ResourceExhausted(), 0)));

  SyncBufferedUart sync_uart(std::move(mock));
  std::array<std::byte, 3> rx_buffer{};
  EXPECT_EQ(sync_uart.ReceiveFor(rx_buffer, 20ms), Status::DeadlineExceeded());
}

TEST(SyncBufferedUartTest, ReceiveForBlockingSuccess) {
  auto mock = std::make_unique<NiceMock<GmockBufferedUartDriver>>();
  pw::Function<void(size_t)> rx_ready_notifier;

  EXPECT_CALL(*mock, SetRxReadyNotifier(_))
      .WillRepeatedly(
          [&rx_ready_notifier](pw::Function<void(size_t)> on_ready) {
            rx_ready_notifier = std::move(on_ready);
            return OkStatus();
          });

  const auto expected = pw::bytes::Array<99, 100, 101>();
  EXPECT_CALL(*mock, Receive(_))
      .WillOnce(Return(StatusWithSize(Status::ResourceExhausted(), 0)))
      .WillOnce([&expected](pw::span<std::byte> rx) {
        std::copy(expected.begin(), expected.end(), rx.begin());
        return StatusWithSize(OkStatus(), expected.size());
      });

  SyncBufferedUart sync_uart(std::move(mock));
  std::array<std::byte, 3> rx_buffer{};

  pw::thread::test::TestThreadContext thread_ctx;
  pw::Thread notifier_thread(thread_ctx.options(), [&]() {
    pw::this_thread::sleep_for(20ms);
    if (rx_ready_notifier) {
      rx_ready_notifier(3);
    }
  });

  EXPECT_EQ(sync_uart.ReceiveFor(rx_buffer, 1000ms), OkStatus());
  notifier_thread.join();

  EXPECT_EQ(rx_buffer[0], std::byte(99));
  EXPECT_EQ(rx_buffer[1], std::byte(100));
  EXPECT_EQ(rx_buffer[2], std::byte(101));
}

TEST(SyncBufferedUartTest, ReceiveForMultiChunkBlocking) {
  auto mock = std::make_unique<NiceMock<GmockBufferedUartDriver>>();
  pw::Function<void(size_t)> rx_ready_notifier;

  EXPECT_CALL(*mock, SetRxReadyNotifier(_))
      .WillRepeatedly(
          [&rx_ready_notifier](pw::Function<void(size_t)> on_ready) {
            rx_ready_notifier = std::move(on_ready);
            return OkStatus();
          });

  const auto chunk1 = pw::bytes::Array<1, 2>();
  const auto chunk2 = pw::bytes::Array<3, 4>();

  EXPECT_CALL(*mock, Receive(_))
      .WillOnce([&chunk1](pw::span<std::byte> rx) {
        std::copy(chunk1.begin(), chunk1.end(), rx.begin());
        return StatusWithSize(Status::ResourceExhausted(), chunk1.size());
      })
      .WillOnce([&chunk2](pw::span<std::byte> rx) {
        std::copy(chunk2.begin(), chunk2.end(), rx.begin());
        return StatusWithSize(OkStatus(), chunk2.size());
      });

  SyncBufferedUart sync_uart(std::move(mock));
  std::array<std::byte, 4> rx_buffer{};

  pw::thread::test::TestThreadContext thread_ctx;
  pw::Thread notifier_thread(thread_ctx.options(), [&]() {
    pw::this_thread::sleep_for(20ms);
    if (rx_ready_notifier) {
      rx_ready_notifier(2);
    }
  });

  EXPECT_EQ(sync_uart.ReceiveFor(rx_buffer, 1000ms), OkStatus());
  notifier_thread.join();

  for (size_t i = 0; i < 4; ++i) {
    EXPECT_EQ(rx_buffer[i], std::byte(i + 1));
  }
}

TEST(SyncBufferedUartTest, ConcurrentDuplexOperation) {
  auto mock = std::make_unique<NiceMock<GmockBufferedUartDriver>>();
  pw::Function<void(size_t)> tx_ready_notifier;
  pw::Function<void(size_t)> rx_ready_notifier;

  EXPECT_CALL(*mock, SetTxReadyNotifier(_))
      .WillRepeatedly(
          [&tx_ready_notifier](pw::Function<void(size_t)> on_ready) {
            tx_ready_notifier = std::move(on_ready);
            return OkStatus();
          });

  EXPECT_CALL(*mock, SetRxReadyNotifier(_))
      .WillRepeatedly(
          [&rx_ready_notifier](pw::Function<void(size_t)> on_ready) {
            rx_ready_notifier = std::move(on_ready);
            return OkStatus();
          });

  const auto tx_data = pw::bytes::Array<10, 20, 30, 40>();
  const auto rx_expected = pw::bytes::Array<50, 60, 70, 80>();

  // Both Send and Receive will need a notification to complete their 2nd chunk
  const auto tx_chunk1 = pw::bytes::Array<10, 20>();
  const auto tx_chunk2 = pw::bytes::Array<30, 40>();
  const auto rx_chunk1 = pw::bytes::Array<50, 60>();
  const auto rx_chunk2 = pw::bytes::Array<70, 80>();

  EXPECT_CALL(*mock, Send(_))
      .WillOnce([&tx_chunk1](pw::span<const std::byte> tx) {
        EXPECT_EQ(tx.size(), 4u);
        return StatusWithSize(Status::ResourceExhausted(), tx_chunk1.size());
      })
      .WillOnce([&tx_chunk2](pw::span<const std::byte> tx) {
        EXPECT_EQ(tx.size(), 2u);
        EXPECT_TRUE(std::equal(tx.begin(), tx.end(), tx_chunk2.begin()));
        return StatusWithSize(OkStatus(), tx_chunk2.size());
      });

  EXPECT_CALL(*mock, Receive(_))
      .WillOnce([&rx_chunk1](pw::span<std::byte> rx) {
        EXPECT_EQ(rx.size(), 4u);
        std::copy(rx_chunk1.begin(), rx_chunk1.end(), rx.begin());
        return StatusWithSize(Status::ResourceExhausted(), rx_chunk1.size());
      })
      .WillOnce([&rx_chunk2](pw::span<std::byte> rx) {
        EXPECT_EQ(rx.size(), 2u);
        std::copy(rx_chunk2.begin(), rx_chunk2.end(), rx.begin());
        return StatusWithSize(OkStatus(), rx_chunk2.size());
      });

  SyncBufferedUart sync_uart(std::move(mock));

  std::array<std::byte, 4> rx_buffer{};
  Status tx_status = Status::Unknown();
  Status rx_status = Status::Unknown();

  pw::thread::test::TestThreadContext tx_thread_ctx;
  pw::thread::test::TestThreadContext rx_thread_ctx;
  pw::thread::test::TestThreadContext notifier_thread_ctx;

  // Run Send on tx_thread
  pw::Thread tx_thread(tx_thread_ctx.options(), [&]() {
    tx_status = sync_uart.SendFor(tx_data, 1000ms);
  });

  // Run Receive on rx_thread concurrently
  pw::Thread rx_thread(rx_thread_ctx.options(), [&]() {
    rx_status = sync_uart.ReceiveFor(rx_buffer, 1000ms);
  });

  // Background thread that notifies both TX and RX after a small delay
  pw::Thread notifier_thread(notifier_thread_ctx.options(), [&]() {
    pw::this_thread::sleep_for(20ms);
    if (tx_ready_notifier) {
      tx_ready_notifier(2);
    }
    pw::this_thread::sleep_for(20ms);
    if (rx_ready_notifier) {
      rx_ready_notifier(2);
    }
  });

  tx_thread.join();
  rx_thread.join();
  notifier_thread.join();

  EXPECT_EQ(tx_status, OkStatus());
  EXPECT_EQ(rx_status, OkStatus());
  for (size_t i = 0; i < rx_expected.size(); ++i) {
    EXPECT_EQ(rx_buffer[i], rx_expected[i]);
  }
}

TEST(SyncBufferedUartTest, NullDriver) {
  SyncBufferedUart sync_uart(nullptr);

  const auto tx_data = pw::bytes::Array<1, 2>();
  EXPECT_EQ(sync_uart.SendFor(tx_data, 100ms), Status::FailedPrecondition());

  std::array<std::byte, 2> rx_data{};
  EXPECT_EQ(sync_uart.ReceiveFor(rx_data, 100ms), Status::FailedPrecondition());
}

}  // namespace
}  // namespace pw::hal::uart
