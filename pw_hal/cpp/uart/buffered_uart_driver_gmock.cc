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

#include "pw_hal/uart/buffered_uart_driver_gmock.h"

namespace pw::hal::uart {

GmockBufferedUartDriver::GmockBufferedUartDriver(size_t tx_buffer_capacity,
                                                 size_t rx_buffer_capacity)
    : BufferedUartDriver(tx_buffer_capacity, rx_buffer_capacity) {}
GmockBufferedUartDriver::~GmockBufferedUartDriver() = default;

}  // namespace pw::hal::uart
