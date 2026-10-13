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

#include "pw_stream_uart_linux/stream.h"

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cinttypes>

#include "pw_log/log.h"

namespace pw::stream {

Result<speed_t> BaudRateToSpeed(uint32_t baud_rate) {
  switch (baud_rate) {
    case 9600:
      return static_cast<speed_t>(B9600);
    case 19200:
      return static_cast<speed_t>(B19200);
    case 38400:
      return static_cast<speed_t>(B38400);
    case 57600:
      return static_cast<speed_t>(B57600);
    case 115200:
      return static_cast<speed_t>(B115200);
    case 230400:
      return static_cast<speed_t>(B230400);
    case 460800:
      return static_cast<speed_t>(B460800);
    case 500000:
      return static_cast<speed_t>(B500000);
    case 576000:
      return static_cast<speed_t>(B576000);
    case 921600:
      return static_cast<speed_t>(B921600);
    case 1000000:
      return static_cast<speed_t>(B1000000);
    case 1152000:
      return static_cast<speed_t>(B1152000);
    case 1500000:
      return static_cast<speed_t>(B1500000);
    case 2000000:
      return static_cast<speed_t>(B2000000);
    case 2500000:
      return static_cast<speed_t>(B2500000);
    case 3000000:
      return static_cast<speed_t>(B3000000);
    case 3500000:
      return static_cast<speed_t>(B3500000);
    case 4000000:
      return static_cast<speed_t>(B4000000);
    default:
      return Status::InvalidArgument();
  }
}

Status UartStreamLinux::Open(const char* path, Config config) {
  std::optional<speed_t> speed;
  if (config.baud_rate.has_value()) {
    const auto speed_result = BaudRateToSpeed(*config.baud_rate);
    if (!speed_result.ok()) {
      PW_LOG_ERROR("Unsupported baud rate: %" PRIu32, *config.baud_rate);
      return speed_result.status();
    }
    speed = speed_result.value();
  }

  if (fd_ != kInvalidFd) {
    PW_LOG_ERROR("UART device already open");
    return Status::FailedPrecondition();
  }

  fd_ = open(path, O_RDWR);
  if (fd_ < 0) {
    PW_LOG_ERROR(
        "Failed to open UART device '%s', %s", path, std::strerror(errno));
    return Status::Unknown();
  }

  struct termios tty;
  int result = tcgetattr(fd_, &tty);
  if (result < 0) {
    PW_LOG_ERROR("Failed to get TTY attributes for '%s', %s",
                 path,
                 std::strerror(errno));
    return Status::Unknown();
  }

  cfmakeraw(&tty);

  if (speed.has_value()) {
    result = cfsetspeed(&tty, *speed);
    if (result < 0) {
      PW_LOG_ERROR(
          "Failed to set TTY speed for '%s', %s", path, std::strerror(errno));
      return Status::Unknown();
    }
  }

  if (config.flow_control.has_value()) {
    if (*config.flow_control) {
      // Enable hardware flow control.
      tty.c_cflag |= (CRTSCTS);
    } else {
      // Disable hardware flow control.
      tty.c_cflag &= ~(CRTSCTS);
    }
  }

  result = tcsetattr(fd_, TCSANOW, &tty);
  if (result < 0) {
    PW_LOG_ERROR("Failed to set TTY attributes for '%s', %s",
                 path,
                 std::strerror(errno));
    return Status::Unknown();
  }

  return OkStatus();
}

void UartStreamLinux::Close() {
  if (fd_ != kInvalidFd) {
    close(fd_);
    fd_ = kInvalidFd;
  }
}

Status UartStreamLinux::DoWrite(ConstByteSpan data) {
  const size_t size = data.size_bytes();
  size_t written = 0;
  while (written < size) {
    const ssize_t bytes = write(fd_, &data[written], size - written);
    if (bytes < 0) {
      PW_LOG_ERROR("Failed to write to UART, %s", std::strerror(errno));
      return Status::Unknown();
    }
    written += static_cast<size_t>(bytes);
  }
  return OkStatus();
}

StatusWithSize UartStreamLinux::DoRead(ByteSpan dest) {
  const ssize_t bytes = read(fd_, &dest[0], dest.size_bytes());
  if (bytes < 0) {
    PW_LOG_ERROR("Failed to read from UART, %s", std::strerror(errno));
    return StatusWithSize::Unknown();
  }
  return StatusWithSize(static_cast<size_t>(bytes));
}

}  // namespace pw::stream
