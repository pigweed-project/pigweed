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
#include "pw_stream_uart_mcuxpresso/interrupt_safe_writer.h"

#include "fsl_clock.h"
#include "pw_function/scope_guard.h"

namespace pw::stream {

pw::Status InterruptSafeUartWriterMcuxpresso::Enable() {
  usart_config_t usart_config;
  USART_GetDefaultConfig(&usart_config);
  usart_config.baudRate_Bps = baudrate_;
  usart_config.enableRx = false;
  usart_config.enableTx = true;
  usart_config.enableHardwareFlowControl = flow_control_;

  // Acquire the clock_tree element. Note that this function only requires the
  // IP clock and not the functional clock. However, ClockMcuxpressoClockIp
  // only provides the combined element, so that's what we use here.
  // Make sure it's released on any function exits through a scoped guard.
  PW_TRY(clock_tree_element_.Acquire());
  pw::ScopeGuard guard([this] { clock_tree_element_.Release().IgnoreError(); });

  if (USART_Init(base(), &usart_config, CLOCK_GetFreq(clock_name_)) !=
      kStatus_Success) {
    return pw::Status::Internal();
  }

  return pw::OkStatus();
}

pw::Status InterruptSafeUartWriterMcuxpresso::DoWrite(pw::ConstByteSpan data) {
  // NOTE: This function does not use USART_WriteBlocking() because that may
  // result in a kStatus_USART_Timeout if UART_RETRY_TIMES is defined.
  //
  // This function will block indefinitely attempting to write (if e.g.,
  // hardware flow control is enabled and CTS is deasserted).

  // Do nothing if input data is empty.
  if (data.empty()) {
    return pw::OkStatus();
  }

  // Acquire the clock_tree_element. Use a scoped guard so it's released when
  // this function returns.
  PW_TRY(clock_tree_element_.Acquire());
  pw::ScopeGuard guard([this] { clock_tree_element_.Release().IgnoreError(); });

  // Verify TX FIFO is enabled.
  if (!(base()->FIFOCFG & USART_FIFOCFG_ENABLETX_MASK)) {
    return pw::Status::FailedPrecondition();
  }

  // Write all of the data into the TX FIFO.
  for (std::byte b : data) {
    // Wait until the FIFO is not full.
    while (true) {
      if (base()->FIFOSTAT & USART_FIFOSTAT_TXNOTFULL_MASK) {
        break;
      }
    }

    // Write the byte into the TX FIFO.
    base()->FIFOWR = static_cast<uint8_t>(b);
  }

  // Wait for the transmitter to become idle, indicating that all queued data
  // has been transmitted.
  while (true) {
    if (base()->STAT & USART_STAT_TXIDLE_MASK) {
      break;
    }
  }

  return pw::OkStatus();
}

}  // namespace pw::stream
