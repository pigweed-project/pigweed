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

// This example demonstrates a common use case for pw_async2: interacting with
// hardware that uses interrupts. It creates a fake UART device with an
// asynchronous reading interface and a separate thread that simulates hardware
// interrupts.

#include <termios.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <thread>

#include "pw_async2/basic_dispatcher.h"
#include "pw_async2/poll.h"
#include "pw_async2/try.h"
#include "pw_async2/value_future.h"
#include "pw_containers/inline_queue.h"
#include "pw_log/log.h"
#include "pw_status/try.h"
#include "pw_sync/interrupt_spin_lock.h"
#include "pw_sync/mutex.h"

namespace {

// DOCSTAG: [pw_async2-examples-interrupt-uart]
// A fake UART device that provides an asynchronous byte reading interface.
class FakeUart {
 public:
  // Asynchronously reads a single byte from the UART.
  //
  // If a byte is available in the receive queue, it returns a Future that is
  // already `Ready(byte)`.
  // If another task is already waiting for a byte, it returns a Future that is
  // `Ready(Status::Unavailable())`.
  // Otherwise, it returns a `Pending` Future and arranges for the task to be
  // woken up when a byte arrives.
  pw::async2::ValueFuture<pw::Result<char>> ReadByte() {
    // Blocking inside an async function is generally an anti-pattern because it
    // prevents the single-threaded dispatcher from making progress on other
    // tasks. However, using `pw::sync::InterruptSpinLock` here is acceptable
    // due to the short-running nature of the ISR.
    std::lock_guard lock(lock_);

    using ResultFuture = pw::async2::ValueFuture<pw::Result<char>>;

    // Check if the UART has been put into a failure state.
    if (!status_.ok()) {
      return ResultFuture::Resolved(status_);
    }

    // If a byte is already in the queue, return it immediately.
    if (!rx_queue_.empty()) {
      char byte = rx_queue_.front();
      rx_queue_.pop();
      return ResultFuture::Resolved(byte);
    }

    // If the queue is empty, the operation can't complete yet. Arrange for the
    // task to be woken up later.
    // `TryGet` returns a future if one is available, or `std::nullopt` if
    // another task is already waiting.
    std::optional<ResultFuture> future = provider_.TryGet();
    if (!future.has_value()) {
      return ResultFuture::Resolved(pw::Status::Unavailable());
    }
    return std::move(*future);
  }

  // Simulates a hardware interrupt that receives a character.
  // This method is safe to call from an interrupt handler.
  void HandleReceiveInterrupt() {
    std::lock_guard lock(lock_);
    if (rx_queue_.full()) {
      // Buffer is full, drop the character.
      PW_LOG_WARN("UART RX buffer full, dropping character.");
      return;
    }

    // Generate a random lowercase letter to simulate receiving data.
    char c = 'a' + (std::rand() % 26);

    // If a task is waiting for a byte, give it the byte immediately.
    if (provider_.has_future()) {
      provider_.Resolve(c);
    } else {
      // Otherwise, store the byte in the queue.
      rx_queue_.push(c);
    }
  }

  // Puts the UART into a terminated state.
  void set_status(pw::Status status) {
    std::lock_guard lock(lock_);
    status_ = status;
    // Wake up any pending task so it can observe the status change and exit.
    provider_.Resolve(status);
  }

 private:
  pw::sync::InterruptSpinLock lock_;
  pw::InlineQueue<char, 16> rx_queue_ PW_GUARDED_BY(lock_);
  pw::async2::ValueProvider<pw::Result<char>> provider_;
  pw::Status status_;
};
// DOCSTAG: [pw_async2-examples-interrupt-uart]

FakeUart fake_uart;

void SigintHandler(int /*signum*/) {
  std::printf("\r\033[K");  // Clear ^C from the terminal.
  fake_uart.set_status(pw::Status::Cancelled());
}

}  // namespace

int main() {
  std::srand(static_cast<unsigned>(std::time(nullptr)));
  std::signal(SIGINT, SigintHandler);

  termios before;
  termios config;

  tcgetattr(STDIN_FILENO, &before);
  config = before;
  config.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
  tcsetattr(STDIN_FILENO, TCSANOW, &config);

  // DOCSTAG: [pw_async2-examples-interrupt-reader]
  pw::async2::BasicDispatcher dispatcher;

  // Create a task that reads from the UART in a loop.
  class ReaderTask : public pw::async2::Task {
   public:
    ReaderTask(FakeUart& uart) : uart_(uart) {}

   private:
    pw::async2::Poll<> DoPend(pw::async2::Context& cx) override {
      while (true) {
        if (!future_.is_pendable()) {
          future_ = uart_.ReadByte();
        }

        PW_TRY_READY_ASSIGN(pw::Result<char> result, future_.Pend(cx));

        if (!result.ok()) {
          PW_LOG_ERROR("UART read failed: %s", result.status().str());
          break;
        }

        PW_LOG_INFO("Received: %c", result.value());
      }

      return pw::async2::Ready();
    }

    FakeUart& uart_;
    pw::async2::ValueFuture<pw::Result<char>> future_;
  };

  ReaderTask reader_task(fake_uart);

  // Post the task to the dispatcher to schedule it for execution.
  dispatcher.Post(reader_task);
  // DOCSTAG: [pw_async2-examples-interrupt-reader]

  std::thread interrupt_thread([] {
    while (true) {
      char c;
      read(STDIN_FILENO, &c, 1);
      if (c == ' ') {
        fake_uart.HandleReceiveInterrupt();
      }
    }
  });
  interrupt_thread.detach();

  PW_LOG_INFO(
      "Fake UART initialized. Press spacebar to simulate receiving a "
      "character.");
  PW_LOG_INFO("Press Ctrl+C to exit.");

  // Run the dispatcher until all registered tasks terminate.
  dispatcher.RunToCompletion();

  tcsetattr(STDIN_FILENO, TCSANOW, &before);
  return 0;
}
