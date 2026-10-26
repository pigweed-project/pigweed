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

#include "gmock/gmock.h"
#include "pw_function/function.h"
#include "pw_hal/uart/uart_driver.h"
#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"

namespace pw::hal::uart {

class GmockBufferedUartDriver : public BufferedUartDriver {
 public:
  explicit GmockBufferedUartDriver(size_t tx_buffer_capacity = 0,
                                   size_t rx_buffer_capacity = 0);
  ~GmockBufferedUartDriver() override;

  MOCK_METHOD(Status, Enable, (), (override));
  MOCK_METHOD(Status, Disable, (), (override));

  MOCK_METHOD(size_t, tx_bytes_available, (), (const, override));
  MOCK_METHOD(Status, SetTxReadyNotifyLevel, (size_t bytes), (override));
  MOCK_METHOD(Status,
              SetTxReadyNotifier,
              (pw::Function<void(size_t)> on_ready),
              (override));
  MOCK_METHOD(StatusWithSize,
              Send,
              (pw::span<const std::byte> tx_buffer),
              (override));

  MOCK_METHOD(size_t, rx_bytes_available, (), (const, override));
  MOCK_METHOD(Status, SetRxReadyNotifyLevel, (size_t bytes), (override));
  MOCK_METHOD(Status,
              SetRxReadyNotifier,
              (pw::Function<void(size_t)> on_ready),
              (override));
  MOCK_METHOD(StatusWithSize,
              Receive,
              (pw::span<std::byte> rx_buffer),
              (override));
};

}  // namespace pw::hal::uart
