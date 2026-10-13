// Copyright 2021 The Pigweed Authors
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

#include "pw_spi_linux/spi.h"

#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <linux/types.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstring>

#include "pw_log/log.h"
#include "pw_status/try.h"

namespace pw::spi {

LinuxInitiator::~LinuxInitiator() {
  if (fd_ >= 0) {
    close(fd_);
  }
}

Status LinuxInitiator::DoConfigure(const Config& config) {
  if (current_config_ == config) {
    // Don't waste time issuing ioctls if the config is not actually changing.
    return OkStatus();
  }

  // Map clock polarity/phase to Linux userspace equivalents
  uint32_t mode = 0;
  if (config.polarity == ClockPolarity::kActiveLow) {
    mode |= SPI_CPOL;  // Clock polarity -- signal is high when idle
  }
  if (config.phase == ClockPhase::kFallingEdge) {
    mode |= SPI_CPHA;  // Clock phase -- latch on falling edge
  }
  if (ioctl(fd_, SPI_IOC_WR_MODE32, &mode) < 0) {
    PW_LOG_ERROR("Unable to set SPI mode");
    return Status::InvalidArgument();
  }

  // Configure LSB/MSB first
  uint8_t lsb_first = 0;
  if (config.bit_order == BitOrder::kLsbFirst) {
    lsb_first = 1;  // non-zero value indicates LSB first
  }
  if (ioctl(fd_, SPI_IOC_WR_LSB_FIRST, &lsb_first) < 0) {
    PW_LOG_ERROR("Unable to set SPI LSB");
    return Status::InvalidArgument();
  }

  // Configure bits-per-word
  uint8_t bits_per_word = config.bits_per_word();
  if (ioctl(fd_, SPI_IOC_WR_BITS_PER_WORD, &bits_per_word) < 0) {
    PW_LOG_ERROR("Unable to set SPI Bits Per Word");
    return Status::InvalidArgument();
  }

  // Configure maximum bus speed
  if (ioctl(fd_, SPI_IOC_WR_MAX_SPEED_HZ, &max_speed_hz_) < 0) {
    PW_LOG_ERROR("Unable to set SPI Max Speed");
    return Status::InvalidArgument();
  }

  current_config_ = config;
  return OkStatus();
}

Status LinuxInitiator::DoWriteRead(ConstByteSpan write_buffer,
                                   ByteSpan read_buffer) {
  // Configure a full-duplex transfer using ioctl()

  // Neither read nor write: invalid.
  if (read_buffer.empty() && write_buffer.empty()) {
    return Status::InvalidArgument();
  }

  struct spi_ioc_transfer transaction[2] = {};
  unsigned long request = SPI_IOC_MESSAGE(1);  // macro arg must be constant

  if (read_buffer.empty()) {
    // Write-only.
    transaction[0].tx_buf = reinterpret_cast<uintptr_t>(write_buffer.data());
    transaction[0].len = static_cast<uint32_t>(write_buffer.size());
  } else if (write_buffer.empty()) {
    // Read-only.
    transaction[0].rx_buf = reinterpret_cast<uintptr_t>(read_buffer.data());
    transaction[0].len = static_cast<uint32_t>(read_buffer.size());
  } else {
    // Read+Write.

    // The common read+write amount.
    const size_t common_len = std::min(write_buffer.size(), read_buffer.size());
    transaction[0].tx_buf = reinterpret_cast<uintptr_t>(write_buffer.data());
    transaction[0].rx_buf = reinterpret_cast<uintptr_t>(read_buffer.data());
    transaction[0].len = static_cast<uint32_t>(common_len);

    // Handle different-sized buffers with a compound transaction
    if (write_buffer.size() > common_len) {
      auto write_remainder = write_buffer.subspan(common_len);
      transaction[1].tx_buf =
          reinterpret_cast<uintptr_t>(write_remainder.data());
      transaction[1].len = static_cast<uint32_t>(write_remainder.size());
      request = SPI_IOC_MESSAGE(2);
    } else if (read_buffer.size() > common_len) {
      auto read_remainder = read_buffer.subspan(common_len);
      transaction[1].rx_buf =
          reinterpret_cast<uintptr_t>(read_remainder.data());
      transaction[1].len = static_cast<uint32_t>(read_remainder.size());
      request = SPI_IOC_MESSAGE(2);
    }
  }

  if (ioctl(fd_, request, transaction) < 0) {
    PW_LOG_ERROR("Unable to perform SPI transfer");
    return Status::Unknown();
  }

  return OkStatus();
}

Status LinuxChipSelector::SetActive(bool /*active*/) {
  // Note: For Linux' SPI userspace support, chip-select control is not exposed
  // directly to the user.  This limits our ability to use the SPI HAL to do
  // composite (multi read-write) transactions with the PW SPI HAL, as Linux
  // performs composite transactions with a single ioctl() call using an array
  // of descriptors provided as a parameter -- there's no way of separating
  // individual operations from userspace.  This could be addressed with a
  // direct "Composite" transaction HAL API, or by using a raw GPIO
  // to control of chip select from userspace (which is not common practice).
  return OkStatus();
}

}  // namespace pw::spi
