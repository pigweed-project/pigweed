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
#include "pw_digital_io_linux/notifier.h"

#include <sys/eventfd.h>

#include <chrono>
#include <thread>

#include "log_errno.h"
#include "pw_assert/check.h"
#include "pw_log/log.h"
#include "pw_sync/counting_semaphore.h"
#include "pw_thread/thread.h"
#include "pw_thread_stl/options.h"
#include "pw_unit_test/framework.h"

namespace pw::digital_io {
namespace {

using namespace std::chrono_literals;

constexpr auto kWaitForDataTimeout = 100ms;

class NotifierTest : public ::testing::Test {
 protected:
  void SetUp() override {
    PW_TEST_ASSERT_OK_AND_ASSIGN(notifier_, LinuxGpioNotifier::Create());
  }

  LinuxGpioNotifier& notifier() { return *notifier_; }

 private:
  std::shared_ptr<LinuxGpioNotifier> notifier_;
};

class FakeLine : public LinuxGpioNotifier::Handler {
 public:
  explicit FakeLine(LinuxGpioNotifier& notifier)
      : notifier_(notifier), event_fd_(eventfd(0, EFD_SEMAPHORE)) {
    PW_CHECK_INT_GE(event_fd_, 0, "Failed to create event fd: %d", errno);
  }

  ~FakeLine() override {
    // Unregister now, if still registered, and make errors fail the test.
    PW_TEST_EXPECT_OK(Unregister());
    int result = close(event_fd_);
    PW_CHECK_INT_EQ(result, 0, "Failed to close event fd, err %d", errno);
  }

  FakeLine(const FakeLine&) = delete;
  FakeLine& operator=(const FakeLine&) = delete;

  // Register the line with the notifier.
  pw::Status Register() {
    PW_CHECK(!registered_);
    registered_ = true;
    return notifier_.RegisterLine(event_fd_, *this);
  }

  // Unregister the line from the notifier.
  pw::Status Unregister() {
    if (!registered_) {
      return OkStatus();
    }
    registered_ = false;
    return notifier_.UnregisterLine(event_fd_);
  }

  // Atomically send one or more events.
  void SendEvents(int count) {
    PW_CHECK_INT_GE(count, 1, "Must send one or more events");
    uint64_t data = static_cast<uint64_t>(count);
    ssize_t result = write(event_fd_, &data, sizeof(data));
    PW_CHECK_INT_EQ(result,
                    sizeof(data),
                    "Failed to write to event fd: " ERRNO_FORMAT_STRING,
                    ERRNO_FORMAT_ARGS(errno));
  }

  // Wait until the line has received an event or internal timeout expires.
  bool TryWaitForData() {
    return received_events_.try_acquire_for(kWaitForDataTimeout);
  }

  unsigned int total_received_events() const { return total_received_events_; }

 protected:
  // Implement LinuxGpioNotifier::Handler
  void HandleEvents() override {
    uint64_t val;
    auto size_read = read(event_fd_, &val, sizeof(val));
    PW_CHECK_INT_GE(size_read,
                    1,
                    "Failed to read an event: " ERRNO_FORMAT_STRING,
                    ERRNO_FORMAT_ARGS(errno));
    ++total_received_events_;
    received_events_.release(1);
  }

  LinuxGpioNotifier& notifier() { return notifier_; }
  int event_fd() const { return event_fd_; }

 private:
  LinuxGpioNotifier& notifier_;
  int event_fd_;
  unsigned int total_received_events_ = 0;
  pw::sync::CountingSemaphore received_events_;

  bool registered_ = false;
};

TEST_F(NotifierTest, TestNoEvent) {
  FakeLine line(notifier());
  PW_TEST_ASSERT_OK(line.Register());

  constexpr auto timeout = 0;  // Don't block
  auto result = notifier().WaitForEvents(timeout);
  EXPECT_EQ(result.status(), pw::Status::DeadlineExceeded());

  EXPECT_EQ(line.total_received_events(), 0u);

  PW_TEST_EXPECT_OK(line.Unregister());
}

TEST_F(NotifierTest, TestSendReceiveOneEventManual) {
  FakeLine line(notifier());
  PW_TEST_ASSERT_OK(line.Register());

  constexpr unsigned int num_events = 1;

  line.SendEvents(num_events);

  constexpr auto timeout = 0;  // Don't block
  PW_TEST_ASSERT_OK_AND_ASSIGN(unsigned int count,
                               notifier().WaitForEvents(timeout));

  EXPECT_EQ(count, num_events);
  EXPECT_EQ(line.total_received_events(), num_events);

  PW_TEST_EXPECT_OK(line.Unregister());
}

TEST_F(NotifierTest, TestSendReceiveMultipleEventsManual) {
  FakeLine line(notifier());
  PW_TEST_ASSERT_OK(line.Register());

  constexpr unsigned int num_events = 4;

  line.SendEvents(num_events);

  // WaitForEvents will only handle one event per line, per iteration.
  // So call it in a loop until it expires.
  unsigned int total_result = 0;
  while (true) {
    constexpr auto timeout = 0;  // Don't block
    auto result = notifier().WaitForEvents(timeout);
    if (!result.ok()) {
      EXPECT_EQ(result.status(), pw::Status::DeadlineExceeded());
      break;
    }
    total_result += result.value();
  }

  EXPECT_EQ(total_result, num_events);
  EXPECT_EQ(line.total_received_events(), num_events);

  PW_TEST_EXPECT_OK(line.Unregister());
}

TEST_F(NotifierTest, TestSendReceiveEventsThread) {
  FakeLine line(notifier());
  PW_TEST_ASSERT_OK(line.Register());

  pw::Thread notif_thread(pw::thread::stl::Options(), notifier());

  constexpr unsigned int num_events = 3;

  line.SendEvents(num_events);

  while (line.TryWaitForData()) {
  }

  EXPECT_EQ(line.total_received_events(), num_events);

  PW_TEST_EXPECT_OK(line.Unregister());

  notifier().CancelWait();
  notif_thread.join();
}

TEST_F(NotifierTest, TestRegisterLineMultipleLinesThread) {
  // Make primary line
  FakeLine line1(notifier());
  PW_TEST_ASSERT_OK(line1.Register());

  pw::Thread notif_thread(pw::thread::stl::Options(), notifier());

  line1.SendEvents(1);
  EXPECT_TRUE(line1.TryWaitForData());

  {
    // Make secondary line in smaller scope
    FakeLine line2(notifier());
    PW_TEST_ASSERT_OK(line2.Register());

    line1.SendEvents(1);
    line2.SendEvents(1);

    EXPECT_TRUE(line1.TryWaitForData());
    EXPECT_TRUE(line2.TryWaitForData());

    PW_TEST_EXPECT_OK(line2.Unregister());
  }

  // Line 1 is once again the only line that is registered.
  line1.SendEvents(1);
  EXPECT_TRUE(line1.TryWaitForData());

  PW_TEST_EXPECT_OK(line1.Unregister());

  notifier().CancelWait();
  notif_thread.join();
}

}  // namespace
}  // namespace pw::digital_io
